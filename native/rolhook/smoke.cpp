// Exercises rolhook without the game: ddraw forwarding, file/.big tracing, D3D9 device + texture hooks and the
// game's own d3d8xstub.dll path. Run from the folder that holds the built ddraw.dll and a rolhook.ini.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <share.h>

#include <cstdio>
#include <cstring>
#include <string>

static const GUID kIID_IDirectDraw7 = {0x15e65ec0, 0x3b9c, 0x11d2, {0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b}};
typedef HRESULT(WINAPI* DDCreateEx_t)(GUID*, void**, REFIID, IUnknown*);
typedef HRESULT(WINAPI* TexMemEx_t)(IDirect3DDevice9*, LPCVOID, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                    DWORD, DWORD, D3DCOLOR, void*, PALETTEENTRY*, IDirect3DTexture9**);

static int g_fail = 0;
static void check(bool ok, const char* what, long code = 0) {
    printf("  %s %s (0x%08lx)\n", ok ? "ok  " : "FAIL", what, code);
    if (!ok) ++g_fail;
}

// Targets of the probe's hardware watchpoints ([debug] watch, written into rolhook.ini before ddraw.dll loads).
static volatile LONG g_watched = 0;
__declspec(noinline) static int watched_fn(int x) { return x * 3 + int(g_watched); }

