// Live window for Refract frames. Receives on TCP 38491 (adb reverse), like
// scripts/live_view.py, but on the GPU:
//  * GPU frames (AXRI v3/v5/v6/v7): opens the D3D11 textures the Refract Vulkan
//    layer shares from the emulator, copies them, and acknowledges the frame.
//    No pixels cross the adb pipe.
//  * Pixel frames (v1/v2/v4): uploads the RGBA payload (fallback path).
//  * Video frames (v8/v9): H.264 from a real device's hardware encoder, decoded with
//    Media Foundation and converted NV12 -> RGB on the GPU into the same eye textures.
// Mouse/keys match live_view.py: right-drag look, left button = right trigger,
// middle button = right grip, wheel = hand distance, Home recenter, F2 screenshot, F3 both eyes,
// F5 flip image, F6 scene/panels, F7 hide composited panels, F11 fullscreen, F1 performance overlay. Mouse state goes to pose_input_server.py over UDP.
#include "android_stats.h"
#include "h264_decoder.h"
#include "image_transport.h"
#include "perf_stats.h"
#include "windows_gpu_frame.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <d2d1_1.h>
#include <d3d11_4.h>
#include <dwrite.h>
#include <d3dcompiler.h>
#include <dxgi1_3.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
namespace proto = refract::protocol;

namespace {

constexpr wchar_t kTitle[] = L"Refract Viewer";  // pose_input_server.py matches "Refract Viewer" for keyboard focus.

// A composite frame's layout (AXRI v10): the slot's slices are its two atlas textures.
struct Composite {
    proto::ImageProjection projection{};  // Both eye views.
    proto::CompositeHeader header{};
    std::vector<proto::CompositeQuad> quads;  // Back to front.
};

// One displayable image: a 2-slice array (eyes, or two UI panels).
struct Slot {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    std::array<ComPtr<ID3D11RenderTargetView>, 2> targets;  // Per eye; video frames are drawn into them.
    UINT width = 0, height = 0, layers = 0;
    DXGI_FORMAT typeless = DXGI_FORMAT_UNKNOWN;
    uint64_t sequence = 0;
    bool flip = false, valid = false;
    Clock::time_point at{};
    std::shared_ptr<const Composite> composite;  // Set: draw the scene region and panels (see draw_composite).
};

struct Shared {
    std::mutex mutex;
    Slot scene, panels;
    uint64_t updates = 0;            // Bumped for every new image.
    std::atomic<uint32_t> sceneFrames{0}, allFrames{0};
    std::atomic<bool> connected{false}, gpu{false}, video{false};
    std::atomic<bool> wantPanels{false};
    // Scene frame timing for the overlay (guarded by mutex).
    std::array<float, 240> intervals{};  // Milliseconds between scene frames, ring buffer.
    size_t intervalCount = 0, intervalNext = 0;
    Clock::time_point lastScene{};
    float copyMs = 0;  // Smoothed time to copy the shared eye textures and wait for the GPU.
    HANDLE newFrame = CreateEventW(nullptr, FALSE, FALSE, nullptr);
};

ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_context;
// Shared GPU frames are copied on a device of their own. A separate thread
// acknowledges each frame after its copy finishes, while the receiver reads on.
// Its GPU queue never waits behind the window's draws and presents; on g_context every
// acknowledgment, and so the game, waited up to a screen refresh (the game locked at ~64 fps).
ComPtr<ID3D11Device> g_copyDevice;
ComPtr<ID3D11DeviceContext> g_copyContext;
// Each copy signals the next fence value, so the ACK thread can sleep until its copy is done
// instead of spinning on a query. Null fence: event-query polling fallback.
ComPtr<ID3D11DeviceContext4> g_copyContext4;
ComPtr<ID3D11Fence> g_copyFence;
HANDLE g_copyFenceEvent = nullptr;
uint64_t g_copyFenceValue = 0;  // Last value signalled; receive thread only.
Shared g_shared;

// Receive thread -> window thread: copies of a shared eye pair (eye 0 left half, eye 1 right half).
// Each texture exists on both devices behind a keyed mutex (key 0). The window thread copies only
// `latest` (inside render, under g_shared.mutex) and the receive thread writes the others in turn.
struct Handoff {
    struct Entry {
        ComPtr<ID3D11Texture2D> copy, draw;  // The same texture on g_copyDevice and g_device.
        ComPtr<IDXGIKeyedMutex> copyLock, drawLock;
    };
    std::array<Entry, 4> entries;
    UINT width = 0, height = 0;  // Per eye.
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    int latest = -1;       // Written by the ACK thread under g_shared.mutex.
    int submitted = -1;    // Next copy entry, including copies still running on the GPU.
    bool pending = false;  // `latest` is newer than its slot (guarded by g_shared.mutex).
};
Handoff g_handoff[2];  // Scene, panels.

bool ensure_handoff(Handoff& handoff, UINT width, UINT height, DXGI_FORMAT format)
{
    if (handoff.entries[0].copy && handoff.width == width && handoff.height == height && handoff.format == format) return true;
    std::lock_guard lock(g_shared.mutex);
    handoff = {};
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width * 2; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = format; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE; desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    for (auto& entry : handoff.entries) {
        ComPtr<IDXGIResource> resource;
        HANDLE handle = nullptr;
        if (FAILED(g_copyDevice->CreateTexture2D(&desc, nullptr, &entry.copy)) || FAILED(entry.copy.As(&resource)) ||
            FAILED(resource->GetSharedHandle(&handle)) || FAILED(g_device->OpenSharedResource(handle, IID_PPV_ARGS(&entry.draw))) ||
            FAILED(entry.copy.As(&entry.copyLock)) || FAILED(entry.draw.As(&entry.drawLock))) {
            std::fprintf(stderr, "viewer: cannot create hand-off textures %ux%u format %d\n", width * 2, height, format);
            handoff = {};
            return false;
        }
    }
    handoff.width = width; handoff.height = height; handoff.format = format;
    return true;
}

bool rgba_group(DXGI_FORMAT f) { return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_TYPELESS; }
bool bgra_group(DXGI_FORMAT f) { return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_TYPELESS; }

// Views are UNORM even for sRGB sources: the bytes are shown as-is, like the pixel stream.
bool ensure_slot(Slot& slot, UINT width, UINT height, DXGI_FORMAT typeless)
{
    if (slot.texture && slot.width == width && slot.height == height && slot.typeless == typeless) return true;
    slot = {};
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 2;
    desc.Format = typeless; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &slot.texture))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = typeless == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    view.Texture2DArray.MipLevels = 1; view.Texture2DArray.ArraySize = 2;
    if (FAILED(g_device->CreateShaderResourceView(slot.texture.Get(), &view, &slot.view))) { slot = {}; return false; }
    slot.width = width; slot.height = height; slot.typeless = typeless;
    return true;
}

