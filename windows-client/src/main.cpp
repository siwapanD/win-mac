// Windows client entry point (spec §66):
// manual host:port (mDNS discovery later) → hello/capabilities → HP stream →
// MF hardware decode → D3D flip-model present, local cursor, remote input.
//
// Threads (spec §43): UI (window + keyboard hook), network receive, input
// send, decode, render. No encode/decode/network work on the UI thread.
//
//   windows-client.exe --host 192.168.1.20
//   windows-client.exe --host 127.0.0.1 --seconds 6 --expect-video   (smoke, vs tools/fake-host)

#include "src/decode/MediaFoundationDecoder.h"
#include "src/input/InputCapture.h"
#include "src/render/D3DRenderer.h"
#include "src/session/HPClientSession.h"

#include <windows.h>
#include <mfapi.h>
#include <shellapi.h>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

using namespace rdp;

namespace {

constexpr UINT_PTR kTimerState = 1;   // 200 ms: input enable, title, fatal errors
constexpr UINT_PTR kTimerStats = 2;   // 1 s: telemetry line (spec §45)
constexpr UINT_PTR kTimerExit  = 3;   // --seconds

struct Options {
    std::string host;
    uint16_t port = kDefaultHPPort;
    KeyboardMode keymap = KeyboardMode::MacFriendly;
    bool fullscreen = false;
    bool vsync = true;
    int seconds = 0;
    bool expectVideo = false;
    std::wstring snapshot;
    int maxFps = 0;
    bool verboseInput = false;
};

void usage() {
    printf(
        "windows-client — Windows → Mac high-performance remote desktop (HP mode)\n\n"
        "usage: windows-client.exe --host <mac-ip|name> [options]\n\n"
        "  --host <addr>          Mac running `mac-host serve --mode hp` (required)\n"
        "  --port <n>             HP UDP port (default 55443)\n"
        "  --keymap mac|native    mac: Ctrl→Cmd, Win→Control, Alt→Option (default)\n"
        "                         native: Ctrl→Control, Alt→Option, Win→Cmd\n"
        "  --fullscreen           start borderless fullscreen\n"
        "  --no-vsync             present immediately (tearing when the GPU allows)\n"
        "  --max-fps <n>          advertised decode fps (default: display refresh, 60–120)\n"
        "  --seconds <n>          run n seconds then exit with a summary (smoke test)\n"
        "  --expect-video         with --seconds: fail unless ≥ 30 fps were presented\n"
        "  --snapshot <file.bmp>  save the 60th presented frame (diagnostics)\n"
        "  --verbose-input        log every forwarded key\n\n"
        "In the window: Ctrl+Alt+Enter toggles fullscreen. Windowed, Windows keeps\n"
        "Win / Alt+Tab / Alt+F4; fullscreen sends them to the Mac.\n");
}

std::string narrow(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

bool parseArgs(int argc, wchar_t** argv, Options* o) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&]() -> const wchar_t* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == L"--host") { auto v = next(); if (!v) return false; o->host = narrow(v); }
        else if (a == L"--port") { auto v = next(); if (!v) return false; o->port = uint16_t(_wtoi(v)); }
        else if (a == L"--keymap") {
            auto v = next(); if (!v) return false;
            const std::wstring m = v;
            if (m == L"mac") o->keymap = KeyboardMode::MacFriendly;
            else if (m == L"native") o->keymap = KeyboardMode::WindowsNative;
            else return false;
        }
        else if (a == L"--fullscreen") o->fullscreen = true;
        else if (a == L"--no-vsync") o->vsync = false;
        else if (a == L"--max-fps") { auto v = next(); if (!v) return false; o->maxFps = _wtoi(v); }
        else if (a == L"--seconds") { auto v = next(); if (!v) return false; o->seconds = _wtoi(v); }
        else if (a == L"--expect-video") o->expectVideo = true;
        else if (a == L"--snapshot") { auto v = next(); if (!v) return false; o->snapshot = v; }
        else if (a == L"--verbose-input") o->verboseInput = true;
        else if (a == L"--help" || a == L"-h") return false;
        else if (o->host.empty() && a.rfind(L"--", 0) != 0) o->host = narrow(a.c_str());
        else { fprintf(stderr, "unknown argument: %s\n", narrow(a.c_str()).c_str()); return false; }
    }
    return !o->host.empty() && o->port != 0;
}

