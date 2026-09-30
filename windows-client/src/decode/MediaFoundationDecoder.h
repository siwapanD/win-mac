// Windows client: Media Foundation hardware H.264 decoder (spec §14).
// Pipeline target: network NAL units → MF decoder → D3D11 GPU texture —
// decoding to a CPU bitmap and copying into the UI framework every frame is
// forbidden (spec §65.5).
//
// STATUS: skeleton. API calls follow MS Learn (MF / D3D11 video decoding) but
// have NOT been compile-verified — this file is built only on Windows
// (VS2022, C++20). See windows-client/README.md.
#pragma once

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <functional>

namespace rdp {

using Microsoft::WRL::ComPtr;

class MediaFoundationDecoder {
public:
    // Output callback receives the decoded D3D11 texture without a GPU→CPU copy.
    using FrameSink = std::function<void(ComPtr<ID3D11Texture2D> texture, uint64_t captureTsUs)>;

    bool initialize(ComPtr<ID3D11DeviceContext> context, ComPtr<ID3D11Device> device);
    void shutdown();

    /// Feed one complete annex-B frame (SPS/PPS included on keyframes by the host).
    bool decode(const uint8_t* annexB, size_t len, uint64_t captureTsUs);

    /// Packet-loss burst / desync / recovery (spec §12) — flush decoder state.
    void reset();

    uint32_t decodedFrames() const { return decodedFrames_; }
    uint32_t decodeErrors() const { return decodeErrors_; }

private:
    bool ensureInputType();
    bool drainOutput();

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IMFTransform> decoder_;
    ComPtr<IMFDXGIDeviceManager> deviceManager_;
    FrameSink sink_;
    bool inputTypeSet_ = false;
    uint32_t decodedFrames_ = 0;
    uint32_t decodeErrors_ = 0;
};

} // namespace rdp