// The guest may overwrite its shared textures once acknowledged, so the copy must be finished.
bool wait_copy(ID3D11Query* query, uint64_t fenceValue)
{
    static proto::PerfStats stats("viewer-copy-complete");
    proto::PerfScope scope(stats);
    // Under the runtime's 3 s acknowledgment timeout, but past ordinary hitches (a failed copy drops the stream).
    const auto deadline = Clock::now() + std::chrono::milliseconds(2500);
    if (!query) {
        // The auto-reset event can carry a stale wakeup from an earlier timed-out wait,
        // so the fence value decides, not the wakeup.
        for (;;) {
            const uint64_t completed = g_copyFence->GetCompletedValue();
            if (completed == UINT64_MAX) return false;  // Device removed.
            if (completed >= fenceValue) return true;
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (left <= 0 || FAILED(g_copyFence->SetEventOnCompletion(fenceValue, g_copyFenceEvent))) return false;
            WaitForSingleObject(g_copyFenceEvent, static_cast<DWORD>(left));
        }
    }
    HRESULT result;
    while ((result = g_copyContext->GetData(query, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE) {
        if (Clock::now() > deadline) return false;
        SwitchToThread();
    }
    return SUCCEEDED(result);
}

// NV12 (BT.601 limited range) -> RGB for decoded video, one draw per eye into the slot.
const char kYuvShader[] = R"(
Texture2D lumaTexture : register(t0);
Texture2D<float2> chromaTexture : register(t1);
SamplerState linearSampler : register(s0);
cbuffer Params : register(b0) { float eye; float scaleX; float scaleY; float unused; };
struct VsOut { float4 position : SV_Position; float2 uv : TEXCOORD0; };
float4 ps(VsOut i) : SV_Target {
    // Both eyes sit side by side in the (aligned) picture.
    float2 source = float2((eye + i.uv.x) * scaleX, i.uv.y * scaleY);
    float y = (lumaTexture.Sample(linearSampler, source).r - 16.0 / 255.0) * (255.0 / 219.0);
    float2 c = (chromaTexture.Sample(linearSampler, source) - 128.0 / 255.0) * (255.0 / 224.0);
    return float4(saturate(float3(y + 1.402 * c.y, y - 0.344136 * c.x - 0.714136 * c.y, y + 1.772 * c.x)), 1);
}
)";

struct Yuv {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> params;
    ComPtr<ID3D11Texture2D> luma, chroma;
    ComPtr<ID3D11ShaderResourceView> lumaView, chromaView;
    UINT width = 0, height = 0;
};
Yuv g_yuv;

// Called with g_shared.mutex held.
bool convert_nv12(Slot& slot, const H264Decoder::Picture& picture, UINT eyeWidth, UINT eyeHeight)
{
    if (!g_yuv.ps || picture.width < eyeWidth * 2 || picture.height < eyeHeight) return false;
    if (!g_yuv.luma || g_yuv.width != picture.width || g_yuv.height != picture.height) {
        g_yuv.luma.Reset(); g_yuv.chroma.Reset(); g_yuv.lumaView.Reset(); g_yuv.chromaView.Reset();
        D3D11_TEXTURE2D_DESC desc{};
        desc.MipLevels = 1; desc.ArraySize = 1; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.Width = picture.width; desc.Height = picture.height; desc.Format = DXGI_FORMAT_R8_UNORM;
        if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &g_yuv.luma))) return false;
        desc.Width = picture.width / 2; desc.Height = picture.height / 2; desc.Format = DXGI_FORMAT_R8G8_UNORM;
        if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &g_yuv.chroma)) ||
            FAILED(g_device->CreateShaderResourceView(g_yuv.luma.Get(), nullptr, &g_yuv.lumaView)) ||
            FAILED(g_device->CreateShaderResourceView(g_yuv.chroma.Get(), nullptr, &g_yuv.chromaView))) { g_yuv.luma.Reset(); return false; }
        g_yuv.width = picture.width; g_yuv.height = picture.height;
    }
    for (UINT eye = 0; eye < 2; ++eye) {
        if (slot.targets[eye]) continue;
        D3D11_RENDER_TARGET_VIEW_DESC target{};
        target.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        target.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        target.Texture2DArray.FirstArraySlice = eye; target.Texture2DArray.ArraySize = 1;
        if (FAILED(g_device->CreateRenderTargetView(slot.texture.Get(), &target, &slot.targets[eye]))) return false;
    }
    g_context->UpdateSubresource(g_yuv.luma.Get(), 0, nullptr, picture.y, picture.pitch, 0);
    g_context->UpdateSubresource(g_yuv.chroma.Get(), 0, nullptr, picture.uv, picture.pitch, 0);
    ID3D11ShaderResourceView* views[] = {g_yuv.lumaView.Get(), g_yuv.chromaView.Get()};
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->IASetInputLayout(nullptr);
    g_context->VSSetShader(g_yuv.vs.Get(), nullptr, 0);
    g_context->PSSetShader(g_yuv.ps.Get(), nullptr, 0);
    g_context->PSSetShaderResources(0, 2, views);
    g_context->PSSetSamplers(0, 1, g_yuv.sampler.GetAddressOf());
    g_context->PSSetConstantBuffers(0, 1, g_yuv.params.GetAddressOf());
    const D3D11_VIEWPORT viewport{0, 0, float(eyeWidth), float(eyeHeight), 0, 1};
    g_context->RSSetViewports(1, &viewport);
    for (UINT eye = 0; eye < 2; ++eye) {
        const float params[4] = {float(eye), float(eyeWidth) / picture.width, float(eyeHeight) / picture.height, 0};
        g_context->UpdateSubresource(g_yuv.params.Get(), 0, nullptr, params, 0, 0);
        g_context->OMSetRenderTargets(1, slot.targets[eye].GetAddressOf(), nullptr);
        g_context->Draw(3, 0);
    }
    ID3D11ShaderResourceView* none[2] = {};
    g_context->PSSetShaderResources(0, 2, none);
    g_context->OMSetRenderTargets(0, nullptr, nullptr);
    return true;
}

// Called with g_shared.mutex held once a slot holds a new image.
void publish(Slot& slot, uint64_t sequence, bool quads, bool flip, Clock::time_point copyStart,
             std::shared_ptr<const Composite> composite = nullptr)
{
    slot.composite = std::move(composite);
    slot.flip = flip;
    slot.sequence = sequence;
    slot.valid = true;
    slot.at = Clock::now();
    if (!quads) {
        const float copyMs = std::chrono::duration<float, std::milli>(slot.at - copyStart).count();
        g_shared.copyMs = g_shared.copyMs ? g_shared.copyMs * 0.9f + copyMs * 0.1f : copyMs;
        if (g_shared.lastScene != Clock::time_point{}) {
            g_shared.intervals[g_shared.intervalNext] = std::chrono::duration<float, std::milli>(slot.at - g_shared.lastScene).count();
            g_shared.intervalNext = (g_shared.intervalNext + 1) % g_shared.intervals.size();
            g_shared.intervalCount = std::min(g_shared.intervalCount + 1, g_shared.intervals.size());
        }
        g_shared.lastScene = slot.at;
    }
    ++g_shared.updates;
    SetEvent(g_shared.newFrame);
}

class Receiver {
public:
    bool ack_ready(uint64_t sequence)
    {
        PendingCopy copy;
        {
            std::lock_guard lock(copyMutex_);
            const auto found = pendingCopies_.find(sequence);
            if (found == pendingCopies_.end()) return true;  // Panel intentionally skipped.
            copy = found->second;
        }
        const bool completed = wait_copy(copy.query.Get(), copy.fenceValue);
        if (!completed) std::fprintf(stderr, "viewer: GPU copy of frame seq=%llu did not complete within 2.5 s\n",
                                     static_cast<unsigned long long>(sequence));
        if (completed) {
            std::lock_guard lock(g_shared.mutex);
            Slot& slot = copy.quads ? g_shared.panels : g_shared.scene;
            if (ensure_slot(slot, copy.width, copy.height, copy.typeless)) {
                auto& handoff = g_handoff[copy.quads ? 1 : 0];
                handoff.latest = copy.index;
                handoff.pending = true;
                slot.layers = 2;
                publish(slot, sequence, copy.quads, false, copy.started, copy.composite);
            }
        }
        {
            std::lock_guard lock(copyMutex_);
            pendingCopies_.erase(sequence);
        }
        copyDone_.notify_all();
        return completed;
    }

