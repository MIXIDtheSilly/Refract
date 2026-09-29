#pragma once
#if defined(_WIN32)
#include <cstdint>
#include <vector>

namespace refract::host {
// The Refract logo above a bar with a pill sliding back and forth, shown until the game's first frame
// arrives. Drawn on the CPU: the static card is composed once and, after the fade-in, only the bar
// strip changes between frames. The palette is neutral grey, so RGBA and BGRA textures take the same
// bytes.
class LoadingScreen {
public:
    struct Rect { uint32_t left, top, right, bottom; };

    // Lays the card out for a width x height image and decodes the embedded logo.
    void prepare(uint32_t width, uint32_t height);
    bool prepared() const { return width_ != 0; }
    // Draws the frame `seconds` after the screen appeared. While fading() every pixel changes;
    // afterwards only bar_rect() does.
    void animate(double seconds);
    bool fading() const { return fading_; }
    Rect bar_rect() const { return bar_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t stride() const { return width_ * 4; }
    const uint8_t* pixels() const { return pixels_.data(); }

private:
    uint32_t width_ = 0, height_ = 0;
    std::vector<float> base_;       // Card without the pill, grey levels 0-255.
    std::vector<uint8_t> pixels_;   // The current frame, RGBA.
    Rect bar_{};
    float trackX_ = 0, trackY_ = 0, trackHalfWidth_ = 0, trackRadius_ = 0, pillHalfWidth_ = 0;
    bool fading_ = true;
};
}
#endif