int displayRefreshHz() {
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        return int(dm.dmDisplayFrequency);
    return 60;
}

// MARK: - application

struct App {
    Options opt;
    HWND hwnd = nullptr;
    D3DRenderer renderer;
    MediaFoundationDecoder decoder;
    HPClientSession session;
    InputCapture input;

    // decode queue (network thread → decode thread), bounded (spec §10/§44)
    struct Queued { CompletedFrame frame; bool resync = false; };
    std::mutex qMutex;
    std::condition_variable qCv;
    std::deque<Queued> queue;
    std::thread decodeThread;
    std::atomic<bool> decoding{false};
    std::atomic<uint64_t> decodeUsSum{0}, decodeSamples{0}, droppedAwaitingKey{0}, queueOverflows{0};

    bool fullscreen = false;
    WINDOWPLACEMENT savedPlacement{sizeof(WINDOWPLACEMENT)};
    uint64_t startUs = 0;
    bool everConnected = false;
    bool hintPrinted = false;
    std::string fatal;

    // stats deltas
    uint64_t lastPackets = 0, lastBytes = 0, lastFrames = 0, lastPresented = 0;
    uint64_t lastAgeSum = 0, lastAgeN = 0, lastDecSum = 0, lastDecN = 0, lastEncSum = 0, lastEncN = 0;
    double lastShownFps = 0, lastMbps = 0, lastAgeMs = 0;
};

App* g_app = nullptr;
HANDLE g_exitDone = nullptr;

void enqueueFrame(App& app, CompletedFrame&& f) {
    {
        std::lock_guard<std::mutex> lock(app.qMutex);
        bool resync = false;
        if (app.queue.size() >= 4) {
            // Decoder can't keep up: old frames are worthless (latest frame
            // wins). Dropping P-frames breaks the reference chain, so resync
            // on the next keyframe.
            app.queue.clear();
            app.queueOverflows++;
            resync = true;
        }
        app.queue.push_back({std::move(f), resync});
    }
    app.qCv.notify_one();
}

void decodeLoop(App& app) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool awaitingKeyframe = true;
    while (app.decoding) {
        App::Queued q;
        {
            std::unique_lock<std::mutex> lock(app.qMutex);
            app.qCv.wait(lock, [&] { return !app.queue.empty() || !app.decoding; });
            if (!app.decoding) break;
            q = std::move(app.queue.front());
            app.queue.pop_front();
        }
        CompletedFrame& f = q.frame;
        if (f.hostSessionChanged || q.resync) {
            app.decoder.reset();
            awaitingKeyframe = true;
        }
        const bool key = f.meta.isKeyframe();
        if (f.meta.codec != 0) continue;   // only H.264 is negotiated today
        if (awaitingKeyframe && !key) {
            app.droppedAwaitingKey++;
            app.session.requestKeyFrame("waiting for keyframe");
            continue;
        }
        if (key) awaitingKeyframe = false;
        else if (f.lostBefore > 0) app.session.requestKeyFrame("frame loss");   // keep decoding meanwhile

        DecodedFrame out;
        const uint64_t t0 = nowUs();
        const auto r = app.decoder.decode(f.annexB.data(), f.annexB.size(), key, &out);
        const uint64_t t1 = nowUs();
        if (r == MediaFoundationDecoder::Result::Error) {
            printf("decode: %s — resyncing on next keyframe\n", app.decoder.lastError().c_str());
            app.decoder.reset();
            awaitingKeyframe = true;
            app.session.requestKeyFrame("decoder error");
            continue;
        }
        if (r != MediaFoundationDecoder::Result::Frame) continue;
        app.decodeUsSum += t1 - t0;
        app.decodeSamples++;
        out.width = f.meta.width;
        out.height = f.meta.height;
        out.completedUs = f.completedUs;
        out.decodedUs = t1;
        app.renderer.setColorInfo(app.decoder.yuvMatrix(), app.decoder.nominalRange());
        app.renderer.submit(std::move(out));
    }
    CoUninitialize();
}

