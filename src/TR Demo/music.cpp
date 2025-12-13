#include "music.h"
#include <string.h>
#include <stdlib.h>

// RXDK/XDK: DirectSound types live in xtl headers.
static LPDIRECTSOUND8       s_ds = NULL;
static LPDIRECTSOUNDBUFFER  s_buf = NULL;

static HANDLE s_file = INVALID_HANDLE_VALUE;

static DWORD  s_dataOffset = 0;
static DWORD  s_dataSize = 0;
static DWORD  s_dataPos = 0;

static WAVEFORMATEX s_wfx;
static bool   s_ready = false;
static bool   s_playing = false;

// Streaming buffer
static DWORD  s_bufBytes = 0;
static DWORD  s_writeCursor = 0;

// Keep some safety margin ahead of play cursor
static const DWORD STREAM_CHUNK_BYTES = (32 * 1024);   // write in chunks
static const DWORD STREAM_BUF_BYTES = (128 * 1024);  // ring buffer size

// -----------------------------------------------------------------------------
// UV analysis state (integer-only; no float->int casts)
// -----------------------------------------------------------------------------
static volatile LONG s_uvPacked = 0; // packed 4x8-bit: low|mid|high|all

static int s_avgFast = 0; // 0..32767-ish
static int s_avgSlow = 0; // 0..32767-ish

static __forceinline int IAbsI(int v) { return (v < 0) ? -v : v; }

static __forceinline BYTE ClampU8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (BYTE)v;
}

static void UV_PushLevels(int low, int mid, int high, int all)
{
    // Stronger attack, slower decay (more “techno beat” reactive)
    // current packed:
    LONG curP = s_uvPacked;

    int curL = (curP >> 0) & 255;
    int curM = (curP >> 8) & 255;
    int curH = (curP >> 16) & 255;
    int curA = (curP >> 24) & 255;

    // Attack: jump quickly to higher values
    // Decay: drop more slowly
    const int decayShift = 3; // 1/8 decay step

    auto blend = [&](int cur, int nxt) -> int
        {
            if (nxt >= cur) return nxt;
            int d = cur - nxt;
            d = d >> decayShift;
            if (d < 1) d = 1;
            return cur - d;
        };

    curL = blend(curL, low);
    curM = blend(curM, mid);
    curH = blend(curH, high);
    curA = blend(curA, all);

    LONG pack = (LONG)((curL & 255) | ((curM & 255) << 8) | ((curH & 255) << 16) | ((curA & 255) << 24));
    s_uvPacked = pack;
}

