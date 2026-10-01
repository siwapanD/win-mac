// Windows client: Media Foundation hardware H.264 decoder (spec §14).
// Pipeline: network annex-B access unit → Microsoft H.264 decoder MFT with a
// D3D11 device manager (DXVA) → NV12 D3D11 texture that never leaves the GPU.
// Decoding to a CPU bitmap and copying it into a UI framework is forbidden
// (spec §65.5); if the MFT cannot run on DXVA, initialize()/decode() fail
// instead of silently falling back to software.
#pragma once

#include <windows.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>

namespace rdp {

using Microsoft::WRL::ComPtr;

/// One decoded picture. Holding `sample` keeps the decoder's DXVA surface
/// from being reused, so the renderer can read it with zero copies.
struct DecodedFrame {
    ComPtr<IMFSample> sample;
    ComPtr<ID3D11Texture2D> texture;   // NV12 texture array owned by the decoder
    UINT subresource = 0;              // array slice of `texture`
    uint32_t width = 0, height = 0;    // visible size (stream header)
    uint64_t completedUs = 0;          // reassembly complete (client clock)
    uint64_t decodedUs = 0;            // decoder output ready (client clock)
};

class MediaFoundationDecoder {
public:
    enum class Result { Frame, NoOutput, Error };

    /// MFStartup must already have been called. `device` must be created with
    /// D3D11_CREATE_DEVICE_VIDEO_SUPPORT and be multithread-protected.
    bool initialize(ID3D11Device* device, std::string* error);
    void shutdown();

    /// Feed one complete annex-B access unit (SPS/PPS inline on keyframes).
    /// Low-latency mode: one AU in → its picture out, no reorder buffering.
    Result decode(const uint8_t* annexB, size_t len, bool keyframe, DecodedFrame* out);

    /// Packet-loss burst / desync / host restart (spec §12) — flush decoder state.
    /// The caller must feed a keyframe next.
    void reset();

    uint32_t decodedFrames() const { return decodedFrames_; }
    uint32_t decodeErrors() const { return decodeErrors_; }
    /// MFVideoTransferMatrix / MFNominalRange of the current output (from the
    /// stream's VUI when present; BT.709 limited range otherwise).
    uint32_t yuvMatrix() const { return yuvMatrix_; }
    uint32_t nominalRange() const { return nominalRange_; }
    std::string lastError() const { return lastError_; }

private:
    bool setInputType();
    bool selectOutputType();
    bool drainOutput(DecodedFrame* out, bool* got);

    ComPtr<IMFTransform> decoder_;
    ComPtr<IMFDXGIDeviceManager> deviceManager_;
    UINT resetToken_ = 0;
    LONGLONG sampleTime_ = 0;
    uint32_t decodedFrames_ = 0;
    uint32_t decodeErrors_ = 0;
    uint32_t yuvMatrix_ = 1;      // MFVideoTransferMatrix_BT709
    uint32_t nominalRange_ = 2;   // MFNominalRange_16_235
    std::string lastError_;
};

} // namespace rdp