    bool on_frame(const proto::ImageFrameHeader& header, const proto::ImageProjection& projection, std::vector<uint8_t>&& payload)
    {
        g_shared.connected = true;
        const bool mixed = proto::mixed_gpu_version(header.version);
        const bool quads = header.version == proto::kQuadImageFrameVersion || header.version == proto::kQuadGpuFrameVersion ||
            header.version == proto::kMixedQuadGpuFrameVersion || header.version == proto::kQuadVideoImageFrameVersion;
        const bool composite = header.version == proto::kCompositeGpuFrameVersion;
        const bool gpu = header.version == proto::kWindowsGpuFrameVersion || header.version == proto::kQuadGpuFrameVersion || mixed || composite;
        const bool video = proto::video_version(header.version);
        g_shared.gpu = gpu;
        g_shared.video = video;
        ++g_shared.allFrames;
        if (!quads) ++g_shared.sceneFrames;

        if (video) {
            // Every access unit is decoded (later frames depend on it), even when not displayed.
            // Rows arrive in pixel-stream order: scene eyes bottom-up, panels top-down.
            const auto start = Clock::now();
            decoder_.decode(payload.data(), payload.size(), int64_t(header.sequence), (header.reserved & proto::kVideoFrameKey) != 0,
                header.width * 2, header.height,
                [&](const H264Decoder::Picture& picture) {
                    std::lock_guard lock(g_shared.mutex);
                    Slot& slot = quads ? g_shared.panels : g_shared.scene;
                    if (!ensure_slot(slot, header.width, header.height, DXGI_FORMAT_R8G8B8A8_TYPELESS) ||
                        !convert_nv12(slot, picture, header.width, header.height)) return;
                    g_handoff[quads ? 1 : 0].pending = false;  // Replaces any shared image not yet shown.
                    slot.layers = 2;
                    publish(slot, header.sequence, quads, !quads, start);
                });
            return true;
        }

        if (mixed && (header.reserved & 0xffff) == 0) batchHasScene_ = !quads;
        if (quads) {
            // Only one panel is shown: the first quad of a batch (after the scene, if the batch has one).
            if (mixed && (header.reserved & 0xffff) != (batchHasScene_ ? 1u : 0u)) return true;
            std::lock_guard lock(g_shared.mutex);
            const bool sceneStale = !g_shared.scene.valid || Clock::now() - g_shared.scene.at > std::chrono::seconds(1);
            if (!g_shared.wantPanels && !sceneStale) return true;  // Not displayed: nothing to read.
        }

        const auto copyStart = Clock::now();
        if (gpu) {
            static proto::PerfStats receiveStats("viewer-gpu-receive");
            proto::PerfScope receiveScope(receiveStats);
            // Copied on the receive device into the next hand-off texture; render() takes it from there.
            proto::WindowsGpuFrame frame{};
            std::memcpy(&frame, payload.data(), sizeof(frame));
            std::shared_ptr<Composite> layout;
            if (composite) {  // The transport validated the table.
                layout = std::make_shared<Composite>();
                layout->projection = projection;
                std::memcpy(&layout->header, payload.data() + sizeof(frame), sizeof(layout->header));
                layout->quads.resize(layout->header.quad_count);
                if (!layout->quads.empty())
                    std::memcpy(layout->quads.data(), payload.data() + sizeof(frame) + sizeof(layout->header),
                                layout->quads.size() * sizeof(proto::CompositeQuad));
                static uint32_t described = 0;
                if (described++ % 600 == 0) {  // The layout, now and then, for debugging panel placement.
                    const auto& v = projection.views[0];
                    std::fprintf(stderr, "viewer: composite scene %ux%u, left eye at (%.2f %.2f %.2f) q(%.2f %.2f %.2f %.2f) fov(%.2f %.2f %.2f %.2f)\n",
                        layout->header.scene_width, layout->header.scene_height, v.pose.x, v.pose.y, v.pose.z,
                        v.pose.qx, v.pose.qy, v.pose.qz, v.pose.qw, v.angle_left, v.angle_right, v.angle_up, v.angle_down);
                    for (const auto& q : layout->quads)
                        std::fprintf(stderr, "viewer:   panel %.2fx%.2f m at (%.2f %.2f %.2f) q(%.2f %.2f %.2f %.2f) eyes=%u flags=%u atlas %u:%u,%u %ux%u\n",
                            q.quad.width, q.quad.height, q.quad.pose.x, q.quad.pose.y, q.quad.pose.z, q.quad.pose.qx, q.quad.pose.qy,
                            q.quad.pose.qz, q.quad.pose.qw, q.quad.eye_visibility, q.quad.layer_flags, q.texture, q.x, q.y, q.width, q.height);
                }
            }
            auto* textures = open_shared(frame.session);
            if (!textures) return reject(header, "shared textures unavailable");
            D3D11_TEXTURE2D_DESC source{};
            (*textures)[0]->GetDesc(&source);
            if (source.Width != header.width || source.Height != header.height)
                return reject(header, "shared texture is %ux%u", source.Width, source.Height);
            const DXGI_FORMAT typeless = bgra_group(source.Format) ? DXGI_FORMAT_B8G8R8A8_TYPELESS :
                rgba_group(source.Format) ? DXGI_FORMAT_R8G8B8A8_TYPELESS : DXGI_FORMAT_UNKNOWN;
            auto& handoff = g_handoff[quads ? 1 : 0];
            if (handoff.entries[0].copy &&
                (handoff.width != header.width || handoff.height != header.height || handoff.format != source.Format)) {
                std::unique_lock lock(copyMutex_);
                copyDone_.wait(lock, [&] { return pendingCopies_.empty(); });
            }
            if (typeless == DXGI_FORMAT_UNKNOWN || !ensure_handoff(handoff, header.width, header.height, source.Format))
                return reject(header, "unsupported shared format %d", int(source.Format));
            // The receive thread can submit another copy while an earlier one completes.
            const int index = (handoff.submitted + 1) % static_cast<int>(handoff.entries.size());
            auto& entry = handoff.entries[index];
            if (entry.copyLock->AcquireSync(0, 1000) != S_OK) return reject(header, "hand-off texture busy");
            for (UINT eye = 0; eye < 2; ++eye)
                g_copyContext->CopySubresourceRegion(entry.copy.Get(), 0, eye * header.width, 0, 0, (*textures)[eye].Get(), 0, nullptr);
            entry.copyLock->ReleaseSync(0);
            PendingCopy pending{nullptr, 0, quads, index, header.width, header.height, typeless, copyStart, std::move(layout)};
            if (g_copyFence) {
                pending.fenceValue = ++g_copyFenceValue;
                if (FAILED(g_copyContext4->Signal(g_copyFence.Get(), pending.fenceValue))) return false;
            } else {
                D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
                if (FAILED(g_copyDevice->CreateQuery(&desc, &pending.query))) return false;
                g_copyContext->End(pending.query.Get());
            }
            g_copyContext->Flush();
            handoff.submitted = index;
            {
                std::lock_guard lock(copyMutex_);
                pendingCopies_.emplace(header.sequence, std::move(pending));
            }
            return true;
        }

        std::lock_guard lock(g_shared.mutex);
        Slot& slot = quads ? g_shared.panels : g_shared.scene;
        if (!ensure_slot(slot, header.width, header.height, DXGI_FORMAT_R8G8B8A8_TYPELESS)) return false;
        g_handoff[quads ? 1 : 0].pending = false;  // This image replaces any shared one not yet shown.
        const size_t eyeBytes = size_t(header.width) * header.height * 4;
        slot.layers = std::min<uint32_t>(header.layers, 2);
        for (UINT eye = 0; eye < slot.layers; ++eye)
            g_context->UpdateSubresource(slot.texture.Get(), eye, nullptr, payload.data() + eye * eyeBytes, header.width * 4, 0);
        // Read-back eyes arrive bottom-up; UI panels top-down.
        publish(slot, header.sequence, quads, !quads, copyStart);
        return true;
    }

private:
    // Rejected GPU frames are acknowledged as UINT64_MAX, which makes the runtime drop the connection.
    template <typename... Args>
    static bool reject(const proto::ImageFrameHeader& header, const char* reason, Args... args)
    {
        static std::atomic<int> reported{0};
        if (reported++ < 20) {
            std::fprintf(stderr, "viewer: rejected frame seq=%llu %ux%u version=%u part=%u: ",
                static_cast<unsigned long long>(header.sequence), header.width, header.height, header.version, header.reserved);
            std::fprintf(stderr, reason, args...);
            std::fputc('\n', stderr);
        }
        return false;
    }

