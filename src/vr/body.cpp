// The avatar: stands under the head, faces where the head faces, its arms reach the controllers (two bone IK) and
// its wrists turn with them; its head is left out, the player's eyes are in it. The gun is held at the right grip,
// pointing where the right hand aims. See body.h.
#include "vr/body.h"

#include <array>
#include <cstdlib>
#include <string>
#include <vector>

namespace vr {

using namespace math;

namespace {

constexpr float kPi = 3.14159265f;

struct Finger {
    int joints[3] = {-1, -1, -1};
    V3 axis[3];  // each joint's bend axis in its own frame
    bool thumb = false;
};

struct Arm {
    int upper = -1, lower = -1, wrist = -1;
    Q neutral;  // the wrist's global turn while the controller points ahead, model space
    V3 palm;    // wrist to the palm's centre, turned into the wrist's rest frame, metres of the model
    Finger fingers[5];
    V3 hold;    // what the closed hand holds, from the palm's centre: the same frame and measure
};

struct Rig {
    int model = -1;
    std::vector<M4> rest;   // global, model space
    std::vector<M4> local;  // to the parent
    int head = -1;
    float eye_height = 1.7f;
    Arm right, left;
    uint32_t parts = ~0u;  // without the head
};

int Find(const Model& m, const char* name) {
    for (size_t i = 0; i < m.joints.size(); i++)
        if (m.joints[i].name == name) return int(i);
    return -1;
}

// a rotation matrix from a direction and a second one made square to it
M4 Basis(V3 f, V3 n) {
    f = Normal(f);
    n = Normal(n - f * Dot(n, f));
    V3 c = Cross(f, n);
    M4 r;
    const V3 cols[3] = {f, n, c};
    for (int k = 0; k < 3; k++) {
        r.m[0][k] = cols[k].x;
        r.m[1][k] = cols[k].y;
        r.m[2][k] = cols[k].z;
    }
    return r;
}

// The hand, measured from the rest pose rather than assumed: where its fingers point, which way its palm faces,
// the axes its fingers close about. The model faces +z, its right hand on -x.
// neutral: holding a gun ahead the fingers point +z and the palm faces in
void Hand(const Model& m, const Rig& r, Arm& arm, bool right) {
    if (arm.wrist < 0) return;
    const char* side = right ? ".R" : ".L";
    const char* names[5] = {"Index", "Middle", "Ring", "Pinky", "Thumb"};
    for (int f = 0; f < 5; f++) {
        arm.fingers[f].thumb = f == 4;
        for (int k = 0; k < 3; k++) {
            // the rig's first finger joints all sit at the palm's root: the knuckles are the second
            std::string name = std::string(names[f]) + char((f == 4 ? '1' : '2') + k) + side;
            arm.fingers[f].joints[k] = Find(m, name.c_str());
        }
    }
    auto at = [&](int j) { return Position(r.rest[size_t(j)]); };
    int index = arm.fingers[0].joints[0], middle = arm.fingers[1].joints[0], pinky = arm.fingers[3].joints[0];
    if (index < 0 || middle < 0 || pinky < 0) return;
    V3 wrist = at(arm.wrist);
    V3 fingers = Normal(at(middle) - wrist);
    V3 across = at(index) - at(pinky);  // pinky to index
    V3 palm = Normal(right ? Cross(across, fingers) : Cross(fingers, across));
    Q rest = Rotation(r.rest[size_t(arm.wrist)]);
    M4 from = Basis(fingers, palm);
    M4 to = Basis({0, 0, 1}, {right ? 1.0f : -1.0f, 0, 0});
    M4 from_t;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) from_t.m[i][j] = from.m[j][i];
    arm.neutral = Rotation(to * from_t) * rest;
    arm.palm = Rotate(Conj(rest), (at(middle) - wrist) * 0.6f);
    // a grip sits in the closed fingers: off the palm's face, a little ahead of its centre
    arm.hold = Rotate(Conj(rest), palm * 0.04f + fingers * 0.015f);
    for (int f = 0; f < 5; f++) {
        Finger& fin = arm.fingers[f];
        for (int k = 0; k < 3; k++) {
            int j = fin.joints[k];
            if (j < 0) continue;
            // towards the palm: about the finger's own direction crossed with the palm's
            int next = k < 2 ? fin.joints[k + 1] : -1;
            V3 along = next >= 0 ? Normal(at(next) - at(j)) : fingers;
            V3 axis = Normal(Cross(along, palm));
            if (fin.thumb) axis = Normal(Cross(along, Normal(palm - across * (1 / std::max(Length(across), 1e-6f)))));
            fin.axis[k] = Rotate(Conj(Rotation(r.rest[size_t(j)])), axis);
        }
    }
}

const Rig& TheRig() {
    static const Rig rig = [] {
        Rig r;
        r.model = ModelIndex("avatar_swat");
        if (r.model < 0) return r;
        const Model& m = Models()[size_t(r.model)];
        r.rest.resize(m.joints.size());
        r.local.resize(m.joints.size());
        for (size_t i = 0; i < m.joints.size(); i++) {
            M4 local = FromColumnMajor(m.joints[i].rest);
            r.local[i] = local;
            r.rest[i] = m.joints[i].parent >= 0 ? r.rest[size_t(m.joints[i].parent)] * local : local;
        }
        r.head = Find(m, "Head");
        if (r.head >= 0) r.eye_height = Position(r.rest[size_t(r.head)]).y + 0.08f;
        r.right = {Find(m, "UpperArm.R"), Find(m, "LowerArm.R"), Find(m, "Wrist.R")};
        r.left = {Find(m, "UpperArm.L"), Find(m, "LowerArm.L"), Find(m, "Wrist.L")};
        Hand(m, r, r.right, true);
        Hand(m, r, r.left, false);
        for (size_t i = 0; i < m.parts.size(); i++)
            if (m.parts[i].name.find("Head") != std::string::npos) r.parts &= ~(1u << i);
        return r;
    }();
    return rig;
}

// a joint set to a global transform; its children follow by their locals
void Set(const Model& m, std::vector<M4>& g, const std::vector<M4>& local, size_t j, const M4& to) {
    g[j] = to;
    for (size_t c = j + 1; c < m.joints.size(); c++) {
        int p = m.joints[c].parent;
        if (p < 0) continue;
        // a descendant of j: parents come before children in the file
        size_t a = size_t(p);
        while (a != j && m.joints[a].parent >= 0 && a > j) a = size_t(m.joints[a].parent);
        if (a == j) g[c] = g[size_t(p)] * local[c];
    }
}

M4 WithRotation(const M4& g, Q q) { return Compose(Position(g), q, Scale(g)); }  // the rig carries a scale

// the fingers closed by curl, 0 flat to 1 a fist: into the locals
void Curl(std::vector<M4>& local, const Arm& arm, float curl, float index_curl) {
    for (int f = 0; f < 5; f++) {
        const Finger& fin = arm.fingers[f];
        float c = f == 0 ? index_curl : curl;
        float angle = c * (fin.thumb ? 0.5f : 1.25f);
        for (int k = 0; k < 3; k++) {
            int j = fin.joints[k];
            if (j < 0) continue;
            M4 bend = Compose({0, 0, 0}, AxisAngle(fin.axis[k], k == 0 && !fin.thumb ? angle * 0.8f : angle));
            local[size_t(j)] = local[size_t(j)] * bend;
        }
    }
}

// the arm's three joints bring the palm to target, the wrist turned to wrist (model space)
void Reach(const Model& m, std::vector<M4>& g, const std::vector<M4>& local, const Arm& arm, V3 target, Q wrist,
           bool right, bool holds) {
    if (arm.upper < 0 || arm.lower < 0 || arm.wrist < 0) return;
    // the wrist sits behind the palm
    target = target - Rotate(wrist, arm.palm);
    if (holds) target = target - Rotate(wrist, arm.hold);
    V3 s = Position(g[size_t(arm.upper)]);
    V3 e = Position(g[size_t(arm.lower)]);
    V3 w = Position(g[size_t(arm.wrist)]);
    float a = Length(e - s), b = Length(w - e);
    V3 to = target - s;
    float d = std::clamp(Length(to), std::fabs(a - b) + 1e-3f, a + b - 1e-3f);
    V3 n = Normal(to);
    // the elbow bends down and out
    V3 pole = Normal(V3{right ? -0.6f : 0.6f, -1, -0.3f});
    V3 side = Normal(pole - n * Dot(pole, n));
    float cos_a = std::clamp((a * a + d * d - b * b) / (2 * a * d), -1.0f, 1.0f);
    V3 elbow = s + n * (a * cos_a) + side * (a * std::sqrt(1 - cos_a * cos_a));
    V3 hand = s + n * d;
    Q turn_upper = Between(e - s, elbow - s);
    Set(m, g, local, size_t(arm.upper), WithRotation(g[size_t(arm.upper)], turn_upper * Rotation(g[size_t(arm.upper)])));
    V3 e2 = Position(g[size_t(arm.lower)]);
    V3 w2 = Position(g[size_t(arm.wrist)]);
    Q turn_lower = Between(w2 - e2, hand - e2);
    Set(m, g, local, size_t(arm.lower), WithRotation(g[size_t(arm.lower)], turn_lower * Rotation(g[size_t(arm.lower)])));
    Set(m, g, local, size_t(arm.wrist), WithRotation(g[size_t(arm.wrist)], wrist));
}

}  // namespace

