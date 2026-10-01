// D3D11 flip-model renderer — see header for the loop and policy.
//
// Colour: the decoded NV12 is converted by the GPU video processor using the
// stream's matrix/range (BT.709 limited range unless the SPS VUI says
// otherwise) into full-range BGRA. Scaling is aspect-preserving with black
// bars; mouse coordinates use the same letterbox() rect (InputCapture).

#include "D3DRenderer.h"
#include "src/protocol/wire.h"   // nowUs()

#include <d3d11_4.h>
#include <dxgi1_5.h>
#include <mfapi.h>
#include <algorithm>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace rdp {

namespace {
std::string hrText(const char* what, HRESULT hr) {
    char buf[160];
    snprintf(buf, sizeof buf, "%s (hr=0x%08lx)", what, static_cast<unsigned long>(hr));
    return buf;
}

std::string utf8(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
} // namespace

D3DRenderer::~D3DRenderer() {
    stop();
    if (frameEvent_) CloseHandle(frameEvent_);
    if (resizeEvent_) CloseHandle(resizeEvent_);
    if (latencyWaitable_) CloseHandle(latencyWaitable_);
}

RECT D3DRenderer::letterbox(int cw, int ch, uint32_t vw, uint32_t vh) {
    RECT r{0, 0, cw, ch};
    if (cw <= 0 || ch <= 0 || vw == 0 || vh == 0) return r;
    // Compare aspect ratios in integers: cw/ch vs vw/vh.
    const long long lhs = 1LL * cw * vh, rhs = 1LL * ch * vw;
    if (lhs > rhs) {            // window wider than video → pillarbox
        const int w = int(rhs / vh);
        r.left = (cw - w) / 2; r.right = r.left + w;
    } else if (lhs < rhs) {     // window taller than video → letterbox
        const int h = int(lhs / vw);
        r.top = (ch - h) / 2; r.bottom = r.top + h;
    }
    return r;
}

bool D3DRenderer::createDevice(std::string* error) {
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                   UINT(std::size(levels)), D3D11_SDK_VERSION,
                                   &device_, &got, &context_);
    if (FAILED(hr)) {
        if (error) *error = hrText("D3D11CreateDevice(HARDWARE, VIDEO_SUPPORT) failed", hr);
        return false;
    }
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(context_.As(&mt))) mt->SetMultithreadProtected(TRUE);
    if (FAILED(device_.As(&videoDevice_)) || FAILED(context_.As(&videoContext_))) {
        if (error) *error = "D3D11 video device/context unavailable";
        return false;
    }
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(device_.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetDesc(&desc))) {
        adapterName_ = utf8(desc.Description);
    }
    return true;
}

bool D3DRenderer::initialize(HWND hwnd, bool vsync, std::string* error) {
    hwnd_ = hwnd;
    vsync_ = vsync;
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    HRESULT hr = device_.As(&dxgiDevice);
    if (SUCCEEDED(hr)) hr = dxgiDevice->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { if (error) *error = hrText("DXGI factory unavailable", hr); return false; }

    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory.As(&factory5))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof allow)))
            tearingSupported_ = allow == TRUE;
    }

    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.Scaling = DXGI_SCALING_STRETCH;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    swapChainFlags_ = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                      (tearingSupported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    d.Flags = swapChainFlags_;

    ComPtr<IDXGISwapChain1> sc1;
    hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd, &d, nullptr, nullptr, &sc1);
    if (FAILED(hr)) { if (error) *error = hrText("CreateSwapChainForHwnd(FLIP_DISCARD) failed", hr); return false; }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);   // fullscreen is borderless, ours
    hr = sc1.As(&swapChain_);
    if (FAILED(hr)) { if (error) *error = hrText("IDXGISwapChain2 unavailable", hr); return false; }
    swapChain_->SetMaximumFrameLatency(1);   // zero-frame queue beyond the one being shown
    latencyWaitable_ = swapChain_->GetFrameLatencyWaitableObject();

    DXGI_SWAP_CHAIN_DESC1 actual{};
    swapChain_->GetDesc1(&actual);
    outW_ = actual.Width; outH_ = actual.Height;

    frameEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    resizeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return true;
}

