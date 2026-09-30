// Windows client entry point (spec §66 end state):
// Discover Mac (mDNS/Bonjour later; manual host:port today) → pair (Phase: security)
// → auto negotiate → HP stream → local cursor → hardware pipeline.
//
// STATUS: skeleton — WinMain + window creation outline only. See README.md.
#include <windows.h>
#include <thread>

#include "src/session/HPClientSession.h"
#include "src/decode/MediaFoundationDecoder.h"
#include "src/render/D3DRenderer.h"
#include "src/input/RawInputCapture.h"

using namespace rdp;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INPUT:
        // Raw input arrives here; local cursor updates before network send (§16).
        // rawInput.onWMInput(lParam, wParam);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    // 1. Create the video child window (native swapchain host — UI layer is
    //    separate; UI framework must not sit in the hot video path, spec §64).
    WNDCLASSEXW wc{ .cbSize = sizeof(wc), .lpfnWndProc = WndProc,
                    .hInstance = hInstance, .lpszClassName = L"RDPRemoteVideo" };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Mac Remote Desktop",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                1920, 1080, nullptr, nullptr, hInstance, nullptr);
    ShowWindow(hwnd, nCmdShow);

    // 2. HP client → MF decoder → D3D renderer → raw input (skeleton wiring):
    //    HPClientSession session;
    //    MediaFoundationDecoder decoder;
    //    D3DRenderer renderer;
    //    RawInputCapture input;
    //    session.connect("192.168.x.x", 55443);
    //    renderer.initialize(hwnd, 1920, 1080);
    //    decoder.initialize(...);
    //    session.onFrame = [&](const uint8_t* nal, size_t len, const VideoFrameHeader& m) {
    //        decoder.decode(nal, len, m.captureTsUs);   // texture goes to renderer
    //    };
    //    std::thread rx([&] { session.receiveLoop(); });  // network thread (§43)

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
