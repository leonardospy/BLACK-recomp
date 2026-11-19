#pragma once
#include <cstdint>

// Ambient occlusion reaches the renderer as a clear with a flag no D3DCLEAR value uses, so the render thread keeps it
// in order with the draws. Its four rects carry the sixteen floats Occlusion sends
namespace gpu {
constexpr uint32_t kClearOcclusion = 0x100;
constexpr uint32_t kOcclusionRects = 4;
// VR: the player's own models (avatar, gun) go into the scene at that point too, the same way
constexpr uint32_t kClearVrModels = 0x200;
}  // namespace gpu

namespace d3d {
// m: the camera to screen matrix, row vectors, as SetTransform takes it; gtao: GTAO, else SSAO
void Occlusion(const float* m, bool gtao);
// VR: the player's own models drawn into the scene, before its alpha list
void VrModels();
}  // namespace d3d