    struct PendingCopy {
        ComPtr<ID3D11Query> query;  // Only without a copy fence.
        uint64_t fenceValue = 0;
        bool quads = false;
        int index = -1;
        UINT width = 0, height = 0;
        DXGI_FORMAT typeless = DXGI_FORMAT_UNKNOWN;
        Clock::time_point started{};
        std::shared_ptr<const Composite> composite;
    };
    std::mutex copyMutex_;
    std::condition_variable copyDone_;
    std::map<uint64_t, PendingCopy> pendingCopies_;
    using Pair = std::array<ComPtr<ID3D11Texture2D>, 2>;

    Pair* open_shared(uint64_t session)
    {
        auto found = sessions_.find(session);
        if (found != sessions_.end()) return &found->second;
        ComPtr<ID3D11Device1> device1;
        if (FAILED(g_copyDevice.As(&device1))) return nullptr;
        Pair pair;
        for (UINT eye = 0; eye < 2; ++eye) {
            wchar_t name[96];
            swprintf_s(name, L"Local\\REFRACT_GPU_%016llx_%u", static_cast<unsigned long long>(session), eye);
            HRESULT result = device1->OpenSharedResourceByName(name, DXGI_SHARED_RESOURCE_READ, IID_PPV_ARGS(&pair[eye]));
            if (FAILED(result)) {
                std::fprintf(stderr, "viewer: cannot open shared texture %ls (0x%08lx)\n", name, result);
                return nullptr;
            }
        }
        if (sessions_.size() >= 32) sessions_.clear();
        std::fprintf(stderr, "viewer: opened shared textures for session %016llx\n", static_cast<unsigned long long>(session));
        return &sessions_.emplace(session, std::move(pair)).first->second;
    }

    std::map<uint64_t, Pair> sessions_;
    bool batchHasScene_ = false;
    H264Decoder decoder_;
};

const char kShader[] = R"(
Texture2DArray image : register(t0);
SamplerState linearSampler : register(s0);
cbuffer Params : register(b0) { float slice; float flip; float2 uvScale; };
struct VsOut { float4 position : SV_Position; float2 uv : TEXCOORD0; };
VsOut vs(uint id : SV_VertexID) {
    VsOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}
float4 ps(VsOut i) : SV_Target {
    float2 uv = i.uv;
    if (flip > 0.5) uv.y = 1 - uv.y;
    return float4(image.Sample(linearSampler, float3(uv * uvScale, slice)).rgb, 1);
}
)";

// A composite frame's UI panel: one quad whose corners arrive in clip space, textured from its
// atlas rectangle, blended as OpenXR composites quad layers (premultiplied by default).
const char kPanelShader[] = R"(
Texture2DArray image : register(t0);
SamplerState linearSampler : register(s0);
cbuffer Panel : register(b0) { float4 corners[4]; float4 uvRect; float4 uvClamp; float slice; float blend; float unpremultiplied; float unused; };
struct VsOut { float4 position : SV_Position; float2 uv : TEXCOORD0; };
VsOut vs(uint id : SV_VertexID) {
    VsOut o;
    o.position = corners[id];
    o.uv = lerp(uvRect.xy, uvRect.zw, float2(id & 1, id >> 1));
    return o;
}
float4 ps(VsOut i) : SV_Target {
    float4 c = image.Sample(linearSampler, float3(clamp(i.uv, uvClamp.xy, uvClamp.zw), slice));
    if (blend < 0.5) c.a = 1;
    else if (unpremultiplied > 0.5) c.rgb *= c.a;
    return c;
}
)";

struct Mouse {
    float yaw = 0, pitch = 0;
    float reach = 0.4f;  // Hand distance in front of the camera, meters (pose_input_server.py DEFAULT_REACH).
    int trigger = 0, grip = 0;
    bool dragging = false;
    POINT last{};
};

struct App {
    HWND window = nullptr;
    ComPtr<IDXGISwapChain1> swapchain;
    HANDLE bufferFree = nullptr;  // Swap chain's frame latency waitable object.
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> params;
    ComPtr<ID3D11VertexShader> panelVs;
    ComPtr<ID3D11PixelShader> panelPs;
    ComPtr<ID3D11Buffer> panelParams;
    ComPtr<ID3D11BlendState> panelBlend;
    ComPtr<ID3D11RasterizerState> panelRaster;
    UINT width = 0, height = 0;
    bool resized = true, both = false, flipOverride = false, closed = false;
    bool saveRequested = false;
    bool hidePanels = false;  // F7: composite frames show the scene alone.
    // Performance overlay (Direct2D text over the swap chain).
    bool overlay = true;
    float gameFps = 0, shownFps = 0;
    ComPtr<ID2D1DeviceContext> d2d;
    ComPtr<ID2D1Bitmap1> d2dTarget;
    ComPtr<IDWriteTextFormat> font;
    ComPtr<ID2D1SolidColorBrush> brush;
    AndroidStats* android = nullptr;
    Mouse mouse;
    float sensitivity = 0.25f;
    std::filesystem::path shots = "screenshots";
};

App g_app;

bool compile(const char* entry, const char* profile, ComPtr<ID3DBlob>& blob, const char* source = kShader)
{
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3DCompile(source, std::strlen(source), "viewer", nullptr, nullptr, entry, profile, 0, 0, &blob, &errors))) {
        std::fprintf(stderr, "viewer: shader %s failed: %s\n", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return false;
    }
    return true;
}

bool create_device()
{
    // Shared textures live on the emulator's GPU; prefer the Nvidia adapter.
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IDXGIAdapter1> chosen, adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (!chosen && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) chosen = adapter;
        if (desc.VendorId == 0x10de) { chosen = adapter; break; }
        adapter.Reset();
    }
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(chosen.Get(), chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, &g_device, nullptr, &g_context))) return false;
    // The receive thread and the window thread share one immediate context.
    ComPtr<ID3D11Multithread> multithread;
    if (FAILED(g_context.As(&multithread))) return false;
    multithread->SetMultithreadProtected(TRUE);
    if (FAILED(D3D11CreateDevice(chosen.Get(), chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
            0, levels, 2, D3D11_SDK_VERSION, &g_copyDevice, nullptr, &g_copyContext))) return false;
    ComPtr<ID3D11Multithread> copyMultithread;
    if (FAILED(g_copyContext.As(&copyMultithread))) return false;
    copyMultithread->SetMultithreadProtected(TRUE);  // Receive submits copies while the ACK thread checks their queries.
    ComPtr<ID3D11Device5> copyDevice5;
    const bool fence = SUCCEEDED(g_copyDevice.As(&copyDevice5)) && SUCCEEDED(g_copyContext.As(&g_copyContext4)) &&
        SUCCEEDED(copyDevice5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_copyFence))) &&
        (g_copyFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr)) != nullptr;
    if (!fence) {
        g_copyFence.Reset();
        g_copyContext4.Reset();
    }
    std::fprintf(stderr, "viewer: GPU copy completion via %s\n", fence ? "fence event" : "query polling");
    if (chosen) {
        DXGI_ADAPTER_DESC1 desc{};
        chosen->GetDesc1(&desc);
        std::fprintf(stderr, "viewer: using %ls\n", desc.Description);
    }
    return true;
}

