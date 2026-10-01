// Microsoft H.264 decoder MFT (CLSID_CMSH264DecoderMFT) driven synchronously
// with a D3D11 device manager. With MFT_MESSAGE_SET_D3D_MANAGER the MFT
// decodes on the GPU's fixed-function decoder (DXVA/D3D11 video) and provides
// its own output samples whose buffers are IMFDXGIBuffer → ID3D11Texture2D.
//
// Latency: MF_LOW_LATENCY + CODECAPI_AVLowLatencyMode make the MFT emit each
// picture as soon as its AU is decoded. The host never sends B-frames
// (VideoToolbox AllowFrameReordering = false), so nothing needs reordering.

#include <initguid.h>   // CODECAPI_* GUID definitions for this TU
#include "MediaFoundationDecoder.h"

#include <codecapi.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <cstdio>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")

namespace rdp {

namespace {
std::string hrText(const char* what, HRESULT hr) {
    char buf[160];
    snprintf(buf, sizeof buf, "%s (hr=0x%08lx)", what, static_cast<unsigned long>(hr));
    return buf;
}
} // namespace

bool MediaFoundationDecoder::initialize(ID3D11Device* device, std::string* error) {
    auto fail = [&](const std::string& msg) {
        lastError_ = msg;
        if (error) *error = msg;
        shutdown();
        return false;
    };

    HRESULT hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&decoder_));
    if (FAILED(hr)) return fail(hrText("H.264 decoder MFT unavailable", hr));

    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(decoder_->GetAttributes(&attrs))) {
        if (!MFGetAttributeUINT32(attrs.Get(), MF_SA_D3D11_AWARE, FALSE)) {
            return fail("H.264 decoder MFT is not D3D11-aware — no hardware decode path");
        }
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    }
    ComPtr<ICodecAPI> codecApi;
    if (SUCCEEDED(decoder_.As(&codecApi))) {
        VARIANT v{}; v.vt = VT_UI4; v.ulVal = 1;
        codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &v);
        VARIANT accel{}; accel.vt = VT_UI4; accel.ulVal = 1;
        codecApi->SetValue(&CODECAPI_AVDecVideoAcceleration_H264, &accel);
    }

    hr = MFCreateDXGIDeviceManager(&resetToken_, &deviceManager_);
    if (FAILED(hr)) return fail(hrText("MFCreateDXGIDeviceManager failed", hr));
    hr = deviceManager_->ResetDevice(device, resetToken_);
    if (FAILED(hr)) return fail(hrText("IMFDXGIDeviceManager::ResetDevice failed", hr));
    hr = decoder_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                  reinterpret_cast<ULONG_PTR>(deviceManager_.Get()));
    if (FAILED(hr)) return fail(hrText("GPU (DXVA) H.264 decoding unavailable on this adapter", hr));

    // Headroom for the surface the renderer holds (latest frame) plus the one
    // being presented, on top of the decoder's own DPB.
    ComPtr<IMFAttributes> outAttrs;
    if (SUCCEEDED(decoder_->GetOutputStreamAttributes(0, &outAttrs))) {
        outAttrs->SetUINT32(MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT, 4);
    }

    if (!setInputType()) return fail(lastError_);
    if (!selectOutputType()) return fail(lastError_);

    MFT_OUTPUT_STREAM_INFO info{};
    hr = decoder_->GetOutputStreamInfo(0, &info);
    if (FAILED(hr) || !(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                        MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
        return fail("decoder does not provide GPU output samples — DXVA not active");
    }

    decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

void MediaFoundationDecoder::shutdown() {
    if (decoder_) {
        decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        decoder_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
    }
    decoder_.Reset();
    deviceManager_.Reset();
}

bool MediaFoundationDecoder::setInputType() {
    ComPtr<IMFMediaType> t;
    HRESULT hr = MFCreateMediaType(&t);
    if (SUCCEEDED(hr)) hr = t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    // Placeholder geometry: the real size comes from the SPS and arrives as a
    // stream change on the first keyframe.
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, 1920, 1080);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, 60, 1);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = decoder_->SetInputType(0, t.Get(), 0);
    if (FAILED(hr)) { lastError_ = hrText("SetInputType(H.264) failed", hr); return false; }
    return true;
}

