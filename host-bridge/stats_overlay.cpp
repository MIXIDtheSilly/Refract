#include "stats_overlay.h"
#if defined(_WIN32)
#include <windows.h>

#include <algorithm>
#include <cmath>

namespace refract::host {
namespace {
constexpr float kCardOpacity = 0.78f;
constexpr float kCardGrey = 18.0f;  // The card's colour, 0-255.
}

StatsOverlay::~StatsOverlay()
{
    if (dc_) DeleteDC(static_cast<HDC>(dc_));
    if (bitmap_) DeleteObject(static_cast<HBITMAP>(bitmap_));
    if (font_) DeleteObject(static_cast<HFONT>(font_));
}

bool StatsOverlay::prepare(uint32_t width, uint32_t height, uint32_t rows)
{
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);  // Top-down.
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(nullptr);
    void* bits = nullptr;
    HBITMAP bitmap = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    const float pad = 0.06f * height;
    const int fontHeight = static_cast<int>((height - 2.0f * pad) / rows * 0.74f);
    // Grey antialiasing (not ClearType): the red, green and blue channels then all hold the coverage.
    HFONT font = CreateFontW(-fontHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (!dc || !bitmap || !font) {
        if (dc) DeleteDC(dc);
        if (bitmap) DeleteObject(bitmap);
        if (font) DeleteObject(font);
        return false;
    }
    SelectObject(dc, bitmap);
    SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    dc_ = dc; bitmap_ = bitmap; font_ = font;
    dib_ = static_cast<uint8_t*>(bits);
    width_ = width; height_ = height; rows_ = rows;

    // Rounded corners, antialiased.
    const float radius = 0.08f * height;
    card_.resize(static_cast<size_t>(width) * height);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const float dx = (std::max)(std::fabs(x + 0.5f - 0.5f * width) - (0.5f * width - radius), 0.0f);
            const float dy = (std::max)(std::fabs(y + 0.5f - 0.5f * height) - (0.5f * height - radius), 0.0f);
            const float distance = std::sqrt(dx * dx + dy * dy) - radius;
            card_[static_cast<size_t>(y) * width + x] = std::clamp(0.5f - distance, 0.0f, 1.0f) * kCardOpacity;
        }
    }
    pixels_.assign(static_cast<size_t>(width) * height * 4, 0);
    return true;
}

void StatsOverlay::draw(const std::vector<Line>& lines)
{
    if (!dc_) return;
    HDC dc = static_cast<HDC>(dc_);
    std::fill(dib_, dib_ + static_cast<size_t>(width_) * height_ * 4, uint8_t(0));
    const float pad = 0.06f * height_;
    const float rowHeight = (height_ - 2.0f * pad) / rows_;
    const size_t count = (std::min<size_t>)(lines.size(), rows_);
    for (size_t i = 0; i < count; ++i) {
        TextOutW(dc, static_cast<int>(1.4f * pad), static_cast<int>(pad + i * rowHeight),
                 lines[i].text.c_str(), static_cast<int>(lines[i].text.size()));
    }
    GdiFlush();

    for (uint32_t y = 0; y < height_; ++y) {
        const size_t row = rowHeight > 0 ? static_cast<size_t>((std::max)(0.0f, (y - pad) / rowHeight)) : 0;
        const uint32_t color = row < count ? lines[row].color : 0xEDEDED;
        const float red = float(color >> 16 & 0xff), green = float(color >> 8 & 0xff), blue = float(color & 0xff);
        for (uint32_t x = 0; x < width_; ++x) {
            const size_t index = static_cast<size_t>(y) * width_ + x;
            const float text = dib_[index * 4 + 1] / 255.0f;
            const float card = card_[index] * (1.0f - text);
            uint8_t* out = &pixels_[index * 4];
            out[0] = static_cast<uint8_t>(kCardGrey * card + red * text + 0.5f);
            out[1] = static_cast<uint8_t>(kCardGrey * card + green * text + 0.5f);
            out[2] = static_cast<uint8_t>(kCardGrey * card + blue * text + 0.5f);
            out[3] = static_cast<uint8_t>((card + text) * 255.0f + 0.5f);
        }
    }
}
}
#endif
