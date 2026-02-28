#include "music.h"
#include <xtl.h>
#include <string.h>
#include <stdlib.h>

static LPDIRECTSOUND8       s_ds = NULL;
static LPDIRECTSOUNDBUFFER  s_buf = NULL;
static HANDLE               s_file = INVALID_HANDLE_VALUE;

static DWORD        s_dataOffset = 0;
static DWORD        s_dataSize = 0;
static DWORD        s_dataPos = 0;
static WAVEFORMATEX s_wfx;
static bool         s_ready = false;
static bool         s_playing = false;
static bool         s_wasPaused = false;

static DWORD s_bufBytes = 0;
static DWORD s_writeCursor = 0;

// -----------------------------------------------------------------------------
// Tuning
// -----------------------------------------------------------------------------
// BUF_BYTES: 2MB ring buffer = ~11.6 seconds at 44.1kHz stereo 16-bit
//   (~176KB/s).  When a scene loads textures and hammers the DVD drive,
//   audio coasts on this RAM buffer without touching the drive at all.
//   2MB is the sweet spot — covers the longest scene load (2-3s) with
//   headroom, and won't eat into the Xbox's 64MB noticeably.
//
// CHUNK_BYTES: 32KB per fill = ~186ms per disk read.  Larger than before
//   because with a 2MB buffer we fill far less often — maybe once every
//   10+ frames — so each individual fill can afford to do more.
//
// TARGET_AHEAD: keep 1MB ahead of the play cursor.  The other 1MB is
//   slack — if we miss several fills during a scene load it doesn't matter.
//
// UV_STRIDE: analyse 1 in every 16 samples — 16x cheaper, visually identical.
// -----------------------------------------------------------------------------
static const DWORD STREAM_BUF_BYTES = (2 * 1024 * 1024);   // 2MB
static const DWORD STREAM_CHUNK_BYTES = (32 * 1024);          // 32KB per fill
static const DWORD TARGET_AHEAD_BYTES = (1 * 1024 * 1024);    // 1MB target ahead
static const int   VU_STRIDE = 16;

// -----------------------------------------------------------------------------
// Volume ramp
// -----------------------------------------------------------------------------
static LONG s_targetVol = DSBVOLUME_MAX;
static LONG s_curVol = DSBVOLUME_MAX;
static int  s_rampLeft = 0;

static void VolumeRamp_Update()
{
    if (!s_buf || !s_playing || s_rampLeft <= 0) return;
    LONG delta = s_targetVol - s_curVol;
    LONG step = delta / (LONG)s_rampLeft;
    if (step == 0) step = (delta > 0) ? 1 : -1;
    s_curVol += step;
    s_rampLeft--;
    if (s_rampLeft <= 0) s_curVol = s_targetVol;
    s_buf->SetVolume(s_curVol);
}

// -----------------------------------------------------------------------------
// VU meter — strided to reduce per-frame cost
// -----------------------------------------------------------------------------
static volatile LONG s_vuPacked = 0;
static int s_avgFast = 0;
static int s_avgSlow = 0;

static void VU_Analyze(const void* data, DWORD bytes)
{
    if (!data || bytes < 2) return;
    const short* s = (const short*)data;
    DWORD total = bytes / 2;
    DWORD count = 0;
    int   sum = 0;

    // Step by VU_STRIDE samples — 16x fewer iterations than before
    for (DWORD i = 0; i < total; i += VU_STRIDE)
    {
        int v = (int)s[i];
        sum += (v < 0) ? -v : v;
        ++count;
    }

    int avg = count ? (sum / (int)count) : 0;
    s_avgFast = (s_avgFast * 3 + avg) / 4;
    s_avgSlow = (s_avgSlow * 31 + avg) / 32;

    int a = s_avgFast >> 5; if (a > 255) a = 255;
    int b = s_avgSlow >> 5; if (b > 255) b = 255;
    s_vuPacked = (LONG)((a) | (b << 8) | (a << 16) | (b << 24));
}

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
static DWORD AlignDown(DWORD v, DWORD align)
{
    return align ? v - (v % align) : v;
}

static DWORD ReadU32(HANDLE f)
{
    DWORD v = 0, br = 0;
    ReadFile(f, &v, 4, &br, NULL);
    return v;
}

static bool ReadChunkHeader(HANDLE f, DWORD& id, DWORD& size)
{
    DWORD br = 0;
    if (!ReadFile(f, &id, 4, &br, NULL) || br != 4) return false;
    if (!ReadFile(f, &size, 4, &br, NULL) || br != 4) return false;
    return true;
}

