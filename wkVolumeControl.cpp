// wkVolumeControl - WormKit module for Worms Armageddon 3.8+
//
// Independent volume control for music, sound effects and master (per-app
// Windows mixer) volume, adjustable in-game through an overlay panel.
//
// Music/SFX: DirectSoundCreate(8) is intercepted via IAT patching, the
// returned IDirectSound vtable is patched to intercept CreateSoundBuffer /
// DuplicateSoundBuffer, and each secondary buffer's SetVolume/GetVolume/
// Release are patched to apply a per-category attenuation on top of the
// volume the game asks for. Buffers are classified by size: the music
// track is a large streaming buffer, effects are small static buffers
// (threshold configurable in wkVolumeControl.ini).
//
// Master: ISimpleAudioVolume on the process audio session (same control as
// the Windows volume mixer).
//
// No external dependencies; builds with MSVC (x86). See build.bat.

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#define PSAPI_VERSION 2
#include <windows.h>
#include <mmsystem.h>
#include <psapi.h>
#include <dsound.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <math.h>
#include <stdio.h>
#include <map>
#include <vector>

// ***************************************************************
// Configuration

enum { CAT_MUSIC = 0, CAT_SFX = 1 };

static struct Config {
    int music;        // 0..100
    int sfx;          // 0..100
    int master;       // 0..100
    int toggleKey;    // virtual-key code, decimal (145 = Scroll Lock)
    int musicMinKB;   // optional size override, 0 = disabled
    int musicLocks;   // locks after which a buffer is considered streaming music
    int debug;        // 1 = write wkVolumeControl.log
    char language[16]; // "en", "es" or "auto"
} g_cfg = { 100, 100, 100, VK_SCROLL, 0, 4, 0, "auto" };

struct Strings {
    const char* title;
    const char* rows[3];
    const char* footer;
};
static const Strings STR_EN = { "Volume",
    { "Music", "Effects", "Master" },
    "Scroll Lock: show/hide \xB7 wheel: fine tune" };
static const Strings STR_ES = { "Volumen",
    { "M\xFAsica", "Efectos", "General" },
    "Bloq Despl: mostrar/ocultar \xB7 rueda: ajuste fino" };
static const Strings* g_str = &STR_EN;

static char g_iniPath[MAX_PATH];
static char g_logPath[MAX_PATH];
static HINSTANCE g_inst;
static CRITICAL_SECTION g_cs;

