#include "controller_bindings.h"

#include <cstdlib>

int main() {
    using refract::host::ControllerInputSlot;
    using refract::host::ControllerProfile;
    using refract::host::controller_input_components;

    const auto leftIndex = controller_input_components(ControllerProfile::ValveIndex, 0);
    const auto rightIndex = controller_input_components(ControllerProfile::ValveIndex, 1);
    if (leftIndex[static_cast<std::size_t>(ControllerInputSlot::Primary)] != "x/click" ||
        leftIndex[static_cast<std::size_t>(ControllerInputSlot::Secondary)] != "y/click" ||
        rightIndex[static_cast<std::size_t>(ControllerInputSlot::Primary)] != "a/click" ||
        rightIndex[static_cast<std::size_t>(ControllerInputSlot::Secondary)] != "b/click" ||
        leftIndex[static_cast<std::size_t>(ControllerInputSlot::Squeeze)] != "squeeze/value" ||
        leftIndex[static_cast<std::size_t>(ControllerInputSlot::ThumbrestTouch)] != "thumbrest/touch" ||
        leftIndex[static_cast<std::size_t>(ControllerInputSlot::Menu)] != "") return EXIT_FAILURE;

    const auto leftTouch = controller_input_components(ControllerProfile::Touch, 0);
    if (leftTouch[static_cast<std::size_t>(ControllerInputSlot::Menu)] != "menu/click" ||
        leftTouch[static_cast<std::size_t>(ControllerInputSlot::Primary)] != "x/click") return EXIT_FAILURE;

    const auto rightVive = controller_input_components(ControllerProfile::Vive, 1);
    if (rightVive[static_cast<std::size_t>(ControllerInputSlot::Stick)] != "trackpad" ||
        rightVive[static_cast<std::size_t>(ControllerInputSlot::Squeeze)] != "squeeze/click" ||
        rightVive[static_cast<std::size_t>(ControllerInputSlot::Primary)] != "") return EXIT_FAILURE;

    return EXIT_SUCCESS;
}