void D3DRenderer::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { renderLoop(); });
}

void D3DRenderer::stop() {
    if (!running_.exchange(false)) return;
    SetEvent(frameEvent_);
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lock(slotMutex_);
    pending_ = {};
    current_ = {};
}

void D3DRenderer::submit(DecodedFrame&& frame) {
    {
        std::lock_guard<std::mutex> lock(slotMutex_);
        if (pending_.texture) stats_.replacedBeforePresent++;   // superseded before it was shown
        pending_ = std::move(frame);
    }
    SetEvent(frameEvent_);
}

void D3DRenderer::requestResize() {
    resizePending_ = true;
    SetEvent(resizeEvent_);
}

void D3DRenderer::setColorInfo(uint32_t mfMatrix, uint32_t mfNominalRange) {
    colorMatrix_ = mfMatrix;
    colorRange_ = mfNominalRange;
}

void D3DRenderer::requestSnapshot(const std::wstring& path, uint64_t afterFrames) {
    snapshotPath_ = path;
    snapshotAfter_ = (std::max<uint64_t>)(1, afterFrames);
}

std::string D3DRenderer::fatalError() const {
    std::lock_guard<std::mutex> lock(fatalMutex_);
    return fatal_;
}

void D3DRenderer::setFatal(const std::string& msg) {
    std::lock_guard<std::mutex> lock(fatalMutex_);
    if (fatal_.empty()) fatal_ = msg;
}

// MARK: - render thread

void D3DRenderer::renderLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    HANDLE waits[2] = {frameEvent_, resizeEvent_};
    bool needRedraw = true;   // first present: black instead of undefined contents

    while (running_) {
        if (!needRedraw) WaitForMultipleObjects(2, waits, FALSE, 100);
        if (!running_) break;

        if (resizePending_.exchange(false)) {
            if (!resizeBuffers()) { Sleep(10); continue; }
            needRedraw = true;
        }

        bool haveNew;
        {
            std::lock_guard<std::mutex> lock(slotMutex_);
            haveNew = pending_.texture != nullptr;
        }
        if (!haveNew && !needRedraw) continue;
        if (outW_ == 0 || outH_ == 0) { needRedraw = false; continue; }   // minimized

        // Block until the previous present has been retired, THEN pick the
        // newest frame — anything decoded meanwhile supersedes the older one.
        WaitForSingleObjectEx(latencyWaitable_, 100, TRUE);
        {
            std::lock_guard<std::mutex> lock(slotMutex_);
            if (pending_.texture) current_ = std::move(pending_);
            pending_ = {};
        }
        if (!draw(current_.texture ? &current_ : nullptr)) continue;
        if (!present()) break;
        needRedraw = false;
    }
}

bool D3DRenderer::resizeBuffers() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    if (rc.right - rc.left <= 0 || rc.bottom - rc.top <= 0) { outW_ = outH_ = 0; return true; }
    outputView_.Reset();   // the only lasting reference to the backbuffer
    HRESULT hr = swapChain_->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, swapChainFlags_);
    if (FAILED(hr)) { setFatal(hrText("ResizeBuffers failed", hr)); return false; }
    DXGI_SWAP_CHAIN_DESC1 d{};
    swapChain_->GetDesc1(&d);
    outW_ = d.Width; outH_ = d.Height;
    return true;
}