static void UV_AnalyzePCM16(const void* data, DWORD bytes)
{
    if (!data || bytes < 4)
        return;

    // Must match supported formats (PCM16, 1-2ch). ParseWav enforces this.
    int ch = (int)s_wfx.nChannels;
    if (ch < 1) ch = 1;
    if (ch > 2) ch = 2;

    // sample frames
    DWORD frames = bytes / (DWORD)(ch * 2);
    if (frames == 0)
        return;

    const short* s16 = (const short*)data;

    unsigned long long sumLow = 0;
    unsigned long long sumMid = 0;
    unsigned long long sumHigh = 0;
    unsigned long long sumAll = 0;

    // integer smoothing constants
    // fast: 1/16, slow: 1/128
    const int fastShift = 4;
    const int slowShift = 7;

    for (DWORD i = 0; i < frames; ++i)
    {
        int v = 0;
        if (ch == 2)
        {
            // average stereo (integer)
            int a = (int)s16[i * 2 + 0];
            int b = (int)s16[i * 2 + 1];
            v = (a + b) >> 1;
        }
        else
        {
            v = (int)s16[i];
        }

        int av = IAbsI(v); // 0..32767

        // update IIR averages (integer only)
        s_avgFast += (av - s_avgFast) >> fastShift;
        s_avgSlow += (av - s_avgSlow) >> slowShift;

        int low = s_avgSlow;              // bass-ish envelope
        int mid = IAbsI(s_avgFast - s_avgSlow);
        int high = IAbsI(av - s_avgFast);  // treble-ish/transients

        sumLow += (unsigned)low;
        sumMid += (unsigned)mid;
        sumHigh += (unsigned)high;
        sumAll += (unsigned)av;
    }

    // Means (0..32767-ish). Keep integer division.
    unsigned long long n = (unsigned long long)frames;

    unsigned meanLow = (unsigned)(sumLow / n);
    unsigned meanMid = (unsigned)(sumMid / n);
    unsigned meanHigh = (unsigned)(sumHigh / n);
    unsigned meanAll = (unsigned)(sumAll / n);

    // Map to 0..255 with “gain” by shifting less.
    // Tweaked for strong movement on beats.
    int lvlLow = (int)(meanLow >> 5); // /32
    int lvlMid = (int)(meanMid >> 4); // /16
    int lvlHigh = (int)(meanHigh >> 4); // /16
    int lvlAll = (int)(meanAll >> 5); // /32

    // Extra punch
    lvlLow = lvlLow + (lvlLow >> 1); // *1.5
    lvlMid = lvlMid + (lvlMid >> 1);
    lvlHigh = lvlHigh + (lvlHigh >> 1);
    lvlAll = lvlAll + (lvlAll >> 1);

    UV_PushLevels(
        (int)ClampU8(lvlLow),
        (int)ClampU8(lvlMid),
        (int)ClampU8(lvlHigh),
        (int)ClampU8(lvlAll)
    );
}

// ------------------------------------------------------------
// Minimal RIFF/WAV parsing: find "fmt " and "data"
// ------------------------------------------------------------
static bool ReadExact(HANDLE h, void* dst, DWORD bytes)
{
    DWORD got = 0;
    if (!ReadFile(h, dst, bytes, &got, NULL)) return false;
    return got == bytes;
}