// rolhook keeps its logs open for writing: read them with sharing allowed.
static std::string read_file(const std::wstring& path) {
    std::string s;
    FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (!f) return s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

int wmain(int argc, wchar_t** argv) {
    std::wstring game = argc > 1 ? argv[1] : L"X:\\SteamLibrary\\steamapps\\common\\Rise Of Legends\\";
    if (game.back() != L'\\') game += L'\\';

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring here(exe);
    here = here.substr(0, here.find_last_of(L'\\') + 1);
    const std::wstring ini = here + L"rolhook.ini";
    wchar_t watch[96];
    swprintf_s(watch, L"%p:4:rw, %p:x", (void*)&g_watched, (void*)&watched_fn);
    WritePrivateProfileStringW(L"debug", L"watch", watch, ini.c_str());
    WritePrivateProfileStringW(L"ui", L"scale", L"200", ini.c_str());
    WritePrivateProfileStringW(L"ui", L"scale_all_pt", L"1", ini.c_str());
    // a test-run screenshot 0.3 s after the first frame (no quit: smoke.exe must live on)
    WritePrivateProfileStringW(L"test", L"start", L"first_frame", ini.c_str());
    WritePrivateProfileStringW(L"test", L"shots", L"0.3", ini.c_str());
    // interface scale overlay: reads of <here>\loc\RU\fonts.xml come from <here>\ovl\150\loc\RU\fonts.xml
    WritePrivateProfileStringW(L"ui", L"layout_scale", L"150", ini.c_str());
    WritePrivateProfileStringW(L"ui", L"layout_overlay", L"ovl", ini.c_str());
    auto put = [](const std::wstring& path, const char* text) {
        for (size_t i = path.find(L'\\', 3); i != std::wstring::npos; i = path.find(L'\\', i + 1))
            CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
        FILE* f = nullptr;
        if (!_wfopen_s(&f, path.c_str(), L"wb") && f) {
            fputs(text, f);
            fclose(f);
        }
    };
    put(here + L"loc\\RU\\fonts.xml", "ORIGINAL");
    put(here + L"ovl\\150\\loc\\RU\\fonts.xml", "OVERLAY");

    HMODULE dd = LoadLibraryW(L"ddraw.dll");
    wchar_t ddpath[MAX_PATH] = {};
    if (dd) GetModuleFileNameW(dd, ddpath, MAX_PATH);
    printf("ddraw.dll loaded from %ls\n", ddpath);
    check(dd && wcsstr(ddpath, L"native") != nullptr, "proxy ddraw.dll is ours");
    auto create = dd ? reinterpret_cast<DDCreateEx_t>(GetProcAddress(dd, "DirectDrawCreateEx")) : nullptr;
    void* ddobj = nullptr;
    HRESULT hr = create ? create(nullptr, &ddobj, kIID_IDirectDraw7, nullptr) : E_FAIL;
    check(SUCCEEDED(hr) && ddobj, "DirectDrawCreateEx forwarded to system ddraw", hr);
    if (ddobj) static_cast<IUnknown*>(ddobj)->Release();

    WIN32_FIND_DATAW fd;
    int n = 0;
    HANDLE fh = FindFirstFileW((game + L"BIGS\\*.big").c_str(), &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do ++n;
        while (FindNextFileW(fh, &fd));
        FindClose(fh);
    }
    check(n >= 40, "scan BIGS\\*.big", n);
    HANDLE f = CreateFileW((game + L"BIGS\\terrain.big").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(f != INVALID_HANDLE_VALUE, "open BIGS\\terrain.big");
    if (f != INVALID_HANDLE_VALUE) {
        BYTE buf[256];
        DWORD rd = 0;
        SetFilePointer(f, 0x508, nullptr, FILE_BEGIN);  // first entry: minimap_modifier_background.tga
        ReadFile(f, buf, 4, &rd, nullptr);
        ReadFile(f, buf, 200, &rd, nullptr);
        CloseHandle(f);
    }
    GetFileAttributesW((game + L"art\\interface\\logo_splash.bmp").c_str());
    GetFileAttributesW((game + L"art\\no_such_file.tga").c_str());
    {
        auto first = [&](DWORD acc) {
            char b[16] = {};
            DWORD rd = 0;
            HANDLE h = CreateFileW((here + L"loc\\RU\\fonts.xml").c_str(), acc, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
                                   nullptr);
            if (h == INVALID_HANDLE_VALUE) return std::string("(open failed)");
            ReadFile(h, b, 15, &rd, nullptr);
            CloseHandle(h);
            return std::string(b, rd);
        };
        check(first(GENERIC_READ) == "OVERLAY", "interface scale 150%: a read is served from the overlay");
        check(first(GENERIC_READ | GENERIC_WRITE) == "ORIGINAL", "interface scale: an open for writing is not redirected");
    }

    HMODULE d9 = LoadLibraryW(L"d3d9.dll");
    auto c9 = d9 ? reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(d9, "Direct3DCreate9")) : nullptr;
    IDirect3D9* d3d = c9 ? c9(D3D_SDK_VERSION) : nullptr;
    check(d3d != nullptr, "Direct3DCreate9 through the hooked GetProcAddress");
    HWND w = CreateWindowExW(0, L"STATIC", L"rolhook smoke", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.BackBufferWidth = 64;
    pp.BackBufferHeight = 64;
    pp.hDeviceWindow = w;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* dev = nullptr;
    hr = d3d ? d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, w, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev) : E_FAIL;
    check(SUCCEEDED(hr) && dev, "CreateDevice", hr);
    if (dev) {
        IDirect3DTexture9* t = nullptr;
        hr = dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr);
        check(SUCCEEDED(hr), "CreateTexture (managed)", hr);
        if (t) {
            D3DLOCKED_RECT lr;
            hr = t->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD);
            printf("  info LockRect(DISCARD) on a managed texture -> 0x%08lx\n", hr);
            if (SUCCEEDED(hr)) t->UnlockRect(0);
            t->Release();
        }
        HMODULE stub = LoadLibraryW((game + L"d3d8xstub.dll").c_str());
        auto get = stub ? reinterpret_cast<void*(WINAPI*)()>(GetProcAddress(stub, "get_d3dx_stub")) : nullptr;
        void* obj = get ? get() : nullptr;
        check(obj != nullptr, "game d3d8xstub.dll get_d3dx_stub");
        if (obj) {
            BYTE dds[128 + 64] = {};
            memcpy(dds, "DDS ", 4);
            DWORD* h = reinterpret_cast<DWORD*>(dds + 4);
            h[0] = 124; h[1] = 0x100F; h[2] = 4; h[3] = 4; h[4] = 16;
            h[18] = 32; h[19] = 0x41; h[21] = 32;
            h[22] = 0x00FF0000; h[23] = 0x0000FF00; h[24] = 0x000000FF; h[25] = 0xFF000000;
            h[26] = 0x1000;
            void** vt = *reinterpret_cast<void***>(obj);
            IDirect3DTexture9* tx = nullptr;
            hr = reinterpret_cast<TexMemEx_t>(vt[0x50 / 4])(dev, dds, sizeof(dds), UINT(-1), UINT(-1), UINT(-1), 0,
                                                            D3DFMT_UNKNOWN, D3DPOOL_MANAGED, DWORD(-1), DWORD(-1), 0,
                                                            nullptr, nullptr, &tx);
            check(SUCCEEDED(hr) && tx, "D3DXCreateTextureFromFileInMemoryEx through the game's stub", hr);
            if (tx) tx->Release();

            // HD override: the last .big entry this thread read is terrain.big :: minimap_modifier_background.tga
            // (65664 bytes unpacked). Hand D3DX a DXT5 256x256 DDS of exactly that size, the way the engine does;
            // rolhook must serve override_smoke\art\terrain\minimap_modifier_background.dds (512x512, 10 mips).
            static BYTE dds2[65664];
            memcpy(dds2, "DDS ", 4);
            DWORD* h2 = reinterpret_cast<DWORD*>(dds2 + 4);
            h2[0] = 124; h2[1] = 0x81007; h2[2] = 256; h2[3] = 256; h2[4] = 65536;
            h2[18] = 32; h2[19] = 0x4;
            memcpy(&h2[20], "DXT5", 4);
            h2[26] = 0x1000;
            IDirect3DTexture9* t2 = nullptr;
            hr = reinterpret_cast<TexMemEx_t>(vt[0x50 / 4])(dev, dds2, sizeof(dds2), UINT(-2), UINT(-2), 4, 0,
                                                            D3DFMT_DXT5, D3DPOOL_MANAGED, DWORD(-1), DWORD(-1), 0,
                                                            nullptr, nullptr, &t2);
            D3DSURFACE_DESC sd = {};
            DWORD levels = 0;
            if (t2) {
                t2->GetLevelDesc(0, &sd);
                levels = t2->GetLevelCount();
                t2->Release();
            }
            printf("  info override result: %ux%u, %lu levels\n", sd.Width, sd.Height, levels);
            check(SUCCEEDED(hr) && sd.Width == 512 && levels == 10, "HD override served with the file's mip chain", hr);
        }
        // a screen-space, alpha-tested triangle: exercises the frame capture ([debug] capture_at) and alpha-to-coverage
        struct V { float x, y, z, rhw; DWORD c; } tri[3] = {
            {10, 10, 0.5f, 1, 0xffffffff}, {100, 10, 0.5f, 1, 0xffffffff}, {10, 100, 0.5f, 1, 0xffffffff}};
        for (int i = 0; i < 30; ++i) {
            dev->BeginScene();
            dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
            dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
            dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
            dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(V));
            dev->EndScene();
            dev->Present(nullptr, nullptr, nullptr, nullptr);
            Sleep(50);
        }
        // UI scale ([ui] scale=200, scale_all_pt=1): a pretransformed quad over virtual pixels 4..11 (vertices at
        // 3.5..11.5 — the engine's -0.5 convention) must land on real pixels 8..23 of the backbuffer.
        {
            struct QV { float x, y, z, rhw; DWORD c; } q[4] = {{3.5f, 3.5f, 0.5f, 1, 0xffff0000},
                                                               {11.5f, 3.5f, 0.5f, 1, 0xffff0000},
                                                               {3.5f, 11.5f, 0.5f, 1, 0xffff0000},
                                                               {11.5f, 11.5f, 0.5f, 1, 0xffff0000}};
            dev->BeginScene();
            dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
            dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
            dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            dev->SetTexture(0, nullptr);
            dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(QV));
            dev->EndScene();
            IDirect3DSurface9 *bb = nullptr, *sys = nullptr;
            dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
            D3DSURFACE_DESC bd = {};
            if (bb) bb->GetDesc(&bd);
            if (bb) dev->CreateOffscreenPlainSurface(bd.Width, bd.Height, bd.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr);
            D3DLOCKED_RECT lr = {};
            bool got = sys && SUCCEEDED(dev->GetRenderTargetData(bb, sys)) &&
                       SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY));
            auto px = [&](int x, int y) {
                return got ? reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lr.pBits) + y * lr.Pitch)[x] & 0xffffff
                           : 0xdeadu;
            };
            bool inside = px(8, 8) == 0xff0000 && px(23, 23) == 0xff0000 && px(8, 23) == 0xff0000;
            bool outside = px(7, 7) == 0 && px(24, 24) == 0 && px(24, 8) == 0 && px(8, 24) == 0;
            printf("  info ui scale pixels: (7,7) %06lx (8,8) %06lx (23,23) %06lx (24,24) %06lx\n", px(7, 7), px(8, 8),
                   px(23, 23), px(24, 24));
            if (got) sys->UnlockRect();
            if (sys) sys->Release();
            if (bb) bb->Release();
            check(got && inside && outside, "ui scale 200%: virtual pixels 4..11 drawn over real pixels 8..23");

            // viewports and Clear rectangles on the backbuffer are virtual pixels too
            D3DVIEWPORT9 vp = {2, 3, 10, 12, 0.0f, 1.0f}, got_vp = {};
            dev->SetViewport(&vp);
            dev->GetViewport(&got_vp);
            printf("  info ui scale viewport 2,3 10x12 -> %lu,%lu %lux%lu\n", got_vp.X, got_vp.Y, got_vp.Width, got_vp.Height);
            check(got_vp.X == 4 && got_vp.Y == 6 && got_vp.Width == 20 && got_vp.Height == 24, "ui scale 200%: viewport scaled");
            D3DVIEWPORT9 full = {0, 0, 64, 64, 0.0f, 1.0f};
            dev->SetViewport(&full);   // bigger than the virtual screen: already real, kept
            dev->GetViewport(&got_vp);
            check(got_vp.Width == 64 && got_vp.Height == 64, "ui scale 200%: real-sized viewport kept");
        }
        // the watchpoints were set at the first Present: one write, one call must show up in probe.log
        g_watched = 7;
        int r = watched_fn(2) + watched_fn(3);
        Sleep(400);   // the log thread flushes every flush_ms
        std::string latest = read_file(here + L"logs\\latest.txt");
        std::wstring session(latest.begin(), latest.end());
        std::string probe = read_file(here + L"logs\\" + session + L"\\probe.log");
        printf("  info watched_fn results %d, probe.log of session %s: %u bytes (read while open)\n", r,
               latest.c_str(), unsigned(probe.size()));
        check(probe.find("watch 0 '") != std::string::npos && probe.find(" access after eip") != std::string::npos,
              "probe: data watchpoint logged the accesses");
        // every hit must come from smoke.exe: one inside ddraw.dll would be the probe tripping over its own read
        bool own = false;
        if (dd) {
            auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(dd) +
                                                                reinterpret_cast<const IMAGE_DOS_HEADER*>(dd)->e_lfanew);
            uintptr_t lo = reinterpret_cast<uintptr_t>(dd), hi = lo + nt->OptionalHeader.SizeOfImage;
            for (size_t p = probe.find("after eip "); p != std::string::npos; p = probe.find("after eip ", p + 1)) {
                uintptr_t eip = strtoul(probe.c_str() + p + 10, nullptr, 16);
                own |= eip >= lo && eip < hi;
            }
        }
        check(!own, "probe: no hits from rolhook's own reads");
        check(probe.find("watch 1 '") != std::string::npos && probe.find(" call  eip") != std::string::npos,
              "probe: execute watchpoint logged the call");
        // the test shot: a PNG of the 64x64 backbuffer, written by rolhook's worker thread
        std::string png;
        for (int i = 0; i < 40 && png.size() < 24; ++i) {
            png = read_file(here + L"logs\\" + session + L"\\shot_1_0.3s.png");
            if (png.size() < 24) Sleep(100);
        }
        auto be32 = [&](size_t at) {
            return png.size() >= at + 4 ? (unsigned(BYTE(png[at])) << 24 | unsigned(BYTE(png[at + 1])) << 16 |
                                           unsigned(BYTE(png[at + 2])) << 8 | unsigned(BYTE(png[at + 3])))
                                        : 0u;
        };
        printf("  info test shot: %u bytes, %ux%u\n", unsigned(png.size()), be32(16), be32(20));
        check(png.size() > 24 && !memcmp(png.data(), "\x89PNG", 4) && be32(16) == 64 && be32(20) == 64,
              "test run: timed screenshot written as a 64x64 PNG");
        dev->Release();
    }
    WritePrivateProfileStringW(L"debug", L"watch", nullptr, ini.c_str());
    WritePrivateProfileStringW(L"ui", nullptr, nullptr, ini.c_str());
    WritePrivateProfileStringW(L"test", nullptr, nullptr, ini.c_str());
    if (d3d) d3d->Release();
    if (w) DestroyWindow(w);
    printf("smoke: %s (%d failure(s))\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