static void Log(const char* fmt, ...)
{
    if (!g_cfg.debug) return;
    FILE* f = fopen(g_logPath, "a");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

static void LoadConfig()
{
    char dir[MAX_PATH];
    GetModuleFileNameA(NULL, dir, MAX_PATH);
    char* slash = strrchr(dir, '\\');
    if (slash) *slash = 0;
    _snprintf(g_iniPath, MAX_PATH, "%s\\wkVolumeControl.ini", dir);
    _snprintf(g_logPath, MAX_PATH, "%s\\wkVolumeControl.log", dir);

    g_cfg.music      = GetPrivateProfileIntA("Volumes",  "Music",          100, g_iniPath);
    g_cfg.sfx        = GetPrivateProfileIntA("Volumes",  "Effects",        100, g_iniPath);
    g_cfg.master     = GetPrivateProfileIntA("Volumes",  "Master",         100, g_iniPath);
    g_cfg.toggleKey  = GetPrivateProfileIntA("Settings", "ToggleKey",      VK_SCROLL, g_iniPath);
    g_cfg.musicMinKB = GetPrivateProfileIntA("Settings", "MusicBufferMinKB", 0, g_iniPath);
    g_cfg.musicLocks = GetPrivateProfileIntA("Settings", "MusicLockThreshold", 4, g_iniPath);
    g_cfg.debug      = GetPrivateProfileIntA("Settings", "Debug",          0, g_iniPath);
    GetPrivateProfileStringA("Settings", "Language", "auto",
                             g_cfg.language, sizeof(g_cfg.language), g_iniPath);

    bool spanish;
    if (!_stricmp(g_cfg.language, "es")) spanish = true;
    else if (!_stricmp(g_cfg.language, "en")) spanish = false;
    else spanish = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_SPANISH;
    g_str = spanish ? &STR_ES : &STR_EN;
}

static void SaveConfig()
{
    char buf[16];
    struct { const char* sec; const char* key; int val; } items[] = {
        { "Volumes",  "Music",            g_cfg.music },
        { "Volumes",  "Effects",          g_cfg.sfx },
        { "Volumes",  "Master",           g_cfg.master },
        { "Settings", "ToggleKey",          g_cfg.toggleKey },
        { "Settings", "MusicBufferMinKB",   g_cfg.musicMinKB },
        { "Settings", "MusicLockThreshold", g_cfg.musicLocks },
        { "Settings", "Debug",              g_cfg.debug },
    };
    for (int i = 0; i < (int)(sizeof(items)/sizeof(items[0])); i++) {
        _snprintf(buf, sizeof(buf), "%d", items[i].val);
        WritePrivateProfileStringA(items[i].sec, items[i].key, buf, g_iniPath);
    }
    WritePrivateProfileStringA("Settings", "Language", g_cfg.language, g_iniPath);
}

// ***************************************************************
// DirectSound buffer tracking

typedef HRESULT (STDMETHODCALLTYPE *CreateSoundBuffer_t)(IDirectSound*, LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER*, LPUNKNOWN);
typedef HRESULT (STDMETHODCALLTYPE *DuplicateSoundBuffer_t)(IDirectSound*, LPDIRECTSOUNDBUFFER, LPDIRECTSOUNDBUFFER*);
typedef HRESULT (STDMETHODCALLTYPE *SetVolume_t)(IDirectSoundBuffer*, LONG);
typedef HRESULT (STDMETHODCALLTYPE *GetVolume_t)(IDirectSoundBuffer*, LPLONG);
typedef ULONG   (STDMETHODCALLTYPE *Release_t)(IUnknown*);
typedef HRESULT (STDMETHODCALLTYPE *Lock_t)(IDirectSoundBuffer*, DWORD, DWORD, LPVOID*, LPDWORD, LPVOID*, LPDWORD, DWORD);

// IDirectSound vtable: 3=CreateSoundBuffer, 5=DuplicateSoundBuffer
// IDirectSoundBuffer vtable: 2=Release, 6=GetVolume, 11=Lock, 15=SetVolume

#define MAX_VTBLS 8

static struct DsVtbl {
    void** vt;
    CreateSoundBuffer_t    CreateSoundBuffer;
    DuplicateSoundBuffer_t DuplicateSoundBuffer;
} g_dsVtbls[MAX_VTBLS];
static int g_dsVtblCount;

static struct BufVtbl {
    void** vt;
    SetVolume_t SetVolume;
    GetVolume_t GetVolume;
    Release_t   Release;
    Lock_t      Lock;
} g_bufVtbls[MAX_VTBLS];
static int g_bufVtblCount;

struct BufInfo {
    int   cat;
    LONG  gameVol;    // volume the game last asked for
    DWORD size;
    int   lockCount;  // sound effects are written once; music is refilled forever
};
static std::map<IDirectSoundBuffer*, BufInfo> g_bufs;

static void PatchVtblEntry(void** vt, int idx, void* newFn)
{
    DWORD old;
    VirtualProtect(&vt[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    vt[idx] = newFn;
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
}

static DsVtbl* FindDsVtbl(void** vt)
{
    for (int i = 0; i < g_dsVtblCount; i++)
        if (g_dsVtbls[i].vt == vt) return &g_dsVtbls[i];
    return NULL;
}

static BufVtbl* FindBufVtbl(void** vt)
{
    for (int i = 0; i < g_bufVtblCount; i++)
        if (g_bufVtbls[i].vt == vt) return &g_bufVtbls[i];
    return NULL;
}

static LONG Combine(LONG gameVol, int cat)
{
    int pct = (cat == CAT_MUSIC) ? g_cfg.music : g_cfg.sfx;
    if (pct <= 0) return DSBVOLUME_MIN;
    LONG v = gameVol + (LONG)(2000.0 * log10(pct / 100.0));
    if (v < DSBVOLUME_MIN) v = DSBVOLUME_MIN;
    if (v > DSBVOLUME_MAX) v = DSBVOLUME_MAX;
    return v;
}

static HRESULT STDMETHODCALLTYPE MySetVolume(IDirectSoundBuffer* self, LONG vol)
{
    BufVtbl* e = FindBufVtbl(*(void***)self);
    if (!e) return DSERR_GENERIC;
    EnterCriticalSection(&g_cs);
    std::map<IDirectSoundBuffer*, BufInfo>::iterator it = g_bufs.find(self);
    if (it != g_bufs.end()) {
        it->second.gameVol = vol;
        vol = Combine(vol, it->second.cat);
    }
    LeaveCriticalSection(&g_cs);
    return e->SetVolume(self, vol);
}

static HRESULT STDMETHODCALLTYPE MyGetVolume(IDirectSoundBuffer* self, LPLONG pVol)
{
    BufVtbl* e = FindBufVtbl(*(void***)self);
    if (!e) return DSERR_GENERIC;
    HRESULT hr = e->GetVolume(self, pVol);
    if (SUCCEEDED(hr) && pVol) {
        // report the volume the game set, not our attenuated one
        EnterCriticalSection(&g_cs);
        std::map<IDirectSoundBuffer*, BufInfo>::iterator it = g_bufs.find(self);
        if (it != g_bufs.end()) *pVol = it->second.gameVol;
        LeaveCriticalSection(&g_cs);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE MyLock(IDirectSoundBuffer* self, DWORD offset, DWORD bytes,
                                        LPVOID* ppv1, LPDWORD pcb1, LPVOID* ppv2, LPDWORD pcb2,
                                        DWORD flags)
{
    BufVtbl* e = FindBufVtbl(*(void***)self);
    if (!e) return DSERR_GENERIC;
    HRESULT hr = e->Lock(self, offset, bytes, ppv1, pcb1, ppv2, pcb2, flags);
    if (SUCCEEDED(hr)) {
        bool promoted = false;
        LONG gameVol = DSBVOLUME_MAX;
        EnterCriticalSection(&g_cs);
        std::map<IDirectSoundBuffer*, BufInfo>::iterator it = g_bufs.find(self);
        if (it != g_bufs.end()) {
            it->second.lockCount++;
            // a buffer that keeps being refilled is the music stream
            if (it->second.cat == CAT_SFX && it->second.size >= 32 * 1024
                && it->second.lockCount >= g_cfg.musicLocks) {
                it->second.cat = CAT_MUSIC;
                gameVol = it->second.gameVol;
                promoted = true;
                Log("buffer %u bytes promoted to MUSIC after %d locks",
                    it->second.size, it->second.lockCount);
            }
        }
        LeaveCriticalSection(&g_cs);
        if (promoted)
            e->SetVolume(self, Combine(gameVol, CAT_MUSIC));
    }
    return hr;
}

static ULONG STDMETHODCALLTYPE MyBufRelease(IUnknown* self)
{
    BufVtbl* e = FindBufVtbl(*(void***)self);
    if (!e) return 0;
    ULONG r = e->Release(self);
    if (r == 0) {
        EnterCriticalSection(&g_cs);
        g_bufs.erase((IDirectSoundBuffer*)self);
        LeaveCriticalSection(&g_cs);
    }
    return r;
}

static void HookBufferVtbl(IDirectSoundBuffer* buf)
{
    void** vt = *(void***)buf;
    EnterCriticalSection(&g_cs);
    if (!FindBufVtbl(vt) && g_bufVtblCount < MAX_VTBLS) {
        BufVtbl& e = g_bufVtbls[g_bufVtblCount];
        e.vt        = vt;
        e.Release   = (Release_t)  vt[2];
        e.GetVolume = (GetVolume_t)vt[6];
        e.Lock      = (Lock_t)     vt[11];
        e.SetVolume = (SetVolume_t)vt[15];
        g_bufVtblCount++;  // publish before patching so hooks can resolve
        PatchVtblEntry(vt, 2,  (void*)MyBufRelease);
        PatchVtblEntry(vt, 6,  (void*)MyGetVolume);
        PatchVtblEntry(vt, 11, (void*)MyLock);
        PatchVtblEntry(vt, 15, (void*)MySetVolume);
    }
    LeaveCriticalSection(&g_cs);
}

static void RegisterBuffer(IDirectSoundBuffer* buf, int cat, DWORD size)
{
    HookBufferVtbl(buf);
    EnterCriticalSection(&g_cs);
    BufInfo info; info.cat = cat; info.gameVol = DSBVOLUME_MAX;
    info.size = size; info.lockCount = 0;
    g_bufs[buf] = info;
    BufVtbl* e = FindBufVtbl(*(void***)buf);
    LeaveCriticalSection(&g_cs);
    if (e) e->SetVolume(buf, Combine(DSBVOLUME_MAX, cat));
}

// Re-apply the category volume to all live buffers (called from the UI thread).
static void ApplyCategory(int cat)
{
    struct Item { IDirectSoundBuffer* buf; LONG vol; SetVolume_t fn; };
    std::vector<Item> items;
    EnterCriticalSection(&g_cs);
    for (std::map<IDirectSoundBuffer*, BufInfo>::iterator it = g_bufs.begin(); it != g_bufs.end(); ++it) {
        if (it->second.cat != cat) continue;
        BufVtbl* e = FindBufVtbl(*(void***)it->first);
        if (!e) continue;
        it->first->AddRef();  // keep alive until we've called SetVolume below
        Item item = { it->first, Combine(it->second.gameVol, cat), e->SetVolume };
        items.push_back(item);
    }
    LeaveCriticalSection(&g_cs);
    for (size_t i = 0; i < items.size(); i++) {
        items[i].fn(items[i].buf, items[i].vol);
        items[i].buf->Release();
    }
}

static HRESULT STDMETHODCALLTYPE MyCreateSoundBuffer(IDirectSound* self, LPCDSBUFFERDESC desc,
                                                     LPDIRECTSOUNDBUFFER* ppBuf, LPUNKNOWN unk)
{
    DsVtbl* e = FindDsVtbl(*(void***)self);
    if (!e) return DSERR_GENERIC;

    BOOL primary = desc && (desc->dwFlags & DSBCAPS_PRIMARYBUFFER);
    BYTE tmp[sizeof(DSBUFFERDESC) + 16];
    if (desc && !primary && !(desc->dwFlags & DSBCAPS_CTRLVOLUME)
        && desc->dwSize <= sizeof(tmp)) {
        // make sure we can control the volume of every secondary buffer
        memcpy(tmp, desc, desc->dwSize);
        ((DSBUFFERDESC*)tmp)->dwFlags |= DSBCAPS_CTRLVOLUME;
        desc = (LPCDSBUFFERDESC)tmp;
    }

    HRESULT hr = e->CreateSoundBuffer(self, desc, ppBuf, unk);
    if (SUCCEEDED(hr) && ppBuf && *ppBuf && desc && !primary) {
        // everything starts as SFX; the Lock hook promotes the streaming
        // music buffer once the game starts refilling it
        int cat = (g_cfg.musicMinKB > 0
                   && desc->dwBufferBytes >= (DWORD)g_cfg.musicMinKB * 1024)
                  ? CAT_MUSIC : CAT_SFX;
        Log("CreateSoundBuffer: %u bytes, flags=%08X -> %s",
            desc->dwBufferBytes, desc->dwFlags, cat == CAT_MUSIC ? "MUSIC" : "SFX");
        RegisterBuffer(*ppBuf, cat, desc->dwBufferBytes);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE MyDuplicateSoundBuffer(IDirectSound* self, LPDIRECTSOUNDBUFFER src,
                                                        LPDIRECTSOUNDBUFFER* ppDup)
{
    DsVtbl* e = FindDsVtbl(*(void***)self);
    if (!e) return DSERR_GENERIC;
    HRESULT hr = e->DuplicateSoundBuffer(self, src, ppDup);
    if (SUCCEEDED(hr) && ppDup && *ppDup) {
        int cat = CAT_SFX;
        DWORD size = 0;
        EnterCriticalSection(&g_cs);
        std::map<IDirectSoundBuffer*, BufInfo>::iterator it = g_bufs.find(src);
        if (it != g_bufs.end()) { cat = it->second.cat; size = it->second.size; }
        LeaveCriticalSection(&g_cs);
        RegisterBuffer(*ppDup, cat, size);
    }
    return hr;
}

static void HookDirectSoundVtbl(IDirectSound* ds)
{
    void** vt = *(void***)ds;
    EnterCriticalSection(&g_cs);
    if (!FindDsVtbl(vt) && g_dsVtblCount < MAX_VTBLS) {
        DsVtbl& e = g_dsVtbls[g_dsVtblCount];
        e.vt                   = vt;
        e.CreateSoundBuffer    = (CreateSoundBuffer_t)   vt[3];
        e.DuplicateSoundBuffer = (DuplicateSoundBuffer_t)vt[5];
        g_dsVtblCount++;
        PatchVtblEntry(vt, 3, (void*)MyCreateSoundBuffer);
        PatchVtblEntry(vt, 5, (void*)MyDuplicateSoundBuffer);
        Log("IDirectSound vtable hooked");
    }
    LeaveCriticalSection(&g_cs);
}

// ***************************************************************
// DirectSoundCreate IAT hooks

typedef HRESULT (WINAPI *DirectSoundCreate_t)(LPCGUID, LPDIRECTSOUND*, LPUNKNOWN);
static DirectSoundCreate_t g_origDSCreate;
static DirectSoundCreate_t g_origDSCreate8;

static HRESULT WINAPI MyDirectSoundCreate(LPCGUID guid, LPDIRECTSOUND* ppDS, LPUNKNOWN unk)
{
    HRESULT hr = g_origDSCreate(guid, ppDS, unk);
    Log("DirectSoundCreate -> %08X", hr);
    if (SUCCEEDED(hr) && ppDS && *ppDS) HookDirectSoundVtbl(*ppDS);
    return hr;
}

static HRESULT WINAPI MyDirectSoundCreate8(LPCGUID guid, LPDIRECTSOUND* ppDS, LPUNKNOWN unk)
{
    HRESULT hr = g_origDSCreate8(guid, ppDS, unk);
    Log("DirectSoundCreate8 -> %08X", hr);
    // IDirectSound8 shares the vtable layout we need (3, 5)
    if (SUCCEEDED(hr) && ppDS && *ppDS) HookDirectSoundVtbl(*ppDS);
    return hr;
}

// Replace every IAT entry in `mod` that currently points at `target`.
static int PatchModuleIAT(HMODULE mod, void* target, void* repl)
{
    int n = 0;
    __try {
        BYTE* base = (BYTE*)mod;
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        IMAGE_DATA_DIRECTORY* dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir->VirtualAddress) return 0;
        for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir->VirtualAddress);
             imp->Name; imp++) {
            for (IMAGE_THUNK_DATA* t = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
                 t->u1.Function; t++) {
                if ((void*)t->u1.Function != target) continue;
                DWORD old;
                VirtualProtect(&t->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
                t->u1.Function = (DWORD_PTR)repl;
                VirtualProtect(&t->u1.Function, sizeof(void*), old, &old);
                n++;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return n;
    }
    return n;
}

static void InstallHooks()
{
    HMODULE ds = GetModuleHandleA("dsound.dll");
    if (!ds) ds = LoadLibraryA("dsound.dll");
    if (!ds) { Log("ERROR: dsound.dll not available"); return; }

    void* p1 = (void*)GetProcAddress(ds, "DirectSoundCreate");
    void* p2 = (void*)GetProcAddress(ds, "DirectSoundCreate8");
    g_origDSCreate  = (DirectSoundCreate_t)p1;
    g_origDSCreate8 = (DirectSoundCreate_t)p2;

    HMODULE mods[1024];
    DWORD needed = 0;
    int patches = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        int count = needed / sizeof(HMODULE);
        for (int i = 0; i < count; i++) {
            if (mods[i] == g_inst || mods[i] == ds) continue;
            if (p1) patches += PatchModuleIAT(mods[i], p1, (void*)MyDirectSoundCreate);
            if (p2) patches += PatchModuleIAT(mods[i], p2, (void*)MyDirectSoundCreate8);
        }
    }
    Log("InstallHooks: %d IAT entries patched", patches);
    if (patches == 0) {
        // force-log this even with Debug=0: without it the module is inert
        int dbg = g_cfg.debug; g_cfg.debug = 1;
        Log("WARNING: no DirectSoundCreate import found - music/effects sliders will have no effect");
        g_cfg.debug = dbg;
    }
}

// ***************************************************************
// Master volume (per-app Windows mixer level via WASAPI)

static const CLSID CLSID_MMDeviceEnumerator_ =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E } };
static const IID IID_IMMDeviceEnumerator_ =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6 } };
static const IID IID_IAudioSessionManager_ =
    { 0xBFA971F1, 0x4D5E, 0x40BB, { 0x93,0x5E,0x96,0x70,0x39,0xBF,0xBE,0xE4 } };

static ISimpleAudioVolume* g_masterVol;

static void InitMasterVolume()
{
    IMMDeviceEnumerator* devEnum = NULL;
    IMMDevice* dev = NULL;
    IAudioSessionManager* mgr = NULL;
    if (SUCCEEDED(CoCreateInstance(CLSID_MMDeviceEnumerator_, NULL, CLSCTX_ALL,
                                   IID_IMMDeviceEnumerator_, (void**)&devEnum))) {
        if (SUCCEEDED(devEnum->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev))) {
            if (SUCCEEDED(dev->Activate(IID_IAudioSessionManager_, CLSCTX_ALL, NULL, (void**)&mgr))) {
                mgr->GetSimpleAudioVolume(NULL, FALSE, &g_masterVol);
                mgr->Release();
            }
            dev->Release();
        }
        devEnum->Release();
    }
    Log("InitMasterVolume: %s", g_masterVol ? "ok" : "FAILED");
}

static void ApplyMaster()
{
    if (g_masterVol)
        g_masterVol->SetMasterVolume(g_cfg.master / 100.0f, NULL);
}

// ***************************************************************
// Overlay panel

#define PANEL_W   330
#define PANEL_H   176
#define ROW_Y0    44
#define ROW_STEP  34
#define ROW_H     30
#define TRACK_X0  96
#define TRACK_X1  252

static HWND g_panel;
static int  g_dragRow = -1;

static int* RowValue(int row)
{
    switch (row) {
        case 0: return &g_cfg.music;
        case 1: return &g_cfg.sfx;
        default: return &g_cfg.master;
    }
}

static void RowChanged(int row)
{
    if (row == 2) ApplyMaster();
    else ApplyCategory(row == 0 ? CAT_MUSIC : CAT_SFX);
    InvalidateRect(g_panel, NULL, FALSE);
}

static int RowFromY(int y)
{
    for (int i = 0; i < 3; i++) {
        int top = ROW_Y0 + i * ROW_STEP;
        if (y >= top && y < top + ROW_H) return i;
    }
    return -1;
}

static void SetRowFromX(int row, int x)
{
    int pct = (x - TRACK_X0) * 100 / (TRACK_X1 - TRACK_X0);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    *RowValue(row) = pct;
    RowChanged(row);
}

static HWND FindGameWindow()
{
    struct Ctx { HWND found; };
    static Ctx ctx;
    ctx.found = NULL;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId() && IsWindowVisible(h)
            && !GetWindow(h, GW_OWNER) && h != g_panel) {
            ((Ctx*)lp)->found = h;
            return FALSE;
        }
        return TRUE;
    }, (LPARAM)&ctx);
    return ctx.found;
}

