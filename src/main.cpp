// ============================================================================
// GoXLR Audio Keep-Alive — Native Win32/WASAPI Edition
// ============================================================================
// Single-file, zero-dependency systray application that prevents Windows from
// putting GoXLR audio endpoints to sleep via WASAPI idle power management.
//
// Opens shared-mode WASAPI render sessions on all GoXLR output endpoints and
// feeds them silence. Sits in the system tray with a context menu.
//
// Build: cl /EHsc /O2 /DUNICODE /D_UNICODE main.cpp /link ole32.lib shell32.lib user32.lib
//    or: use the provided build.bat / CMakeLists.txt
//
// Author: Claude × Colin — "50 KB tout mouillé" (narrator: it was not 50 KB)
// License: MIT
// ============================================================================

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <shellapi.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <endpointvolume.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>

// ============================================================================
// Constants
// ============================================================================

static constexpr UINT WM_TRAYICON       = WM_USER + 1;
static constexpr UINT IDM_STATUS        = 1001;
static constexpr UINT IDM_RESTART       = 1002;
static constexpr UINT IDM_QUIT          = 1003;
static constexpr UINT TIMER_HEALTHCHECK = 1;
static constexpr UINT HEALTHCHECK_MS    = 5000;

static constexpr int  SILENCE_SAMPLERATE  = 48000;
static constexpr int  SILENCE_CHANNELS    = 2;
static constexpr int  SILENCE_BITDEPTH    = 16;

static const wchar_t* APP_NAME     = L"GoXLR Keep-Alive";
static const wchar_t* WINDOW_CLASS = L"GoXLRKeepAliveWndClass";
static const wchar_t* MATCH_PATTERN = L"GoXLR";

// ============================================================================
// COM smart pointer (minimal, no ATL/WRL dependency)
// ============================================================================

template<typename T>
class ComPtr {
    T* ptr_ = nullptr;
public:
    ComPtr() = default;
    ~ComPtr() { Release(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& o) noexcept : ptr_(o.ptr_) { o.ptr_ = nullptr; }
    ComPtr& operator=(ComPtr&& o) noexcept {
        if (this != &o) { Release(); ptr_ = o.ptr_; o.ptr_ = nullptr; }
        return *this;
    }

    void Release() { if (ptr_) { ptr_->Release(); ptr_ = nullptr; } }

    T*  Get() const        { return ptr_; }
    T** GetAddressOf()     { return &ptr_; }
    T*  operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }
};

// ============================================================================
// Per-endpoint keep-alive state
// ============================================================================

struct EndpointSession {
    std::wstring          deviceName;
    std::wstring          deviceId;
    ComPtr<IMMDevice>     device;
    ComPtr<IAudioClient>  audioClient;
    UINT32                bufferFrames = 0;
    bool                  alive = false;

    bool Start() {
        Stop(); // Clean slate

        // Initialize audio client in shared mode
        WAVEFORMATEX wfx = {};
        wfx.wFormatTag      = WAVE_FORMAT_PCM;
        wfx.nChannels        = SILENCE_CHANNELS;
        wfx.nSamplesPerSec   = SILENCE_SAMPLERATE;
        wfx.wBitsPerSample   = SILENCE_BITDEPTH;
        wfx.nBlockAlign      = wfx.nChannels * wfx.wBitsPerSample / 8;
        wfx.nAvgBytesPerSec  = wfx.nSamplesPerSec * wfx.nBlockAlign;
        wfx.cbSize           = 0;

        HRESULT hr = device->Activate(
            __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(audioClient.GetAddressOf()));
        if (FAILED(hr)) return false;

        // Shared mode, 200ms buffer (we don't care about latency)
        // AUDCLNT_STREAMFLAGS_NOPERSIST: don't save this session in the volume mixer
        hr = audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_NOPERSIST,
            2000000,  // 200ms in 100ns units
            0,        // periodicity (0 = default for shared)
            &wfx,
            nullptr); // session GUID

