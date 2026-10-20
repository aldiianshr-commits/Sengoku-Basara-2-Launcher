// Direct3D 12 presenter (Windows).
//
// The PS2 display image produced by the GS is uploaded into a texture and
// drawn into the swap chain back buffer by HLSL shaders compiled at start-up
// with D3DCompile (d3dcompiler_47.dll ships with Windows 10/11):
//   pass 1 (optional)  FXAA at source resolution into an intermediate target
//   pass 2             scaling filter (nearest / bilinear / sharp bilinear /
//                      Catmull-Rom bicubic / Lanczos-2), field blending,
//                      contrast adaptive sharpening, colour controls, scanlines
//   pass 3             Dear ImGui (launcher UI / in-game menu / OSD)
#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_sdl3.h"
#include "imgui.h"
#include "sb2/presenter.hpp"

namespace sb2 {

namespace {

template <typename T>
class Com {
public:
    Com() = default;
    ~Com() { reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    T** put() {
        reset();
        return &p_;
    }
    void** putVoid() { return reinterpret_cast<void**>(put()); }
    void reset() {
        if (p_) p_->Release();
        p_ = nullptr;
    }
    explicit operator bool() const { return p_ != nullptr; }

private:
    T* p_ = nullptr;
};

const char* kShaderSource = R"HLSL(
cbuffer Params : register(b0) {
    float4 src;   // width, height, 1/width, 1/height
    float4 dst;   // width, height, filter, sharpen
    float4 col;   // scanlines, brightness, contrast, gamma
    float4 col2;  // saturation, blend fields, uv scale x, uv scale y
};
Texture2D<float4> t0 : register(t0);
Texture2D<float4> t1 : register(t1);
SamplerState sPoint : register(s0);
SamplerState sLinear : register(s1);

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv * col2.zw;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float3 sampleBicubic(Texture2D<float4> tex, float2 uv) {
    float2 samplePos = uv * src.xy;
    float2 tp1 = floor(samplePos - 0.5) + 0.5;
    float2 f = samplePos - tp1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 tp0 = (tp1 - 1.0) * src.zw;
    float2 tp3 = (tp1 + 2.0) * src.zw;
    float2 tp12 = (tp1 + w2 / w12) * src.zw;
    float3 r = 0;
    r += tex.SampleLevel(sLinear, float2(tp0.x,  tp0.y), 0).rgb * w0.x  * w0.y;
    r += tex.SampleLevel(sLinear, float2(tp12.x, tp0.y), 0).rgb * w12.x * w0.y;
    r += tex.SampleLevel(sLinear, float2(tp3.x,  tp0.y), 0).rgb * w3.x  * w0.y;
    r += tex.SampleLevel(sLinear, float2(tp0.x,  tp12.y), 0).rgb * w0.x  * w12.y;
    r += tex.SampleLevel(sLinear, float2(tp12.x, tp12.y), 0).rgb * w12.x * w12.y;
    r += tex.SampleLevel(sLinear, float2(tp3.x,  tp12.y), 0).rgb * w3.x  * w12.y;
    r += tex.SampleLevel(sLinear, float2(tp0.x,  tp3.y), 0).rgb * w0.x  * w3.y;
    r += tex.SampleLevel(sLinear, float2(tp12.x, tp3.y), 0).rgb * w12.x * w3.y;
    r += tex.SampleLevel(sLinear, float2(tp3.x,  tp3.y), 0).rgb * w3.x  * w3.y;
    return saturate(r);
}

float lanczos2(float x) {
    x = abs(x);
    if (x < 1e-5) return 1.0;
    if (x >= 2.0) return 0.0;
    float px = 3.14159265 * x;
    return 2.0 * sin(px) * sin(px * 0.5) / (px * px);
}

float3 sampleLanczos(Texture2D<float4> tex, float2 uv) {
    float2 pos = uv * src.xy - 0.5;
    float2 base = floor(pos);
    float2 f = pos - base;
    float3 sum = 0;
    float wsum = 0;
    [unroll] for (int j = -1; j <= 2; ++j) {
        [unroll] for (int i = -1; i <= 2; ++i) {
            float w = lanczos2((float)i - f.x) * lanczos2((float)j - f.y);
            sum += tex.SampleLevel(sPoint, (base + float2(i, j) + 0.5) * src.zw, 0).rgb * w;
            wsum += w;
        }
    }
    return saturate(sum / wsum);
}

float3 sampleSharpBilinear(Texture2D<float4> tex, float2 uv) {
    float2 texel = uv * src.xy;
    float2 scale = max(floor(dst.xy / src.xy), 1.0);
    float2 tf = floor(texel);
    float2 s = frac(texel);
    float2 range = 0.5 - 0.5 / scale;
    float2 cd = s - 0.5;
    float2 f = (cd - clamp(cd, -range, range)) * scale + 0.5;
    return tex.SampleLevel(sLinear, (tf + f) * src.zw, 0).rgb;
}

float3 sampleFilter(Texture2D<float4> tex, float2 uv) {
    int mode = (int)dst.z;
    if (mode == 0) return tex.SampleLevel(sPoint, uv, 0).rgb;
    if (mode == 2) return sampleSharpBilinear(tex, uv);
    if (mode == 3) return sampleBicubic(tex, uv);
    if (mode == 4) return sampleLanczos(tex, uv);
    return tex.SampleLevel(sLinear, uv, 0).rgb;
}

float4 PSFinal(VSOut i) : SV_Target {
    float3 c = sampleFilter(t0, i.uv);
    if (col2.y > 0.5) c = 0.5 * (c + sampleFilter(t1, i.uv));
    if (dst.w > 0.0) {
        float2 d = src.zw;
        float3 n = t0.SampleLevel(sLinear, i.uv + float2(0, -d.y), 0).rgb;
        float3 s = t0.SampleLevel(sLinear, i.uv + float2(0,  d.y), 0).rgb;
        float3 e = t0.SampleLevel(sLinear, i.uv + float2( d.x, 0), 0).rgb;
        float3 w = t0.SampleLevel(sLinear, i.uv + float2(-d.x, 0), 0).rgb;
        float3 mn = min(c, min(min(n, s), min(e, w)));
        float3 mx = max(c, max(max(n, s), max(e, w)));
        float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));
        float3 wgt = amp * (-1.0 / lerp(8.0, 5.0, dst.w));
        c = saturate((c + (n + s + e + w) * wgt) / (1.0 + 4.0 * wgt));
    }
    c = c * col.y;
    c = (c - 0.5) * col.z + 0.5;
    float l = dot(c, float3(0.299, 0.587, 0.114));
    c = lerp(l.xxx, c, col2.x);
    c = pow(saturate(c), 1.0 / col.w);
    if (col.x > 0.0) {
        float s = frac(i.uv.y * src.y);
        float line = 0.5 + 0.5 * cos(6.2831853 * (s - 0.5));
        c *= lerp(1.0 - col.x, 1.0, line);
    }
    return float4(c, 1.0);
}

float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float4 PSFxaa(VSOut i) : SV_Target {
    float2 rcp = src.zw;
    float2 uv = i.uv;
    float3 nw = t0.SampleLevel(sLinear, uv + float2(-1, -1) * rcp, 0).rgb;
    float3 ne = t0.SampleLevel(sLinear, uv + float2( 1, -1) * rcp, 0).rgb;
    float3 sw = t0.SampleLevel(sLinear, uv + float2(-1,  1) * rcp, 0).rgb;
    float3 se = t0.SampleLevel(sLinear, uv + float2( 1,  1) * rcp, 0).rgb;
    float3 m  = t0.SampleLevel(sLinear, uv, 0).rgb;
    float lNW = luma(nw), lNE = luma(ne), lSW = luma(sw), lSE = luma(se), lM = luma(m);
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    float2 dir;
    dir.x = -((lNW + lNE) - (lSW + lSE));
    dir.y =  ((lNW + lSW) - (lNE + lSE));
    float dirReduce = max((lNW + lNE + lSW + lSE) * (0.25 * 0.125), 1.0 / 128.0);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, -8.0, 8.0) * rcp;
    float3 a = 0.5 * (t0.SampleLevel(sLinear, uv + dir * (1.0 / 3.0 - 0.5), 0).rgb +
                      t0.SampleLevel(sLinear, uv + dir * (2.0 / 3.0 - 0.5), 0).rgb);
    float3 b = a * 0.5 + 0.25 * (t0.SampleLevel(sLinear, uv - dir * 0.5, 0).rgb +
                                 t0.SampleLevel(sLinear, uv + dir * 0.5, 0).rgb);
    float lB = luma(b);
    return float4((lB < lMin || lB > lMax) ? a : b, 1.0);
}
)HLSL";