static bool ParseWav(HANDLE f, WAVEFORMATEX& fmt, DWORD& dataOffset, DWORD& dataSize)
{
    SetFilePointer(f, 0, NULL, FILE_BEGIN);
    DWORD riff = ReadU32(f); ReadU32(f); DWORD wave = ReadU32(f);
    if (riff != 'FFIR' || wave != 'EVAW') return false;

    bool gotFmt = false, gotData = false;
    DWORD id = 0, size = 0;
    while (ReadChunkHeader(f, id, size))
    {
        DWORD here = SetFilePointer(f, 0, NULL, FILE_CURRENT);
        if (id == ' tmf' && size >= 16)
        {
            DWORD br = 0;
            ZeroMemory(&fmt, sizeof(fmt));
            ReadFile(f, &fmt.wFormatTag, 2, &br, NULL);
            ReadFile(f, &fmt.nChannels, 2, &br, NULL);
            ReadFile(f, &fmt.nSamplesPerSec, 4, &br, NULL);
            ReadFile(f, &fmt.nAvgBytesPerSec, 4, &br, NULL);
            ReadFile(f, &fmt.nBlockAlign, 2, &br, NULL);
            ReadFile(f, &fmt.wBitsPerSample, 2, &br, NULL);
            if (size > 16) SetFilePointer(f, size - 16, NULL, FILE_CURRENT);
            gotFmt = true;
        }
        else if (id == 'atad')
        {
            dataOffset = here;
            dataSize = size;
            gotData = true;
        }
        else
        {
            SetFilePointer(f, size, NULL, FILE_CURRENT);
        }
        if (size & 1) SetFilePointer(f, 1, NULL, FILE_CURRENT);
        if (gotFmt && gotData) break;
    }
    return gotFmt && gotData && fmt.wFormatTag == 1;
}

// -----------------------------------------------------------------------------
// Looping reader
// -----------------------------------------------------------------------------
static DWORD ReadAudioLoop(BYTE* dst, DWORD bytes)
{
    if (!dst || !bytes || s_file == INVALID_HANDLE_VALUE) return 0;
    DWORD total = 0;
    while (bytes > 0)
    {
        DWORD rem = s_dataSize - s_dataPos;
        DWORD toRead = (bytes < rem) ? bytes : rem;
        DWORD br = 0;
        SetFilePointer(s_file, s_dataOffset + s_dataPos, NULL, FILE_BEGIN);
        ReadFile(s_file, dst, toRead, &br, NULL);
        if (!br) break;
        dst += br;
        bytes -= br;
        total += br;
        s_dataPos += br;
        if (s_dataPos >= s_dataSize) s_dataPos = 0;
    }
    return total;
}

// -----------------------------------------------------------------------------
// Buffer fill — write one aligned chunk at s_writeCursor
// -----------------------------------------------------------------------------
static void FillBuffer(DWORD bytes)
{
    if (!s_buf || !s_ready || !bytes) return;
    bytes = AlignDown(bytes, s_wfx.nBlockAlign);
    if (!bytes) return;

    void* p1 = NULL; void* p2 = NULL;
    DWORD b1 = 0, b2 = 0;
    if (FAILED(s_buf->Lock(s_writeCursor, bytes, &p1, &b1, &p2, &b2, 0))) return;

    if (p1 && b1) { ReadAudioLoop((BYTE*)p1, b1); VU_Analyze(p1, b1); }
    if (p2 && b2) { ReadAudioLoop((BYTE*)p2, b2); VU_Analyze(p2, b2); }

    s_buf->Unlock(p1, b1, p2, b2);
    s_writeCursor = (s_writeCursor + bytes) % s_bufBytes;
}