        if (FAILED(hr)) {
            // If our format isn't supported in shared mode, try the mix format
            WAVEFORMATEX* mixFmt = nullptr;
            HRESULT hr2 = audioClient->GetMixFormat(&mixFmt);
            if (SUCCEEDED(hr2) && mixFmt) {
                audioClient.Release();
                device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                    reinterpret_cast<void**>(audioClient.GetAddressOf()));
                hr = audioClient->Initialize(
                    AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_NOPERSIST,
                    2000000, 0, mixFmt, nullptr);
                CoTaskMemFree(mixFmt);
            }
            if (FAILED(hr)) {
                audioClient.Release();
                return false;
            }
        }

        hr = audioClient->GetBufferSize(&bufferFrames);
        if (FAILED(hr)) { audioClient.Release(); return false; }

        // Get render client, fill initial buffer with silence, start
        ComPtr<IAudioRenderClient> renderClient;
        hr = audioClient->GetService(__uuidof(IAudioRenderClient),
            reinterpret_cast<void**>(renderClient.GetAddressOf()));
        if (FAILED(hr)) { audioClient.Release(); return false; }

        BYTE* data = nullptr;
        hr = renderClient->GetBuffer(bufferFrames, &data);
        if (SUCCEEDED(hr)) {
            // AUDCLNT_BUFFERFLAGS_SILENT tells the engine this is silence
            // without us needing to zero-fill (but we do anyway, belt+suspenders)
            memset(data, 0, bufferFrames * SILENCE_CHANNELS * (SILENCE_BITDEPTH / 8));
            renderClient->ReleaseBuffer(bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
        }

        hr = audioClient->Start();
        if (FAILED(hr)) { audioClient.Release(); return false; }

        alive = true;
        return true;
    }

    void Stop() {
        if (audioClient.Get()) {
            audioClient->Stop();
        }
        audioClient.Release();
        bufferFrames = 0;
        alive = false;
    }

    bool HealthCheck() {
        if (!audioClient.Get()) {
            return Start();
        }

        // Try to get current padding — if the device was invalidated, this fails
        UINT32 padding = 0;
        HRESULT hr = audioClient->GetCurrentPadding(&padding);

        if (hr == AUDCLNT_E_DEVICE_INVALIDATED ||
            hr == AUDCLNT_E_SERVICE_NOT_RUNNING ||
            hr == AUDCLNT_E_NOT_INITIALIZED ||
            FAILED(hr)) {
            // Session is dead, try to restart
            alive = false;
            // Need to re-activate from the device
            audioClient.Release();
            return Start();
        }

        // Feed more silence if buffer has space
        ComPtr<IAudioRenderClient> renderClient;
        hr = audioClient->GetService(__uuidof(IAudioRenderClient),
            reinterpret_cast<void**>(renderClient.GetAddressOf()));
        if (SUCCEEDED(hr)) {
            UINT32 available = bufferFrames - padding;
            if (available > 0) {
                BYTE* data = nullptr;
                hr = renderClient->GetBuffer(available, &data);
                if (SUCCEEDED(hr)) {
                    renderClient->ReleaseBuffer(available, AUDCLNT_BUFFERFLAGS_SILENT);
                }
            }
        }

        alive = true;
        return true;
    }
};

// ============================================================================
// Application state
// ============================================================================

struct AppState {
    HWND                          hwnd = nullptr;
    NOTIFYICONDATAW               nid = {};
    std::vector<EndpointSession>  sessions;
    int                           aliveCount = 0;
    int                           totalCount = 0;
    bool                          comInitialized = false;

    bool EnumerateAndStart() {
        sessions.clear();
        aliveCount = 0;

        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(enumerator.GetAddressOf()));
        if (FAILED(hr)) return false;

        ComPtr<IMMDeviceCollection> collection;
        hr = enumerator->EnumAudioEndpoints(
            eRender, DEVICE_STATE_ACTIVE,
            collection.GetAddressOf());
        if (FAILED(hr)) return false;

        UINT count = 0;
        collection->GetCount(&count);

        for (UINT i = 0; i < count; i++) {
            ComPtr<IMMDevice> device;
            hr = collection->Item(i, device.GetAddressOf());
            if (FAILED(hr)) continue;

            // Get device name
            ComPtr<IPropertyStore> props;
            hr = device->OpenPropertyStore(STGM_READ, props.GetAddressOf());
            if (FAILED(hr)) continue;

            PROPVARIANT varName;
            PropVariantInit(&varName);
            hr = props->GetValue(PKEY_Device_FriendlyName, &varName);
            if (FAILED(hr)) continue;

            std::wstring name(varName.pwszVal ? varName.pwszVal : L"");
            PropVariantClear(&varName);

            // Check if this is a GoXLR endpoint
            // Case-insensitive search
            std::wstring nameLower = name;
            std::wstring patternLower = MATCH_PATTERN;
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::towlower);
            std::transform(patternLower.begin(), patternLower.end(), patternLower.begin(), ::towlower);

