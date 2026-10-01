#pragma once
#if defined(_WIN32)
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>

// Opt-in diagnostics only. Four occasional readbacks after loading, never part
// of the normal shared-GPU path. Set REFRACT_CAPTURE_PREFIX before starting host.
// A frame may be captured slice by slice (scene eyes, then each panel); the
// first slice of a new frame decides whether that frame is captured at all.
inline void debug_capture_frame(ID3D11DeviceContext* context, ID3D11Texture2D* image,
    UINT width, UINT height, uint64_t sequence, UINT slice = 0, const char* label = nullptr) {
    static const std::string prefix = [] {const char* p=std::getenv("REFRACT_CAPTURE_PREFIX");return p?std::string(p):std::string();}();
    static unsigned captured=0;
    static uint64_t frame=UINT64_MAX;
    static auto previous=std::chrono::steady_clock::now();
    if (prefix.empty() || sequence<2000) return;
    if (sequence!=frame) {
        const auto now=std::chrono::steady_clock::now();
        if (captured>=4 || now-previous<std::chrono::seconds(5)) return;
        previous=now; frame=sequence; ++captured;
    }
    Microsoft::WRL::ComPtr<ID3D11Device> device; context->GetDevice(&device);
    D3D11_TEXTURE2D_DESC desc{};image->GetDesc(&desc);
    const UINT mips=desc.MipLevels;
    const bool bgra=desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || desc.Format==DXGI_FORMAT_B8G8R8A8_TYPELESS;
    if (!bgra && desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && desc.Format!=DXGI_FORMAT_R8G8B8A8_TYPELESS) return;
    if (slice>=desc.ArraySize) return;
    desc.Format=bgra?DXGI_FORMAT_B8G8R8A8_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Width=width;desc.Height=height;desc.ArraySize=1;desc.MipLevels=1;
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc,nullptr,&staging))) return;
    D3D11_BOX box{0,0,0,width,height,1};
    context->CopySubresourceRegion(staging.Get(),0,0,0,0,image,D3D11CalcSubresource(0,slice,mips),&box);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped))) return;
    const auto file=prefix+"-"+std::to_string(sequence)+"-s"+std::to_string(slice)+".ppm";
    std::ofstream output(file,std::ios::binary);
    output<<"P6\n"<<width<<" "<<height<<"\n255\n";
    std::string row(width*3,'\0');
    unsigned alphaMin=255,alphaMax=0,rgbMax=0;
    for (UINT y=0;y<height;++y) {
        auto* pixels=static_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch;
        for (UINT x=0;x<width;++x) {
            row[x*3]=pixels[x*4+(bgra?2:0)];row[x*3+1]=pixels[x*4+1];row[x*3+2]=pixels[x*4+(bgra?0:2)];
            alphaMin=(std::min)(alphaMin,unsigned(pixels[x*4+3]));alphaMax=(std::max)(alphaMax,unsigned(pixels[x*4+3]));
            rgbMax=(std::max)({rgbMax,unsigned(pixels[x*4]),unsigned(pixels[x*4+1]),unsigned(pixels[x*4+2])});
        }
        output.write(row.data(),row.size());
    }
    context->Unmap(staging.Get(),0);
    std::fprintf(stderr,"Refract diagnostic capture: %s rgbMax=%u alpha=%u..%u %s\n",file.c_str(),rgbMax,alphaMin,alphaMax,label?label:"");
}
#endif