bool create_pipeline()
{
    ComPtr<ID3DBlob> vsBlob, psBlob;
    if (!compile("vs", "vs_5_0", vsBlob) || !compile("ps", "ps_5_0", psBlob)) return false;
    if (FAILED(g_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_app.vs)) ||
        FAILED(g_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_app.ps))) return false;
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(g_device->CreateSamplerState(&sampler, &g_app.sampler))) return false;
    D3D11_BUFFER_DESC buffer{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    if (FAILED(g_device->CreateBuffer(&buffer, nullptr, &g_app.params))) return false;
    ComPtr<ID3DBlob> panelVsBlob, panelPsBlob;
    D3D11_BUFFER_DESC panelBuffer{112, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0] = {TRUE, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD,
                             D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL};
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE; raster.DepthClipEnable = TRUE;
    if (!compile("vs", "vs_5_0", panelVsBlob, kPanelShader) || !compile("ps", "ps_5_0", panelPsBlob, kPanelShader) ||
        FAILED(g_device->CreateVertexShader(panelVsBlob->GetBufferPointer(), panelVsBlob->GetBufferSize(), nullptr, &g_app.panelVs)) ||
        FAILED(g_device->CreatePixelShader(panelPsBlob->GetBufferPointer(), panelPsBlob->GetBufferSize(), nullptr, &g_app.panelPs)) ||
        FAILED(g_device->CreateBuffer(&panelBuffer, nullptr, &g_app.panelParams)) ||
        FAILED(g_device->CreateBlendState(&blend, &g_app.panelBlend)) ||
        FAILED(g_device->CreateRasterizerState(&raster, &g_app.panelRaster))) return false;
    // Video conversion runs on the receive thread; it gets its own objects.
    ComPtr<ID3DBlob> yuvBlob;
    g_yuv.vs = g_app.vs;
    if (!compile("ps", "ps_5_0", yuvBlob, kYuvShader) ||
        FAILED(g_device->CreatePixelShader(yuvBlob->GetBufferPointer(), yuvBlob->GetBufferSize(), nullptr, &g_yuv.ps)) ||
        FAILED(g_device->CreateSamplerState(&sampler, &g_yuv.sampler)) ||
        FAILED(g_device->CreateBuffer(&buffer, nullptr, &g_yuv.params))) g_yuv.ps.Reset();  // Video frames are then ignored.
    return true;
}

bool create_swapchain()
{
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(g_device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    // Draw only when a back buffer is free (see the main loop). A draw into a buffer the compositor
    // still holds stalls the shared GPU context, and the receive thread's copies (and so the
    // game's frame acknowledgments) would wait up to a screen refresh behind it.
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (FAILED(factory->CreateSwapChainForHwnd(g_device.Get(), g_app.window, &desc, nullptr, nullptr, &g_app.swapchain))) return false;
    ComPtr<IDXGISwapChain2> waitable;
    if (FAILED(g_app.swapchain.As(&waitable)) || FAILED(waitable->SetMaximumFrameLatency(1))) return false;
    g_app.bufferFree = waitable->GetFrameLatencyWaitableObject();
    factory->MakeWindowAssociation(g_app.window, DXGI_MWA_NO_ALT_ENTER);
    return true;
}

bool create_overlay()
{
    ComPtr<ID2D1Factory1> factory;
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<ID2D1Device> device;
    ComPtr<IDWriteFactory> dwrite;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf())) ||
        FAILED(g_device.As(&dxgiDevice)) || FAILED(factory->CreateDevice(dxgiDevice.Get(), &device)) ||
        FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_app.d2d))) return false;
    g_app.d2d->SetDpi(96.0f, 96.0f);  // Draw in pixels.
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()))) ||
        FAILED(dwrite->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 15.0f, L"en-us", &g_app.font))) return false;
    return SUCCEEDED(g_app.d2d->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &g_app.brush));
}

void resize_targets()
{
    RECT rect{};
    GetClientRect(g_app.window, &rect);
    const UINT w = std::max<LONG>(1, rect.right), h = std::max<LONG>(1, rect.bottom);
    g_app.target.Reset();
    if (g_app.d2d) g_app.d2d->SetTarget(nullptr);
    g_app.d2dTarget.Reset();
    g_context->OMSetRenderTargets(0, nullptr, nullptr);
    g_app.swapchain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
    ComPtr<ID3D11Texture2D> back;
    g_app.swapchain->GetBuffer(0, IID_PPV_ARGS(&back));
    g_device->CreateRenderTargetView(back.Get(), nullptr, &g_app.target);
    ComPtr<IDXGISurface> surface;
    if (g_app.d2d && SUCCEEDED(back.As(&surface))) {
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        if (SUCCEEDED(g_app.d2d->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &g_app.d2dTarget)))
            g_app.d2d->SetTarget(g_app.d2dTarget.Get());
    }
    g_app.width = w; g_app.height = h;
    g_app.resized = false;
}

void save_png(const Slot& slot)
{
    D3D11_TEXTURE2D_DESC desc{};
    slot.texture->GetDesc(&desc);
    desc.ArraySize = 1; desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &staging))) return;
    g_context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, slot.texture.Get(), 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(g_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return;
    const bool bgra = slot.typeless == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    const bool flip = slot.flip != g_app.flipOverride;
    std::vector<uint8_t> pixels(size_t(slot.width) * slot.height * 4);
    for (UINT y = 0; y < slot.height; ++y) {
        const auto* row = static_cast<const uint8_t*>(mapped.pData) + size_t(flip ? slot.height - 1 - y : y) * mapped.RowPitch;
        auto* out = pixels.data() + size_t(y) * slot.width * 4;
        for (UINT x = 0; x < slot.width; ++x) {  // WIC wants BGRA; force opaque.
            out[x * 4 + 0] = row[x * 4 + (bgra ? 0 : 2)];
            out[x * 4 + 1] = row[x * 4 + 1];
            out[x * 4 + 2] = row[x * 4 + (bgra ? 2 : 0)];
            out[x * 4 + 3] = 255;
        }
    }
    if (slot.composite) {  // The PNG is opaque; report what the panels' alpha really is (left-eye texture).
        for (const auto& panel : slot.composite->quads) {
            if (panel.texture & 1) continue;
            int low = 255, high = 0, rgbHigh = 0;
            for (UINT y = panel.y; y < panel.y + panel.height; ++y) {
                const auto* row = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
                for (UINT x = panel.x; x < panel.x + panel.width; ++x) {
                    low = std::min<int>(low, row[x * 4 + 3]); high = std::max<int>(high, row[x * 4 + 3]);
                    rgbHigh = std::max<int>({rgbHigh, row[x * 4], row[x * 4 + 1], row[x * 4 + 2]});
                }
            }
            std::fprintf(stderr, "viewer: panel %ux%u at %u,%u: alpha %d..%d, max color %d\n",
                         panel.width, panel.height, panel.x, panel.y, low, high, rgbHigh);
        }
    }
    g_context->Unmap(staging.Get(), 0);

    std::filesystem::create_directories(g_app.shots);
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    const auto path = g_app.shots / (std::string("yeeps-") + stamp + "-seq" + std::to_string(slot.sequence) + ".png");

    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(slot.width, slot.height)) || FAILED(frame->SetPixelFormat(&format)) ||
        FAILED(frame->WritePixels(slot.height, slot.width * 4, static_cast<UINT>(pixels.size()), pixels.data())) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        std::fprintf(stderr, "viewer: screenshot failed\n");
        return;
    }
    std::printf("saved %s\n", path.string().c_str());
    std::fflush(stdout);
}