static bool SeekAbs(HANDLE h, DWORD pos)
{
    return SetFilePointer(h, (LONG)pos, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER;
}

static DWORD AlignDown(DWORD v, DWORD a) { return (a == 0) ? v : (v / a) * a; }

static bool ParseWav(HANDLE h, WAVEFORMATEX& outFmt, DWORD& outDataOff, DWORD& outDataSize)
{
    // RIFF header: "RIFF" + size + "WAVE"
    DWORD riff = 0, riffSize = 0, wave = 0;
    if (!ReadExact(h, &riff, 4)) return false;
    if (!ReadExact(h, &riffSize, 4)) return false;
    if (!ReadExact(h, &wave, 4)) return false;

    if (riff != 0x46464952) return false; // "RIFF"
    if (wave != 0x45564157) return false; // "WAVE"

    bool haveFmt = false;
    bool haveData = false;

    WAVEFORMATEX fmt = {};
    DWORD dataOff = 0, dataSz = 0;

    // Walk chunks
    while (1)
    {
        DWORD cid = 0, csz = 0;
        DWORD here = SetFilePointer(h, 0, NULL, FILE_CURRENT);
        if (here == INVALID_SET_FILE_POINTER) break;

        if (!ReadExact(h, &cid, 4)) break;
        if (!ReadExact(h, &csz, 4)) break;

        // "fmt "
        if (cid == 0x20746D66)
        {
            // Read WAVEFORMATEX (at least 16 bytes)
            BYTE tmp[64];
            DWORD toRead = csz;
            if (toRead > sizeof(tmp)) toRead = sizeof(tmp);

            if (!ReadExact(h, tmp, toRead)) return false;

            // Skip remainder if fmt chunk bigger than we read
            if (csz > toRead)
                SetFilePointer(h, (LONG)(csz - toRead), NULL, FILE_CURRENT);

            // Copy minimum
            if (csz < 16) return false;

            memcpy(&fmt, tmp, sizeof(WAVEFORMATEX));
            haveFmt = true;
        }
        // "data"
        else if (cid == 0x61746164)
        {
            dataOff = SetFilePointer(h, 0, NULL, FILE_CURRENT);
            dataSz = csz;

            // Skip over data for now
            SetFilePointer(h, (LONG)csz, NULL, FILE_CURRENT);
            haveData = true;
        }
        else
        {
            // skip chunk
            SetFilePointer(h, (LONG)csz, NULL, FILE_CURRENT);
        }

        // pad to even
        if (csz & 1)
            SetFilePointer(h, 1, NULL, FILE_CURRENT);

        if (haveFmt && haveData)
            break;
    }

    if (!haveFmt || !haveData)
        return false;

    // Only support PCM 16-bit for now (safe + simple)
    if (fmt.wFormatTag != WAVE_FORMAT_PCM)
        return false;

    if (fmt.wBitsPerSample != 16)
        return false;

    if (fmt.nChannels < 1 || fmt.nChannels > 2)
        return false;

    if (fmt.nBlockAlign == 0 || fmt.nAvgBytesPerSec == 0)
        return false;

    outFmt = fmt;
    outDataOff = dataOff;
    outDataSize = dataSz;
    return true;
}

// ------------------------------------------------------------
// File read helper that loops back to start of data
// ------------------------------------------------------------
static DWORD ReadAudioLoop(BYTE* dst, DWORD bytes)
{
    if (s_file == INVALID_HANDLE_VALUE || !dst || bytes == 0)
        return 0;

    DWORD total = 0;

    while (total < bytes)
    {
        // If at end, wrap
        if (s_dataPos >= s_dataSize)
            s_dataPos = 0;

        DWORD remain = s_dataSize - s_dataPos;
        DWORD want = bytes - total;
        if (want > remain) want = remain;

        if (!SeekAbs(s_file, s_dataOffset + s_dataPos))
            break;

        DWORD got = 0;
        if (!ReadFile(s_file, dst + total, want, &got, NULL))
            break;

        total += got;
        s_dataPos += got;

        if (got != want)
            break;
    }

    return total;
}

// ------------------------------------------------------------
// Streaming fill: write 'bytes' into buffer at s_writeCursor
// ------------------------------------------------------------
static void FillBuffer(DWORD bytes)
{
    if (!s_buf || !s_ready || bytes == 0)
        return;

    // align to block
    bytes = AlignDown(bytes, s_wfx.nBlockAlign);
    if (bytes == 0) return;

    void* p1 = NULL; void* p2 = NULL;
    DWORD b1 = 0;    DWORD b2 = 0;

    if (FAILED(s_buf->Lock(s_writeCursor, bytes, &p1, &b1, &p2, &b2, 0)))
        return;

    if (p1 && b1)
    {
        ReadAudioLoop((BYTE*)p1, b1);
        UV_AnalyzePCM16(p1, b1);
    }
    if (p2 && b2)
    {
        ReadAudioLoop((BYTE*)p2, b2);
        UV_AnalyzePCM16(p2, b2);
    }

    s_buf->Unlock(p1, b1, p2, b2);

    s_writeCursor = (s_writeCursor + bytes) % s_bufBytes;
}

// ------------------------------------------------------------
// Public API
// ------------------------------------------------------------
bool Music_Init(const char* path)
{
    Music_Shutdown();

    if (!path || !path[0])
        return false;

    s_file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (s_file == INVALID_HANDLE_VALUE)
        return false;

    // Parse wav
    if (!ParseWav(s_file, s_wfx, s_dataOffset, s_dataSize))
    {
        Music_Shutdown();
        return false;
    }

    // Create DirectSound
    if (FAILED(DirectSoundCreate(NULL, &s_ds, NULL)) || !s_ds)
    {
        Music_Shutdown();
        return false;
    }

    // Create secondary streaming buffer
    s_bufBytes = STREAM_BUF_BYTES;
    s_bufBytes = AlignDown(s_bufBytes, s_wfx.nBlockAlign);
    if (s_bufBytes < (DWORD)(s_wfx.nBlockAlign * 256))
        s_bufBytes = AlignDown(s_wfx.nBlockAlign * 256, s_wfx.nBlockAlign);

    DSBUFFERDESC desc;
    ZeroMemory(&desc, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags =
        DSBCAPS_CTRLVOLUME |
        DSBCAPS_CTRLPOSITIONNOTIFY;
    desc.dwBufferBytes = s_bufBytes;
    desc.lpwfxFormat = &s_wfx;

    if (FAILED(s_ds->CreateSoundBuffer(&desc, &s_buf, NULL)) || !s_buf)
    {
        Music_Shutdown();
        return false;
    }

    // Start file read at beginning of data
    s_dataPos = 0;
    s_writeCursor = 0;

    // reset UV state
    s_avgFast = 0;
    s_avgSlow = 0;
    s_uvPacked = 0;

    // Prime the whole ring buffer so playback can start immediately
    FillBuffer(s_bufBytes);

    // default volume (0 = max, negative = quieter)
    s_buf->SetVolume(DSBVOLUME_MAX);

    s_ready = true;
    s_playing = false;
    return true;
}

void Music_Shutdown()
{
    s_ready = false;
    s_playing = false;

    if (s_buf) { s_buf->Stop(); s_buf->Release(); s_buf = NULL; }
    if (s_ds) { s_ds->Release(); s_ds = NULL; }

    if (s_file != INVALID_HANDLE_VALUE) { CloseHandle(s_file); s_file = INVALID_HANDLE_VALUE; }

    memset(&s_wfx, 0, sizeof(s_wfx));
    s_dataOffset = 0;
    s_dataSize = 0;
    s_dataPos = 0;

    s_bufBytes = 0;
    s_writeCursor = 0;

    s_avgFast = 0;
    s_avgSlow = 0;
    s_uvPacked = 0;
}

void Music_Play()
{
    if (!s_ready || !s_buf)
        return;

    // Start looping playback from current cursor
    s_buf->Play(0, 0, DSBPLAY_LOOPING);
    s_playing = true;
}

void Music_Pause()
{
    if (!s_ready || !s_buf)
        return;

    s_buf->Stop();
    s_playing = false;
}

void Music_Update()
{
    if (!s_ready || !s_buf || !s_playing)
        return;

    DWORD play = 0, write = 0;
    if (FAILED(s_buf->GetCurrentPosition(&play, &write)))
        return;

    // We want to keep the buffer "ahead" of the play cursor.
    // Compute distance from writeCursor to play cursor in ring space.
    // Refill until we're at least ~half-buffer ahead.
    DWORD targetAhead = s_bufBytes / 2;

    // distance from play to our write cursor (how much audio remains between play and writeCursor)
    DWORD ahead = 0;
    if (s_writeCursor >= play) ahead = s_writeCursor - play;
    else ahead = (s_bufBytes - play) + s_writeCursor;

    // If we're not far enough ahead, write more
    while (ahead < targetAhead)
    {
        DWORD bytes = STREAM_CHUNK_BYTES;
        if (bytes > (targetAhead - ahead))
            bytes = (targetAhead - ahead);

        bytes = AlignDown(bytes, s_wfx.nBlockAlign);
        if (bytes == 0) break;

        FillBuffer(bytes);

        // recompute ahead after advancing write cursor
        if (s_writeCursor >= play) ahead = s_writeCursor - play;
        else ahead = (s_bufBytes - play) + s_writeCursor;
    }
}

bool Music_IsReady() { return s_ready; }
bool Music_IsPlaying() { return s_playing; }

void Music_GetUVLevels(int out4[4])
{
    if (!out4) return;
    LONG p = s_uvPacked;
    out4[0] = (p >> 0) & 255;
    out4[1] = (p >> 8) & 255;
    out4[2] = (p >> 16) & 255;
    out4[3] = (p >> 24) & 255;
}
