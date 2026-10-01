// Windows client: D3D11/DXGI Flip Model renderer (spec §15/§37).
// Latest decoded frame wins; never queue frames behind a slow vsync (§65.3).
//
// Render thread: wait for a new frame → wait for the swapchain's frame-latency
// object (max latency 1) → re-take the newest frame (it may have been replaced
// while we waited) → VideoProcessorBlt NV12→BGRA with letterbox scaling →
// Present. frame_age_at_present_ms = present time − reassembly-complete time.
#pragma once

#include "src/decode/MediaFoundationDecoder.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rdp {

using Microsoft::WRL::ComPtr;

class D3DRenderer {
public:
    struct Stats {
        std::atomic<uint64_t> presented{0};
        std::atomic<uint64_t> replacedBeforePresent{0};  // decoded but superseded (drop-stale, §37)
        std::atomic<uint64_t> ageUsSum{0};
        std::atomic<uint64_t> ageUsMax{0};
        std::atomic<uint64_t> ageSamples{0};
    };

    ~D3DRenderer();

    /// Hardware D3D11 device with video support, multithread-protected so the
    /// decoder (decode thread) and renderer (render thread) can share it.
    bool createDevice(std::string* error);
    ID3D11Device* device() const { return device_.Get(); }
    std::string adapterName() const { return adapterName_; }

    bool initialize(HWND hwnd, bool vsync, std::string* error);
    void start();
    void stop();

    /// Decode thread → render thread handoff (single latest-frame slot).
    void submit(DecodedFrame&& frame);
    /// UI thread: window size changed.
    void requestResize();
    void setColorInfo(uint32_t mfMatrix, uint32_t mfNominalRange);
    /// Save the backbuffer as a 32-bit BMP once `afterFrames` frames were presented.
    void requestSnapshot(const std::wstring& path, uint64_t afterFrames);
    bool snapshotWritten() const { return snapshotWritten_.load(); }

    /// Size of the most recently presented video (for mouse mapping).
    void videoSize(uint32_t* w, uint32_t* h) const { *w = videoW_.load(); *h = videoH_.load(); }
    const Stats& stats() const { return stats_; }
    /// Max frame age since the previous call (per-interval telemetry).
    uint64_t takeAgeMaxUs() { return stats_.ageUsMax.exchange(0); }
    std::string fatalError() const;

    /// Aspect-preserving destination rect of a vw×vh video inside cw×ch.
    static RECT letterbox(int cw, int ch, uint32_t vw, uint32_t vh);

private:
    struct InputViewEntry {
        ID3D11Texture2D* texture;
        UINT slice;
        ComPtr<ID3D11VideoProcessorInputView> view;
        ComPtr<ID3D11Texture2D> keepAlive;
    };

    void renderLoop();
    bool resizeBuffers();
    bool ensureProcessor(const D3D11_TEXTURE2D_DESC& inDesc);
    bool ensureOutputView();
    ID3D11VideoProcessorInputView* inputView(ID3D11Texture2D* tex, UINT slice);
    bool draw(const DecodedFrame* frame);
    bool present();
    void writeSnapshot();
    void setFatal(const std::string& msg);

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VideoDevice> videoDevice_;
    ComPtr<ID3D11VideoContext> videoContext_;
    ComPtr<IDXGISwapChain2> swapChain_;      // DXGI_SWAP_EFFECT_FLIP_DISCARD, 2 buffers
    HANDLE latencyWaitable_ = nullptr;       // SetMaximumFrameLatency(1)
    UINT swapChainFlags_ = 0;
    bool tearingSupported_ = false;
    bool vsync_ = true;
    HWND hwnd_ = nullptr;
    std::string adapterName_;

    ComPtr<ID3D11VideoProcessorEnumerator> vpEnum_;
    ComPtr<ID3D11VideoProcessor> processor_;
    ComPtr<ID3D11VideoProcessorOutputView> outputView_;
    std::vector<InputViewEntry> inputViews_;
    UINT procInW_ = 0, procInH_ = 0, outW_ = 0, outH_ = 0;
    std::atomic<uint32_t> colorMatrix_{1};   // MFVideoTransferMatrix_BT709
    std::atomic<uint32_t> colorRange_{2};    // MFNominalRange_16_235
    uint32_t appliedMatrix_ = 0, appliedRange_ = 0;

    std::thread thread_;
    std::atomic<bool> running_{false};
    HANDLE frameEvent_ = nullptr;
    HANDLE resizeEvent_ = nullptr;
    std::atomic<bool> resizePending_{false};

    std::mutex slotMutex_;
    DecodedFrame pending_;        // newest decoded, not yet presented
    DecodedFrame current_;        // on screen (kept for redraw after resize)

    std::atomic<uint32_t> videoW_{0}, videoH_{0};
    std::wstring snapshotPath_;
    uint64_t snapshotAfter_ = 0;
    std::atomic<bool> snapshotWritten_{false};

    mutable std::mutex fatalMutex_;
    std::string fatal_;
    Stats stats_;
};

} // namespace rdp