// Letterboxed rectangle for an image of the given aspect inside (x, y, w, h).
D3D11_VIEWPORT fit(float x, float y, float w, float h, float aspect)
{
    float vw = w, vh = w / aspect;
    if (vh > h) { vh = h; vw = h * aspect; }
    return {x + (w - vw) / 2, y + (h - vh) / 2, vw, vh, 0, 1};
}

// Frame-time bar color: within a 90 fps budget green, within 60 fps yellow, else red.
D2D1_COLOR_F frame_color(float ms)
{
    return ms <= 11.2f ? D2D1::ColorF(0.3f, 0.85f, 0.4f) : ms <= 16.7f ? D2D1::ColorF(0.95f, 0.8f, 0.2f) : D2D1::ColorF(0.95f, 0.3f, 0.25f);
}

// Called with g_shared.mutex held, after the image is drawn.
void draw_overlay(const Slot& slot)
{
    if (!g_app.overlay || !g_app.d2dTarget) return;
    std::vector<float> frames;  // Oldest first.
    for (size_t i = 0; i < g_shared.intervalCount; ++i) {
        const size_t index = (g_shared.intervalNext + g_shared.intervals.size() - g_shared.intervalCount + i) % g_shared.intervals.size();
        frames.push_back(g_shared.intervals[index]);
    }
    float average = 0, worst = 0, low = 0;
    if (!frames.empty()) {
        std::vector<float> sorted = frames;
        std::sort(sorted.begin(), sorted.end());
        for (float f : frames) average += f;
        average /= frames.size();
        worst = sorted.back();
        low = sorted[std::min(sorted.size() - 1, sorted.size() * 99 / 100)];  // 99th percentile frame time.
    }
    const auto android = g_app.android ? g_app.android->snapshot() : AndroidSnapshot{};
    std::wstring text;
    wchar_t line[256];
    swprintf_s(line, L"Game     %5.0f fps   %5.1f ms avg   %5.1f ms worst   1%% low %3.0f fps\n",
        g_app.gameFps, average, worst, low > 0 ? 1000.0f / low : 0.0f);
    text += line;
    swprintf_s(line, L"Viewer   %5.0f fps shown   copy %.2f ms   %ls\n", g_app.shownFps, g_shared.copyMs,
        !g_shared.connected ? L"waiting for game" : g_shared.gpu ? L"GPU shared textures" : g_shared.video ? L"H.264 stream (copy = decode)" : L"pixel stream");
    text += line;
    if (slot.composite)
        swprintf_s(line, L"Render   %u x %u per eye + %zu UI panel(s)\n", slot.composite->header.scene_width,
            slot.composite->header.scene_height, slot.composite->quads.size());
    else
        swprintf_s(line, L"Render   %u x %u per eye%ls\n", slot.width, slot.height, &slot == &g_shared.panels ? L" (UI panel)" : L"");
    text += line;
    if (android.valid) {
        swprintf_s(line, L"Android  CPU %3.0f%% of %d cores (%.1f cores busy)\nThreads ", android.cpuPercent, android.cores,
            android.cpuPercent * android.cores / 100.0);
        text += line;
        for (size_t i = 0; i < std::min<size_t>(4, android.threads.size()); ++i) {
            swprintf_s(line, L" %hs %.0f%%", android.threads[i].name.c_str(), android.threads[i].percent);
            text += line;
        }
        text += L"\n";
    } else {
        text += L"Android  stats unavailable (adb)\n\n";
    }
    text += L"(thread % = share of one core)   F1 hide";

    constexpr float x = 12, y = 12, width = 640, textHeight = 112, graphHeight = 70;
    auto& d2d = *g_app.d2d.Get();
    d2d.BeginDraw();
    g_app.brush->SetColor(D2D1::ColorF(0, 0, 0, 0.6f));
    d2d.FillRectangle(D2D1::RectF(x, y, x + width, y + textHeight + graphHeight + 20), g_app.brush.Get());
    g_app.brush->SetColor(D2D1::ColorF(D2D1::ColorF::White));
    d2d.DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), g_app.font.Get(),
        D2D1::RectF(x + 10, y + 8, x + width - 10, y + textHeight), g_app.brush.Get());
    // Frame-time graph, newest on the right; 0-33 ms tall, lines at 90 and 60 fps.
    const float graphTop = y + textHeight + 10, graphBottom = graphTop + graphHeight, scale = graphHeight / 33.3f;
    const float barWidth = (width - 20) / float(g_shared.intervals.size());
    for (size_t i = 0; i < frames.size(); ++i) {
        const float left = x + 10 + (g_shared.intervals.size() - frames.size() + i) * barWidth;
        g_app.brush->SetColor(frame_color(frames[i]));
        d2d.FillRectangle(D2D1::RectF(left, std::max(graphTop, graphBottom - frames[i] * scale), left + barWidth, graphBottom), g_app.brush.Get());
    }
    g_app.brush->SetColor(D2D1::ColorF(1, 1, 1, 0.35f));
    for (float ms : {11.1f, 16.7f})
        d2d.DrawLine(D2D1::Point2F(x + 10, graphBottom - ms * scale), D2D1::Point2F(x + width - 10, graphBottom - ms * scale), g_app.brush.Get());
    d2d.EndDraw();
}

struct Vec3 { float x, y, z; };
Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
// Rotates v by the unit quaternion (qx, qy, qz, qw); `inverse` rotates by its conjugate.
Vec3 rotate(const proto::Pose& q, Vec3 v, bool inverse = false)
{
    const Vec3 u{inverse ? -q.qx : q.qx, inverse ? -q.qy : q.qy, inverse ? -q.qz : q.qz};
    return v + cross(u, cross(u, v) + v * q.qw) * 2.0f;
}

// Width / height of an eye's field of view.
float view_aspect(const proto::ImageProjectionView& view)
{
    return (std::tan(view.angle_right) - std::tan(view.angle_left)) / (std::tan(view.angle_up) - std::tan(view.angle_down));
}