RECT videoRectClient(App& app) {
    RECT rc{};
    GetClientRect(app.hwnd, &rc);
    uint32_t vw = 0, vh = 0;
    app.renderer.videoSize(&vw, &vh);
    if (vw == 0 || vh == 0) return rc;
    return D3DRenderer::letterbox(rc.right - rc.left, rc.bottom - rc.top, vw, vh);
}

void setFullscreen(App& app, bool on) {
    if (on == app.fullscreen) return;
    const LONG_PTR style = GetWindowLongPtrW(app.hwnd, GWL_STYLE);
    if (on) {
        GetWindowPlacement(app.hwnd, &app.savedPlacement);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(app.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(app.hwnd, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(app.hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrW(app.hwnd, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(app.hwnd, &app.savedPlacement);
        SetWindowPos(app.hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    app.fullscreen = on;
    app.input.setFullscreen(on);
}

void updateState(App& app) {
    const bool up = app.session.handshakeComplete();
    app.input.setEnabled(up);
    if (up) app.everConnected = true;

    if (!app.fatal.empty()) return;
    std::string fatal = app.renderer.fatalError();
    if (!fatal.empty()) {
        app.fatal = fatal;
        fprintf(stderr, "render: fatal — %s\n", fatal.c_str());
        PostMessageW(app.hwnd, WM_CLOSE, 0, 0);
        return;
    }

    std::wstring title = L"Mac Remote Desktop — " + widen(app.opt.host);
    if (!up) {
        title += app.everConnected ? L" — reconnecting…" : L" — connecting…";
        if (!app.hintPrinted && nowUs() - app.startUs > 5'000'000) {
            app.hintPrinted = true;
            printf("session: no answer from %s:%u yet — is `mac-host serve --mode hp` running there,\n"
                   "         and is UDP %u reachable (same LAN, macOS firewall)?\n",
                   app.opt.host.c_str(), app.opt.port, app.opt.port);
        }
    } else {
        wchar_t buf[128];
        swprintf(buf, 128, L" — %.0f fps · %.1f Mbps · %.1f ms", app.lastShownFps, app.lastMbps, app.lastAgeMs);
        title += buf;
    }
    SetWindowTextW(app.hwnd, title.c_str());
}

void printStats(App& app) {
    const auto& s = app.session.stats();
    const auto& r = app.renderer.stats();
    const uint64_t packets = s.packetsReceived, bytes = s.bytesReceived, frames = s.framesCompleted;
    const uint64_t presented = r.presented, ageSum = r.ageUsSum, ageN = r.ageSamples;
    const uint64_t decSum = app.decodeUsSum, decN = app.decodeSamples;
    const uint64_t encSum = s.hostEncodeUsSum, encN = s.hostEncodeSamples;
    const uint64_t ageMax = app.renderer.takeAgeMaxUs();

    auto avgMs = [](uint64_t sum, uint64_t n) { return n ? double(sum) / double(n) / 1000.0 : 0.0; };
    const double recvFps = double(frames - app.lastFrames);
    app.lastShownFps = double(presented - app.lastPresented);
    app.lastMbps = double(bytes - app.lastBytes) * 8.0 / 1e6;
    app.lastAgeMs = avgMs(ageSum - app.lastAgeSum, ageN - app.lastAgeN);
    const double decMs = avgMs(decSum - app.lastDecSum, decN - app.lastDecN);
    const double encMs = avgMs(encSum - app.lastEncSum, encN - app.lastEncN);

    if (app.session.handshakeComplete() || packets != app.lastPackets) {
        printf("HP  recv %5.1f fps  shown %5.1f fps  %5.1f Mbps  decode %4.1f ms  "
               "age@present %4.1f/%4.1f ms  host-encode %4.1f ms  lost %llu  stale %llu  "
               "superseded %llu  kf-req %llu\n",
               recvFps, app.lastShownFps, app.lastMbps, decMs, app.lastAgeMs, double(ageMax) / 1000.0,
               encMs, static_cast<unsigned long long>(s.framesLost.load()),
               static_cast<unsigned long long>(s.framesDroppedStale.load()),
               static_cast<unsigned long long>(r.replacedBeforePresent.load()),
               static_cast<unsigned long long>(s.keyframeRequests.load()));
    }
    app.lastPackets = packets; app.lastBytes = bytes; app.lastFrames = frames; app.lastPresented = presented;
    app.lastAgeSum = ageSum; app.lastAgeN = ageN; app.lastDecSum = decSum; app.lastDecN = decN;
    app.lastEncSum = encSum; app.lastEncN = encN;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    App* app = g_app;
    if (app && app->hwnd == hwnd && app->input.onMouseMessage(msg, wParam, lParam)) return 0;
    switch (msg) {
    case WM_SIZE:
        if (app) app->renderer.requestResize();
        return 0;
    case WM_ERASEBKGND:
        return 1;   // the swapchain owns every pixel
    case WM_PAINT:
        ValidateRect(hwnd, nullptr);
        return 0;
    case WM_ACTIVATE:
        if (app && LOWORD(wParam) == WA_INACTIVE) app->input.resetKeyState();   // spec §49
        break;
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_KEYMENU) return 0;   // Alt alone must not enter menu mode
        break;
    case WM_DPICHANGED: {
        const RECT* r = reinterpret_cast<const RECT*>(lParam);
        if (app && !app->fullscreen)
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case kMsgToggleFullscreen:
        if (app) setFullscreen(*app, !app->fullscreen);
        return 0;
    case WM_TIMER:
        if (!app) return 0;
        if (wParam == kTimerState) updateState(*app);
        else if (wParam == kTimerStats) printStats(*app);
        else if (wParam == kTimerExit) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

BOOL WINAPI consoleHandler(DWORD type) {
    if (!g_app || !g_app->hwnd) return FALSE;
    PostMessageW(g_app->hwnd, WM_CLOSE, 0, 0);
    // Console close kills the process when this returns: let shutdown send
    // the key-ups first (spec §49).
    if (type == CTRL_CLOSE_EVENT && g_exitDone) WaitForSingleObject(g_exitDone, 3000);
    return TRUE;
}

HWND createWindow(const Options& opt) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);   // local cursor (spec §16)
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"RDPRemoteVideo";
    RegisterClassExW(&wc);

    // 16:9 client area at ~75% of the work area.
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int cw = (work.right - work.left) * 3 / 4, ch = cw * 9 / 16;
    if (ch > (work.bottom - work.top) * 3 / 4) { ch = (work.bottom - work.top) * 3 / 4; cw = ch * 16 / 9; }
    RECT r{0, 0, cw, ch};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, GetDpiForSystem());
    const std::wstring title = L"Mac Remote Desktop — " + widen(opt.host);
    return CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                           nullptr, nullptr, wc.hInstance, nullptr);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);   // telemetry stays live when redirected
    App app;
    if (!parseArgs(argc, argv, &app.opt)) { usage(); return 64; }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);   // 1:1 video pixels

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);

    std::string err;
    if (!app.renderer.createDevice(&err)) {
        fprintf(stderr, "error: %s\n", err.c_str());
        return 3;
    }
    if (!app.decoder.initialize(app.renderer.device(), &err)) {
        fprintf(stderr, "error: %s\nHP mode needs GPU H.264 decoding — use VNC mode (port 55444) instead.\n",
                err.c_str());
        return 3;
    }
    printf("gpu: %s · H.264 hardware decode (DXVA) ready\n", app.renderer.adapterName().c_str());

    app.hwnd = createWindow(app.opt);
    if (!app.hwnd || !app.renderer.initialize(app.hwnd, app.opt.vsync, &err)) {
        fprintf(stderr, "error: %s\n", err.empty() ? "cannot create window" : err.c_str());
        return 3;
    }
    g_app = &app;
    g_exitDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(consoleHandler, TRUE);
    if (!app.opt.snapshot.empty()) app.renderer.requestSnapshot(app.opt.snapshot, 60);
    app.renderer.start();
    ShowWindow(app.hwnd, SW_SHOWNORMAL);
    if (app.opt.fullscreen) setFullscreen(app, true);

    // Pipeline wiring: network → decode queue → decoder → renderer.
    app.decoding = true;
    app.decodeThread = std::thread([&] { decodeLoop(app); });
    app.session.setOnVideoFrame([&](CompletedFrame&& f) { enqueueFrame(app, std::move(f)); });

    const int refresh = displayRefreshHz();
    ClientCapabilities caps;
    caps.maxFps = app.opt.maxFps > 0 ? app.opt.maxFps : (std::min)(120, (std::max)(60, refresh));
    caps.displayRefreshRate = refresh;
    app.session.setCapabilities(caps);

    app.input.setSinks(
        [&](MouseKind k, uint8_t b, float x, float y) { app.session.sendMouse(k, b, x, y); },
        [&](bool down, uint16_t code, uint64_t flags) { app.session.sendKey(down, code, flags); },
        [&](float dx, float dy) { app.session.sendScroll(dx, dy); },
        [&] { return videoRectClient(app); });
    if (!app.input.install(app.hwnd, app.opt.keymap, app.opt.verboseInput)) {
        fprintf(stderr, "warning: keyboard hook unavailable — keyboard input disabled\n");
    }

    if (!app.session.connect(app.opt.host, app.opt.port, &err)) {
        fprintf(stderr, "error: %s\n", err.c_str());
        app.decoding = false;
        app.qCv.notify_all();
        app.decodeThread.join();
        return 2;
    }
    app.startUs = nowUs();
    printf("session: connecting to %s:%u (UDP) · keymap %s · display %d Hz · Ctrl+Alt+Enter = fullscreen\n",
           app.opt.host.c_str(), app.opt.port,
           app.opt.keymap == KeyboardMode::MacFriendly ? "mac-friendly" : "windows-native", refresh);

    SetTimer(app.hwnd, kTimerState, 200, nullptr);
    SetTimer(app.hwnd, kTimerStats, 1000, nullptr);
    if (app.opt.seconds > 0) SetTimer(app.hwnd, kTimerExit, UINT(app.opt.seconds) * 1000, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Shutdown: key/button-ups go out before the socket closes (spec §49).
    app.input.resetKeyState();
    app.input.uninstall();
    app.session.disconnect();
    app.decoding = false;
    app.qCv.notify_all();
    if (app.decodeThread.joinable()) app.decodeThread.join();
    app.renderer.stop();
    app.decoder.shutdown();

    const auto& s = app.session.stats();
    const auto& r = app.renderer.stats();
    const uint64_t presented = r.presented.load();
    printf("summary: handshake=%s  frames received %llu  presented %llu  lost %llu  "
           "keyframe requests %llu  heartbeats echoed %llu  input packets %llu  age@present avg %.1f ms\n",
           app.everConnected ? "ok" : "none",
           static_cast<unsigned long long>(s.framesCompleted.load()),
           static_cast<unsigned long long>(presented),
           static_cast<unsigned long long>(s.framesLost.load()),
           static_cast<unsigned long long>(s.keyframeRequests.load()),
           static_cast<unsigned long long>(s.heartbeatsEchoed.load()),
           static_cast<unsigned long long>(s.inputPacketsSent.load()),
           r.ageSamples ? double(r.ageUsSum) / double(r.ageSamples) / 1000.0 : 0.0);

    int code = 0;
    if (!app.fatal.empty()) code = 3;
    else if (!app.everConnected) {
        fprintf(stderr, "handshake failed — no host capabilities from %s:%u\n", app.opt.host.c_str(), app.opt.port);
        code = 2;
    } else if (app.opt.expectVideo) {
        const uint64_t need = uint64_t(app.opt.seconds) * 30;
        if (presented < need) {
            fprintf(stderr, "smoke: FAIL — %llu frames presented in %d s (need >= %llu)\n",
                    static_cast<unsigned long long>(presented), app.opt.seconds,
                    static_cast<unsigned long long>(need));
            code = 1;
        } else {
            printf("smoke: PASS\n");
        }
    }
    MFShutdown();
    CoUninitialize();
    g_app = nullptr;
    SetEvent(g_exitDone);
    return code;
}
