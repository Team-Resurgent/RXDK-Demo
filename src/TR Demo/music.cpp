#include "music.h"
#include <xtl.h>
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
static bool   s_wasPaused = false;   // NEW: distinguishes resume vs fresh start

// Streaming buffer
static DWORD  s_bufBytes = 0;
static DWORD  s_writeCursor = 0;

// -----------------------------------------------------------------------------
// Startup squelch/click prevention + volume ramp (integer-only, RXDK-safe)
// -----------------------------------------------------------------------------
static LONG s_targetVol = DSBVOLUME_MAX;
static LONG s_curVol = DSBVOLUME_MAX;
static int  s_rampLeft = 0;

static void ClearBufferToSilence()
{
    if (!s_buf || s_bufBytes == 0)
        return;

    void* p1 = NULL; void* p2 = NULL;
    DWORD b1 = 0;    DWORD b2 = 0;

    if (FAILED(s_buf->Lock(0, s_bufBytes, &p1, &b1, &p2, &b2, 0)))
        return;

    if (p1 && b1) memset(p1, 0, b1);
    if (p2 && b2) memset(p2, 0, b2);

    s_buf->Unlock(p1, b1, p2, b2);
}

static void VolumeRamp_Update()
{
    if (!s_buf || !s_playing || s_rampLeft <= 0)
        return;

    LONG cur = s_curVol;
    LONG tgt = s_targetVol;
    LONG delta = tgt - cur;

    LONG step = delta / (LONG)s_rampLeft;
    if (step == 0)
        step = (delta > 0) ? 1 : -1;

    cur += step;
    s_curVol = cur;
    s_rampLeft--;

    s_buf->SetVolume(cur);

    if (s_rampLeft <= 0)
    {
        s_curVol = tgt;
        s_buf->SetVolume(tgt);
    }
}

// Keep some safety margin ahead of play cursor
static const DWORD STREAM_CHUNK_BYTES = (32 * 1024);   // write in chunks
static const DWORD STREAM_BUF_BYTES = (128 * 1024);  // ring buffer size

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

static DWORD AlignDown(DWORD v, DWORD align)
{
    if (align == 0) return v;
    return v - (v % align);
}

static DWORD ReadU32(HANDLE f)
{
    DWORD v = 0, br = 0;
    ReadFile(f, &v, 4, &br, NULL);
    return v;
}

static bool ReadChunkHeader(HANDLE f, DWORD& outId, DWORD& outSize)
{
    DWORD br = 0;
    if (!ReadFile(f, &outId, 4, &br, NULL) || br != 4) return false;
    if (!ReadFile(f, &outSize, 4, &br, NULL) || br != 4) return false;
    return true;
}

// Minimal PCM WAV parser (RIFF/WAVE, fmt , data)
static bool ParseWav(HANDLE f, WAVEFORMATEX& outFmt, DWORD& outDataOffset, DWORD& outDataSize)
{
    SetFilePointer(f, 0, NULL, FILE_BEGIN);

    DWORD riff = ReadU32(f);
    DWORD riffSize = ReadU32(f);
    DWORD wave = ReadU32(f);

    (void)riffSize;

    if (riff != 'FFIR' || wave != 'EVAW')
        return false;

    bool gotFmt = false;
    bool gotData = false;

    DWORD id = 0, size = 0;
    while (ReadChunkHeader(f, id, size))
    {
        DWORD here = SetFilePointer(f, 0, NULL, FILE_CURRENT);

        if (id == ' tmf')
        {
            // Read fmt chunk
            if (size < 16) return false;

            ZeroMemory(&outFmt, sizeof(outFmt));
            DWORD br = 0;
            ReadFile(f, &outFmt.wFormatTag, 2, &br, NULL);
            ReadFile(f, &outFmt.nChannels, 2, &br, NULL);
            ReadFile(f, &outFmt.nSamplesPerSec, 4, &br, NULL);
            ReadFile(f, &outFmt.nAvgBytesPerSec, 4, &br, NULL);
            ReadFile(f, &outFmt.nBlockAlign, 2, &br, NULL);
            ReadFile(f, &outFmt.wBitsPerSample, 2, &br, NULL);

            // skip any extra fmt bytes
            if (size > 16)
                SetFilePointer(f, size - 16, NULL, FILE_CURRENT);

            gotFmt = true;
        }
        else if (id == 'atad')
        {
            outDataOffset = here;
            outDataSize = size;
            SetFilePointer(f, size, NULL, FILE_CURRENT);
            gotData = true;
        }
        else
        {
            SetFilePointer(f, size, NULL, FILE_CURRENT);
        }

        // Chunks are word-aligned
        if (size & 1)
            SetFilePointer(f, 1, NULL, FILE_CURRENT);

        if (gotFmt && gotData)
            break;
    }

    if (!gotFmt || !gotData)
        return false;

    // Only support PCM
    if (outFmt.wFormatTag != 1)
        return false;

    return true;
}

// --------------------------------------------------------------------------
// UV analyzer (integer-only)
// --------------------------------------------------------------------------

static volatile LONG s_uvPacked = 0; // packed 4x8-bit: low|mid|high|all
static int s_avgFast = 0; // 0..32767-ish
static int s_avgSlow = 0; // 0..32767-ish

static __forceinline int IAbsI(int v) { return (v < 0) ? -v : v; }