bool D3DRenderer::ensureProcessor(const D3D11_TEXTURE2D_DESC& in) {
    if (processor_ && in.Width == procInW_ && in.Height == procInH_) return true;
    processor_.Reset();
    vpEnum_.Reset();
    outputView_.Reset();
    inputViews_.clear();

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate = {60, 1};
    cd.InputWidth = in.Width;
    cd.InputHeight = in.Height;
    cd.OutputFrameRate = {60, 1};
    cd.OutputWidth = (std::max)(outW_, 1u);
    cd.OutputHeight = (std::max)(outH_, 1u);
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    HRESULT hr = videoDevice_->CreateVideoProcessorEnumerator(&cd, &vpEnum_);
    if (FAILED(hr)) { setFatal(hrText("CreateVideoProcessorEnumerator failed", hr)); return false; }
    UINT support = 0;
    if (FAILED(vpEnum_->CheckVideoProcessorFormat(in.Format, &support)) ||
        !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) {
        setFatal("GPU video processor cannot read the decoder's output format");
        return false;
    }
    hr = videoDevice_->CreateVideoProcessor(vpEnum_.Get(), 0, &processor_);
    if (FAILED(hr)) { setFatal(hrText("CreateVideoProcessor failed", hr)); return false; }
    videoContext_->VideoProcessorSetStreamAutoProcessingMode(processor_.Get(), 0, FALSE);
    videoContext_->VideoProcessorSetStreamFrameFormat(processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out{};
    out.RGB_Range = 0;   // full-range RGB for the desktop
    videoContext_->VideoProcessorSetOutputColorSpace(processor_.Get(), &out);
    D3D11_VIDEO_COLOR black{};
    black.RGBA = {0.f, 0.f, 0.f, 1.f};
    videoContext_->VideoProcessorSetOutputBackgroundColor(processor_.Get(), FALSE, &black);
    procInW_ = in.Width;
    procInH_ = in.Height;
    appliedMatrix_ = appliedRange_ = 0;   // force colour-space upload
    return true;
}

bool D3DRenderer::ensureOutputView() {
    if (outputView_) return true;
    ComPtr<ID3D11Texture2D> back;
    HRESULT hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&back));
    if (FAILED(hr)) { setFatal(hrText("GetBuffer failed", hr)); return false; }
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC vd{};
    vd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    hr = videoDevice_->CreateVideoProcessorOutputView(back.Get(), vpEnum_.Get(), &vd, &outputView_);
    if (FAILED(hr)) { setFatal(hrText("CreateVideoProcessorOutputView failed", hr)); return false; }
    return true;
}

ID3D11VideoProcessorInputView* D3DRenderer::inputView(ID3D11Texture2D* tex, UINT slice) {
    for (auto& e : inputViews_)
        if (e.texture == tex && e.slice == slice) return e.view.Get();
    if (inputViews_.size() >= 48) inputViews_.clear();   // decoder re-allocated its pool
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC d{};
    d.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    d.Texture2D.ArraySlice = slice;
    ComPtr<ID3D11VideoProcessorInputView> view;
    HRESULT hr = videoDevice_->CreateVideoProcessorInputView(tex, vpEnum_.Get(), &d, &view);
    if (FAILED(hr)) { setFatal(hrText("CreateVideoProcessorInputView failed", hr)); return nullptr; }
    inputViews_.push_back({tex, slice, view, tex});
    return view.Get();
}