void PoseBody(const BodyInput& in, BodyOutput& out) {
    out.count = 0;
    const auto& models = Models();
    // the gun in the right hand: at the grip, pointing where the hand aims
    if (in.gun >= 0 && in.right_grip.valid && in.right_aim.valid) {
        ModelDraw& d = out.draws[out.count++];
        d.model = in.gun;
        // without the magazine when it is out of the gun
        const Model& g = models[size_t(in.gun)];
        d.parts = in.magazine || g.magazine < 0 ? ~0u : ~(1u << g.magazine);
        if (g.shell >= 0) d.parts &= ~(1u << g.shell);  // a shotgun's shell shows only in the hand
        M4 world = Compose(in.right_grip.pos, in.right_aim.rot, 1);
        std::memcpy(d.world, world.m, sizeof(d.world));
    }
    const Rig& rig = TheRig();
    if (rig.model < 0 || !in.head.valid) return;
    const Model& m = models[size_t(rig.model)];

    // the body under the head, turned with the head's yaw; the model faces +z, the player -z
    V3 fwd = Rotate(in.head.rot, {0, 0, -1});
    float yaw = std::atan2(-fwd.x, -fwd.z);
    Q body = AxisAngle({0, 1, 0}, yaw + kPi);
    float height = std::max(in.head.pos.y - in.floor, 0.5f);
    float scale = height / rig.eye_height;
    V3 head_in_model = rig.head >= 0 ? Position(rig.rest[size_t(rig.head)]) : V3{0, rig.eye_height, 0};
    V3 head_offset = Rotate(body, V3{head_in_model.x, 0, head_in_model.z + 0.06f} * scale);
    V3 root_pos{in.head.pos.x - head_offset.x, in.floor, in.head.pos.z - head_offset.z};
    M4 root = Compose(root_pos, body, scale);
    M4 to_model = RigidInverse(root);
    Q body_inv = Conj(body);

    std::vector<M4> g = rig.rest;
    std::vector<M4> local = rig.local;
    // the right hand closed on the gun, its index on the trigger; the left by what it holds
    Curl(local, rig.right, in.gun >= 0 ? 0.9f : 0.25f, in.gun >= 0 ? 0.45f + 0.35f * in.trigger : 0.25f);
    Curl(local, rig.left, in.left_curl, in.left_curl);
    // rest wrist turn to the controller's: model space
    Q to_controller = AxisAngle({0, 1, 0}, kPi);  // the controller pointing ahead, in model space, is turned this much
    static const bool no_ik = std::getenv("BLACK_VR_NOIK") != nullptr;  // debug: the rest pose
    if (!no_ik && in.right_grip.valid && in.right_aim.valid) {
        Q wrist = body_inv * in.right_aim.rot * Conj(to_controller) * rig.right.neutral;
        Reach(m, g, local, rig.right, Apply(to_model, in.right_grip.pos), wrist, true, in.gun >= 0);
    }
    if (!no_ik && in.left_grip.valid) {
        Q wrist = body_inv * in.left_grip.rot * Conj(to_controller) * rig.left.neutral;
        Reach(m, g, local, rig.left, Apply(to_model, in.left_grip.pos), wrist, false, false);
    }
    // the fingers, and anything else whose local changed, follow their wrists
    for (const Arm* arm : {&rig.right, &rig.left})
        if (arm->wrist >= 0) Set(m, g, local, size_t(arm->wrist), g[size_t(arm->wrist)]);
    size_t n = std::min<size_t>(m.joints.size(), kMaxBones);
    for (size_t j = 0; j < n; j++) {
        M4 skin = g[j] * FromColumnMajor(m.joints[j].inverse_bind);
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 4; c++) out.bones[j][r][c] = skin.m[r][c];
    }
    if (out.count < kMaxModelDraws) {
        ModelDraw& d = out.draws[out.count++];
        d.model = rig.model;
        d.parts = rig.parts;
        d.skinned = 1;
        std::memcpy(d.world, root.m, sizeof(d.world));
    }
}

}  // namespace vr
