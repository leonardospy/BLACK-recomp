#pragma once
// VR through OpenXR. The game draws one eye a frame, in turn (alternate eye rendering): the renderer moves every
// perspective vertex from the game's camera to that eye's view and projection after the game's own transform, so
// fixed function and vertex programs alike go stereo. The head's yaw and pitch turn the player's view, so the game
// culls and aims where the player looks; roll, position and the eyes' offsets are the renderer's.
// Outside play (menus, films, pauses) the whole picture shows on a screen in front of the player.
//
// Threads: GameFrame and TakeLook on the game thread, the rest on the render thread.
#include <array>
#include <cstdint>

#include "vr/body.h"

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;

namespace vr {

// What a frame is drawn with, made by the game thread at the swap before it and taken by the render thread by the
// frame's number (presents before it)
struct Frame {
    float stereo[4][4];  // clip of the game's camera to clip of this frame's eye, by rows
    float flat[4][4];    // the same for the HUD, drawn flat in front of the eye
    float gun[4][4];     // the same for the gun and arms, moved from where the game holds them into the hand
    float rows[4][4];    // eye and in play; the eye's orientation; its position and the game's aspect; its fov angles
    float depth[2];      // the play camera's depth: view z = depth[0] / (ndc z - depth[1])
    float model_vp[4][4];  // the headset's space (metres) to this eye's clip, for the player's own models
    BodyOutput body;       // those models
    float laser[8] = {};   // the aiming laser in the headset's space: from xyz, 1 when shown, direction xyz
};

// started in VR (--vr, as BLACK_VR.exe starts BLACK.exe): VR from the start, no option
bool Wanted();

// the headset paces the frames (a session, not the debug head)
bool Paced();

// the headset session started
bool Active();

// the player's own avatar and guns are there (vr_assets): they replace the game's first person gun and arms
bool OwnModels();

// game thread, the look update: the pitch the game kept of what TakeLook last gave it, in degrees
void LookKept(float pitch);
// game thread: where the hand aims and where the shots start, from the game's camera in its units: right, up,
// ahead. False without a tracked hand
bool AimRay(float dir[3], float from[3]);

// game thread: a shot just left the gun (the muzzle flash shows)
void Fired();
// game thread, each frame of play: the player's view in the game's world (eye, and its right, up and forward)
void PlayerView(const float eye[3], const float right[3], const float up[3], const float forward[3]);
// game thread: shots fired so far (debug)
uint32_t Shots();
// game thread: the muzzle of the gun in the hand from the game's camera, in the game's units: to its right, up
// and ahead. False without a gun of the player's own in the hand
bool Muzzle(float& right, float& up, float& ahead);

// game thread: the rounds in the held gun, -1 unknown
int Rounds();
// game thread: sets them
void SetRounds(int n);
// game thread: the rounds the player carries for it (not in the gun), -1 unknown; and setting them
int Reserve();
void SetReserve(int n);
// game thread: where those are (guest address, 0 unknown); a word written there
uint32_t ReserveAt();
void WriteGuest16(uint32_t va, uint16_t v);
// game thread: the game reloaded by itself, this many rounds (debug)
void ReloadSeen(int loaded);
// game thread: the held weapon's data (guest address)
uint32_t HeldData();

// game thread: the gun model shown for the weapon held (models.h GunForCode), -1 for the game's own
int HeldGun();

// game thread, each frame of play: the weapon the player holds and its data (guest addresses)
void HeldWeapon(uint32_t weapon, uint32_t data);

// game thread, controller port 0: the Touch controllers as the console controller, over what pad already holds.
// Left stick moves, right stick turns (up and down: dpad), triggers fire and aim, A B X Y as on the pad, grips the
// black and white buttons, stick clicks the thumb buttons, menu is start, menu with a grip held is back
void ControllerPad(int16_t& lx, int16_t& ly, int16_t& rx, int16_t& ry, uint16_t& buttons, uint8_t (&analog)[8]);
// game thread: the game's vibration on the controllers, 0 to 65535 each side
void Rumble(uint16_t left, uint16_t right);

// game thread, right after the swap: frame number `frame`'s eye and matrices, from the game's projection (D3D, row
// vectors) and the latest head pose
void GameFrame(uint64_t frame, const float* game_projection, float game_aspect);
// render thread: frame number `frame`'s data; identity matrices and the screen when the game made none
Frame FrameFor(uint64_t frame);

// game thread, while the world is drawn (before its alpha list): the play camera's projection, D3D row vectors.
// The world drawn and the look update running recently is what play is
void SceneDrawn(const float* projection);

// game thread, from the look update: degrees the head turned since last taken, yaw positive left, and the head's
// pitch, positive down. False outside a session
bool TakeLook(float& yaw, float& pitch);

// degrees across a 4:3 frame the game's view must span so its culling covers an eye and the head's roll
uint32_t GameFov();
// the frame height that gives the game's view the headset's pixels, 0 outside a headset session
uint32_t EyeHeight();
// the test room: a grey room of the VR mode's own with the game's guns on its floor, shown in place of the game
bool RoomActive();
void SetRoom(bool on);
// game thread, in it: the gun in the hand (models.h), its rounds, set, filled by a new magazine
int RoomHeldGun();
int RoomRounds();
void RoomSetRounds(int n);
void RoomReload();
int RoomReserve();
void RoomSetReserve(int n);

// render thread: how far the laser goes before the scene stops it, metres (0 unknown)
void LaserHit(float metres);

// render thread
bool Start(ID3D12Device* dev, ID3D12CommandQueue* queue);  // false without a runtime or headset
void Stop();
// paces to the headset and locates the views; false: nothing goes to the headset this frame
bool BeginFrame();
// the frame's data rows as the game left them for the frame just drawn
void FrameDrawn(const float rows[4][4]);
// which picture the frame just drawn feeds: 0 or 1 an eye, 2 the screen
int DrawnView();
// the image to draw that picture into, in D3D12_RESOURCE_STATE_RENDER_TARGET, R8G8B8A8 (sRGB encoded values)
struct Image {
    ID3D12Resource* res = nullptr;
    uint32_t w = 0, h = 0;
};
Image Acquire(int view);
void Release(int view);
// ends the frame: the eyes as a projection layer in play, else the screen
void EndFrame();

}  // namespace vr
