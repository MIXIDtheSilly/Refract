#pragma once

#include "pose_frame.h"

#include <cmath>

// Velocity math for xrLocateSpace (ported from AXRB-BS PR #9). Velocities are in the tracking world:
// linear m/s of the space's origin, angular rad/s.
namespace refract::protocol {

inline Vector3 add(Vector3 a, Vector3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vector3 subtract(Vector3 a, Vector3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vector3 scale(Vector3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vector3 cross(Vector3 a, Vector3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline bool finite(Vector3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
inline bool zero(Vector3 a) { return a.x == 0 && a.y == 0 && a.z == 0; }

// Rotates v by the pose's orientation (or its inverse). An invalid orientation gives zero.
inline Vector3 rotate(const Pose& p, Vector3 v, bool inverse = false)
{
    const float norm = std::sqrt(p.qx * p.qx + p.qy * p.qy + p.qz * p.qz + p.qw * p.qw);
    if (!std::isfinite(norm) || norm < 1e-6f) return {};
    const float s = (inverse ? -1.0f : 1.0f) / norm;
    const Vector3 q{p.qx * s, p.qy * s, p.qz * s};
    const Vector3 t = scale(cross(q, v), 2.0f);
    return add(v, add(scale(t, p.qw / norm), cross(q, t)));
}

// Keeps only the OpenXR validity bits and zeroes components that are invalid or not finite.
inline SpaceVelocity clean_velocity(SpaceVelocity v)
{
    v.flags &= 3;
    if (!finite(v.linear)) v.flags &= ~uint64_t{1};
    if (!finite(v.angular)) v.flags &= ~uint64_t{2};
    if (!(v.flags & 1)) v.linear = {};
    if (!(v.flags & 2)) v.angular = {};
    return v;
}

// Velocity of a point rigidly attached to a tracked space, offset from its origin by worldOffset
// (in the world frame): a rotating controller moves its offset points, too.
inline SpaceVelocity offset_velocity(SpaceVelocity v, Vector3 worldOffset)
{
    v = clean_velocity(v);
    if (!zero(worldOffset)) {
        if ((v.flags & 3) == 3) v.linear = add(v.linear, cross(v.angular, worldOffset));
        else v.flags &= ~uint64_t{1};
    }
    return clean_velocity(v);
}

// Velocity of space relative to base, expressed in base's frame: the derivative of inverse(base) * space,
// which includes base's rotation carrying the space around it, not only the difference of the velocities.
inline SpaceVelocity relative_velocity(SpaceVelocity space, SpaceVelocity base, const Pose& spacePose, const Pose& basePose)
{
    space = clean_velocity(space);
    base = clean_velocity(base);
    SpaceVelocity out{};
    if (space.flags & base.flags & 2) {
        out.flags |= 2;
        out.angular = rotate(basePose, subtract(space.angular, base.angular), true);
    }
    const Vector3 displacement{spacePose.x - basePose.x, spacePose.y - basePose.y, spacePose.z - basePose.z};
    if ((space.flags & base.flags & 1) && ((base.flags & 2) || zero(displacement))) {
        out.flags |= 1;
        out.linear = rotate(basePose, subtract(subtract(space.linear, base.linear), cross(base.angular, displacement)), true);
    }
    return clean_velocity(out);
}

} // namespace refract::protocol