static void UV_AnalyzePCM16(const void* data, DWORD bytes)
{
    if (!data || bytes < 4) return;

    const short* s = (const short*)data;
    DWORD samples = bytes / 2;

    int sum = 0;
    for (DWORD i = 0; i < samples; ++i)
        sum += IAbsI((int)s[i]);

    int avg = (samples > 0) ? (sum / (int)samples) : 0;

    // Simple fast/slow smoothing (integer only)
    s_avgFast = (s_avgFast * 3 + avg) / 4;
    s_avgSlow = (s_avgSlow * 31 + avg) / 32;

    int a = s_avgFast >> 5; if (a > 255) a = 255;
    int b = s_avgSlow >> 5; if (b > 255) b = 255;

    LONG packed = (LONG)((a & 255) | ((b & 255) << 8) | ((a & 255) << 16) | ((b & 255) << 24));
    s_uvPacked = packed;
}

// --------------------------------------------------------------------------
// Audio loop reader: reads from WAV data, loops seamlessly
// --------------------------------------------------------------------------

static DWORD ReadAudioLoop(BYTE* dst, DWORD bytes)
{
    if (!dst || bytes == 0 || s_file == INVALID_HANDLE_VALUE)
        return 0;

    DWORD total = 0;

    while (bytes > 0)
    {
        DWORD remaining = s_dataSize - s_dataPos;
        DWORD toRead = (bytes < remaining) ? bytes : remaining;

        DWORD br = 0;
        SetFilePointer(s_file, s_dataOffset + s_dataPos, NULL, FILE_BEGIN);
        ReadFile(s_file, dst, toRead, &br, NULL);

        if (br < toRead)
            toRead = br;

        dst += toRead;
        bytes -= toRead;
        total += toRead;

        s_dataPos += toRead;
        if (s_dataPos >= s_dataSize)
            s_dataPos = 0;

        if (toRead == 0)
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

    if (!ParseWav(s_file, s_wfx, s_dataOffset, s_dataSize))
    {
        Music_Shutdown();
        return false;
    }

    if (FAILED(DirectSoundCreate(NULL, &s_ds, NULL)) || !s_ds)
    {
        Music_Shutdown();
        return false;
    }

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

    s_dataPos = 0;
    s_writeCursor = 0;

    s_avgFast = 0;
    s_avgSlow = 0;
    s_uvPacked = 0;

    // Prime ring (FillBuffer is guarded by s_ready)
    s_ready = true;

    s_buf->Stop();
    s_buf->SetCurrentPosition(0);
    s_writeCursor = 0;
    s_dataPos = 0;

    ClearBufferToSilence();
    FillBuffer(s_bufBytes);

    s_targetVol = DSBVOLUME_MAX;
    s_curVol = DSBVOLUME_MAX;
    s_rampLeft = 0;
    s_buf->SetVolume(s_targetVol);

    s_playing = false;
    s_wasPaused = false; // NEW
    return true;
}

void Music_Shutdown()
{
    s_ready = false;
    s_playing = false;
    s_wasPaused = false;

    if (s_buf)
    {
        s_buf->Stop();
        s_buf->Release();
        s_buf = NULL;
    }
    if (s_ds)
    {
        s_ds->Release();
        s_ds = NULL;
    }

    if (s_file != INVALID_HANDLE_VALUE)
    {
        CloseHandle(s_file);
        s_file = INVALID_HANDLE_VALUE;
    }

    s_dataOffset = 0;
    s_dataSize = 0;
    s_dataPos = 0;
    s_bufBytes = 0;
    s_writeCursor = 0;

    s_avgFast = 0;
    s_avgSlow = 0;
    s_uvPacked = 0;

    s_targetVol = DSBVOLUME_MAX;
    s_curVol = DSBVOLUME_MAX;
    s_rampLeft = 0;
}

void Music_Play()
{
    if (!s_ready || !s_buf)
        return;

    // Resume from pause (do NOT reset stream/buffer)
    if (s_wasPaused)
    {
        s_targetVol = DSBVOLUME_MAX;
        s_curVol = DSBVOLUME_MAX;
        s_rampLeft = 0;

        s_buf->SetVolume(s_targetVol);
        s_buf->Play(0, 0, DSBPLAY_LOOPING);

        s_playing = true;
        s_wasPaused = false;
        return;
    }

    // Fresh start (full reset)
    s_buf->Stop();
    s_playing = false;

    s_dataPos = 0;
    s_writeCursor = 0;
    s_buf->SetCurrentPosition(0);

    ClearBufferToSilence();
    FillBuffer(s_bufBytes);

    // Gentle ramp-in to avoid any residual click at start (integer-only).
    s_targetVol = DSBVOLUME_MAX;
    s_curVol = -2400; // ~ -24 dB
    s_rampLeft = 12;    // ramp over ~12 updates

    s_buf->SetVolume(s_curVol);
    s_buf->Play(0, 0, DSBPLAY_LOOPING);
    s_playing = true;
    s_wasPaused = false;
}

void Music_Pause()
{
    if (!s_ready || !s_buf)
        return;

    if (s_playing)
    {
        s_buf->Stop();
        s_playing = false;
        s_wasPaused = true; // NEW: allow resume
    }
}

void Music_Update()
{
    if (!s_ready || !s_buf || !s_playing)
        return;

    DWORD play = 0, write = 0;
    if (FAILED(s_buf->GetCurrentPosition(&play, &write)))
        return;

    VolumeRamp_Update();

    DWORD targetAhead = s_bufBytes / 2;

    DWORD ahead = 0;
    if (s_writeCursor >= play) ahead = s_writeCursor - play;
    else ahead = (s_bufBytes - play) + s_writeCursor;

    while (ahead < targetAhead)
    {
        DWORD bytes = STREAM_CHUNK_BYTES;
        if (bytes > (targetAhead - ahead))
            bytes = (targetAhead - ahead);

        bytes = AlignDown(bytes, s_wfx.nBlockAlign);
        if (bytes == 0) break;

        FillBuffer(bytes);

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
