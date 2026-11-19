#pragma once
// The VR player's own models: the avatar's body, which follows the head and the hands, and the guns held in the
// hand, from vr_assets\*.bvr beside the executable (tools/vr_assets.py makes them). The game thread poses them each
// frame (Pose, in the headset's space, metres); the renderer draws them into the scene with the frame's eye.
#include <cstdint>
#include <string>
#include <vector>

namespace vr {

struct ModelVertex {
    float pos[3];
    float normal[3];
    float colour[3];
    uint8_t joints[4];
    float weights[4];
    uint32_t part;
    float uv[2];  // into the model's texture
};
static_assert(sizeof(ModelVertex) == 68);

struct ModelPart {
    std::string name;
    uint32_t first;
    uint32_t count;
};

struct ModelJoint {
    std::string name;
    int32_t parent;
    float inverse_bind[16];  // column major, column vectors
    float rest[16];          // local to the parent
};

struct Model {
    std::string name;
    std::vector<ModelVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<ModelPart> parts;
    std::vector<ModelJoint> joints;
    // a gun's magazine: its part, its centre and the middle of its top (where it goes in), in the gun's space
    int magazine = -1;
    float magazine_centre[3] = {};
    float magazine_top[3] = {};
    float muzzle[3] = {};  // a gun's barrel end, in its space
    // the game's own guns (tools/game_guns.py): their texture, RGBA8 rows top down (sRGB values; alpha the gloss),
    // and the part that goes back on a shot (slide, bolt or pump) and how far
    std::vector<uint8_t> texture;
    uint32_t texture_w = 0, texture_h = 0;
    int slide = -1;
    float slide_travel = 0;
    // where the left hand holds a gun held with both hands (its fore end or pump), in the gun's space
    float fore[3] = {};
    bool two_handed = false;
    bool pump = false;  // the slide is a pump the left hand racks after each shot
    // how the game holds it (tools/game_anim.py): the right hand's place on it (the hands' right one is made on the
    // pistol) and the left hand's turn on its fore end; column major, in the gun's space
    float hand[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float fore_rot[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    int shell = -1;     // a shotgun's shell, loaded by hand one at a time: its part, and its middle at the loading port
    float shell_centre[3] = {};
};

// the gun model for the game's weapon code (weapon data +0x11E, "FTAK_"), -1 for the game's own
int GunForCode(const char* code);

// every model, loaded on first use; index by ModelIndex
const std::vector<Model>& Models();
int ModelIndex(const char* name);  // -1 without that model

constexpr int kMaxBones = 64;
constexpr int kMaxModelDraws = 48;

// one model drawn this frame: the model's space to the headset's (metres), by rows, column vectors; parts a mask of
// the model's parts to draw; skinned: Frame::bones is its palette
struct ModelDraw {
    int32_t model = -1;
    uint32_t parts = ~0u;
    uint32_t skinned = 0;
    float glow = 0;  // above 0: unlit, its colours times this (the muzzle flash)
    float world[4][4];
};

}  // namespace vr
