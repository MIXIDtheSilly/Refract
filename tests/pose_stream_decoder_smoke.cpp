#include "pose_stream_decoder.h"
#include "controller_input.h"
#include "space_velocity.h"
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>

int main()
{
    using namespace refract::protocol;
    ControllerInput input{1, PrimaryClick | TriggerTouch, 0.75f, 0.3f, -0.4f, 0.8f};
    if (controller_binding(input, "x/click").x != 1 || controller_binding(input, "b/click").x != 0 ||
        controller_binding(input, "trigger/value").x != 0.75f || controller_binding(input, "trigger/click").x != 1 ||
        controller_binding(input, "squeeze/click").x != 0 || controller_binding(input, "thumbstick").y != 0.8f ||
        controller_binding(input, "thumbstick/x").x != -0.4f || controller_binding(input, "unknown").active) return EXIT_FAILURE;
    input.active = 0;
    if (controller_binding(input, "trigger/value").active || controller_binding(input, "x/click").x != 0) return EXIT_FAILURE;
    std::array<PoseFrame, 3> frames{};
    for (size_t i = 0; i < frames.size(); ++i) {
        frames[i].sequence = i + 1;
        frames[i].display_period_ns = 13'888'889;
        frames[i].render_width = 2880; frames[i].render_height = 3200;
        frames[i].local_origin = {0.4f, 1.6f, -0.2f, 0, 0, 0, 1};
        frames[i].local_origin_flags = frames[i].hmd_flags = 15;
        frames[i].hmd.x = static_cast<float>(i + 10);
        frames[i].aim[0].x = static_cast<float>(i + 20);
        frames[i].aim[1].qx = 0.5f;
        frames[i].hands[1].active = 1; frames[i].hands[1].source = 2;
        frames[i].hands[1].joints[25] = {15, {0.1f, 0.2f, 0.3f}, 0.007f};
        frames[i].grip_flags[0] = 15; frames[i].aim_flags[1] = 3; frames[i].aim_active[1] = 1;
        frames[i].controllers[1] = {1, PrimaryClick, 0.75f, 0.5f, -0.2f, 0.3f};
        frames[i].grip_velocity[1] = {3, {1.5f, 0, 0}, {0, 2.0f, 0}};
    }
    // Every possible split of a TCP record must preserve the incomplete tail,
    // then drain coalesced records through to the newest pose.
    for (size_t split = 1; split < sizeof(PoseFrame); ++split) {
        PoseStreamDecoder decoder;
        PoseFrame latest{};
        const auto* data = reinterpret_cast<const unsigned char*>(frames.data());
        if (!decoder.append(data, split, latest) || latest.sequence != 0 ||
            !decoder.append(data + split, sizeof(frames) - split, latest) ||
            latest.sequence != 3 || latest.hmd.x != 12.0f || latest.display_period_ns != 13'888'889 ||
            latest.render_width != 2880 || latest.render_height != 3200 ||
            latest.local_origin.y != 1.6f || latest.local_origin_flags != 15 || latest.hmd_flags != 15 ||
            latest.aim[0].x != 22.0f || latest.aim[1].qx != 0.5f ||
            latest.hands[1].source != 2 || latest.hands[1].joints[25].flags != 15 ||
            latest.hands[1].joints[25].radius != 0.007f || latest.hands[1].joints[25].pose.z != 0.3f ||
            latest.grip_flags[0] != 15 || latest.aim_flags[1] != 3 || latest.aim_active[1] != 1 ||
            latest.controllers[1].buttons != PrimaryClick || latest.controllers[1].trigger != 0.75f ||
            latest.grip_velocity[1].flags != 3 || latest.grip_velocity[1].linear.x != 1.5f ||
            latest.grip_velocity[1].angular.y != 2.0f) { return EXIT_FAILURE; }
    }
    PoseStreamDecoder decoder;
    PoseFrame latest{};
    if (display_period_or_default(latest) != 11'111'111 || valid_display_period(100'000'000) ||
        valid_display_period(0) || !valid_display_period(13'888'889)) return EXIT_FAILURE;
    if (!decoder.append(&frames[0], 31, latest)) { return EXIT_FAILURE; }
    decoder.reset(); // Reconnect must discard the previous connection's tail.
    if (!decoder.append(&frames[1], sizeof(PoseFrame), latest) || latest.sequence != 2) { return EXIT_FAILURE; }
    auto bad = frames[0];
    bad.magic = 0;
    if (decoder.append(&bad, sizeof(bad), latest) || latest.sequence != 2) { return EXIT_FAILURE; }
    decoder.reset();
    auto legacy = frames[0];
    legacy.version = 1;
    if (!decoder.append(&legacy, 112, latest) || latest.sequence != 1 ||
        latest.controllers[1].active != 0 ||
        !decoder.append(&frames[2], sizeof(PoseFrame), latest) || latest.sequence != 3 ||
        latest.controllers[1].active != 1) { return EXIT_FAILURE; }
    decoder.reset();
    legacy.version = 2;
    if (!decoder.append(&legacy, 160, latest) || latest.version != 2 || latest.controllers[1].active != 1 ||
        latest.aim_active[1] != 0 || !decoder.append(&frames[2], sizeof(PoseFrame), latest) ||
        latest.version != kPoseFrameVersion || latest.aim[0].x != 22.0f) return EXIT_FAILURE;
    decoder.reset();
    legacy.version = 3;
    if (!decoder.append(&legacy, 2360, latest) || latest.version != 3 || latest.render_width || latest.render_height ||
        latest.display_period_ns != 13'888'889 || !decoder.append(&frames[2], sizeof(PoseFrame), latest) ||
        latest.render_width != 2880 || latest.version != kPoseFrameVersion) return EXIT_FAILURE;
    decoder.reset(); legacy.version = 4;
    if (!decoder.append(&legacy, 2368, latest) || latest.version != 4 || latest.local_origin_flags || latest.hmd_flags ||
        latest.local_origin.y != 0 || !decoder.append(&frames[2], sizeof(PoseFrame), latest) ||
        latest.version != kPoseFrameVersion || latest.local_origin.y != 1.6f) return EXIT_FAILURE;
    decoder.reset(); legacy.version = 5;  // v5 hosts: 2408 bytes, no velocities.
    if (!decoder.append(&legacy, 2408, latest) || latest.version != 5 || latest.local_origin.y != 1.6f ||
        !decoder.append(&frames[2], sizeof(PoseFrame), latest) || latest.version != kPoseFrameVersion ||
        latest.grip_velocity[1].linear.x != 1.5f) return EXIT_FAILURE;

    auto same = [](Vector3 a, Vector3 b) {
        return std::fabs(a.x - b.x) < 1e-5f && std::fabs(a.y - b.y) < 1e-5f && std::fabs(a.z - b.z) < 1e-5f;
    };
    // A point 1 m along +x from a controller spinning at 2 rad/s about +y moves at 2 m/s along -z.
    SpaceVelocity spin{3, {}, {0, 2.0f, 0}};
    if (!same(offset_velocity(spin, {1, 0, 0}).linear, {0, 0, -2.0f})) return EXIT_FAILURE;
    // Without angular velocity an offset point's linear velocity is unknown.
    if (offset_velocity({1, {1, 0, 0}, {}}, {1, 0, 0}).flags != 0) return EXIT_FAILURE;
    // Base turned 90 degrees about +y (its +x points along world -z): world +x motion is base +z motion...
    const float h = std::sqrt(0.5f);
    const Pose base{0, 0, 0, 0, h, 0, h}, still{};
    SpaceVelocity moving{3, {1, 0, 0}, {}}, fixed{3, {}, {}};
    if (!same(relative_velocity(moving, fixed, still, base).linear, {0, 0, 1})) return EXIT_FAILURE;
    // ...and a still space 1 m in front of a base spinning about +y sweeps past it, seen from the base.
    SpaceVelocity turning{3, {}, {0, 1.0f, 0}};
    const auto seen = relative_velocity(fixed, turning, Pose{0, 0, -1, 0, 0, 0, 1}, still);
    if (seen.flags != 3 || !same(seen.linear, {1, 0, 0}) || !same(seen.angular, {0, -1, 0})) return EXIT_FAILURE;
    // Nonfinite input is dropped, not passed on.
    if (clean_velocity({3, {NAN, 0, 0}, {}}).flags != 2) return EXIT_FAILURE;
    if (valid_render_extent(0, 3200) || valid_render_extent(8193, 3200) ||
        !valid_render_extent(2880, 3200)) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