// Draws one eye of a composite frame into the current viewport: the scene region of the atlas,
// then every panel that eye sees, projected with the eye's pose and field of view.
// The scene pipeline (g_app.vs/ps, sampler, slot view) is bound on entry and on return.
void draw_composite(const Slot& slot, const Composite& composite, UINT eye)
{
    const auto& header = composite.header;
    if (header.scene_width) {
        const float params[4] = {float(eye), 0, float(header.scene_width) / slot.width, float(header.scene_height) / slot.height};
        g_context->UpdateSubresource(g_app.params.Get(), 0, nullptr, params, 0, 0);
        g_context->Draw(3, 0);
    }
    if (composite.quads.empty() || g_app.hidePanels) return;
    const auto& view = composite.projection.views[eye];
    const Vec3 eyePosition{view.pose.x, view.pose.y, view.pose.z};
    const float left = std::tan(view.angle_left), right = std::tan(view.angle_right);
    const float up = std::tan(view.angle_up), down = std::tan(view.angle_down);
    constexpr float kNear = 0.01f, kFar = 1000.0f;
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    g_context->VSSetShader(g_app.panelVs.Get(), nullptr, 0);
    g_context->PSSetShader(g_app.panelPs.Get(), nullptr, 0);
    g_context->VSSetConstantBuffers(0, 1, g_app.panelParams.GetAddressOf());
    g_context->PSSetConstantBuffers(0, 1, g_app.panelParams.GetAddressOf());
    g_context->OMSetBlendState(g_app.panelBlend.Get(), nullptr, 0xffffffff);
    g_context->RSSetState(g_app.panelRaster.Get());
    for (const auto& panel : composite.quads) {
        const auto& quad = panel.quad;
        if (quad.eye_visibility && quad.eye_visibility != eye + 1) continue;  // 1 = left only, 2 = right only.
        struct { float corners[4][4]; float uvRect[4]; float uvClamp[4]; float slice, blend, unpremultiplied, unused; } params{};
        const Vec3 center{quad.pose.x, quad.pose.y, quad.pose.z};
        bool inFront = false;
        for (int corner = 0; corner < 4; ++corner) {
            // Top-left, top-right, bottom-left, bottom-right: image row 0 is the panel's top (+Y).
            const Vec3 local{(corner & 1 ? 0.5f : -0.5f) * quad.width, (corner & 2 ? -0.5f : 0.5f) * quad.height, 0};
            const Vec3 world = center + rotate(quad.pose, local);
            const Vec3 e = rotate(view.pose, world - eyePosition, true);  // OpenXR eye space: -Z forward.
            const float w = -e.z;
            inFront = inFront || w > kNear;
            float* out = params.corners[corner];
            out[0] = (2 * e.x - w * (right + left)) / (right - left);
            out[1] = (2 * e.y - w * (up + down)) / (up - down);
            out[2] = (w - kNear) * kFar / (kFar - kNear);
            out[3] = w;
        }
        if (!inFront) continue;
        const float texelU = 1.0f / slot.width, texelV = 1.0f / slot.height;
        params.uvRect[0] = panel.x * texelU; params.uvRect[1] = panel.y * texelV;
        params.uvRect[2] = (panel.x + panel.width) * texelU; params.uvRect[3] = (panel.y + panel.height) * texelV;
        // Half a texel in, so filtering never reads a neighbouring panel.
        params.uvClamp[0] = params.uvRect[0] + texelU / 2; params.uvClamp[1] = params.uvRect[1] + texelV / 2;
        params.uvClamp[2] = params.uvRect[2] - texelU / 2; params.uvClamp[3] = params.uvRect[3] - texelV / 2;
        if (panel.texture & proto::kCompositeQuadFlipped) std::swap(params.uvRect[1], params.uvRect[3]);  // Rows bottom-up.
        params.slice = float(panel.texture & 1);
        params.blend = (quad.layer_flags & 2) ? 1.0f : 0.0f;            // XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
        params.unpremultiplied = (quad.layer_flags & 4) ? 1.0f : 0.0f;  // XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT
        g_context->UpdateSubresource(g_app.panelParams.Get(), 0, nullptr, &params, 0, 0);
        g_context->Draw(4, 0);
    }
    g_context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    g_context->RSSetState(nullptr);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(g_app.vs.Get(), nullptr, 0);
    g_context->PSSetShader(g_app.ps.Get(), nullptr, 0);
    ID3D11Buffer* none = nullptr;
    g_context->VSSetConstantBuffers(0, 1, &none);
    g_context->PSSetConstantBuffers(0, 1, g_app.params.GetAddressOf());
}

void render()
{
    std::lock_guard lock(g_shared.mutex);  // Direct2D shares the D3D context with the receive thread.
    if (g_app.resized) resize_targets();
    // Take the newest shared GPU image from the receive thread (see Handoff).
    for (int kind = 0; kind < 2; ++kind) {
        auto& handoff = g_handoff[kind];
        Slot& slot = kind ? g_shared.panels : g_shared.scene;
        if (!handoff.pending || handoff.latest < 0 || !slot.texture) continue;
        auto& entry = handoff.entries[handoff.latest];
        if (entry.drawLock->AcquireSync(0, 100) != S_OK) continue;
        for (UINT eye = 0; eye < 2; ++eye) {
            const D3D11_BOX box{eye * handoff.width, 0, 0, (eye + 1) * handoff.width, handoff.height, 1};
            g_context->CopySubresourceRegion(slot.texture.Get(), eye, 0, 0, 0, entry.draw.Get(), 0, &box);
        }
        entry.drawLock->ReleaseSync(0);
        handoff.pending = false;
    }
    const float background[4] = {0.05f, 0.05f, 0.06f, 1};
    g_context->OMSetRenderTargets(1, g_app.target.GetAddressOf(), nullptr);
    g_context->ClearRenderTargetView(g_app.target.Get(), background);
    {
        const bool sceneStale = !g_shared.scene.valid || Clock::now() - g_shared.scene.at > std::chrono::seconds(1);
        const Slot& slot = (g_shared.wantPanels || sceneStale) && g_shared.panels.valid ? g_shared.panels : g_shared.scene;
        if (slot.valid) {
            if (g_app.saveRequested) save_png(slot);
            g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g_context->IASetInputLayout(nullptr);
            g_context->VSSetShader(g_app.vs.Get(), nullptr, 0);
            g_context->PSSetShader(g_app.ps.Get(), nullptr, 0);
            g_context->PSSetShaderResources(0, 1, slot.view.GetAddressOf());
            g_context->PSSetSamplers(0, 1, g_app.sampler.GetAddressOf());
            g_context->PSSetConstantBuffers(0, 1, g_app.params.GetAddressOf());
            const UINT count = g_app.both && slot.layers >= 2 ? 2 : 1;
            const Composite* composite = slot.composite.get();
            const float aspect = !composite ? float(slot.width) / float(slot.height) :
                composite->header.scene_width ? float(composite->header.scene_width) / float(composite->header.scene_height) :
                view_aspect(composite->projection.views[0]);
            for (UINT i = 0; i < count; ++i) {
                const float cellWidth = float(g_app.width) / count;
                const auto viewport = fit(cellWidth * i, 0, cellWidth, float(g_app.height), aspect);
                g_context->RSSetViewports(1, &viewport);
                if (composite) {
                    draw_composite(slot, *composite, i);
                    continue;
                }
                const float params[4] = {float(i), (slot.flip != g_app.flipOverride) ? 1.0f : 0.0f, 1, 1};
                g_context->UpdateSubresource(g_app.params.Get(), 0, nullptr, params, 0, 0);
                g_context->Draw(3, 0);
            }
            ID3D11ShaderResourceView* none = nullptr;
            g_context->PSSetShaderResources(0, 1, &none);
        }
        g_app.saveRequested = false;
        draw_overlay(slot);
    }
    g_app.swapchain->Present(0, 0);
}

