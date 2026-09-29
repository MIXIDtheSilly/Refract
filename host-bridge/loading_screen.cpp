#include "loading_screen.h"
#if defined(_WIN32)
#include "refract_logo_png.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace refract::host {
namespace {
using Microsoft::WRL::ComPtr;

constexpr float kPi = 3.14159265f;
constexpr float kFadeSeconds = 0.6f;
constexpr float kSweepSeconds = 2.2f;  // One full back-and-forth of the pill.
constexpr float kForeground = 237.0f;  // The launcher's foreground grey.
constexpr float kTrack = 38.0f;

float smoothstep(float edge0, float edge1, float x)
{
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Signed distance from (x, y) to a horizontal capsule centred on (cx, cy).
float capsule_distance(float x, float y, float cx, float cy, float halfWidth, float radius)
{
    const float dx = (std::max)(std::fabs(x - cx) - (halfWidth - radius), 0.0f);
    return std::sqrt(dx * dx + (y - cy) * (y - cy)) - radius;
}

// Decodes the embedded logo to premultiplied RGBA scaled to size x size.
bool decode_logo(uint32_t size, std::vector<uint8_t>& out)
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        ComPtr<IWICBitmapScaler> scaler;
        out.resize(static_cast<size_t>(size) * size * 4);
        ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
             SUCCEEDED(factory->CreateStream(&stream)) &&
             SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(kRefractLogoPng), sizeof(kRefractLogoPng))) &&
             SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
             SUCCEEDED(decoder->GetFrame(0, &frame)) &&
             SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
             SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPRGBA, WICBitmapDitherTypeNone,
                 nullptr, 0.0, WICBitmapPaletteTypeCustom)) &&
             SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
             SUCCEEDED(scaler->Initialize(converter.Get(), size, size, WICBitmapInterpolationModeHighQualityCubic)) &&
             SUCCEEDED(scaler->CopyPixels(nullptr, size * 4, static_cast<UINT>(out.size()), out.data()));
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return ok;
}
}

void LoadingScreen::prepare(uint32_t width, uint32_t height)
{
    width_ = width;
    height_ = height;
    fading_ = true;
    base_.assign(static_cast<size_t>(width) * height, 0.0f);
    pixels_.assign(static_cast<size_t>(width) * height * 4, 255);

    const float h = static_cast<float>(height);
    const uint32_t logoSize = static_cast<uint32_t>(0.44f * h);
    const float logoX = 0.5f * width, logoY = 0.42f * h;
    trackX_ = 0.5f * width;
    trackY_ = 0.7f * h;
    trackHalfWidth_ = 0.15f * width;
    trackRadius_ = (std::max)(2.0f, 0.004f * width);
    pillHalfWidth_ = 0.3f * trackHalfWidth_;

    const float margin = 2.0f;  // Room for the antialiased edge.
    bar_.left = static_cast<uint32_t>((std::max)(0.0f, trackX_ - trackHalfWidth_ - margin));
    bar_.right = static_cast<uint32_t>((std::min)(static_cast<float>(width), trackX_ + trackHalfWidth_ + margin));
    bar_.top = static_cast<uint32_t>((std::max)(0.0f, trackY_ - trackRadius_ - margin));
    bar_.bottom = static_cast<uint32_t>((std::min)(h, trackY_ + trackRadius_ + margin));

    std::vector<uint8_t> logo;
    if (!decode_logo(logoSize, logo)) {
        std::fprintf(stderr, "Refract Loading: could not decode the logo; showing the bar only\n");
        logo.clear();
    }
    const int logoLeft = static_cast<int>(logoX) - static_cast<int>(logoSize / 2);
    const int logoTop = static_cast<int>(logoY) - static_cast<int>(logoSize / 2);

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            float v = 0.0f;
            const int lx = static_cast<int>(x) - logoLeft, ly = static_cast<int>(y) - logoTop;
            if (!logo.empty() && lx >= 0 && ly >= 0 && lx < static_cast<int>(logoSize) && ly < static_cast<int>(logoSize)) {
                const uint8_t* p = &logo[(static_cast<size_t>(ly) * logoSize + lx) * 4];
                v = p[0] * (kForeground / 255.0f) + v * (1.0f - p[3] / 255.0f);
            }
            const float track = std::clamp(0.5f - capsule_distance(px, py, trackX_, trackY_, trackHalfWidth_, trackRadius_), 0.0f, 1.0f);
            base_[static_cast<size_t>(y) * width + x] = v + (kTrack - v) * track;
        }
    }
}

void LoadingScreen::animate(double seconds)
{
    const float fade = smoothstep(0.0f, kFadeSeconds, static_cast<float>(seconds));
    // One last full pass once the fade ends, so the whole card sits at full brightness.
    const Rect area = fade < 1.0f || fading_ ? Rect{0, 0, width_, height_} : bar_;
    fading_ = fade < 1.0f;

    // Eased at both ends; the pill stretches while it moves fastest, mid-track.
    const float phase = static_cast<float>(std::fmod(seconds, static_cast<double>(kSweepSeconds))) / kSweepSeconds * 2.0f * kPi;
    const float travel = 0.5f - 0.5f * std::cos(phase);
    const float halfWidth = pillHalfWidth_ * (1.0f + 0.35f * std::fabs(std::sin(phase)));
    const float span = trackHalfWidth_ - pillHalfWidth_;
    const float pillX = trackX_ - span + 2.0f * span * travel;

    for (uint32_t y = area.top; y < area.bottom; ++y) {
        for (uint32_t x = area.left; x < area.right; ++x) {
            float v = base_[static_cast<size_t>(y) * width_ + x];
            if (x >= bar_.left && x < bar_.right && y >= bar_.top && y < bar_.bottom) {
                const float px = x + 0.5f, py = y + 0.5f;
                const float d = (std::max)(capsule_distance(px, py, pillX, trackY_, halfWidth, trackRadius_),
                    capsule_distance(px, py, trackX_, trackY_, trackHalfWidth_, trackRadius_));
                v += (kForeground - v) * std::clamp(0.5f - d, 0.0f, 1.0f);
            }
            const uint8_t level = static_cast<uint8_t>(std::clamp(v * fade + 0.5f, 0.0f, 255.0f));
            uint8_t* out = &pixels_[(static_cast<size_t>(y) * width_ + x) * 4];
            out[0] = out[1] = out[2] = level;
            out[3] = 255;
        }
    }
}
}
#endif