struct Params {
    float src[4];
    float dst[4];
    float col[4];
    float col2[4];
};

constexpr int kBackBuffers = 3;
constexpr int kFrames = 2;
constexpr int kSrvHeapSize = 256;
constexpr int kSrvTex0 = 0, kSrvTex1 = 1, kSrvInter = 2, kSrvFirstImgui = 8;
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

class D3D12Presenter;
D3D12Presenter* g_self = nullptr;

class D3D12Presenter final : public Presenter {
public:
    ~D3D12Presenter() override { shutdown(); }

    bool init(SDL_Window* window, const GraphicsSettings& g, std::string& err) {
        window_ = window;
        hwnd_ = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        if (!hwnd_) {
            err = "no HWND";
            return false;
        }
        const bool debug = std::getenv("SB2_D3D12_DEBUG") != nullptr;
        if (debug) {
            Com<ID3D12Debug> dbg;
            if (SUCCEEDED(D3D12GetDebugInterface(__uuidof(ID3D12Debug), dbg.putVoid()))) dbg->EnableDebugLayer();
        }
        if (FAILED(CreateDXGIFactory2(debug ? DXGI_CREATE_FACTORY_DEBUG : 0, __uuidof(IDXGIFactory4), factory_.putVoid()))) {
            err = "CreateDXGIFactory2 failed";
            return false;
        }
        if (!createDevice(err)) return false;
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device_->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), queue_.putVoid()))) {
            err = "CreateCommandQueue failed";
            return false;
        }
        // Tearing support (VSync off without blocking).
        {
            Com<IDXGIFactory5> f5;
            BOOL allow = FALSE;
            if (SUCCEEDED(factory_->QueryInterface(__uuidof(IDXGIFactory5), f5.putVoid())) &&
                SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof allow)))
                tearingSupported_ = allow == TRUE;
        }
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        width_ = std::max(1, w);
        height_ = std::max(1, h);
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = (UINT)width_;
        sd.Height = (UINT)height_;
        sd.Format = kFormat;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = kBackBuffers;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        swapFlags_ = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearingSupported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
        sd.Flags = swapFlags_;
        Com<IDXGISwapChain1> sc1;
        if (FAILED(factory_->CreateSwapChainForHwnd(queue_.get(), hwnd_, &sd, nullptr, nullptr, sc1.put()))) {
            err = "CreateSwapChainForHwnd failed";
            return false;
        }
        if (FAILED(sc1->QueryInterface(__uuidof(IDXGISwapChain3), swap_.putVoid()))) {
            err = "IDXGISwapChain3 unavailable";
            return false;
        }
        factory_->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
        setFrameLatency(g.maxFrameLatency);
        waitable_ = swap_->GetFrameLatencyWaitableObject();

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = kBackBuffers + 1;
        if (FAILED(device_->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), rtvHeap_.putVoid()))) {
            err = "RTV heap";
            return false;
        }
        rtvInc_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kSrvHeapSize;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device_->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), srvHeap_.putVoid()))) {
            err = "SRV heap";
            return false;
        }
        srvInc_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (int i = kSrvHeapSize - 1; i >= kSrvFirstImgui; --i) srvFree_.push_back(i);
        createBackBufferViews();

        for (int i = 0; i < kFrames; ++i) {
            if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                                       frames_[i].alloc.putVoid()))) {
                err = "command allocator";
                return false;
            }
        }
        if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_[0].alloc.get(), nullptr,
                                              __uuidof(ID3D12GraphicsCommandList), cmd_.putVoid()))) {
            err = "command list";
            return false;
        }
        cmd_->Close();
        if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), fence_.putVoid()))) {
            err = "fence";
            return false;
        }
        fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!createPipelines(err)) return false;

        vsync_ = g.vsync;
        allowTearing_ = g.allowTearing;

        ImGui_ImplSDL3_InitForD3D(window);
        sdlImguiInit_ = true;
        g_self = this;
        ImGui_ImplDX12_InitInfo ii;
        ii.Device = device_.get();
        ii.CommandQueue = queue_.get();
        ii.NumFramesInFlight = kFrames;
        ii.RTVFormat = kFormat;
        ii.DSVFormat = DXGI_FORMAT_UNKNOWN;
        ii.SrvDescriptorHeap = srvHeap_.get();
        ii.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* c, D3D12_GPU_DESCRIPTOR_HANDLE* gp) {
            g_self->allocSrv(c, gp);
        };
        ii.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE c, D3D12_GPU_DESCRIPTOR_HANDLE) {
            g_self->freeSrv(c);
        };
        if (!ImGui_ImplDX12_Init(&ii)) {
            err = "ImGui DX12 backend";
            return false;
        }
        imguiInit_ = true;
        return true;
    }

    const char* name() const override { return "Direct3D 12"; }
    std::string adapterName() const override { return adapterName_; }
    PresenterCaps caps() const override { return PresenterCaps{true, tearingSupported_}; }

    void setVsync(bool vsync, bool allowTearing) override {
        vsync_ = vsync;
        allowTearing_ = allowTearing;
    }

    void setFrameLatency(int frames) override {
        if (swap_) {
            Com<IDXGISwapChain2> sc2;
            if (SUCCEEDED(swap_->QueryInterface(__uuidof(IDXGISwapChain2), sc2.putVoid())))
                sc2->SetMaximumFrameLatency((UINT)std::clamp(frames, 1, 3));
        }
    }

    void uploadFrame(const uint32_t* rgba, int w, int h, bool interlaced) override {
        staged_.assign(rgba, rgba + (size_t)w * h);
        stagedW_ = w;
        stagedH_ = h;
        stagedInterlaced_ = interlaced;
        hasStaged_ = true;
    }

    void clearFrame() override {
        hasFrame_ = false;
        hasStaged_ = false;
    }

    void newImGuiFrame() override { ImGui_ImplDX12_NewFrame(); }

    void onResize(int w, int h) override {
        w = std::max(1, w);
        h = std::max(1, h);
        if (w == width_ && h == height_) return;
        waitIdle();
        for (auto& b : backBuffers_) b.reset();
        const HRESULT hr = swap_->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, swapFlags_);
        if (FAILED(hr)) std::fprintf(stderr, "d3d12: ResizeBuffers failed (%08lx)\n", (unsigned long)hr);
        width_ = w;
        height_ = h;
        createBackBufferViews();
    }

    void render(const RectF& dstIn, const PostParams& post, bool showFrame) override {
        if (deviceLost_) return;
        int ww = 0, wh = 0;
        SDL_GetWindowSizeInPixels(window_, &ww, &wh);
        if (ww > 0 && wh > 0 && (ww != width_ || wh != height_)) onResize(ww, wh);
        if (waitable_) WaitForSingleObjectEx(waitable_, 1000, TRUE);

        Frame& fr = frames_[frameIndex_];
        waitFence(fr.fenceValue);
        fr.alloc->Reset();
        cmd_->Reset(fr.alloc.get(), nullptr);

        if (hasStaged_) uploadStaged(fr);

        ID3D12DescriptorHeap* heaps[] = {srvHeap_.get()};
        cmd_->SetDescriptorHeaps(1, heaps);
        const bool drawFrame = showFrame && hasFrame_;
        int srcSlot = cur_ == 0 ? kSrvTex0 : kSrvTex1;
        const int prevSlot = cur_ == 0 ? kSrvTex1 : kSrvTex0;
        Params p{};
        p.src[0] = (float)frameW_;
        p.src[1] = (float)frameH_;
        p.src[2] = 1.0f / (float)std::max(1, frameW_);
        p.src[3] = 1.0f / (float)std::max(1, frameH_);
        p.dst[0] = dstIn.w;
        p.dst[1] = dstIn.h;
        p.dst[2] = (float)(int)post.filter;
        p.dst[3] = std::clamp(post.sharpen, 0.0f, 1.0f);
        p.col[0] = std::clamp(post.scanlines, 0.0f, 1.0f);
        p.col[1] = post.brightness;
        p.col[2] = post.contrast;
        p.col[3] = std::max(0.1f, post.gamma);
        p.col2[0] = post.saturation;
        p.col2[1] = (post.blendFields && frameInterlaced_ && prevValid_) ? 1.0f : 0.0f;
        p.col2[2] = 1.0f;
        p.col2[3] = 1.0f;

        if (drawFrame && post.fxaa) {
            ensureIntermediate();
            transition(inter_.get(), interState_, D3D12_RESOURCE_STATE_RENDER_TARGET);
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHandle(kBackBuffers);
            cmd_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            setViewport(0, 0, (float)frameW_, (float)frameH_);
            cmd_->SetGraphicsRootSignature(rootSig_.get());
            cmd_->SetPipelineState(psoFxaa_.get());
            cmd_->SetGraphicsRoot32BitConstants(0, sizeof(Params) / 4, &p, 0);
            cmd_->SetGraphicsRootDescriptorTable(1, srvGpu(srcSlot));
            cmd_->SetGraphicsRootDescriptorTable(2, srvGpu(prevSlot));
            cmd_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            cmd_->DrawInstanced(3, 1, 0, 0);
            transition(inter_.get(), interState_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            srcSlot = kSrvInter;
        }

        const UINT bb = swap_->GetCurrentBackBufferIndex();
        ID3D12Resource* back = backBuffers_[bb].get();
        D3D12_RESOURCE_STATES bbState = D3D12_RESOURCE_STATE_PRESENT;
        transition(back, bbState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHandle((int)bb);
        cmd_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float black[4] = {0, 0, 0, 1};
        cmd_->ClearRenderTargetView(rtv, black, 0, nullptr);
        if (drawFrame && dstIn.w > 0 && dstIn.h > 0) {
            setViewport(dstIn.x, dstIn.y, dstIn.w, dstIn.h);
            cmd_->SetGraphicsRootSignature(rootSig_.get());
            cmd_->SetPipelineState(psoFinal_.get());
            cmd_->SetGraphicsRoot32BitConstants(0, sizeof(Params) / 4, &p, 0);
            cmd_->SetGraphicsRootDescriptorTable(1, srvGpu(srcSlot));
            cmd_->SetGraphicsRootDescriptorTable(2, srvGpu(prevSlot));
            cmd_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            cmd_->DrawInstanced(3, 1, 0, 0);
        }
        setViewport(0, 0, (float)width_, (float)height_);
        if (ImDrawData* dd = ImGui::GetDrawData()) ImGui_ImplDX12_RenderDrawData(dd, cmd_.get());

        const bool shot = wantShot_;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        if (shot) {
            const D3D12_RESOURCE_DESC bd = back->GetDesc();
            UINT64 total = 0;
            device_->GetCopyableFootprints(&bd, 0, 1, 0, &fp, nullptr, nullptr, &total);
            ensureReadback(total);
            transition(back, bbState, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dstLoc{};
            dstLoc.pResource = readback_.get();
            dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dstLoc.PlacedFootprint = fp;
            D3D12_TEXTURE_COPY_LOCATION srcLoc{};
            srcLoc.pResource = back;
            srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            srcLoc.SubresourceIndex = 0;
            cmd_->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
        }
        transition(back, bbState, D3D12_RESOURCE_STATE_PRESENT);
        cmd_->Close();
        ID3D12CommandList* lists[] = {cmd_.get()};
        queue_->ExecuteCommandLists(1, lists);

        UINT interval = vsync_ ? 1 : 0;
        UINT flags = (!vsync_ && allowTearing_ && tearingSupported_) ? DXGI_PRESENT_ALLOW_TEARING : 0;
        const HRESULT hr = swap_->Present(interval, flags);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            std::fprintf(stderr, "d3d12: device lost (%08lx, reason %08lx)\n", (unsigned long)hr,
                         (unsigned long)device_->GetDeviceRemovedReason());
            deviceLost_ = true;
        }
        fr.fenceValue = ++fenceCounter_;
        queue_->Signal(fence_.get(), fr.fenceValue);
        frameIndex_ = (frameIndex_ + 1) % kFrames;

        if (shot) {
            waitFence(fenceCounter_);
            void* mapped = nullptr;
            D3D12_RANGE rr{0, (SIZE_T)(fp.Footprint.RowPitch * fp.Footprint.Height)};
            if (SUCCEEDED(readback_->Map(0, &rr, &mapped))) {
                shotW_ = (int)fp.Footprint.Width;
                shotH_ = (int)fp.Footprint.Height;
                shot_.resize((size_t)shotW_ * shotH_);
                for (int y = 0; y < shotH_; ++y) {
                    const uint32_t* row = (const uint32_t*)((const uint8_t*)mapped + (size_t)y * fp.Footprint.RowPitch);
                    for (int x = 0; x < shotW_; ++x) shot_[(size_t)y * shotW_ + x] = row[x] | 0xFF000000u;
                }
                D3D12_RANGE wr{0, 0};
                readback_->Unmap(0, &wr);
                shotReady_ = true;
            }
            wantShot_ = false;
        }
    }

    void requestScreenshot() override { wantShot_ = true; }
    bool takeScreenshot(std::vector<uint32_t>& px, int& w, int& h) override {
        if (!shotReady_) return false;
        px = shot_;
        w = shotW_;
        h = shotH_;
        shotReady_ = false;
        return true;
    }

    void allocSrv(D3D12_CPU_DESCRIPTOR_HANDLE* c, D3D12_GPU_DESCRIPTOR_HANDLE* g) {
        const int slot = srvFree_.empty() ? kSrvFirstImgui : srvFree_.back();
        if (!srvFree_.empty()) srvFree_.pop_back();
        *c = srvCpu(slot);
        *g = srvGpu(slot);
    }
    void freeSrv(D3D12_CPU_DESCRIPTOR_HANDLE c) {
        const int slot = (int)((c.ptr - srvHeap_->GetCPUDescriptorHandleForHeapStart().ptr) / srvInc_);
        srvFree_.push_back(slot);
    }

private:
    struct Frame {
        Com<ID3D12CommandAllocator> alloc;
        Com<ID3D12Resource> upload;
        UINT64 uploadSize = 0;
        uint8_t* uploadPtr = nullptr;
        UINT64 fenceValue = 0;
    };

    bool createDevice(std::string& err) {
        const bool forceWarp = std::getenv("SB2_D3D12_WARP") != nullptr;
        if (!forceWarp) {
            Com<IDXGIFactory6> f6;
            if (SUCCEEDED(factory_->QueryInterface(__uuidof(IDXGIFactory6), f6.putVoid()))) {
                for (UINT i = 0;; ++i) {
                    Com<IDXGIAdapter1> ad;
                    if (FAILED(f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1),
                                                              ad.putVoid())))
                        break;
                    if (tryAdapter(ad.get(), false)) return true;
                }
            }
            for (UINT i = 0;; ++i) {
                Com<IDXGIAdapter1> ad;
                if (factory_->EnumAdapters1(i, ad.put()) == DXGI_ERROR_NOT_FOUND) break;
                if (tryAdapter(ad.get(), false)) return true;
            }
        }
        Com<IDXGIAdapter1> warp;
        if (SUCCEEDED(factory_->EnumWarpAdapter(__uuidof(IDXGIAdapter1), warp.putVoid())) && tryAdapter(warp.get(), true))
            return true;
        err = "no Direct3D 12 capable adapter (feature level 11.0)";
        return false;
    }

    bool tryAdapter(IDXGIAdapter1* ad, bool allowSoftware) {
        DXGI_ADAPTER_DESC1 d{};
        ad->GetDesc1(&d);
        if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && !allowSoftware) return false;
        if (FAILED(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), device_.putVoid()))) return false;
        char name[256];
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, nullptr, nullptr);
        adapterName_ = name;
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) adapterName_ += " [software]";
        return true;
    }

    bool compile(const char* entry, const char* target, Com<ID3DBlob>& out, std::string& err) {
        Com<ID3DBlob> errors;
        const HRESULT hr = D3DCompile(kShaderSource, std::strlen(kShaderSource), "sb2_post.hlsl", nullptr, nullptr, entry, target,
                                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out.put(), errors.put());
        if (FAILED(hr)) {
            err = std::string("shader ") + entry + ": " +
                  (errors ? std::string((const char*)errors->GetBufferPointer(), errors->GetBufferSize()) : "D3DCompile failed");
            return false;
        }
        return true;
    }

    bool createPipelines(std::string& err) {
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        for (int i = 0; i < 2; ++i) {
            ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            ranges[i].NumDescriptors = 1;
            ranges[i].BaseShaderRegister = (UINT)i;
            ranges[i].OffsetInDescriptorsFromTableStart = 0;
        }
        D3D12_ROOT_PARAMETER rp[3]{};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[0].Constants.ShaderRegister = 0;
        rp[0].Constants.Num32BitValues = sizeof(Params) / 4;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        for (int i = 0; i < 2; ++i) {
            rp[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rp[1 + i].DescriptorTable.NumDescriptorRanges = 1;
            rp[1 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
            rp[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        }
        D3D12_STATIC_SAMPLER_DESC ss[2]{};
        for (int i = 0; i < 2; ++i) {
            ss[i].Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            ss[i].AddressU = ss[i].AddressV = ss[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            ss[i].MaxLOD = D3D12_FLOAT32_MAX;
            ss[i].ShaderRegister = (UINT)i;
            ss[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        }
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 3;
        rsd.pParameters = rp;
        rsd.NumStaticSamplers = 2;
        rsd.pStaticSamplers = ss;
        Com<ID3DBlob> blob, errors;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), errors.put())) ||
            FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature),
                                                rootSig_.putVoid()))) {
            err = "root signature";
            return false;
        }
        Com<ID3DBlob> vs, psFinal, psFxaa;
        if (!compile("VSMain", "vs_5_0", vs, err) || !compile("PSFinal", "ps_5_0", psFinal, err) ||
            !compile("PSFxaa", "ps_5_0", psFxaa, err))
            return false;
        auto makePso = [&](ID3DBlob* ps, Com<ID3D12PipelineState>& out) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = rootSig_.get();
            pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
            pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
            pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            pd.SampleMask = UINT_MAX;
            pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pd.RasterizerState.DepthClipEnable = TRUE;
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.NumRenderTargets = 1;
            pd.RTVFormats[0] = kFormat;
            pd.SampleDesc.Count = 1;
            return SUCCEEDED(device_->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), out.putVoid()));
        };
        if (!makePso(psFinal.get(), psoFinal_) || !makePso(psFxaa.get(), psoFxaa_)) {
            err = "pipeline state";
            return false;
        }
        return true;
    }

    void createBackBufferViews() {
        for (int i = 0; i < kBackBuffers; ++i) {
            swap_->GetBuffer((UINT)i, __uuidof(ID3D12Resource), backBuffers_[i].putVoid());
            device_->CreateRenderTargetView(backBuffers_[i].get(), nullptr, rtvHandle(i));
        }
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle(int i) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)i * rtvInc_;
        return h;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu(int i) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)i * srvInc_;
        return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu(int i) const {
        D3D12_GPU_DESCRIPTOR_HANDLE h = srvHeap_->GetGPUDescriptorHandleForHeapStart();
        h.ptr += (UINT64)i * srvInc_;
        return h;
    }

    void setViewport(float x, float y, float w, float h) {
        D3D12_VIEWPORT vp{x, y, w, h, 0.0f, 1.0f};
        D3D12_RECT sc{(LONG)std::max(0.0f, x), (LONG)std::max(0.0f, y), (LONG)std::min((float)width_, x + w),
                      (LONG)std::min((float)height_, y + h)};
        if (sc.right <= sc.left || sc.bottom <= sc.top) sc = {0, 0, 1, 1};
        cmd_->RSSetViewports(1, &vp);
        cmd_->RSSetScissorRects(1, &sc);
    }

    void transition(ID3D12Resource* r, D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES to) {
        if (state == to) return;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = state;
        b.Transition.StateAfter = to;
        cmd_->ResourceBarrier(1, &b);
        state = to;
    }

    bool createTexture(int w, int h, bool renderTarget, Com<ID3D12Resource>& out) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = (UINT64)w;
        rd.Height = (UINT)h;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = kFormat;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags = renderTarget ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
        return SUCCEEDED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                          __uuidof(ID3D12Resource), out.putVoid()));
    }

    void createSrv(ID3D12Resource* r, int slot) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = kFormat;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(r, &sd, srvCpu(slot));
    }

    void uploadStaged(Frame& fr) {
        hasStaged_ = false;
        const int w = stagedW_, h = stagedH_;
        if (w <= 0 || h <= 0) return;
        if (w != frameW_ || h != frameH_ || !tex_[0]) {
            // The GPU may still sample the old textures from a frame in flight.
            cmd_->Close();
            waitIdle();
            Frame& f = frames_[frameIndex_];
            f.alloc->Reset();
            cmd_->Reset(f.alloc.get(), nullptr);
            for (int i = 0; i < 2; ++i) {
                createTexture(w, h, false, tex_[i]);
                texState_[i] = D3D12_RESOURCE_STATE_COMMON;
                createSrv(tex_[i].get(), i == 0 ? kSrvTex0 : kSrvTex1);
            }
            frameW_ = w;
            frameH_ = h;
            prevValid_ = false;
            inter_.reset();
        } else {
            prevValid_ = hasFrame_;
            cur_ ^= 1;
        }
        const UINT pitch = (UINT)((w * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1));
        const UINT64 need = (UINT64)pitch * h;
        if (fr.uploadSize < need) {
            fr.upload.reset();
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = need;
            rd.Height = 1;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        __uuidof(ID3D12Resource), fr.upload.putVoid())))
                return;
            D3D12_RANGE none{0, 0};
            fr.upload->Map(0, &none, (void**)&fr.uploadPtr);
            fr.uploadSize = need;
        }
        for (int y = 0; y < h; ++y) std::memcpy(fr.uploadPtr + (size_t)y * pitch, &staged_[(size_t)y * w], (size_t)w * 4);
        ID3D12Resource* dst = tex_[cur_].get();
        transition(dst, texState_[cur_], D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION d{};
        d.pResource = dst;
        d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION s{};
        s.pResource = fr.upload.get();
        s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        s.PlacedFootprint.Footprint.Format = kFormat;
        s.PlacedFootprint.Footprint.Width = (UINT)w;
        s.PlacedFootprint.Footprint.Height = (UINT)h;
        s.PlacedFootprint.Footprint.Depth = 1;
        s.PlacedFootprint.Footprint.RowPitch = pitch;
        cmd_->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        transition(dst, texState_[cur_], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        // The other texture keeps the previous field for blending.
        const int other = cur_ ^ 1;
        if (texState_[other] == D3D12_RESOURCE_STATE_COMMON)
            transition(tex_[other].get(), texState_[other], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        frameInterlaced_ = stagedInterlaced_;
        hasFrame_ = true;
    }

    void ensureIntermediate() {
        if (inter_) return;
        createTexture(frameW_, frameH_, true, inter_);
        interState_ = D3D12_RESOURCE_STATE_COMMON;
        device_->CreateRenderTargetView(inter_.get(), nullptr, rtvHandle(kBackBuffers));
        createSrv(inter_.get(), kSrvInter);
    }

    void ensureReadback(UINT64 size) {
        if (readbackSize_ >= size) return;
        readback_.reset();
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = size;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (SUCCEEDED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       __uuidof(ID3D12Resource), readback_.putVoid())))
            readbackSize_ = size;
    }

    void waitFence(UINT64 v) {
        if (v == 0 || fence_->GetCompletedValue() >= v) return;
        fence_->SetEventOnCompletion(v, fenceEvent_);
        WaitForSingleObject(fenceEvent_, 5000);
    }

    void waitIdle() {
        if (!queue_ || !fence_) return;
        queue_->Signal(fence_.get(), ++fenceCounter_);
        waitFence(fenceCounter_);
    }

    void shutdown() {
        waitIdle();
        if (imguiInit_) ImGui_ImplDX12_Shutdown();
        if (sdlImguiInit_) ImGui_ImplSDL3_Shutdown();
        imguiInit_ = sdlImguiInit_ = false;
        for (auto& f : frames_) {
            if (f.upload && f.uploadPtr) f.upload->Unmap(0, nullptr);
        }
        if (fenceEvent_) CloseHandle(fenceEvent_);
        fenceEvent_ = nullptr;
        if (waitable_) CloseHandle(waitable_);
        waitable_ = nullptr;
        if (g_self == this) g_self = nullptr;
    }

    SDL_Window* window_ = nullptr;
    HWND hwnd_ = nullptr;
    Com<IDXGIFactory4> factory_;
    Com<ID3D12Device> device_;
    Com<ID3D12CommandQueue> queue_;
    Com<IDXGISwapChain3> swap_;
    Com<ID3D12DescriptorHeap> rtvHeap_, srvHeap_;
    UINT rtvInc_ = 0, srvInc_ = 0;
    std::vector<int> srvFree_;
    Com<ID3D12Resource> backBuffers_[kBackBuffers];
    Frame frames_[kFrames];
    Com<ID3D12GraphicsCommandList> cmd_;
    Com<ID3D12Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    HANDLE waitable_ = nullptr;
    UINT64 fenceCounter_ = 0;
    int frameIndex_ = 0;
    Com<ID3D12RootSignature> rootSig_;
    Com<ID3D12PipelineState> psoFinal_, psoFxaa_;
    Com<ID3D12Resource> tex_[2];
    D3D12_RESOURCE_STATES texState_[2] = {D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON};
    Com<ID3D12Resource> inter_;
    D3D12_RESOURCE_STATES interState_ = D3D12_RESOURCE_STATE_COMMON;
    Com<ID3D12Resource> readback_;
    UINT64 readbackSize_ = 0;
    int cur_ = 0;
    int frameW_ = 0, frameH_ = 0;
    bool frameInterlaced_ = false, prevValid_ = false, hasFrame_ = false;
    std::vector<uint32_t> staged_;
    int stagedW_ = 0, stagedH_ = 0;
    bool stagedInterlaced_ = false, hasStaged_ = false;
    int width_ = 0, height_ = 0;
    UINT swapFlags_ = 0;
    bool tearingSupported_ = false;
    bool vsync_ = true, allowTearing_ = true;
    bool imguiInit_ = false, sdlImguiInit_ = false;
    bool deviceLost_ = false;
    bool wantShot_ = false, shotReady_ = false;
    std::vector<uint32_t> shot_;
    int shotW_ = 0, shotH_ = 0;
    std::string adapterName_;
};

} // namespace

std::unique_ptr<Presenter> createD3D12Presenter(SDL_Window* window, const GraphicsSettings& g, std::string& err) {
    auto p = std::make_unique<D3D12Presenter>();
    if (!p->init(window, g, err)) return nullptr;
    return p;
}

} // namespace sb2

#endif // _WIN32