// Borderless fullscreen on the window's monitor; a second call restores the window.
void toggle_fullscreen()
{
    static WINDOWPLACEMENT saved{sizeof(WINDOWPLACEMENT)};
    const LONG style = GetWindowLongW(g_app.window, GWL_STYLE);
    if (style & WS_OVERLAPPEDWINDOW) {
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetWindowPlacement(g_app.window, &saved) ||
            !GetMonitorInfoW(MonitorFromWindow(g_app.window, MONITOR_DEFAULTTOPRIMARY), &monitor)) return;
        SetWindowLongW(g_app.window, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        const RECT& r = monitor.rcMonitor;
        SetWindowPos(g_app.window, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(g_app.window, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g_app.window, &saved);
        SetWindowPos(g_app.window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    auto& mouse = g_app.mouse;
    switch (message) {
    case WM_SIZE: g_app.resized = true; return 0;
    case WM_CLOSE: g_app.closed = true; DestroyWindow(window); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_RBUTTONDOWN:
        mouse.dragging = true;
        mouse.last = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        SetCapture(window);
        return 0;
    case WM_RBUTTONUP: mouse.dragging = false; ReleaseCapture(); return 0;
    case WM_MOUSEMOVE:
        if (mouse.dragging) {
            const POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            const float dx = float(p.x - mouse.last.x), dy = float(p.y - mouse.last.y);
            mouse.last = p;
            mouse.yaw = std::fmod(mouse.yaw - dx * g_app.sensitivity + 540.0f, 360.0f) - 180.0f;
            mouse.pitch = std::clamp(mouse.pitch - dy * g_app.sensitivity, -80.0f, 80.0f);
        }
        return 0;
    case WM_LBUTTONDOWN: mouse.trigger = 1; return 0;
    case WM_LBUTTONUP: mouse.trigger = 0; return 0;
    case WM_MBUTTONDOWN: mouse.grip = 1; return 0;
    case WM_MBUTTONUP: mouse.grip = 0; return 0;
    case WM_MOUSEWHEEL:  // Scroll up pushes the hands away, down pulls them in.
        mouse.reach = std::clamp(mouse.reach + GET_WHEEL_DELTA_WPARAM(w) / float(WHEEL_DELTA) * 0.05f, 0.1f, 1.0f);
        return 0;
    case WM_KEYDOWN:
        if (w == VK_F1) {
            g_app.overlay = !g_app.overlay;
            if (g_app.android) g_app.android->enabled = g_app.overlay;
        }
        if (w == VK_F2) g_app.saveRequested = true;
        if (w == VK_F3) g_app.both = !g_app.both;
        if (w == VK_F5) g_app.flipOverride = !g_app.flipOverride;
        if (w == VK_F6) g_shared.wantPanels = !g_shared.wantPanels;
        if (w == VK_F7) g_app.hidePanels = !g_app.hidePanels;
        if (w == VK_HOME) mouse.yaw = mouse.pitch = 0;
        if (w == VK_F11 || (w == VK_ESCAPE && !(GetWindowLongW(window, GWL_STYLE) & WS_OVERLAPPEDWINDOW))) toggle_fullscreen();
        SetEvent(g_shared.newFrame);  // Redraw with the new setting.
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}

}  // namespace

int main(int argc, char** argv)
{
    int port = 38491, controlPort = 38495;
    std::string adb = R"(C:\Users\mixid\Android\Sdk\platform-tools\adb.exe)", serial = "emulator-5582", package = "com.TrassGames.Yeeps";
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--port") port = std::atoi(argv[++i]);
        else if (arg == "--control-port") controlPort = std::atoi(argv[++i]);
        else if (arg == "--shots") g_app.shots = argv[++i];
        else if (arg == "--sensitivity") g_app.sensitivity = float(std::atof(argv[++i]));
        else if (arg == "--adb") adb = argv[++i];
        else if (arg == "--serial") serial = argv[++i];
        else if (arg == "--package") package = argv[++i];
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!create_device() || !create_pipeline()) { std::fprintf(stderr, "viewer: D3D11 setup failed\n"); return 1; }

    WNDCLASSW wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"RefractViewer";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    g_app.window = CreateWindowExW(0, wc.lpszClassName, kTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        1000, 1040, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_app.window || !create_swapchain()) { std::fprintf(stderr, "viewer: window setup failed\n"); return 1; }
    if (!create_overlay()) std::fprintf(stderr, "viewer: overlay setup failed; no performance stats\n");
    AndroidStats android(adb, serial, package);
    android.start();
    g_app.android = &android;
    // A launcher may start us hidden; the first ShowWindow uses that startup value.
    ShowWindow(g_app.window, SW_SHOWNORMAL);
    ShowWindow(g_app.window, SW_SHOW);

    Receiver receiver;
    std::thread([&] {
        proto::TcpImageServer server;
        server.serve_with_callback(static_cast<uint16_t>(port), 0,
            [&](const proto::ImageFrameHeader& header, const proto::ImageProjection& projection, std::vector<uint8_t>&& payload) {
                return receiver.on_frame(header, projection, std::move(payload));
            }, [&](uint64_t sequence) { return receiver.ack_ready(sequence); });
        std::fprintf(stderr, "viewer: image server stopped (port %d busy?)\n", port);
    }).detach();

    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);  // Before the UDP socket; the image server thread starts its own.
    SOCKET control = socket(AF_INET, SOCK_DGRAM, 0);
    if (control == INVALID_SOCKET) std::fprintf(stderr, "viewer: control socket failed (%d); mouse input disabled\n", WSAGetLastError());
    sockaddr_in controlAddress{};
    controlAddress.sin_family = AF_INET;
    controlAddress.sin_port = htons(static_cast<u_short>(controlPort));
    inet_pton(AF_INET, "127.0.0.1", &controlAddress.sin_addr);
    std::string sent;
    auto sentAt = Clock::now(), statsAt = Clock::now();
    uint64_t shownUpdates = UINT64_MAX;
    uint32_t presented = 0;

    bool bufferFree = false;  // A back buffer is free: its waitable signal was taken and not yet drawn into.
    while (!g_app.closed) {
        const HANDLE handles[] = {g_shared.newFrame, g_app.bufferFree};
        const DWORD woke = MsgWaitForMultipleObjects(bufferFree ? 1 : 2, handles, FALSE, 50, QS_ALLINPUT);
        if (woke == WAIT_OBJECT_0 + 1 || (!bufferFree && WaitForSingleObject(g_app.bufferFree, 0) == WAIT_OBJECT_0))
            bufferFree = true;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_app.closed = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_app.closed) break;

        const auto now = Clock::now();
        char view[96];
        const auto& mouse = g_app.mouse;
        std::snprintf(view, sizeof(view), "view %.2f %.2f %d %d %.2f", mouse.yaw, mouse.pitch, mouse.trigger, mouse.grip, mouse.reach);
        // Resend periodically too, so a restarted input server picks the view back up.
        if (control != INVALID_SOCKET && (sent != view || now - sentAt > std::chrono::seconds(1))) {
            sendto(control, view, static_cast<int>(std::strlen(view)), 0, reinterpret_cast<sockaddr*>(&controlAddress), sizeof(controlAddress));
            sent = view;
            sentAt = now;
        }

        uint64_t updates;
        {
            std::lock_guard lock(g_shared.mutex);
            updates = g_shared.updates;
        }
        if (bufferFree && (updates != shownUpdates || g_app.resized || g_app.saveRequested)) {
            render();
            bufferFree = false;
            shownUpdates = updates;
            ++presented;
        }

        const double elapsed = std::chrono::duration<double>(now - statsAt).count();
        if (elapsed >= 1.0) {
            g_app.gameFps = float(g_shared.sceneFrames / elapsed);
            g_app.shownFps = float(presented / elapsed);
            wchar_t title[256];
            const bool panels = g_shared.wantPanels;
            swprintf_s(title, L"%ls  |  game %.0f fps, shown %.0f fps (%ls%ls)  |  right-drag look, click trigger, wheel hand distance  |  "
                L"F1 stats, F2 screenshot, F3 both eyes, F6 %ls, F11 fullscreen, Home recenter", kTitle,
                g_shared.sceneFrames.exchange(0) / elapsed, presented / elapsed,
                !g_shared.connected ? L"waiting for game" : g_shared.gpu ? L"GPU shared" : g_shared.video ? L"H.264" : L"pixel stream",
                panels ? L", panels" : L"", panels ? L"scene" : L"panels");
            SetWindowTextW(g_app.window, title);
            g_shared.allFrames = 0;
            presented = 0;
            statsAt = now;
        }
    }
    return 0;
}
