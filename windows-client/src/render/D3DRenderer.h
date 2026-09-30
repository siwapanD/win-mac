// Windows client: D3D11/DXGI Flip Model renderer (spec §15/§37).
// Latest decoded frame wins; never queue frames behind a slow vsync (§65.3).
//
// STATUS: skeleton — compile-verified only on Windows. See README.md.
#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdint>

namespace rdp {

using Microsoft::WRL::ComPtr;

class D3DRenderer {
public:
    bool initialize(HWND hwnd, uint32_t width, uint32_t height);
    void shutdown();

    /// Present the newest decoded texture. Ownership of `texture` transfers to
    /// the renderer (it may drop a stale texture if a newer frame arrived —
    /// spec §37 drop-stale policy). Returns frame age in ms for the overlay.
    double present(ComPtr<ID3D11Texture2D> texture, uint64_t captureTsUs);

    /// Resize when the video resolution changes (capture profile switch).
    bool resize(uint32_t width, uint32_t height);

private:
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swapChain_;      // DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL / FLIP_DISCARD
    ComPtr<ID3D11VideoDevice> videoDevice_;
    ComPtr<ID3D11VideoContext> videoContext_;
    ComPtr<ID3D11VideoProcessor> videoProcessor_;  // YUV→RGB + stretch via GPU
    uint32_t width_ = 0, height_ = 0;
    uint64_t lastPresentedCaptureTs_ = 0;
};

} // namespace rdp
