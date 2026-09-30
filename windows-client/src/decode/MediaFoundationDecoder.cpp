// Implementation notes (skeleton — see header):
//
// initialize():
//   1. MFStartup(MF_VERSION, MFSTARTUP_LITE);
//   2. Create the H.264 decoder MFT: CoCreateInstance(CLSID_CMSH264DecoderMFT…)
//      or MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_HARDWARE, …).
//   3. Create DXGI device manager: MFCreateDXGIDeviceManager(&resetToken, &deviceManager_)
//      then deviceManager_->ResetDevice(device_.Get(), resetToken);
//   4. IMFTransform::ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, …) — decode
//      surfaces must stay on the GPU (DXVA). Verify with
//      IMFGetService → MR_VIDEO_ACCELERATION_SERVICE → GetVideoAccelerationId
//      that DXVA2 is actually in use; if the MFT falls back to software, HP
//      mode must downgrade its profile (spec §31 decode capability probe).
//
// decode(annexB):
//   1. Convert annex-B → MF samples. Prefer MFCreateSample from a pool of
//      IMFSample objects wrapping D3D11 textures; copy NALs into the sample
//      buffer (this copy is host→GPU upload, not a CPU bitmap round-trip).
//   2. Set MFSampleExtension_CleanPoint on keyframes.
//   3. ProcessInput; then drainOutput() — MF_E_TRANSFORM_NEED_MORE_INPUT is
//      the normal no-output case for P-frames.
//
// drainOutput():
//   ProcessOutput with MFT_OUTPUT_DATA_BUFFER; on success the sample's
//   IMFMediaBuffer can be cast (MFGetService / IMFGetService on the sample)
//   to IMFDXGIBuffer → ID3D11Texture2D. Hand that texture to D3DRenderer.
//   Never call IMFMediaBuffer::Lock into CPU memory in this path.
//
// latest-frame policy (spec §37): the renderer owns presentation; if decode
// output outpaces present, drop the older texture here (release it back to
// the MFT pool) instead of queueing. Track frame_age_at_present_ms in the
// renderer, not here.
//
// reset(): ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH), clear inputTypeSet_ so
// the next keyframe (requested via ControlOp::RequestKeyFrame) re-seeds SPS/PPS.

#include "MediaFoundationDecoder.h"

namespace rdp {

bool MediaFoundationDecoder::initialize(ComPtr<ID3D11DeviceContext> context, ComPtr<ID3D11Device> device) {
    // TODO(windows): implement per the notes above. Compile target: VS2022 C++20.
    (void)context; (void)device;
    return false;
}

void MediaFoundationDecoder::shutdown() {}

bool MediaFoundationDecoder::decode(const uint8_t* annexB, size_t len, uint64_t captureTsUs) {
    // TODO(windows).
    (void)annexB; (void)len; (void)captureTsUs;
    return false;
}

void MediaFoundationDecoder::reset() {}

} // namespace rdp