            if (nameLower.find(patternLower) == std::wstring::npos) continue;

            // Get device ID
            LPWSTR deviceId = nullptr;
            device->GetId(&deviceId);
            std::wstring id(deviceId ? deviceId : L"");
            if (deviceId) CoTaskMemFree(deviceId);

            EndpointSession session;
            session.deviceName = std::move(name);
            session.deviceId = std::move(id);
            session.device = std::move(device);

            sessions.push_back(std::move(session));
        }

        totalCount = static_cast<int>(sessions.size());

        // Start all sessions
        for (auto& s : sessions) {
            if (s.Start()) {
                aliveCount++;
            }
        }

        return totalCount > 0;
    }

    void RunHealthChecks() {
        aliveCount = 0;
        for (auto& s : sessions) {
            if (s.HealthCheck()) {
                aliveCount++;
            }
        }
        UpdateTrayTooltip();
    }

    void StopAll() {
        for (auto& s : sessions) {
            s.Stop();
        }
        aliveCount = 0;
    }

    void UpdateTrayTooltip() {
        wchar_t tip[128];
        _snwprintf_s(tip, _countof(tip), _TRUNCATE,
            L"GoXLR Keep-Alive: %d/%d endpoints alive",
            aliveCount, totalCount);
        wcscpy_s(nid.szTip, tip);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    std::wstring GetStatusText() {
        std::wstring status;
        status += L"GoXLR Audio Keep-Alive\n";
        status += L"━━━━━━━━━━━━━━━━━━━━━━━\n\n";

        if (sessions.empty()) {
            status += L"No GoXLR endpoints found.\n";
        } else {
            for (const auto& s : sessions) {
                status += s.alive ? L"● " : L"○ ";
                status += s.deviceName;
                status += s.alive ? L"  [alive]\n" : L"  [dead]\n";
            }
            status += L"\n";
            wchar_t buf[64];
            _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                L"%d/%d endpoints active", aliveCount, totalCount);
            status += buf;
        }
        return status;
    }
};

static AppState g_app;

// ============================================================================
// Systray icon resource (embedded XPM-style — a tiny green circle)
// We generate a simple icon programmatically since we have no .ico resource
// ============================================================================

static HICON CreateKeepAliveIcon(bool allAlive) {
    const int size = 16;

    // Create a simple colored circle icon
    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP hbmColor = CreateCompatibleBitmap(screenDC, size, size);
    HBITMAP hbmMask  = CreateBitmap(size, size, 1, 1, nullptr);

    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, hbmColor);

    // Fill transparent
    HBRUSH bgBrush = CreateSolidBrush(RGB(0, 0, 0));
    RECT rc = { 0, 0, size, size };
    FillRect(memDC, &rc, bgBrush);
    DeleteObject(bgBrush);

    // Draw colored circle
    COLORREF color = allAlive ? RGB(0, 200, 80) : RGB(220, 60, 60);
    HBRUSH circleBrush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HBRUSH oldBr = (HBRUSH)SelectObject(memDC, circleBrush);
    HPEN oldPen = (HPEN)SelectObject(memDC, pen);
    Ellipse(memDC, 2, 2, size - 2, size - 2);
    SelectObject(memDC, oldBr);
    SelectObject(memDC, oldPen);
    DeleteObject(circleBrush);
    DeleteObject(pen);

    SelectObject(memDC, oldBmp);

    // Create mask (circle area = 0 = visible, rest = 1 = transparent)
    HDC maskDC = CreateCompatibleDC(screenDC);
    HBITMAP oldMask = (HBITMAP)SelectObject(maskDC, hbmMask);
    HBRUSH whiteBrush = (HBRUSH)GetStockObject(WHITE_BRUSH);
    FillRect(maskDC, &rc, whiteBrush);
    HBRUSH blackBrush = (HBRUSH)GetStockObject(BLACK_BRUSH);
    HPEN blackPen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    SelectObject(maskDC, blackBrush);
    SelectObject(maskDC, blackPen);
    Ellipse(maskDC, 2, 2, size - 2, size - 2);
    DeleteObject(blackPen);
    SelectObject(maskDC, oldMask);

    DeleteDC(maskDC);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    ICONINFO ii = {};
    ii.fIcon    = TRUE;
    ii.hbmMask  = hbmMask;
    ii.hbmColor = hbmColor;
    HICON hIcon = CreateIconIndirect(&ii);

    DeleteObject(hbmColor);
    DeleteObject(hbmMask);

    return hIcon;
}