static void PositionPanel()
{
    int x, y;
    HWND game = FindGameWindow();
    RECT r;
    if (game && GetWindowRect(game, &r)) {
        x = r.right - PANEL_W - 28;
        y = r.top + 64;
    } else {
        x = (GetSystemMetrics(SM_CXSCREEN) - PANEL_W) / 2;
        y = (GetSystemMetrics(SM_CYSCREEN) - PANEL_H) / 2;
    }
    SetWindowPos(g_panel, HWND_TOPMOST, x, y, PANEL_W, PANEL_H,
                 SWP_NOACTIVATE | SWP_NOSIZE);
}

static void PaintPanel(HDC dc)
{
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, PANEL_W, PANEL_H);
    HBITMAP oldBmp = (HBITMAP)SelectObject(mem, bmp);

    RECT full = { 0, 0, PANEL_W, PANEL_H };
    HBRUSH bg = CreateSolidBrush(RGB(28, 28, 32));
    FillRect(mem, &full, bg);
    DeleteObject(bg);
    HBRUSH border = CreateSolidBrush(RGB(86, 156, 214));
    FrameRect(mem, &full, border);
    DeleteObject(border);

    HFONT font = CreateFontA(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    HFONT fontB = CreateFontA(-14, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                              0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    HFONT fontS = CreateFontA(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    HFONT oldFont = (HFONT)SelectObject(mem, fontB);
    SetBkMode(mem, TRANSPARENT);

    SetTextColor(mem, RGB(230, 230, 235));
    RECT tr = { 14, 12, PANEL_W - 14, 34 };
    DrawTextA(mem, g_str->title, -1, &tr, DT_LEFT | DT_SINGLELINE);

    SelectObject(mem, font);
    for (int i = 0; i < 3; i++) {
        int top = ROW_Y0 + i * ROW_STEP;
        int cy = top + ROW_H / 2;
        int pct = *RowValue(i);

        SetTextColor(mem, RGB(170, 170, 180));
        RECT lr = { 14, top, TRACK_X0 - 8, top + ROW_H };
        DrawTextA(mem, g_str->rows[i], -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        RECT track = { TRACK_X0, cy - 3, TRACK_X1, cy + 3 };
        HBRUSH tb = CreateSolidBrush(RGB(60, 60, 68));
        FillRect(mem, &track, tb);
        DeleteObject(tb);

        int fx = TRACK_X0 + (TRACK_X1 - TRACK_X0) * pct / 100;
        RECT fill = { TRACK_X0, cy - 3, fx, cy + 3 };
        HBRUSH fb = CreateSolidBrush(RGB(86, 156, 214));
        FillRect(mem, &fill, fb);

        RECT thumb = { fx - 4, cy - 8, fx + 4, cy + 8 };
        FillRect(mem, &thumb, fb);
        DeleteObject(fb);

        char txt[8];
        _snprintf(txt, sizeof(txt), "%d%%", pct);
        SetTextColor(mem, RGB(230, 230, 235));
        RECT vr = { TRACK_X1 + 8, top, PANEL_W - 12, top + ROW_H };
        DrawTextA(mem, txt, -1, &vr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    SelectObject(mem, fontS);
    SetTextColor(mem, RGB(120, 120, 130));
    RECT fr = { 14, PANEL_H - 24, PANEL_W - 14, PANEL_H - 6 };
    DrawTextA(mem, g_str->footer, -1, &fr, DT_LEFT | DT_SINGLELINE);

    BitBlt(dc, 0, 0, PANEL_W, PANEL_H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldFont);
    SelectObject(mem, oldBmp);
    DeleteObject(font); DeleteObject(fontB); DeleteObject(fontS);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        PaintPanel(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;  // never steal focus from the game
    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        int row = RowFromY(y);
        if (row >= 0 && x >= TRACK_X0 - 10 && x <= TRACK_X1 + 10) {
            g_dragRow = row;
            SetCapture(hwnd);
            SetRowFromX(row, x);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_dragRow >= 0)
            SetRowFromX(g_dragRow, (short)LOWORD(lp));
        return 0;
    case WM_LBUTTONUP:
        if (g_dragRow >= 0) {
            g_dragRow = -1;
            ReleaseCapture();
            SaveConfig();
        }
        return 0;
    case WM_MOUSEWHEEL: {
        POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) };
        ScreenToClient(hwnd, &p);
        int row = RowFromY(p.y);
        if (row >= 0) {
            int v = *RowValue(row) + (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 2 : -2);
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            *RowValue(row) = v;
            RowChanged(row);
            SaveConfig();
        }
        return 0;
    }
    case WM_TIMER: {
        static bool wasDown = false;
        bool down = (GetAsyncKeyState(g_cfg.toggleKey) & 0x8000) != 0;
        if (down && !wasDown) {
            if (IsWindowVisible(hwnd)) {
                ShowWindow(hwnd, SW_HIDE);
            } else {
                PositionPanel();
                ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            }
        }
        wasDown = down;
        if (IsWindowVisible(hwnd))
            PositionPanel();  // follow the game window
        return 0;
    }
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static DWORD WINAPI UiThread(LPVOID)
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    InitMasterVolume();
    ApplyMaster();

    WNDCLASSA wc = {};
    wc.lpfnWndProc   = PanelProc;
    wc.hInstance     = g_inst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "wkVolumeControlPanel";
    RegisterClassA(&wc);

    g_panel = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        wc.lpszClassName, "wkVolumeControl", WS_POPUP,
        0, 0, PANEL_W, PANEL_H, NULL, NULL, g_inst, NULL);
    SetLayeredWindowAttributes(g_panel, 0, 235, LWA_ALPHA);
    SetTimer(g_panel, 1, 30, NULL);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

// ***************************************************************

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_inst = inst;
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_cs);
        LoadConfig();
        SaveConfig();  // create the ini with defaults on first run
        InstallHooks();
        CreateThread(NULL, 0, UiThread, NULL, 0, NULL);
    }
    return TRUE;
}
