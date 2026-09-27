#pragma once
#include "pose_frame.h"
#include <cstdint>
#include <string_view>

namespace refract::protocol {
struct InputValue { bool active = false; float x = 0, y = 0; };

// Controller input components, resolved once from binding paths (see input_component).
enum class InputComponent : uint8_t {
    None, TriggerValue, TriggerClick, SqueezeValue, SqueezeClick, Stick, StickX, StickY,
    PrimaryClick, SecondaryClick, MenuClick, StickClick, PrimaryTouch, SecondaryTouch,
    TriggerTouch, StickTouch, ThumbrestTouch, Pose,
};

// `component` is the part after /user/hand/<side>/input/.
inline InputComponent input_component(std::string_view component) {
    using C = InputComponent;
    if (component == "trigger/value") return C::TriggerValue;
    if (component == "trigger/click") return C::TriggerClick;
    if (component == "squeeze/value") return C::SqueezeValue;
    if (component == "squeeze/click") return C::SqueezeClick;
    if (component == "thumbstick" || component == "trackpad") return C::Stick;
    if (component == "thumbstick/x" || component == "trackpad/x") return C::StickX;
    if (component == "thumbstick/y" || component == "trackpad/y") return C::StickY;
    if (component == "a/click" || component == "x/click" || component == "select/click") return C::PrimaryClick;
    if (component == "b/click" || component == "y/click") return C::SecondaryClick;
    if (component == "menu/click") return C::MenuClick;
    if (component == "thumbstick/click" || component == "trackpad/click") return C::StickClick;
    if (component == "a/touch" || component == "x/touch") return C::PrimaryTouch;
    if (component == "b/touch" || component == "y/touch") return C::SecondaryTouch;
    if (component == "trigger/touch") return C::TriggerTouch;
    if (component == "thumbstick/touch" || component == "trackpad/touch") return C::StickTouch;
    if (component == "thumbrest/touch") return C::ThumbrestTouch;
    if (component == "grip/pose" || component == "aim/pose") return C::Pose;
    return C::None;
}

inline InputValue controller_value(const ControllerInput& input, InputComponent component) {
    using C = InputComponent;
    if (!input.active) return {};
    const auto button = [&](uint32_t bit) { return InputValue{true, (input.buttons & bit) ? 1.0f : 0.0f, 0}; };
    switch (component) {
    case C::TriggerValue: return {true, input.trigger};
    case C::TriggerClick: return {true, input.trigger > 0.5f ? 1.0f : 0.0f};
    case C::SqueezeValue: return {true, input.squeeze};
    case C::SqueezeClick: return {true, input.squeeze > 0.5f ? 1.0f : 0.0f};
    case C::Stick: return {true, input.stick_x, input.stick_y};
    case C::StickX: return {true, input.stick_x};
    case C::StickY: return {true, input.stick_y};
    case C::PrimaryClick: return button(PrimaryClick);
    case C::SecondaryClick: return button(SecondaryClick);
    case C::MenuClick: return button(MenuClick);
    case C::StickClick: return button(StickClick);
    case C::PrimaryTouch: return button(PrimaryTouch);
    case C::SecondaryTouch: return button(SecondaryTouch);
    case C::TriggerTouch: return button(TriggerTouch);
    case C::StickTouch: return button(StickTouch);
    case C::ThumbrestTouch: return button(ThumbrestTouch);
    case C::Pose: return {true};
    case C::None: break;
    }
    return {};
}

inline InputValue controller_binding(const ControllerInput& input, std::string_view component) {
    return controller_value(input, input_component(component));
}
}