bool D3DRenderer::draw(const DecodedFrame* f) {
    if (!f) {   // nothing decoded yet: present black
        ComPtr<ID3D11Texture2D> back;
        ComPtr<ID3D11RenderTargetView> rtv;
        if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&back))) ||
            FAILED(device_->CreateRenderTargetView(back.Get(), nullptr, &rtv))) return false;
        const float black[4] = {0.f, 0.f, 0.f, 1.f};
        context_->ClearRenderTargetView(rtv.Get(), black);
        return true;
    }

    D3D11_TEXTURE2D_DESC in{};
    f->texture->GetDesc(&in);
    if (!ensureProcessor(in) || !ensureOutputView()) return false;

    // Output size may have changed since the processor was created (resize):
    // the content desc output size is only a hint, so just re-target rects.
    const uint32_t mtx = colorMatrix_.load(), rng = colorRange_.load();
    if (mtx != appliedMatrix_ || rng != appliedRange_) {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE cs{};
        cs.Usage = 0;
        cs.RGB_Range = 0;
        cs.YCbCr_Matrix = mtx == MFVideoTransferMatrix_BT601 ? 0 : 1;
        cs.YCbCr_xvYCC = 0;
        cs.Nominal_Range = rng == MFNominalRange_0_255 ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255
                                                       : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        videoContext_->VideoProcessorSetStreamColorSpace(processor_.Get(), 0, &cs);
        appliedMatrix_ = mtx;
        appliedRange_ = rng;
    }

    ID3D11VideoProcessorInputView* iv = inputView(f->texture.Get(), f->subresource);
    if (!iv) return false;

    const UINT vw = (f->width && f->width <= in.Width) ? f->width : in.Width;
    const UINT vh = (f->height && f->height <= in.Height) ? f->height : in.Height;
    const RECT src{0, 0, LONG(vw), LONG(vh)};
    const RECT dst = letterbox(int(outW_), int(outH_), vw, vh);
    const RECT full{0, 0, LONG(outW_), LONG(outH_)};
    videoContext_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE, &src);
    videoContext_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE, &dst);
    videoContext_->VideoProcessorSetOutputTargetRect(processor_.Get(), TRUE, &full);

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = iv;
    HRESULT hr = videoContext_->VideoProcessorBlt(processor_.Get(), outputView_.Get(), 0, 1, &stream);
    if (FAILED(hr)) { setFatal(hrText("VideoProcessorBlt failed", hr)); return false; }
    videoW_ = vw;
    videoH_ = vh;
    return true;
}

bool D3DRenderer::present() {
    const bool isVideo = current_.texture != nullptr;
    if (isVideo && !snapshotPath_.empty() && !snapshotWritten_ &&
        stats_.presented.load() + 1 >= snapshotAfter_) {
        writeSnapshot();   // before Present: FLIP_DISCARD leaves the buffer undefined after
    }

    const UINT interval = vsync_ ? 1 : 0;
    const UINT flags = (!vsync_ && tearingSupported_) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HRESULT hr = swapChain_->Present(interval, flags);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        setFatal(hrText("GPU device removed/reset", device_->GetDeviceRemovedReason()));
        return false;
    }
    if (isVideo) {
        const uint64_t now = nowUs();
        stats_.presented++;
        if (current_.completedUs && now >= current_.completedUs) {
            const uint64_t age = now - current_.completedUs;   // frame_age_at_present (client side)
            stats_.ageUsSum += age;
            stats_.ageSamples++;
            uint64_t prev = stats_.ageUsMax.load();
            while (age > prev && !stats_.ageUsMax.compare_exchange_weak(prev, age)) {}
        }
        current_.completedUs = 0;   // a redraw of the same frame is not a new sample
    }
    return true;
}

void D3DRenderer::writeSnapshot() {
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
    D3D11_TEXTURE2D_DESC d{};
    back->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device_->CreateTexture2D(&d, nullptr, &staging))) return;
    context_->CopyResource(staging.Get(), back.Get());   // diagnostics only, never the hot path
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return;

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, snapshotPath_.c_str(), L"wb") == 0 && fp) {
        BITMAPFILEHEADER fh{};
        BITMAPINFOHEADER ih{};
        ih.biSize = sizeof ih;
        ih.biWidth = LONG(d.Width);
        ih.biHeight = -LONG(d.Height);   // top-down
        ih.biPlanes = 1;
        ih.biBitCount = 32;
        ih.biCompression = BI_RGB;
        const DWORD imageBytes = d.Width * d.Height * 4;
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof fh + sizeof ih;
        fh.bfSize = fh.bfOffBits + imageBytes;
        fwrite(&fh, sizeof fh, 1, fp);
        fwrite(&ih, sizeof ih, 1, fp);
        const auto* row = static_cast<const uint8_t*>(m.pData);
        for (UINT y = 0; y < d.Height; ++y) fwrite(row + size_t(y) * m.RowPitch, 4, d.Width, fp);
        fclose(fp);
        snapshotWritten_ = true;
        printf("render: snapshot %ux%u written\n", d.Width, d.Height);
    }
    context_->Unmap(staging.Get(), 0);
}

} // namespace rdp
