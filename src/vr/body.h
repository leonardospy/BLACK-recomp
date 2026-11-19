#pragma once
// The avatar's pose and the held gun, from the tracked head and hands (the headset's space, metres).
#include <cstdint>

#include "vr/math.h"
#include "vr/models.h"

namespace vr {

struct Tracked {
    bool valid = false;
    math::V3 pos;
    math::Q rot;
};

struct BodyInput {
    Tracked head;
    Tracked right_grip;  // the hand's palm
    Tracked right_aim;   // where it points (-z)
    Tracked left_grip;
    float floor = -1.65f;  // the floor's height
    int gun = -1;          // ModelIndex of the gun in the right hand, -1 none
    bool magazine = true;  // the magazine is in the gun
    float trigger = 0;     // the right trigger, 0 to 1: the index finger
    float left_curl = 0.25f;  // the left hand, 0 flat to 1 a fist
};

struct BodyOutput {
    ModelDraw draws[kMaxModelDraws];
    uint32_t count = 0;
    float bones[kMaxBones][3][4];  // the avatar's skinning palette, rows of joint space to model space
};

void PoseBody(const BodyInput& in, BodyOutput& out);

}  // namespace vr
