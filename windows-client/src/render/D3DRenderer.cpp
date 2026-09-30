// Implementation notes (skeleton — see header):
//
// initialize():
//   1. D3D11CreateDevice with D3D_DRIVER_TYPE_HARDWARE, BGRA support, and
//      D3D11_CREATE_DEVICE_VIDEO_SUPPORT. Query ID3D11VideoDevice/Context.
//   2. IDXGIFactory2::CreateSwapChainForHwnd with:
//        SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL (or FLIP_DISCARD)
//        BufferCount = 2
//        Format = DXGI_FORMAT_B8G8R8A8_UNORM
//        SwapChainFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
//      The video swapchain window must be a child window separate from the
//      WinUI/WPF UI layer (spec §15 window composition).
//   3. SetMaximumFrameLatency(1) — keep end-to-end queue at zero frames.
//
// present():
//   1. If texture's captureTsUs <= lastPresentedCaptureTs_ → stale frame,
//      release and return without presenting (spec §37).
//   2. ID3D11VideoProcessor: NV12 → BGRA, sharp scaling (spec §23 text quality).
//   3. Present(0 /*no vsync wait*/, DXGI_PRESENT_ALLOW_TEARING when enabled)
//      or Present(1) per user's "Sync to display" setting.
//   4. frame_age_at_present_ms = nowUs() - captureTsUs — feed the performance
//      overlay (target < 25–35 ms on LAN, spec §37/§42).
//
// resize(): ResizeBuffers with the new video size; recreate video processor.

#include "D3DRenderer.h"

namespace rdp {

bool D3DRenderer::initialize(HWND hwnd, uint32_t width, uint32_t height) {
    // TODO(windows): implement per the notes above. Compile target: VS2022 C++20.
    (void)hwnd; (void)width; (void)height;
    return false;
}

void D3DRenderer::shutdown() {}

double D3DRenderer::present(ComPtr<ID3D11Texture2D> texture, uint64_t captureTsUs) {
    // TODO(windows).
    (void)texture; (void)captureTsUs;
    return 0.0;
}

bool D3DRenderer::resize(uint32_t width, uint32_t height) {
    (void)width; (void)height;
    return false;
}

} // namespace rdp