// ============================================================================
// Window procedure
// ============================================================================

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    case WM_CREATE:
        SetTimer(hwnd, TIMER_HEALTHCHECK, HEALTHCHECK_MS, nullptr);
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_HEALTHCHECK) {
            g_app.RunHealthChecks();
        }
        return 0;

    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd);

            HMENU hMenu = CreatePopupMenu();

            // Status line (disabled, just informational)
            wchar_t statusLine[64];
            _snwprintf_s(statusLine, _countof(statusLine), _TRUNCATE,
                L"%d/%d endpoints alive", g_app.aliveCount, g_app.totalCount);
            AppendMenuW(hMenu, MF_STRING | MF_DISABLED, 0, statusLine);
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

            AppendMenuW(hMenu, MF_STRING, IDM_STATUS,  L"Show &Details...");
            AppendMenuW(hMenu, MF_STRING, IDM_RESTART, L"&Restart Sessions");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hMenu, MF_STRING, IDM_QUIT,    L"&Quit");

            TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(hMenu);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_STATUS:
            MessageBoxW(hwnd, g_app.GetStatusText().c_str(), APP_NAME, MB_OK | MB_ICONINFORMATION);
            break;
        case IDM_RESTART:
            g_app.StopAll();
            g_app.EnumerateAndStart();
            g_app.UpdateTrayTooltip();
            break;
        case IDM_QUIT:
            PostQuitMessage(0);
            break;
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_HEALTHCHECK);
        Shell_NotifyIconW(NIM_DELETE, &g_app.nid);
        if (g_app.nid.hIcon) DestroyIcon(g_app.nid.hIcon);
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// ============================================================================
// Entry point
// ============================================================================

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    // Initialize COM (STA for shell integration)
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"Failed to initialize COM.", APP_NAME, MB_ICONERROR);
        return 1;
    }
    g_app.comInitialized = true;

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = WINDOW_CLASS;
    RegisterClassExW(&wc);

    // Create hidden message-only window
    g_app.hwnd = CreateWindowExW(
        0, WINDOW_CLASS, APP_NAME,
        0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, hInstance, nullptr);

    if (!g_app.hwnd) {
        CoUninitialize();
        return 1;
    }

    // Enumerate GoXLR endpoints and start sessions
    bool found = g_app.EnumerateAndStart();

    // Setup systray icon
    HICON hIcon = CreateKeepAliveIcon(found && g_app.aliveCount == g_app.totalCount);

    g_app.nid.cbSize           = sizeof(g_app.nid);
    g_app.nid.hWnd             = g_app.hwnd;
    g_app.nid.uID              = 1;
    g_app.nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_app.nid.uCallbackMessage = WM_TRAYICON;
    g_app.nid.hIcon            = hIcon;

    wchar_t tip[128];
    if (found) {
        _snwprintf_s(tip, _countof(tip), _TRUNCATE,
            L"GoXLR Keep-Alive: %d/%d endpoints alive",
            g_app.aliveCount, g_app.totalCount);
    } else {
        wcscpy_s(tip, L"GoXLR Keep-Alive: no endpoints found");
    }
    wcscpy_s(g_app.nid.szTip, tip);

    Shell_NotifyIconW(NIM_ADD, &g_app.nid);

    // Message loop
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Cleanup
    g_app.StopAll();
    CoUninitialize();
    return 0;
}

// ============================================================================
// Subsystem: Windows (no console) — but allow console too for debugging
// If you want a console version, compile without /SUBSYSTEM:WINDOWS and
// change wWinMain to wmain.
// ============================================================================
