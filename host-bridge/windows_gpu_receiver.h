#pragma once
#if defined(_WIN32)
#include <d3d11_4.h>
#include <cstdio>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <chrono>
#include "windows_gpu_frame.h"

namespace refract::host {
// Waits for copies submitted on one immediate context. A D3D11 fence lets the thread sleep until
// the GPU is done; the event query it replaces was polled with SwitchToThread, keeping a host core
// busy that the emulator's vCPUs could use. Without ID3D11Device5 the reused query is still polled.
class CopyCompletion {
public:
    CopyCompletion() = default;
    CopyCompletion(const CopyCompletion&) = delete;
    CopyCompletion& operator=(const CopyCompletion&) = delete;
    ~CopyCompletion() { if (event_) CloseHandle(event_); }
    bool wait(ID3D11Device* device, ID3D11DeviceContext* context) {
        if (device != device_.Get() || context != context_.Get()) bind(device, context);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        if (fence_ && SUCCEEDED(context4_->Signal(fence_.Get(), ++value_))) {
            context->Flush();
            // The auto-reset event can carry a stale wakeup from an earlier timed-out wait,
            // so the fence value decides, not the wakeup.
            for (;;) {
                const uint64_t completed = fence_->GetCompletedValue();
                if (completed == UINT64_MAX) return false;  // Device removed.
                if (completed >= value_) return true;
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
                if (left <= 0 || FAILED(fence_->SetEventOnCompletion(value_, event_))) return false;
                WaitForSingleObject(event_, static_cast<DWORD>(left));
            }
        }
        D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
        if (!query_ && FAILED(device->CreateQuery(&desc, &query_))) return false;
        context->End(query_.Get()); context->Flush();
        HRESULT result;
        while ((result = context->GetData(query_.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE) {
            if (std::chrono::steady_clock::now() > deadline) { query_.Reset(); return false; }
            SwitchToThread();
        }
        return SUCCEEDED(result);
    }
private:
    void bind(ID3D11Device* device, ID3D11DeviceContext* context) {
        device_ = device; context_ = context;
        query_.Reset(); fence_.Reset(); context4_.Reset(); value_ = 0;
        Microsoft::WRL::ComPtr<ID3D11Device5> device5;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device5))) &&
            SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context4_))) &&
            SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))) &&
            (event_ || (event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr)))) {
            report("fence event");
            return;
        }
        fence_.Reset(); context4_.Reset();
        report("polled query (no D3D11 fence)");
    }
    // Once per method: every mixed-layer part has its own receiver.
    static void report(const char* method) {
        static const char* reported = nullptr;
        if (reported != method) std::fprintf(stderr, "Refract GPU receiver: copy completion via %s\n", reported = method);
    }
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4_;
    Microsoft::WRL::ComPtr<ID3D11Fence> fence_;
    Microsoft::WRL::ComPtr<ID3D11Query> query_;
    HANDLE event_ = nullptr;
    uint64_t value_ = 0;
};

class WindowsGpuReceiver {
public:
    ~WindowsGpuReceiver() { if (cacheHandle_) CloseHandle(cacheHandle_); }
    // Keep a host-owned GPU copy so the guest can reuse shared images as soon
    // as this copy finishes, even when OpenXR rotates through swapchain images.
    bool receive(ID3D11Device* device, ID3D11DeviceContext* context,
                 const protocol::WindowsGpuFrame& frame, uint64_t sequence,
                 UINT width, UINT height, DXGI_FORMAT targetFormat) {
        if (session_ == frame.session && sequence_ == sequence && cached_) return true;
        if (!frame.session || frame.formats[0] != frame.formats[1]) return false;
        if (session_ != frame.session || width_ != width || height_ != height) {
            cached_.Reset(); renderCache_.Reset(); shared_[0].Reset(); shared_[1].Reset();
            if (cacheHandle_) { CloseHandle(cacheHandle_); cacheHandle_ = nullptr; }
            Microsoft::WRL::ComPtr<ID3D11Device1> device1;
            if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1)))) return false;
            for (UINT eye = 0; eye < 2; ++eye) {
                wchar_t name[96]; swprintf_s(name, L"Local\\REFRACT_GPU_%016llx_%u", frame.session, eye);
                if (FAILED(device1->OpenSharedResourceByName(name, DXGI_SHARED_RESOURCE_READ, IID_PPV_ARGS(&shared_[eye])))) return false;
                D3D11_TEXTURE2D_DESC source{}; shared_[eye]->GetDesc(&source);
                if (source.Width != width || source.Height != height || source.ArraySize != 1 || source.SampleDesc.Count != 1) return false;
                bool rgba = source.Format == DXGI_FORMAT_R8G8B8A8_UNORM || source.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                bool targetRgba = targetFormat == DXGI_FORMAT_R8G8B8A8_UNORM || targetFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                bool bgra = source.Format == DXGI_FORMAT_B8G8R8A8_UNORM || source.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
                bool targetBgra = targetFormat == DXGI_FORMAT_B8G8R8A8_UNORM || targetFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
                if (!(rgba && targetRgba) && !(bgra && targetBgra)) return false;
            }
            D3D11_TEXTURE2D_DESC desc{}; desc.Width = width; desc.Height = height;
            desc.MipLevels = 1; desc.ArraySize = 2; desc.SampleDesc.Count = 1; desc.Format = targetFormat;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &cached_))) return false;
            Microsoft::WRL::ComPtr<IDXGIResource1> resource;
            if (FAILED(cached_.As(&resource)) || FAILED(resource->CreateSharedHandle(nullptr,
                    DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &cacheHandle_))) return false;
            session_ = frame.session; width_ = width; height_ = height;
        }
        for (UINT eye = 0; eye < 2; ++eye) context->CopySubresourceRegion(cached_.Get(), eye, 0, 0, 0, shared_[eye].Get(), 0, nullptr);
        if (!receiveCompletion_.wait(device, context)) return false;
        sequence_ = sequence; return true;
    }
    bool copy_to(ID3D11DeviceContext* context, ID3D11Texture2D* destination, UINT firstSlice = 0, UINT sliceCount = 2) {
        D3D11_TEXTURE2D_DESC destinationInfo{}; destination->GetDesc(&destinationInfo);
        if (sliceCount < 1 || sliceCount > 2 || firstSlice + sliceCount > destinationInfo.ArraySize) return false;
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        if (!renderCache_) {
            Microsoft::WRL::ComPtr<ID3D11Device1> device1;
            if (FAILED(device.As(&device1)) || FAILED(device1->OpenSharedResource1(
                    cacheHandle_, IID_PPV_ARGS(&renderCache_)))) return false;
        }
        for (UINT eye = 0; eye < sliceCount; ++eye) context->CopySubresourceRegion(destination, firstSlice + eye, 0, 0, 0, renderCache_.Get(), eye, nullptr);
        // The caller holds the cache mutex until this cross-device read is
        // finished. The receiver may then safely overwrite the shared cache.
        return renderCompletion_.wait(device.Get(), context);
    }
private:
    uint64_t session_ = 0, sequence_ = UINT64_MAX;
    UINT width_ = 0, height_ = 0;
    HANDLE cacheHandle_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> shared_[2], cached_, renderCache_;
    CopyCompletion receiveCompletion_, renderCompletion_;
};
}
#endif
