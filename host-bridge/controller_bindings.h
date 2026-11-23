#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace refract::host {

enum class ControllerProfile {
    Touch,
    ValveIndex,
    Vive,
};

enum class ControllerInputSlot : std::size_t {
    Trigger,
    Squeeze,
    Stick,
    Primary,
    Secondary,
    Menu,
    StickPress,
    PrimaryTouch,
    SecondaryTouch,
    TriggerTouch,
    StickTouch,
    ThumbrestTouch,
    Count,
};

inline constexpr std::size_t kControllerInputCount = static_cast<std::size_t>(ControllerInputSlot::Count);
using ControllerInputComponents = std::array<std::string_view, kControllerInputCount>;

inline ControllerInputComponents controller_input_components(ControllerProfile profile, std::size_t hand) {
    if (hand > 1) return {};
    if (profile == ControllerProfile::Vive) {
        return {
            "trigger/value", "squeeze/click", "trackpad", "", "", "", "trackpad/click",
            "", "", "", "trackpad/touch", "",
        };
    }

    const bool left = hand == 0;
    const bool touch = profile == ControllerProfile::Touch;
    const bool index = profile == ControllerProfile::ValveIndex;
    return {
        "trigger/value",
        "squeeze/value",
        "thumbstick",
        left ? "x/click" : "a/click",
        left ? "y/click" : "b/click",
        touch && left ? "menu/click" : "",
        "thumbstick/click",
        left ? "x/touch" : "a/touch",
        left ? "y/touch" : "b/touch",
        "trigger/touch",
        "thumbstick/touch",
        (touch || index) ? "thumbrest/touch" : "",
    };
}

} // namespace refract::host
