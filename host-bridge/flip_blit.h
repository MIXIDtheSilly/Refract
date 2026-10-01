#pragma once
#if defined(_WIN32)
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>

namespace refract::host {
// Copies a width x height region of one array slice (at sourceX, sourceY) to (0, 0) of another texture's
// slice upside down, for panels stored bottom-up (protocol::kQuadLayerFlipped). D3D11 copies cannot
// flip, and SteamVR has no image-layout extension, so this draws one triangle that reads the rows in reverse.
class FlipBlit {
public:
    bool copy(ID3D11DeviceContext* context, ID3D11Texture2D* source, UINT sourceSlice,
              ID3D11Texture2D* destination, UINT destinationSlice, UINT width, UINT height, DXGI_FORMAT format,
              UINT sourceX = 0, UINT sourceY = 0) {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        if (!ready(device.Get())) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC read{};
        read.Format = format; read.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        read.Texture2DArray.MipLevels = 1; read.Texture2DArray.FirstArraySlice = sourceSlice; read.Texture2DArray.ArraySize = 1;
        D3D11_RENDER_TARGET_VIEW_DESC write{};
        write.Format = format; write.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        write.Texture2DArray.FirstArraySlice = destinationSlice; write.Texture2DArray.ArraySize = 1;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> input;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> output;
        if (FAILED(device->CreateShaderResourceView(source, &read, &input)) ||
            FAILED(device->CreateRenderTargetView(destination, &write, &output))) {
            if (!reportedViews_) { reportedViews_ = true; std::fprintf(stderr, "Refract Flip: cannot view textures as format %d\n", format); }
            return false;
        }
        const float rows[4] = {static_cast<float>(height), static_cast<float>(sourceX), static_cast<float>(sourceY), 0};
        context->UpdateSubresource(constants_.Get(), 0, nullptr, rows, 0, 0);
        const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
        ID3D11RenderTargetView* targets[] = {output.Get()};
        ID3D11ShaderResourceView* inputs[] = {input.Get()};
        ID3D11Buffer* buffers[] = {constants_.Get()};
        context->OMSetRenderTargets(1, targets, nullptr);
        context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        context->OMSetDepthStencilState(nullptr, 0);
        context->RSSetState(nullptr);
        context->RSSetViewports(1, &viewport);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex_.Get(), nullptr, 0);
        context->PSSetShader(pixel_.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, inputs);
        context->PSSetConstantBuffers(0, 1, buffers);
        context->Draw(3, 0);
        // Leave nothing bound: the same textures are copied to and read by OpenXR next.
        ID3D11ShaderResourceView* noInput = nullptr;
        context->PSSetShaderResources(0, 1, &noInput);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        return true;
    }
private:
    bool ready(ID3D11Device* device) {
        if (device_.Get() == device && pixel_) return true;
        if (failed_) return false;
        failed_ = true;
        // One triangle covering the viewport; each pixel loads the mirrored row (Load, so no filtering).
        static const char shader[] =
            "Texture2DArray<float4> source : register(t0);\n"
            "cbuffer Rows : register(b0) { float rows; float2 origin; float unused; };\n"
            "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
            "    float2 uv = float2((id << 1) & 2, id & 2);\n"
            "    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
            "}\n"
            "float4 ps(float4 position : SV_Position) : SV_Target {\n"
            "    return source.Load(int4(int(origin.x) + int(position.x), int(origin.y) + int(rows) - 1 - int(position.y), 0, 0));\n"
            "}\n";
        Microsoft::WRL::ComPtr<ID3DBlob> vertexCode, pixelCode, errors;
        if (FAILED(D3DCompile(shader, std::strlen(shader), "refract_flip", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vertexCode, &errors)) ||
            FAILED(D3DCompile(shader, std::strlen(shader), "refract_flip", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &pixelCode, &errors))) {
            std::fprintf(stderr, "Refract Flip: shader compile failed: %s\n",
                errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown error");
            return false;
        }
        D3D11_BUFFER_DESC buffer{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        if (FAILED(device->CreateVertexShader(vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), nullptr, &vertex_)) ||
            FAILED(device->CreatePixelShader(pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(), nullptr, &pixel_)) ||
            FAILED(device->CreateBuffer(&buffer, nullptr, &constants_))) {
            std::fprintf(stderr, "Refract Flip: cannot create shaders\n");
            pixel_.Reset();
            return false;
        }
        device_ = device;
        failed_ = false;
        return true;
    }
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    bool failed_ = false, reportedViews_ = false;
};
}
#endif