bool MediaFoundationDecoder::selectOutputType() {
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> t;
        HRESULT hr = decoder_->GetOutputAvailableType(0, i, &t);
        if (hr == MF_E_NO_MORE_TYPES) break;
        if (FAILED(hr)) { lastError_ = hrText("GetOutputAvailableType failed", hr); return false; }
        GUID sub{};
        if (FAILED(t->GetGUID(MF_MT_SUBTYPE, &sub)) || sub != MFVideoFormat_NV12) continue;
        hr = decoder_->SetOutputType(0, t.Get(), 0);
        if (FAILED(hr)) { lastError_ = hrText("SetOutputType(NV12) failed", hr); return false; }
        yuvMatrix_ = MFGetAttributeUINT32(t.Get(), MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        nominalRange_ = MFGetAttributeUINT32(t.Get(), MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        return true;
    }
    lastError_ = "decoder offers no NV12 output";
    return false;
}

MediaFoundationDecoder::Result MediaFoundationDecoder::decode(const uint8_t* annexB, size_t len,
                                                              bool keyframe, DecodedFrame* out) {
    if (!decoder_ || len == 0) return Result::Error;

    ComPtr<IMFMediaBuffer> buf;
    HRESULT hr = MFCreateMemoryBuffer(DWORD(len), &buf);
    if (FAILED(hr)) { lastError_ = hrText("MFCreateMemoryBuffer failed", hr); ++decodeErrors_; return Result::Error; }
    BYTE* dst = nullptr;
    // This is the compressed bitstream upload (kilobytes), not a picture copy.
    if (FAILED(buf->Lock(&dst, nullptr, nullptr))) { ++decodeErrors_; return Result::Error; }
    memcpy(dst, annexB, len);
    buf->Unlock();
    buf->SetCurrentLength(DWORD(len));

    ComPtr<IMFSample> sample;
    MFCreateSample(&sample);
    sample->AddBuffer(buf.Get());
    sample->SetSampleTime(sampleTime_);
    sample->SetSampleDuration(166'667);   // 100 ns units; timing is not used for presentation
    sampleTime_ += 166'667;
    if (keyframe) sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);

    bool got = false;
    hr = decoder_->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        if (!drainOutput(out, &got)) { ++decodeErrors_; return Result::Error; }
        hr = decoder_->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) {
        lastError_ = hrText("ProcessInput failed", hr);
        ++decodeErrors_;
        return Result::Error;
    }
    if (!drainOutput(out, &got)) { ++decodeErrors_; return Result::Error; }
    return got ? Result::Frame : Result::NoOutput;
}

bool MediaFoundationDecoder::drainOutput(DecodedFrame* out, bool* got) {
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER odb{};
        odb.dwStreamID = 0;
        DWORD status = 0;
        HRESULT hr = decoder_->ProcessOutput(0, 1, &odb, &status);
        if (odb.pEvents) odb.pEvents->Release();
        ComPtr<IMFSample> sample;
        sample.Attach(odb.pSample);

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {   // first SPS or resolution change
            if (!selectOutputType()) return false;
            continue;
        }
        if (FAILED(hr)) { lastError_ = hrText("ProcessOutput failed", hr); return false; }
        if (!sample) continue;

        ComPtr<IMFMediaBuffer> mb;
        ComPtr<IMFDXGIBuffer> dxgi;
        if (FAILED(sample->GetBufferByIndex(0, &mb)) || FAILED(mb.As(&dxgi))) {
            lastError_ = "decoder produced a system-memory picture — DXVA not active";
            return false;
        }
        ComPtr<ID3D11Texture2D> tex;
        UINT index = 0;
        if (FAILED(dxgi->GetResource(IID_PPV_ARGS(&tex))) || FAILED(dxgi->GetSubresourceIndex(&index))) {
            lastError_ = "cannot get the D3D11 texture of a decoded picture";
            return false;
        }
        // Latest picture wins if one AU ever yields more than one output.
        out->sample = sample;
        out->texture = tex;
        out->subresource = index;
        *got = true;
        ++decodedFrames_;
    }
}

void MediaFoundationDecoder::reset() {
    if (!decoder_) return;
    decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
}

} // namespace rdp