static void ClearBuffer()
{
    if (!s_buf || !s_bufBytes) return;
    void* p1 = NULL; void* p2 = NULL;
    DWORD b1 = 0, b2 = 0;
    if (FAILED(s_buf->Lock(0, s_bufBytes, &p1, &b1, &p2, &b2, 0))) return;
    if (p1 && b1) memset(p1, 0, b1);
    if (p2 && b2) memset(p2, 0, b2);
    s_buf->Unlock(p1, b1, p2, b2);
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------
bool Music_Init(const char* path)
{
    Music_Shutdown();
    if (!path || !path[0]) return false;

    s_file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (s_file == INVALID_HANDLE_VALUE) return false;

    if (!ParseWav(s_file, s_wfx, s_dataOffset, s_dataSize))
    {
        Music_Shutdown(); return false;
    }

    if (FAILED(DirectSoundCreate(NULL, &s_ds, NULL)) || !s_ds)
    {
        Music_Shutdown(); return false;
    }

    s_bufBytes = AlignDown(STREAM_BUF_BYTES, s_wfx.nBlockAlign);
    if (s_bufBytes < (DWORD)(s_wfx.nBlockAlign * 256))
        s_bufBytes = AlignDown(s_wfx.nBlockAlign * 256, s_wfx.nBlockAlign);

    DSBUFFERDESC desc;
    ZeroMemory(&desc, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPOSITIONNOTIFY;
    desc.dwBufferBytes = s_bufBytes;
    desc.lpwfxFormat = &s_wfx;

    if (FAILED(s_ds->CreateSoundBuffer(&desc, &s_buf, NULL)) || !s_buf)
    {
        Music_Shutdown(); return false;
    }

    s_dataPos = s_writeCursor = 0;
    s_avgFast = s_avgSlow = 0;
    s_vuPacked = 0;
    s_ready = true;

    s_buf->Stop();
    s_buf->SetCurrentPosition(0);
    ClearBuffer();
    FillBuffer(s_bufBytes);  // prime on init — only happens once

    s_targetVol = s_curVol = DSBVOLUME_MAX;
    s_rampLeft = 0;
    s_buf->SetVolume(s_targetVol);
    s_playing = s_wasPaused = false;
    return true;
}

void Music_Shutdown()
{
    s_ready = s_playing = s_wasPaused = false;
    if (s_buf) { s_buf->Stop(); s_buf->Release(); s_buf = NULL; }
    if (s_ds) { s_ds->Release(); s_ds = NULL; }
    if (s_file != INVALID_HANDLE_VALUE) { CloseHandle(s_file); s_file = INVALID_HANDLE_VALUE; }
    s_dataOffset = s_dataSize = s_dataPos = 0;
    s_bufBytes = s_writeCursor = 0;
    s_avgFast = s_avgSlow = 0;
    s_vuPacked = 0;
    s_targetVol = s_curVol = DSBVOLUME_MAX;
    s_rampLeft = 0;
}

void Music_Play()
{
    if (!s_ready || !s_buf) return;

    if (s_wasPaused)
    {
        s_targetVol = s_curVol = DSBVOLUME_MAX;
        s_rampLeft = 0;
        s_buf->SetVolume(s_targetVol);
        s_buf->Play(0, 0, DSBPLAY_LOOPING);
        s_playing = true; s_wasPaused = false;
        return;
    }

    s_buf->Stop();
    s_playing = false;
    s_dataPos = s_writeCursor = 0;
    s_buf->SetCurrentPosition(0);
    ClearBuffer();
    FillBuffer(s_bufBytes);

    s_targetVol = DSBVOLUME_MAX;
    s_curVol = -2400;  // -24dB start, ramp up to avoid click
    s_rampLeft = 12;
    s_buf->SetVolume(s_curVol);
    s_buf->Play(0, 0, DSBPLAY_LOOPING);
    s_playing = true; s_wasPaused = false;
}

void Music_Pause()
{
    if (!s_ready || !s_buf || !s_playing) return;
    s_buf->Stop();
    s_playing = false;
    s_wasPaused = true;
}

void Music_Update()
{
    if (!s_ready || !s_buf || !s_playing) return;

    DWORD play = 0, write = 0;
    if (FAILED(s_buf->GetCurrentPosition(&play, &write))) return;

    VolumeRamp_Update();

    // How far ahead of the play cursor is our write cursor?
    DWORD ahead = (s_writeCursor >= play)
        ? s_writeCursor - play
        : (s_bufBytes - play) + s_writeCursor;

    // Only fill if we've fallen below the target — and cap at ONE chunk per
    // Update call.  This keeps per-frame disk I/O bounded regardless of how
    // far behind we might fall (e.g. during a scene load stall).
    // The 192KB buffer gives ~2s of headroom so one missed frame is fine.
    if (ahead < TARGET_AHEAD_BYTES)
    {
        DWORD needed = TARGET_AHEAD_BYTES - ahead;
        DWORD fill = (needed < STREAM_CHUNK_BYTES) ? needed : STREAM_CHUNK_BYTES;
        FillBuffer(fill);
    }
}

bool Music_IsReady() { return s_ready; }
bool Music_IsPlaying() { return s_playing; }

void Music_GetVULevels(int out4[4])
{
    if (!out4) return;
    LONG p = s_vuPacked;
    out4[0] = (p >> 0) & 255;
    out4[1] = (p >> 8) & 255;
    out4[2] = (p >> 16) & 255;
    out4[3] = (p >> 24) & 255;
}