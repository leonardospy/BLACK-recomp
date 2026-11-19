// VR through OpenXR: the session, the swapchains and the frame loop on the render thread, the head's look and the
// stereo matrices on the game thread. See vr.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#define XR_USE_GRAPHICS_API_D3D12
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "audio/output.h"
#include "common/options.h"
#include "core/log.h"
#include "vr/vr.h"

namespace vr {

int DebugMode();  // below Start

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = 180.0f / kPi;
constexpr int kEyes = 2;
constexpr int kScreen = 2;  // the swapchain of the screen shown outside play
constexpr int kViews = 3;
constexpr float kScreenDistance = 2.0f;  // metres in front of the player
constexpr float kScreenWidth = 2.6f;
constexpr float kHudDistance = 1.5f;  // metres the HUD seems away, by the eyes' disparity
constexpr float kEyeHeight = 1.62f;   // metres above the floor of the head where play starts
constexpr auto kPlayTimeout = std::chrono::milliseconds(250);  // no look update this long: out of play

// --- math: column vectors, m[row][col]

struct Mat {
    float m[4][4] = {};
    static Mat Identity() {
        Mat r;
        for (int i = 0; i < 4; i++) r.m[i][i] = 1;
        return r;
    }
};

Mat operator*(const Mat& a, const Mat& b) {
    Mat r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

bool Invert(const Mat& in, Mat& out) {
    const float* m = &in.m[0][0];
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
              m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
              m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!std::isfinite(det) || std::fabs(det) < 1e-20f) return false;
    for (int i = 0; i < 16; i++) (&out.m[0][0])[i] = inv[i] / det;
    return true;
}

// rotation of a unit quaternion, 3x3 in a 4x4
Mat Rotation(const XrQuaternionf& q) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    Mat r = Mat::Identity();
    r.m[0][0] = 1 - 2 * (y * y + z * z);
    r.m[0][1] = 2 * (x * y - z * w);
    r.m[0][2] = 2 * (x * z + y * w);
    r.m[1][0] = 2 * (x * y + z * w);
    r.m[1][1] = 1 - 2 * (x * x + z * z);
    r.m[1][2] = 2 * (y * z - x * w);
    r.m[2][0] = 2 * (x * z - y * w);
    r.m[2][1] = 2 * (y * z + x * w);
    r.m[2][2] = 1 - 2 * (x * x + y * y);
    return r;
}

Mat Transposed3(const Mat& a) {
    Mat r = Mat::Identity();
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r.m[i][j] = a.m[j][i];
    return r;
}

Mat RotY(float a) {
    Mat r = Mat::Identity();
    r.m[0][0] = std::cos(a);
    r.m[0][2] = std::sin(a);
    r.m[2][0] = -std::sin(a);
    r.m[2][2] = std::cos(a);
    return r;
}

Mat RotX(float a) {
    Mat r = Mat::Identity();
    r.m[1][1] = std::cos(a);
    r.m[1][2] = -std::sin(a);
    r.m[2][1] = std::sin(a);
    r.m[2][2] = std::cos(a);
    return r;
}

// yaw (positive left) and pitch (positive up) of an orientation's forward, -z in OpenXR
void YawPitch(const XrQuaternionf& q, float& yaw, float& pitch) {
    Mat r = Rotation(q);
    float fx = -r.m[0][2], fy = -r.m[1][2], fz = -r.m[2][2];
    yaw = std::atan2(-fx, -fz);
    pitch = std::asin(std::clamp(fy, -1.0f, 1.0f));
}

void Store(const Mat& m, float (&out)[4][4]) { std::memcpy(out, m.m, sizeof(out)); }

// --- frames, from the game thread to the render thread by number



Frame Plain(float aspect) {
    Frame f{};
    Store(Mat::Identity(), f.stereo);
    Store(Mat::Identity(), f.flat);
    Store(Mat::Identity(), f.gun);
    f.rows[0][0] = float(kScreen);
    f.rows[2][3] = aspect;
    return f;
}

std::mutex g_frames_lock;
constexpr uint64_t kFrameSlots = 8;
Frame g_frames[kFrameSlots];
uint64_t g_frame_ids[kFrameSlots];  // the number each slot holds, +1 (0 empty)

void Publish(uint64_t frame, const Frame& f) {
    std::lock_guard lock(g_frames_lock);
    g_frames[frame % kFrameSlots] = f;
    g_frame_ids[frame % kFrameSlots] = frame + 1;
}

// --- the head, from the render thread to the game thread

struct Head {
    bool valid = false;
    XrPosef view{};          // the head, between the eyes
    XrView eyes[kEyes]{};
    bool hand_valid = false;
    XrPosef hand{};          // the right controller's aim: where it points, -z
    bool grip_valid[2] = {};
    XrPosef grip[2]{};       // the controllers' grips, left and right: where the palms are
};

std::mutex g_head_lock;
Head g_head;

// --- game thread

struct Game {
    int eye = 0;
    bool was_in_play = false;
    float last_yaw = 0;
    float pending_yaw = 0;  // degrees, positive left
    float pitch = 0;        // degrees, positive down
    float pitch_lo = -89, pitch_hi = 89;  // what the game lets the view pitch, learned from what it kept
    float written = 1000;   // the pitch last handed to the game, 1000 none
    float roll_up = 0;      // running mean of the aim pose's up while it points level: below 0 it is upside down
    XrVector3f origin{};    // the head's position on entering play
    float scene[16] = {};   // the play camera's projection, SceneDrawn's
    bool has_scene = false;
    std::chrono::steady_clock::time_point last_scene{};
    bool look = false;
    std::chrono::steady_clock::time_point last_look{};
};
Game g_game;
std::mutex g_look_lock;

// --- render thread

struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    uint32_t w = 0, h = 0;
    std::vector<XrSwapchainImageD3D12KHR> images;
    uint32_t acquired = 0;
};

struct Xr {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace local = XR_NULL_HANDLE;
    XrSpace view = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;
    Swapchain chains[kViews];
    XrFrameState frame{XR_TYPE_FRAME_STATE};
    bool in_frame = false;
    // the frame just drawn
    float drawn[4][4] = {};
    // what each eye's last image was drawn with
    bool has_image[kEyes] = {};
    XrPosef pose[kEyes]{};
    XrFovf fov[kEyes]{};
    bool screen_image = false;
    float screen_aspect = 16.0f / 9.0f;
    bool last_in_play = false;
    bool screen_placed = false;
    XrPosef screen_pose{};
};
Xr g_xr;
std::atomic<bool> g_active{false};

// --- the Touch controllers: actions synced on the render thread, read by the game thread

enum Hand { kLeft, kRight, kHands };

struct Controls {
    XrActionSet set = XR_NULL_HANDLE;
    XrAction stick = XR_NULL_HANDLE;    // vector2f, per hand
    XrAction trigger = XR_NULL_HANDLE;  // float, per hand
    XrAction grip = XR_NULL_HANDLE;     // float, per hand
    XrAction click = XR_NULL_HANDLE;    // bool, stick click per hand
    XrAction a = XR_NULL_HANDLE, b = XR_NULL_HANDLE, x = XR_NULL_HANDLE, y = XR_NULL_HANDLE;
    XrAction menu = XR_NULL_HANDLE;
    XrAction haptic = XR_NULL_HANDLE;  // per hand
    XrAction aim = XR_NULL_HANDLE;     // pose, per hand
    XrSpace aim_space = XR_NULL_HANDLE;  // the right hand's
    XrSpace aim_space_left = XR_NULL_HANDLE;  // the left hand's, the gun hand of a left handed player
    XrAction grip_pose = XR_NULL_HANDLE;  // pose, per hand
    XrAction trigger_touch = XR_NULL_HANDLE;  // bool, per hand: the index finger on the trigger
    XrAction thumb_touch = XR_NULL_HANDLE;    // bool, per hand: the thumb on the stick, a face button or the rest
    XrSpace grip_space[2] = {};           // left, right
    XrPath hand[kHands] = {};
};
Controls g_ctl;

// a left handed player (the menu's VR LEFT HANDED): the controllers trade places, so the gun is in the physical left
// hand and everything below reads the gun hand as kRight; the pouch moves to the right hip and the hands are drawn
// mirrored (a left glove on the gun, a right one free)
bool LeftHanded() { return game::Opt().vr_left_handed; }
int Physical(int hand) { return LeftHanded() ? kHands - 1 - hand : hand; }

// what the controllers held at the last sync (by role: kRight the gun hand)
struct Held {
    float stick[kHands][2] = {};
    float trigger[kHands] = {};
    float grip[kHands] = {};
    bool click[kHands] = {};
    bool trigger_touch[kHands] = {};
    bool thumb_touch[kHands] = {};
    bool a = false, b = false, x = false, y = false, menu = false;
};
std::mutex g_held_lock;
Held g_held;
std::atomic<uint32_t> g_rumble{0};  // left << 16 | right

// --- the magazine of the player's own gun, game thread (the pad and the frame)

enum MagState { kMagIn, kMagOut, kMagHeld };
struct Mag {
    MagState state = kMagIn;
    bool in_play = false;
    bool eject = false;      // A dropped it: the frame starts its fall from the gun
    bool prev_a = false;
    bool prev_grip = false;
    int reload = 0;          // pad polls the game's reload stays pressed, once the new magazine is in
    math::M4 drop;           // where the dropped magazine fell from, and when
    std::chrono::steady_clock::time_point dropped{};
    std::chrono::steady_clock::time_point out_since{};
    // the game's rounds: its reloads but ours undone (Rounds, SetRounds)
    uint32_t data = 0;       // the weapon data they are for
    int last_rounds = -1;
    int loading = 0;         // polls left of a reload a new magazine pressed
    uint32_t held_at = 0;    // the carried rounds held back while the gun is empty: where they were, how many
    int held = 0;
    int shells_in = 0;       // shells put into a shotgun, for the pad to load
    struct Lying {
        int model, part;
        math::M4 world;  // in the world (Anchor)
    } lying[12];
    int lying_count = 0;
    struct Falling {
        int model, part;
        math::M4 from;
        std::chrono::steady_clock::time_point since;
    } falling[6];
    int falling_count = 0;
};
std::mutex g_mag_lock;
Mag g_mag;

// a gun held with both hands, game thread: the left hand on its fore end, and a pump's state
struct TwoHands {
    bool on = false;            // the left hand holds the fore end
    math::V3 left;              // where the left hand is (the headset's space)
    math::V3 fore;              // the fore end's rest place, the pump forward
    math::Q rot;                // the gun's turn
    float pump = 0;             // metres the pump is back
    bool racked = false;        // the pump went back after the shot
};
TwoHands g_two;
std::atomic<bool> g_pump_needed{false};  // a pump gun fired: the pump must go back and forth before the next shot

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::atomic<bool> g_fire{false};     // the fire trigger as the game last got it
std::atomic<int64_t> g_flash_ms{0};  // the muzzle flash shows until then

#define XR_CHECK(call)                                                   \
    do {                                                                 \
        XrResult r_ = (call);                                            \
        if (XR_FAILED(r_)) {                                             \
            LOGE("vr: %s failed (%d)", #call, int(r_));                  \
            return false;                                                \
        }                                                                \
    } while (0)

bool CreateSwapchain(Swapchain& sc, uint32_t w, uint32_t h, int64_t format) {
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = format;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XR_CHECK(xrCreateSwapchain(g_xr.session, &ci, &sc.handle));
    uint32_t n = 0;
    XR_CHECK(xrEnumerateSwapchainImages(sc.handle, 0, &n, nullptr));
    sc.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    XR_CHECK(xrEnumerateSwapchainImages(sc.handle, n, &n,
                                        reinterpret_cast<XrSwapchainImageBaseHeader*>(sc.images.data())));
    sc.w = w;
    sc.h = h;
    return true;
}

// an sRGB format: the picture's values are sRGB encoded, written through a UNORM view of the typeless image
int64_t PickFormat() {
    uint32_t n = 0;
    xrEnumerateSwapchainFormats(g_xr.session, 0, &n, nullptr);
    std::vector<int64_t> formats(n);
    xrEnumerateSwapchainFormats(g_xr.session, n, &n, formats.data());
    for (int64_t want : {int64_t(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB), int64_t(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)}) {
        if (std::find(formats.begin(), formats.end(), want) != formats.end()) return want;
    }
    return formats.empty() ? int64_t(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) : formats[0];
}

bool CreateInstance() {
    // the headset's own audio device, when the runtime names it (Meta's does)
    uint32_t n = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
    std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data());
    bool audio = false;
    for (const auto& p : props) audio |= std::strcmp(p.extensionName, "XR_OCULUS_audio_device_guid") == 0;
    const char* ext[] = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME, "XR_OCULUS_audio_device_guid"};
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(ci.applicationInfo.applicationName, "Black XBE Recomp", XR_MAX_APPLICATION_NAME_SIZE - 1);
    ci.applicationInfo.applicationVersion = 1;
    std::strncpy(ci.applicationInfo.engineName, "XboxXBE-Recomp", XR_MAX_ENGINE_NAME_SIZE - 1);
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = audio ? 2 : 1;
    ci.enabledExtensionNames = ext;
    XR_CHECK(xrCreateInstance(&ci, &g_xr.instance));
    // the game's sound to the headset: XR_OCULUS_audio_device_guid's output endpoint
    using GetAudioGuid = XrResult(XRAPI_PTR*)(XrInstance, wchar_t*);
    GetAudioGuid get = nullptr;
    if (audio && XR_SUCCEEDED(xrGetInstanceProcAddr(g_xr.instance, "xrGetAudioOutputDeviceGuidOculus",
                                                    reinterpret_cast<PFN_xrVoidFunction*>(&get))) && get) {
        wchar_t id[128] = {};
        if (XR_SUCCEEDED(get(g_xr.instance, id)) && id[0]) {
            LOGR("vr: the headset's audio device %ls", id);
            audio::SetDevice(id);
        }
    }
    XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    // no headset yet is the usual wait before Link connects: the caller says so once
    XrResult r = xrGetSystem(g_xr.instance, &si, &g_xr.system);
    if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE) return false;
    XR_CHECK(r);
    return true;
}

bool CreateSession(ID3D12Device* dev, ID3D12CommandQueue* queue) {
    PFN_xrGetD3D12GraphicsRequirementsKHR requirements = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(g_xr.instance, "xrGetD3D12GraphicsRequirementsKHR",
                                   reinterpret_cast<PFN_xrVoidFunction*>(&requirements)));
    XrGraphicsRequirementsD3D12KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    XR_CHECK(requirements(g_xr.instance, g_xr.system, &req));
    LUID luid = dev->GetAdapterLuid();
    if (std::memcmp(&luid, &req.adapterLuid, sizeof(LUID)) != 0) {
        LOGW("vr: the headset runs on another GPU than the game's, the picture is copied between them");
    }
    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.device = dev;
    binding.queue = queue;
    XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO};
    ci.next = &binding;
    ci.systemId = g_xr.system;
    XR_CHECK(xrCreateSession(g_xr.instance, &ci, &g_xr.session));
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace.orientation.w = 1;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XR_CHECK(xrCreateReferenceSpace(g_xr.session, &rs, &g_xr.local));
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XR_CHECK(xrCreateReferenceSpace(g_xr.session, &rs, &g_xr.view));
    return true;
}

bool CreateSwapchains() {
    uint32_t n = 0;
    XrViewConfigurationView views[kEyes]{{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    XR_CHECK(xrEnumerateViewConfigurationViews(g_xr.instance, g_xr.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                               kEyes, &n, views));
    int64_t format = PickFormat();
    for (int e = 0; e < kEyes; e++) {
        if (!CreateSwapchain(g_xr.chains[e], views[e].recommendedImageRectWidth, views[e].recommendedImageRectHeight,
                             format))
            return false;
    }
    uint32_t sw = views[0].recommendedImageRectWidth;
    if (!CreateSwapchain(g_xr.chains[kScreen], sw, sw * 9 / 16, format)) return false;
    LOGR("vr: eyes %ux%u, format %lld", views[0].recommendedImageRectWidth, views[0].recommendedImageRectHeight,
         (long long)format);
    return true;
}

void PollEvents() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g_xr.instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* sc = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            g_xr.state = sc->state;
            if (sc->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(g_xr.session, &bi))) {
                    g_xr.running = true;
                    LOGR("vr: session started");
                }
            } else if (sc->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(g_xr.session);
                g_xr.running = false;
            } else if (sc->state == XR_SESSION_STATE_EXITING || sc->state == XR_SESSION_STATE_LOSS_PENDING) {
                g_xr.running = false;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            g_xr.running = false;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

XrPath Path(const char* s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(g_xr.instance, s, &p);
    return p;
}

bool MakeAction(XrAction& out, const char* name, const char* label, XrActionType type, bool hands) {
    XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
    std::strncpy(ai.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(ai.localizedActionName, label, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    ai.actionType = type;
    if (hands) {
        ai.countSubactionPaths = kHands;
        ai.subactionPaths = g_ctl.hand;
    }
    XR_CHECK(xrCreateAction(g_ctl.set, &ai, &out));
    return true;
}

// the action set and the Touch bindings, attached to the session
bool CreateActions() {
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(si.actionSetName, "gameplay", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    std::strncpy(si.localizedActionSetName, "Gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    XR_CHECK(xrCreateActionSet(g_xr.instance, &si, &g_ctl.set));
    g_ctl.hand[kLeft] = Path("/user/hand/left");
    g_ctl.hand[kRight] = Path("/user/hand/right");
    bool ok = MakeAction(g_ctl.stick, "stick", "Stick", XR_ACTION_TYPE_VECTOR2F_INPUT, true) &&
              MakeAction(g_ctl.trigger, "trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT, true) &&
              MakeAction(g_ctl.grip, "grip", "Grip", XR_ACTION_TYPE_FLOAT_INPUT, true) &&
              MakeAction(g_ctl.click, "stick_click", "Stick click", XR_ACTION_TYPE_BOOLEAN_INPUT, true) &&
              MakeAction(g_ctl.a, "a", "A", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
              MakeAction(g_ctl.b, "b", "B", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
              MakeAction(g_ctl.x, "x", "X", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
              MakeAction(g_ctl.y, "y", "Y", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
              MakeAction(g_ctl.menu, "menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
              MakeAction(g_ctl.haptic, "rumble", "Rumble", XR_ACTION_TYPE_VIBRATION_OUTPUT, true) &&
              MakeAction(g_ctl.aim, "aim", "Aim", XR_ACTION_TYPE_POSE_INPUT, true) &&
              MakeAction(g_ctl.grip_pose, "grip_pose", "Grip pose", XR_ACTION_TYPE_POSE_INPUT, true) &&
              MakeAction(g_ctl.trigger_touch, "trigger_touch", "Trigger touch", XR_ACTION_TYPE_BOOLEAN_INPUT, true) &&
              MakeAction(g_ctl.thumb_touch, "thumb_touch", "Thumb touch", XR_ACTION_TYPE_BOOLEAN_INPUT, true);
    if (!ok) return false;
    std::vector<XrActionSuggestedBinding> b = {
        {g_ctl.stick, Path("/user/hand/left/input/thumbstick")},
        {g_ctl.stick, Path("/user/hand/right/input/thumbstick")},
        {g_ctl.trigger, Path("/user/hand/left/input/trigger/value")},
        {g_ctl.trigger, Path("/user/hand/right/input/trigger/value")},
        {g_ctl.grip, Path("/user/hand/left/input/squeeze/value")},
        {g_ctl.grip, Path("/user/hand/right/input/squeeze/value")},
        {g_ctl.click, Path("/user/hand/left/input/thumbstick/click")},
        {g_ctl.click, Path("/user/hand/right/input/thumbstick/click")},
        {g_ctl.a, Path("/user/hand/right/input/a/click")},
        {g_ctl.b, Path("/user/hand/right/input/b/click")},
        {g_ctl.x, Path("/user/hand/left/input/x/click")},
        {g_ctl.y, Path("/user/hand/left/input/y/click")},
        {g_ctl.menu, Path("/user/hand/left/input/menu/click")},
        {g_ctl.haptic, Path("/user/hand/left/output/haptic")},
        {g_ctl.haptic, Path("/user/hand/right/output/haptic")},
        {g_ctl.aim, Path("/user/hand/left/input/aim/pose")},
        {g_ctl.aim, Path("/user/hand/right/input/aim/pose")},
        {g_ctl.grip_pose, Path("/user/hand/left/input/grip/pose")},
        {g_ctl.grip_pose, Path("/user/hand/right/input/grip/pose")},
        {g_ctl.trigger_touch, Path("/user/hand/left/input/trigger/touch")},
        {g_ctl.trigger_touch, Path("/user/hand/right/input/trigger/touch")},
        {g_ctl.thumb_touch, Path("/user/hand/left/input/thumbstick/touch")},
        {g_ctl.thumb_touch, Path("/user/hand/right/input/thumbstick/touch")},
        {g_ctl.thumb_touch, Path("/user/hand/left/input/thumbrest/touch")},
        {g_ctl.thumb_touch, Path("/user/hand/right/input/thumbrest/touch")},
        {g_ctl.thumb_touch, Path("/user/hand/left/input/x/touch")},
        {g_ctl.thumb_touch, Path("/user/hand/left/input/y/touch")},
        {g_ctl.thumb_touch, Path("/user/hand/right/input/a/touch")},
        {g_ctl.thumb_touch, Path("/user/hand/right/input/b/touch")},
    };
    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = Path("/interaction_profiles/oculus/touch_controller");
    sb.countSuggestedBindings = uint32_t(b.size());
    sb.suggestedBindings = b.data();
    XR_CHECK(xrSuggestInteractionProfileBindings(g_xr.instance, &sb));
    XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    at.countActionSets = 1;
    at.actionSets = &g_ctl.set;
    XR_CHECK(xrAttachSessionActionSets(g_xr.session, &at));
    XrActionSpaceCreateInfo asi{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    asi.action = g_ctl.aim;
    asi.subactionPath = g_ctl.hand[kRight];
    asi.poseInActionSpace.orientation.w = 1;
    XR_CHECK(xrCreateActionSpace(g_xr.session, &asi, &g_ctl.aim_space));
    asi.subactionPath = g_ctl.hand[kLeft];
    XR_CHECK(xrCreateActionSpace(g_xr.session, &asi, &g_ctl.aim_space_left));
    for (int i = 0; i < kHands; i++) {
        asi.action = g_ctl.grip_pose;
        asi.subactionPath = g_ctl.hand[i];
        XR_CHECK(xrCreateActionSpace(g_xr.session, &asi, &g_ctl.grip_space[i]));
    }
    return true;
}

float GetFloat(XrAction a, XrPath sub) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = sub;
    XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(xrGetActionStateFloat(g_xr.session, &gi, &s)) && s.isActive ? s.currentState : 0.0f;
}

bool GetBool(XrAction a, XrPath sub) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = sub;
    XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(xrGetActionStateBoolean(g_xr.session, &gi, &s)) && s.isActive && s.currentState;
}

// the controllers' state for the game thread, and the game's vibration out to them
void SyncControls() {
    Held h;
    if (g_xr.state == XR_SESSION_STATE_FOCUSED) {
        XrActiveActionSet active{g_ctl.set, XR_NULL_PATH};
        XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
        si.countActiveActionSets = 1;
        si.activeActionSets = &active;
        if (XR_SUCCEEDED(xrSyncActions(g_xr.session, &si))) {
            for (int i = 0; i < kHands; i++) {
                const XrPath from = g_ctl.hand[Physical(i)];  // role i's controller
                XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
                gi.action = g_ctl.stick;
                gi.subactionPath = from;
                XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
                if (XR_SUCCEEDED(xrGetActionStateVector2f(g_xr.session, &gi, &v)) && v.isActive) {
                    h.stick[i][0] = v.currentState.x;
                    h.stick[i][1] = v.currentState.y;
                }
                h.trigger[i] = GetFloat(g_ctl.trigger, from);
                h.grip[i] = GetFloat(g_ctl.grip, from);
                h.click[i] = GetBool(g_ctl.click, from);
                h.trigger_touch[i] = GetBool(g_ctl.trigger_touch, from);
                h.thumb_touch[i] = GetBool(g_ctl.thumb_touch, from);
            }
            // A and B are the gun hand's buttons, X and Y the other's
            const bool swap = LeftHanded();
            h.a = GetBool(swap ? g_ctl.x : g_ctl.a, XR_NULL_PATH);
            h.b = GetBool(swap ? g_ctl.y : g_ctl.b, XR_NULL_PATH);
            h.x = GetBool(swap ? g_ctl.a : g_ctl.x, XR_NULL_PATH);
            h.y = GetBool(swap ? g_ctl.b : g_ctl.y, XR_NULL_PATH);
            h.menu = GetBool(g_ctl.menu, XR_NULL_PATH);
        }
        uint32_t r = g_rumble.load(std::memory_order_relaxed);
        float side[kHands] = {float(r >> 16) / 65535.0f, float(r & 0xFFFF) / 65535.0f};
        for (int i = 0; i < kHands; i++) {
            XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
            hi.action = g_ctl.haptic;
            hi.subactionPath = g_ctl.hand[Physical(i)];
            if (side[i] > 0.01f) {
                XrHapticVibration vib{XR_TYPE_HAPTIC_VIBRATION};
                vib.amplitude = std::min(side[i], 1.0f);
                vib.duration = 50'000'000;  // 50 ms, renewed each frame while the game rumbles
                vib.frequency = XR_FREQUENCY_UNSPECIFIED;
                xrApplyHapticFeedback(g_xr.session, &hi, reinterpret_cast<XrHapticBaseHeader*>(&vib));
            }
        }
    }
    std::lock_guard lock(g_held_lock);
    g_held = h;
}

void LocateHead(XrTime t) {
    Head h;
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    bool head = XR_SUCCEEDED(xrLocateSpace(g_xr.view, g_xr.local, t, &loc)) &&
                (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = t;
    li.space = g_xr.local;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    uint32_t n = 0;
    h.eyes[0] = h.eyes[1] = {XR_TYPE_VIEW};
    bool eyes = XR_SUCCEEDED(xrLocateViews(g_xr.session, &li, &vs, kEyes, &n, h.eyes)) && n == kEyes &&
                (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
    h.valid = head && eyes;
    h.view = loc.pose;
    if (g_ctl.aim_space) {
        XrSpaceLocation hand{XR_TYPE_SPACE_LOCATION};
        constexpr XrSpaceLocationFlags kBoth =
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
        XrSpace aim = LeftHanded() && g_ctl.aim_space_left ? g_ctl.aim_space_left : g_ctl.aim_space;
        h.hand_valid = XR_SUCCEEDED(xrLocateSpace(aim, g_xr.local, t, &hand)) &&
                       (hand.locationFlags & kBoth) == kBoth;
        h.hand = hand.pose;
        for (int i = 0; i < kHands; i++) {
            XrSpaceLocation grip{XR_TYPE_SPACE_LOCATION};
            h.grip_valid[i] = XR_SUCCEEDED(xrLocateSpace(g_ctl.grip_space[Physical(i)], g_xr.local, t, &grip)) &&
                              (grip.locationFlags & kBoth) == kBoth;
            h.grip[i] = grip.pose;
        }
    }
    std::lock_guard lock(g_head_lock);
    g_head = h;
}

// clip of a camera looking down +z (left handed, D3D) through fov, the z row and w row the game's
Mat EyeProjection(const XrFovf& f, const Mat& game) {
    float l = std::tan(f.angleLeft), r = std::tan(f.angleRight);
    float u = std::tan(f.angleUp), d = std::tan(f.angleDown);
    Mat p;
    p.m[0][0] = 2 / (r - l);
    p.m[0][2] = -(r + l) / (r - l);
    p.m[1][1] = 2 / (u - d);
    p.m[1][2] = -(u + d) / (u - d);
    for (int j = 0; j < 4; j++) {
        p.m[2][j] = game.m[2][j];
        p.m[3][j] = game.m[3][j];
    }
    return p;
}

}  // namespace

bool Wanted() {
    // BLACK_VR.exe starts BLACK.exe with --vr, which a relaunch keeps
    static const bool wanted = std::wcsstr(GetCommandLineW(), L"--vr") != nullptr;
    return wanted;
}

bool Active() { return g_active.load(std::memory_order_relaxed); }

bool Paced() { return Active() && !DebugMode(); }

// BLACK_VR_AUTOPLAY=seconds, for tests: start and A pressed in turn every 2 s, then A alone, that long from 8 s on:
// through the menus into the first mission. Works without VR too
void Autoplay(Held& h) {
    // BLACK_VR_AUTOFIRE=seconds: the fire trigger pulled for 300 ms every second from then on
    static const char* fire = std::getenv("BLACK_VR_AUTOFIRE");
    static const auto t0 = std::chrono::steady_clock::now();
    if (fire) {
        auto t = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (t > std::atoi(fire) * 1000 && t % 1000 < 300) h.trigger[kRight] = 1;
    }
    // BLACK_VR_AUTORELOAD: A pressed once the gun has been empty 3 s (the debug left hand then brings a magazine)
    static const bool autoreload = std::getenv("BLACK_VR_AUTORELOAD") != nullptr;
    if (autoreload) {
        static int64_t empty_since = 0;
        int64_t now = NowMs();
        if (Rounds() != 0) empty_since = 0;
        else if (!empty_since) empty_since = now;
        if (empty_since && now - empty_since > 3000 && now - empty_since < 3150) h.a = true;
    }
    static const char* until = std::getenv("BLACK_VR_AUTOPLAY");
    if (!until) return;
    static const auto start = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    // BLACK_VR_AUTOPLAY_STEP=ms: the presses' period (2000 by default; shorter gets into play sooner)
    static const int64_t step = std::getenv("BLACK_VR_AUTOPLAY_STEP") ? std::atoi(std::getenv("BLACK_VR_AUTOPLAY_STEP")) : 2000;
    static const int64_t from = step < 2000 ? 3000 : 8000;  // presses from then: the logos skip on one
    static const int64_t menu_until = from + 6 * step;
    // in play: no more presses (A would drop the magazine)
    static int64_t played = 0;
    if (g_mag.in_play && !played) played = ms;
    if (played && ms - played > 1000) return;
    if (ms > from && ms < std::atoi(until) * 1000 && ms % step < 150) {
        if (ms < menu_until && (ms / step) % 2) h.menu = true;
        else if (ms > 45000 && (ms / step) % 2) h.stick[kRight][1] = 1;  // the next weapon, now and then
        else h.a = true;
    }
}

// The Touch layout, over the game's controller actions (mouse_keyboard.cpp kActions):
//   left stick move, its click crouch; right stick left and right turn, up and down change weapon, its click the
//   firing mode or scope zoom; right trigger fire, left trigger grenade; A reload, B melee, X interact, Y health
//   pack; left grip the suppressor; menu pause, menu with the left grip the objectives.
// The game's aim (right stick click) is never pressed: the hand aims
namespace {
void RoomFire(float trigger);  // the test room, below
void GunTraits(const std::string& name, int& capacity, bool& automatic);
}  // namespace

void ControllerPad(int16_t& lx, int16_t& ly, int16_t& rx, int16_t& ry, uint16_t& buttons, uint8_t (&analog)[8]) {
    Held h;
    if (Active()) {
        std::lock_guard lock(g_held_lock);
        h = g_held;
    }
    Autoplay(h);
    // a gun with a magazine of its own: A drops the magazine instead of the game's reload, which the new magazine
    // going in presses; without a magazine in, the gun holds fire and the left grip is the hand's
    bool reload = false;
    if (Active()) {
        int gun = HeldGun();
        const Model* held_model = gun >= 0 ? &Models()[size_t(gun)] : nullptr;
        bool shells = held_model && held_model->magazine < 0 && held_model->shell >= 0;
        bool own = held_model && (held_model->magazine >= 0 || shells);
        std::lock_guard lock(g_mag_lock);
        if (own && g_mag.in_play) {
            if (h.a && !g_mag.prev_a && g_mag.state == kMagIn && !shells) {
                g_mag.state = kMagOut;
                g_mag.eject = true;
            }
            g_mag.prev_a = h.a;
            h.a = false;
            if (g_mag.state != kMagIn) {
                h.trigger[kRight] = 0;
                h.grip[kLeft] = 0;
            }
            // The game reloads an empty gun by itself: only a magazine going in may. A reload of its own is undone:
            // its rounds back to what the player carries
            int rounds = Rounds();
            uint32_t data = HeldData();
            if (data != g_mag.data) {
                // another weapon: what was held back of the last one's carried rounds goes back to it
                if (g_mag.held_at) WriteGuest16(g_mag.held_at, uint16_t(g_mag.held));
                g_mag.held_at = 0;
                g_mag.held = 0;
                g_mag.data = data;
                g_mag.last_rounds = rounds;
                g_mag.loading = 0;
            }
            // the gun empty and no magazine going in: the carried rounds are held back, so the game has none to
            // reload with (it would, again and again); they come back as the new magazine goes in
            if (rounds == 0 && !g_mag.loading && !g_mag.held_at && !RoomActive()) {
                int carried = Reserve();
                if (carried > 0) {
                    g_mag.held_at = ReserveAt();
                    g_mag.held = carried;
                    SetReserve(0);
                }
            }
            if (rounds > 0 && g_mag.last_rounds == 0 && !g_mag.loading && !RoomActive()) {
                ReloadSeen(rounds);
                int carried = Reserve();
                if (carried >= 0) {
                    SetReserve(carried + rounds);
                    SetRounds(0);
                    LOGR("vr: the game's own reload undone, %d rounds back to the %d carried", rounds, carried);
                    rounds = 0;
                }
            }
            // a shotgun's shells, one at a time, from what is carried, up to what its tube holds
            if (g_mag.shells_in > 0) {
                int capacity;
                bool automatic;
                GunTraits(held_model->name, capacity, automatic);
                for (; g_mag.shells_in > 0; g_mag.shells_in--) {
                    int carried = g_mag.held_at ? g_mag.held : Reserve();
                    if (rounds >= capacity || carried <= 0) continue;
                    SetRounds(++rounds);
                    if (g_mag.held_at) g_mag.held--;
                    else SetReserve(carried - 1);
                }
                g_mag.last_rounds = rounds;  // its own loading, not the game's reload
            }
            // rounds in the gun again: what was held back is carried again
            if (rounds > 0 && g_mag.held_at && g_mag.last_rounds > 0) {
                WriteGuest16(g_mag.held_at, uint16_t(g_mag.held));
                g_mag.held_at = 0;
                g_mag.held = 0;
            }
            if (g_mag.loading && (rounds > g_mag.last_rounds || --g_mag.loading == 0)) g_mag.loading = 0;
            g_mag.last_rounds = rounds;
            // an empty gun, or a pump gun not racked: the trigger never reaches the game
            if (rounds == 0) h.trigger[kRight] = 0;
            if (g_mag.reload > 0) {
                // the new magazine: the carried rounds back, and the game's reload, its rounds go in when it ends
                // (in the test room, its own magazine)
                if (RoomActive()) {
                    RoomReload();
                    g_mag.reload = 0;
                } else {
                    if (g_mag.held_at) {
                        WriteGuest16(g_mag.held_at, uint16_t(g_mag.held));
                        g_mag.held_at = 0;
                        g_mag.held = 0;
                    }
                    g_mag.reload--;
                    g_mag.loading = 400;  // polls the game's reload may take
                    reload = true;
                }
            }
        } else {
            g_mag.state = kMagIn;
        }
        // a pump gun holds fire until the pump has gone back and forth after the last shot
        if (gun < 0 || !Models()[size_t(gun)].pump) g_pump_needed.store(false, std::memory_order_relaxed);
        else if (g_pump_needed.load(std::memory_order_relaxed)) h.trigger[kRight] = 0;
    }
    auto axis = [](float v) { return int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f)); };
    auto over = [](int16_t pad, int16_t vr) { return std::abs(vr) > std::abs(pad) ? vr : pad; };
    lx = over(lx, axis(h.stick[kLeft][0]));
    ly = over(ly, axis(h.stick[kLeft][1]));
    rx = over(rx, axis(h.stick[kRight][0]));
    if (Active()) ry = 0;  // the hand pitches the view
    constexpr float kPush = 0.7f;
    constexpr uint16_t kUp = 1, kDown = 2, kLeftPad = 4, kRightPad = 8, kStart = 0x10, kBack = 0x20,
                       kLeftThumb = 0x40, kRightThumb = 0x80;
    if (h.stick[kRight][1] > kPush) buttons |= kRightPad;   // weapon up
    if (h.stick[kRight][1] < -kPush) buttons |= kLeftPad;   // weapon down
    if (h.click[kRight]) buttons |= kUp;                    // firing mode, scope zoom
    if (h.y) buttons |= kDown;                              // health pack
    if (h.click[kLeft]) buttons |= kLeftThumb;              // crouch
    if (h.menu) buttons |= h.grip[kLeft] > 0.5f ? kBack : kStart;
    if (Active()) buttons &= uint16_t(~kRightThumb);        // never the game's aim
    auto press = [](uint8_t& a, bool on) { if (on) a = 0xFF; };
    auto pull = [](uint8_t& a, float v) { a = std::max(a, uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255))); };
    press(analog[0], h.a || reload);                        // reload, menus confirm
    press(analog[1], h.b);                                  // melee, menus back
    press(analog[2], h.x);                                  // interact
    press(analog[3], !h.menu && h.grip[kLeft] > 0.8f);      // suppressor
    pull(analog[6], h.trigger[kLeft]);                      // grenade
    pull(analog[7], h.trigger[kRight]);                     // fire
    g_fire.store(h.trigger[kRight] > 0.5f, std::memory_order_relaxed);
    // both stick clicks held a second and a half: into the test room, or out of it
    if (Active()) {
        static int64_t chord_since = 0;
        int64_t now = NowMs();
        bool chord = h.click[kLeft] && h.click[kRight];
        if (!chord) chord_since = 0;
        else if (!chord_since) chord_since = now;
        else if (chord_since > 0 && now - chord_since > 1500) {
            SetRoom(!RoomActive());
            chord_since = -1;  // once a hold
        }
    }
    // in the test room the game hears nothing; the trigger fires the room's gun
    if (RoomActive()) {
        RoomFire(h.trigger[kRight]);
        lx = ly = rx = ry = 0;
        buttons = 0;
        for (auto& a : analog) a = 0;
    }
}

void Rumble(uint16_t left, uint16_t right) {
    g_rumble.store(uint32_t(left) << 16 | right, std::memory_order_relaxed);
}

std::mutex g_muzzle_lock;
bool g_muzzle_valid = false;
float g_muzzle[3] = {};
bool g_aim_valid = false;
float g_aim[3] = {};
float g_hand[3] = {};
bool AimRay(float dir[3], float from[3]) {
    std::lock_guard lock(g_muzzle_lock);
    std::memcpy(dir, g_aim, sizeof(g_aim));
    std::memcpy(from, g_muzzle_valid ? g_muzzle : g_hand, sizeof(g_hand));
    return g_aim_valid;
}
bool Muzzle(float& right, float& up, float& ahead) {
    std::lock_guard lock(g_muzzle_lock);
    right = g_muzzle[0];
    up = g_muzzle[1];
    ahead = g_muzzle[2];
    return g_muzzle_valid;
}
// the player's view in the game's world, game thread (PlayerView): where dropped magazines are kept
struct PlayerViewState {
    bool valid = false;
    math::V3 eye, right, up, forward;
} g_view;
void PlayerView(const float eye[3], const float right[3], const float up[3], const float forward[3]) {
    g_view.valid = true;
    g_view.eye = {eye[0], eye[1], eye[2]};
    g_view.right = {right[0], right[1], right[2]};
    g_view.up = {up[0], up[1], up[2]};
    g_view.forward = {forward[0], forward[1], forward[2]};
}
std::atomic<float> g_laser_hit{0};  // metres the laser goes before the scene, the renderer's probe
void LaserHit(float metres) {
    g_laser_hit.store(metres, std::memory_order_relaxed);
    static int64_t said = 0;
    if (DebugMode() && NowMs() - said > 2000) {
        said = NowMs();
        LOGR("vr: laser probe %.2f m", metres);
    }
}
std::atomic<int64_t> g_shot_ms{0};  // the last shot: the gun kicks in the hand
std::atomic<uint32_t> g_shots{0};
uint32_t Shots() { return g_shots.load(std::memory_order_relaxed); }
void Fired() {
    int64_t now = NowMs();
    g_flash_ms.store(now + 60, std::memory_order_relaxed);
    g_shot_ms.store(now, std::memory_order_relaxed);
    g_shots.fetch_add(1, std::memory_order_relaxed);
    int gun = HeldGun();
    if (gun >= 0 && Models()[size_t(gun)].pump) g_pump_needed.store(true, std::memory_order_relaxed);
}

uint32_t GameFov() {
    // the widest eye's tangents, both sides, plus room for the roll: over the 4:3 frame's half width
    XrFovf f{};
    {
        std::lock_guard lock(g_head_lock);
        if (!g_head.valid) return 120;
        f = g_head.eyes[0].fov;
    }
    float half = std::max({std::fabs(std::tan(f.angleLeft)), std::fabs(std::tan(f.angleRight)),
                           std::fabs(std::tan(f.angleUp)), std::fabs(std::tan(f.angleDown))});
    // the game's degrees span a 4:3 frame's width: its height takes 3/4 of the tangent
    float deg = 2 * std::atan(half * 1.2f / 0.75f) * kDeg;
    return uint32_t(std::clamp(deg, 90.0f, 140.0f));
}

uint32_t EyeHeight() {
    if (!Active() || DebugMode()) return 0;
    // the game's frame is GameFov's, 1.2 times the eye's tangents: that many more rows
    return std::min(uint32_t(float(g_xr.chains[0].h) * 1.2f), 2880u);
}

void LookKept(float pitch) {
    std::lock_guard lock(g_look_lock);
    if (g_game.written > 999) return;
    // the game clamped what it was handed: that is its limit
    if (pitch < g_game.written - 0.05f) g_game.pitch_hi = std::min(g_game.pitch_hi, pitch);
    if (pitch > g_game.written + 0.05f) g_game.pitch_lo = std::max(g_game.pitch_lo, pitch);
}

bool TakeLook(float& yaw, float& pitch) {
    if (!Active()) return false;
    std::lock_guard lock(g_look_lock);
    g_game.last_look = std::chrono::steady_clock::now();
    LOG_FIRST(1, "vr: the look update runs");
    if (!g_game.look) return false;
    yaw = g_game.pending_yaw;
    pitch = std::clamp(g_game.pitch, g_game.pitch_lo, g_game.pitch_hi);
    g_game.written = pitch;
    g_game.pending_yaw = 0;
    return true;
}

// The magazine (or a shotgun's shell), under g_mag_lock. A gun with a magazine starts with one in; A drops it (the
// game's own reload never runs on its own: only a new magazine going in presses it); the left grip takes the spare
// from its pouch on the belt, there while rounds are carried, and it goes in when its top meets the gun's magazine
// well. A shotgun takes its shells one at a time from the pouch into its loading port. What is let go falls and lies
// on the floor, kept where it fell in the world (anchor: the headset's space to the world's and back). Draws the
// pouch's, the falling, the lying and the held
struct Anchor {
    std::function<math::M4(const math::M4&)> to_world, from_world;
};
void Magazine(const Model& gun, const BodyInput& in, BodyOutput& out, int carried, const Anchor& anchor) {
    using namespace math;
    using Clock = std::chrono::steady_clock;
    auto now = Clock::now();
    auto seconds = [&](Clock::time_point t) { return std::chrono::duration<float>(now - t).count(); };
    const bool shells = gun.magazine < 0 && gun.shell >= 0;
    const int part = shells ? gun.shell : gun.magazine;
    M4 gun_world = Compose(in.right_grip.pos, in.right_aim.rot);
    V3 centre = shells ? V3{gun.shell_centre[0], gun.shell_centre[1], gun.shell_centre[2]}
                       : V3{gun.magazine_centre[0], gun.magazine_centre[1], gun.magazine_centre[2]};
    V3 top = shells ? centre : V3{gun.magazine_top[0], gun.magazine_top[1], gun.magazine_top[2]};
    V3 well = Apply(gun_world, top);
    auto lie = [&](const M4& m) {
        if (g_mag.lying_count >= int(std::size(g_mag.lying))) {
            std::rotate(std::begin(g_mag.lying), std::begin(g_mag.lying) + 1, std::end(g_mag.lying));
            g_mag.lying_count--;
        }
        g_mag.lying[g_mag.lying_count++] = {in.gun, part, anchor.to_world(m)};
    };
    auto fall = [&](const M4& from) {
        if (g_mag.falling_count >= int(std::size(g_mag.falling))) return;
        g_mag.falling[g_mag.falling_count++] = {in.gun, part, from, now};
    };
    if (g_mag.eject) {
        g_mag.eject = false;
        fall(gun_world);
        g_mag.out_since = now;
    }
    bool grip;
    {
        std::lock_guard lock(g_held_lock);
        grip = g_held.grip[kLeft] > 0.6f;
    }
    Tracked left = in.left_grip;
    // the pouch: on the belt, front left (front right for a left handed player), turned with the head's heading
    V3 ahead = Rotate(in.head.rot, V3{0, 0, -1});
    ahead.y = 0;
    if (Length(ahead) < 1e-3f) ahead = V3{0, 0, -1};
    ahead = ahead * (1.0f / Length(ahead));
    V3 right{-ahead.z, 0, ahead.x};
    Q heading = AxisAngle({0, 1, 0}, std::atan2(-ahead.x, -ahead.z));
    V3 pouch = in.head.pos + right * (LeftHanded() ? 0.14f : -0.14f) + ahead * 0.10f + V3{0, -0.62f, 0};
    // the debug head's left hand goes to the pouch over a second after the drop, takes the magazine, and brings it
    // in over the next second
    int capacity = 0;
    bool automatic = false;
    GunTraits(gun.name, capacity, automatic);
    // (the debug hand loads shells once the gun is empty, until it is full)
    static bool debug_loading = false;
    if (Rounds() == 0) debug_loading = true;
    if (Rounds() >= capacity) debug_loading = false;
    bool waiting = shells ? g_mag.state != kMagHeld && carried > 0 && debug_loading : g_mag.state != kMagIn;
    if (DebugMode() && (waiting || g_mag.state == kMagHeld)) {
        float t = seconds(g_mag.out_since);
        grip = t > 1.0f;
        if (g_mag.state != kMagHeld) left.pos = left.pos + (pouch - left.pos) * std::clamp(t, 0.0f, 1.0f);
        if (g_mag.state == kMagHeld) {
            left.rot = in.right_aim.rot;
            V3 to = well - Rotate(left.rot, top - centre);
            float k = std::clamp(t - 1.0f, 0.0f, 1.0f);
            left.pos = left.pos + (to - left.pos) * k;
        }
    }
    bool edge = grip && !g_mag.prev_grip;
    g_mag.prev_grip = grip;
    // a new magazine (shell) only from the pouch, while rounds are carried, the hand on it as the grip closes
    bool can_take = carried > 0 && (shells ? g_mag.state != kMagHeld : g_mag.state == kMagOut);
    if (can_take && edge && left.valid && Length(left.pos - pouch) < 0.16f) {
        g_mag.state = kMagHeld;
    }
    auto add = [&](int model, int p, const M4& world) {
        if (model < 0 || out.count >= kMaxModelDraws) return;
        ModelDraw& d = out.draws[out.count++];
        d = ModelDraw{};
        d.model = model;
        d.parts = 1u << p;
        d.skinned = 0;
        std::memcpy(d.world, world.m, sizeof(d.world));
    };
    // the spare in its pouch, standing, its top up: only while there are rounds for it
    if (g_mag.state != kMagHeld && carried > 0) add(in.gun, part, Compose(pouch, heading) * Compose(centre * -1.0f, Q{}));
    if (g_mag.state == kMagHeld && left.valid) {
        M4 hand = Compose(left.pos, left.rot) * Compose(centre * -1.0f, Q{});
        if (!grip) {
            g_mag.state = shells ? kMagIn : kMagOut;  // let go: it falls, the rounds still carried
            fall(hand);
        } else if (Length(Apply(hand, top) - well) < (shells ? 0.07f : 0.09f)) {
            g_mag.state = kMagIn;
            if (shells) {
                g_mag.shells_in++;
                g_mag.out_since = now;
            } else {
                g_mag.reload = 6;
            }
        } else {
            add(in.gun, part, hand);
        }
    }
    // falling to the floor, then lying there
    for (int i = 0; i < g_mag.falling_count;) {
        auto& f = g_mag.falling[i];
        float t = seconds(f.since);
        M4 w = f.from;
        w.m[1][3] -= 4.9f * t * t;
        if (w.m[1][3] <= in.floor + 0.02f || t > 2.0f) {
            // on its side on the floor, turned as it fell
            V3 at{w.m[0][3], in.floor + 0.02f, w.m[2][3]};
            float yaw = std::atan2(-w.m[0][2], -w.m[2][2]);
            lie(Compose(at, AxisAngle({0, 1, 0}, yaw) * AxisAngle({0, 0, 1}, 1.5708f)) * Compose(centre * -1.0f, Q{}));
            std::copy(std::begin(g_mag.falling) + i + 1, std::begin(g_mag.falling) + g_mag.falling_count,
                      std::begin(g_mag.falling) + i);
            g_mag.falling_count--;
            continue;
        }
        add(f.model, f.part, w);
        i++;
    }
    for (int i = 0; i < g_mag.lying_count; i++) add(g_mag.lying[i].model, g_mag.lying[i].part, anchor.from_world(g_mag.lying[i].world));
}

// metres from the game's camera to the grip of the gun it draws, in the camera's right handed axes (x right, y up,
// -z ahead); BLACK_VR_GUN="x,y,z" tunes it
const float* GunOffset() {
    static const std::array<float, 3> offset = [] {
        std::array<float, 3> g{0.12f, -0.18f, -0.30f};
        if (const char* v = std::getenv("BLACK_VR_GUN")) std::sscanf(v, "%f,%f,%f", &g[0], &g[1], &g[2]);
        return g;
    }();
    return offset.data();
}

void SceneDrawn(const float* projection) {
    if (!Active()) return;
    std::lock_guard lock(g_look_lock);
    std::memcpy(g_game.scene, projection, sizeof(g_game.scene));
    g_game.has_scene = true;
    g_game.last_scene = std::chrono::steady_clock::now();
}

Frame FrameFor(uint64_t frame) {
    std::lock_guard lock(g_frames_lock);
    if (g_frame_ids[frame % kFrameSlots] == frame + 1) return g_frames[frame % kFrameSlots];
    return Plain(16.0f / 9.0f);
}

// --- the VR test room, game thread: a grey room of the player's own, drawn in place of the game while it waits in
// its menus; the game's guns lie on the floor to be taken with the right grip, shot (marks on the walls), emptied,
// reloaded from the belt and dropped. The room's space: metres, the floor at y 0, x right, -z ahead of where the
// player stood on entering it

namespace {

struct RoomGun {
    int model = -1;
    math::V3 pos;  // in the room
    math::Q rot;
    float fall = 0;  // metres a second down, while it falls
    int rounds = 0;
    int capacity = 0;
    int reserve = 0;  // carried for it: three magazines more
    bool automatic = false;
};

constexpr float kRoomX = 5.0f, kRoomBack = 5.0f, kRoomFront = -7.0f, kRoomHigh = 3.0f;
// the ring of a table around the player: its top's height and inner and outer radii; the guns on it 1 m apart
constexpr float kTableHigh = 0.9f, kTableIn = 1.75f, kTableOut = 2.35f;
constexpr int kHoles = 24;

struct Room {
    bool ready = false;
    math::V3 origin;  // the room's floor under the player, in the headset's space
    math::Q heading;  // the room's axes in the headset's space
    std::vector<RoomGun> guns;
    int held = -1;  // the gun in the right hand, an index into guns
    bool prev_grip = false;
    bool prev_trigger = false;
    bool prev_turn = false;
    int64_t next_shot = 0;
    math::V3 hit, hit_normal;  // where the laser meets the room, and the wall's normal (the headset's space)
    bool hit_valid = false;
    math::V3 holes[kHoles], hole_normals[kHoles];
    int hole_count = 0;
};
Room g_room_state;  // game thread
std::atomic<bool> g_room_on{std::getenv("BLACK_VR_ROOM") != nullptr};
std::atomic<bool> g_room_was{false};

// a gun's rounds and whether it fires while the trigger is held, by its model
void GunTraits(const std::string& name, int& capacity, bool& automatic) {
    static const struct {
        const char* name;
        int capacity;
        bool automatic;
    } kTraits[] = {
        {"g_pst", 15, false}, {"g_gk1", 17, false}, {"g_mgn", 6, false},  {"g_shg", 8, false}, {"g_rm1", 6, false},
        {"g_snr", 10, false}, {"g_smg", 32, true},  {"g_sm3", 32, true},  {"g_p90", 50, true}, {"g_mp1", 30, true},
        {"g_asr", 30, true},  {"g_m16", 30, true},  {"g_ak1", 30, true},
    };
    capacity = 15;
    automatic = false;
    for (const auto& t : kTraits)
        if (name == t.name) {
            capacity = t.capacity;
            automatic = t.automatic;
        }
}

math::V3 RoomToWorld(const math::V3& p) { return g_room_state.origin + math::Rotate(g_room_state.heading, p); }
math::V3 WorldToRoom(const math::V3& p) {
    math::Q inv{-g_room_state.heading.x, -g_room_state.heading.y, -g_room_state.heading.z, g_room_state.heading.w};
    return math::Rotate(inv, p - g_room_state.origin);
}

// the guns laid out on the floor ahead, side by side, barrels ahead, on their right sides
void LayOutRoom() {
    using namespace math;
    Room& r = g_room_state;
    r.guns.clear();
    const auto& models = Models();
    for (size_t i = 0; i < models.size(); i++) {
        if (models[i].name.rfind("g_", 0) != 0 || models[i].name == "g_hands") continue;
        RoomGun g;
        g.model = int(i);
        GunTraits(models[i].name, g.capacity, g.automatic);
        g.rounds = g.capacity;
        r.guns.push_back(g);
    }
    // round the ring table, a metre apart along it, ahead first, the barrels pointing away, on their sides
    const float radius = (kTableIn + kTableOut) / 2;
    for (size_t i = 0; i < r.guns.size(); i++) {
        float k = float(i) - float(r.guns.size() - 1) / 2;
        float angle = k * 1.0f / radius;  // a metre of arc each
        r.guns[i].pos = V3{std::sin(angle) * radius, kTableHigh + 0.04f, -std::cos(angle) * radius};
        r.guns[i].rot = AxisAngle({0, 1, 0}, -angle) * AxisAngle({0, 0, 1}, -1.5708f);
        r.guns[i].reserve = r.guns[i].capacity * 3;
    }
    r.held = -1;
    r.hole_count = 0;
    static const char* first = std::getenv("BLACK_VR_ROOM_GUN");  // debug: in the hand from the start
    if (first)
        for (size_t i = 0; i < r.guns.size(); i++)
            if (models[size_t(r.guns[i].model)].name == first) r.held = int(i);
}

// the laser's ray against the room's walls, floor and ceiling (the headset's space): metres, the point and normal
float RoomRay(const math::V3& from_w, const math::V3& dir_w, math::V3& hit, math::V3& normal) {
    using namespace math;
    V3 o = WorldToRoom(from_w);
    Q inv{-g_room_state.heading.x, -g_room_state.heading.y, -g_room_state.heading.z, g_room_state.heading.w};
    V3 d = Rotate(inv, dir_w);
    float best = 80;
    V3 n{};
    auto plane = [&](float o_k, float d_k, float at, V3 nn) {
        if (std::fabs(d_k) < 1e-6f) return;
        float t = (at - o_k) / d_k;
        if (t > 0.01f && t < best) {
            best = t;
            n = nn;
        }
    };
    plane(o.x, d.x, kRoomX, {-1, 0, 0});
    plane(o.x, d.x, -kRoomX, {1, 0, 0});
    plane(o.y, d.y, 0, {0, 1, 0});
    plane(o.y, d.y, kRoomHigh, {0, -1, 0});
    plane(o.z, d.z, kRoomBack, {0, 0, -1});
    plane(o.z, d.z, kRoomFront, {0, 0, 1});
    if (std::fabs(d.y) > 1e-6f) {
        float t = (kTableHigh - o.y) / d.y;
        V3 p = o + d * t;
        float rr = std::sqrt(p.x * p.x + p.z * p.z);
        if (t > 0.01f && t < best && rr > kTableIn && rr < kTableOut) {
            best = t;
            n = V3{0, d.y < 0 ? 1.0f : -1.0f, 0};
        }
    }
    hit = RoomToWorld(o + d * best);
    normal = Rotate(g_room_state.heading, n);
    return best;
}

}  // namespace

bool RoomActive() { return g_room_on.load(std::memory_order_relaxed); }
void SetRoom(bool on) { g_room_on.store(on, std::memory_order_relaxed); }

int RoomHeldGun() {
    const Room& r = g_room_state;
    return r.held >= 0 && r.held < int(r.guns.size()) ? r.guns[size_t(r.held)].model : -1;
}
int RoomRounds() {
    const Room& r = g_room_state;
    return r.held >= 0 && r.held < int(r.guns.size()) ? r.guns[size_t(r.held)].rounds : -1;
}
void RoomSetRounds(int n) {
    Room& r = g_room_state;
    if (r.held >= 0 && r.held < int(r.guns.size())) r.guns[size_t(r.held)].rounds = std::max(n, 0);
    if (DebugMode()) LOGR("vr: test room: %d rounds in the gun, %d carried", RoomRounds(), RoomReserve());
}
int RoomReserve() {
    const Room& r = g_room_state;
    return r.held >= 0 && r.held < int(r.guns.size()) ? r.guns[size_t(r.held)].reserve : 0;
}
void RoomSetReserve(int n) {
    Room& r = g_room_state;
    if (r.held >= 0 && r.held < int(r.guns.size())) r.guns[size_t(r.held)].reserve = std::max(n, 0);
}
// a new magazine: the dropped one's rounds were kept; it takes a full one from what is carried
void RoomReload() {
    Room& r = g_room_state;
    if (r.held >= 0 && r.held < int(r.guns.size())) {
        RoomGun& g = r.guns[size_t(r.held)];
        g.reserve += g.rounds;
        g.rounds = std::min(g.capacity, g.reserve);
        g.reserve -= g.rounds;
    }
    LOGR("vr: test room: a new magazine, %d rounds", RoomRounds());
}

namespace {

// pad thread side, ControllerPad: the trigger fires the held gun (once a pull, or on while held for automatics)
void RoomFire(float trigger) {
    Room& r = g_room_state;
    bool pulled = trigger > 0.5f;
    bool edge = pulled && !r.prev_trigger;
    r.prev_trigger = pulled;
    if (r.held < 0 || r.held >= int(r.guns.size()) || !pulled) return;
    RoomGun& g = r.guns[size_t(r.held)];
    int64_t now = NowMs();
    if (!(edge || (g.automatic && now >= r.next_shot))) return;
    if (g.rounds <= 0) {
        if (edge) LOGR("vr: test room: empty");
        if (edge) g_rumble.store(0x10001000u, std::memory_order_relaxed);  // the dry click
        return;
    }
    g.rounds--;
    if (DebugMode()) LOGR("vr: test room: shot, %d left", g.rounds);
    r.next_shot = now + 100;
    Fired();
    g_rumble.store(g.automatic ? 0x00004000u : 0x00009000u, std::memory_order_relaxed);  // the right hand's kick
    if (r.hit_valid) {
        int k = r.hole_count++ % kHoles;
        r.holes[k] = r.hit + r.hit_normal * 0.02f;  // in front of the targets on the wall
        r.hole_normals[k] = r.hit_normal;
    }
}

// GameFrame, in the room: takes and drops the guns, lays the fallen down, aims the laser at the walls, and adds the
// room, the guns on the floor and the marks to the frame's models
void RoomFrame(const Head& h, const BodyInput& body, Frame& f) {
    using namespace math;
    Room& r = g_room_state;
    if (!r.ready) {
        r.ready = true;
        float yaw, pitch;
        YawPitch(h.view.orientation, yaw, pitch);
        r.heading = AxisAngle({0, 1, 0}, yaw);
        r.origin = V3{h.view.position.x, body.floor, h.view.position.z};
        LayOutRoom();
    }
    // walking: the left stick moves the player (the room the other way) where the head faces, 1.6 m a second; the
    // right stick turns them 30 degrees a push, about where they stand
    {
        float sx, sy, turn;
        {
            std::lock_guard held_lock(g_held_lock);
            sx = g_held.stick[kLeft][0];
            sy = g_held.stick[kLeft][1];
            turn = g_held.stick[kRight][0];
        }
        float yaw, pitch;
        YawPitch(h.view.orientation, yaw, pitch);
        V3 ahead{-std::sin(yaw), 0, -std::cos(yaw)}, side{std::cos(yaw), 0, -std::sin(yaw)};
        float dt = 1.0f / 72.0f;
        if (std::fabs(sx) > 0.15f || std::fabs(sy) > 0.15f) r.origin = r.origin - (ahead * sy + side * sx) * (1.6f * dt);
        bool pushed = std::fabs(turn) > 0.7f;
        if (pushed && !r.prev_turn) {
            Q spin = AxisAngle({0, 1, 0}, turn > 0 ? 0.5236f : -0.5236f);
            V3 head{h.view.position.x, r.origin.y, h.view.position.z};
            r.origin = head + Rotate(spin, r.origin - head);
            r.heading = spin * r.heading;
        }
        r.prev_turn = pushed;
    }
    // the right hand takes the gun nearest its palm as the grip closes, and lets it go as the grip opens
    float grip;
    {
        std::lock_guard held_lock(g_held_lock);
        grip = g_held.grip[kRight];
    }
    if (DebugMode()) grip = r.held >= 0 ? 1.0f : 0.0f;
    bool closed = grip > 0.6f, open = grip < 0.35f;
    V3 palm = body.right_grip.pos;
    if (closed && !r.prev_grip && r.held < 0) {
        float nearest = 0.2f;
        for (size_t i = 0; i < r.guns.size(); i++) {
            float d = Length(RoomToWorld(r.guns[i].pos) - palm);
            if (d < nearest) {
                nearest = d;
                r.held = int(i);
            }
        }
        if (r.held >= 0) g_mag.state = kMagIn;  // a gun taken up has its magazine in (g_mag_lock is held)
    }
    if (open && r.held >= 0) {
        // dropped where the hand holds it: it falls to the floor
        RoomGun& g = r.guns[size_t(r.held)];
        g.pos = WorldToRoom(palm);
        Q inv{-r.heading.x, -r.heading.y, -r.heading.z, r.heading.w};
        g.rot = inv * body.right_aim.rot;
        g.fall = 0.01f;
        r.held = -1;
    }
    r.prev_grip = closed;
    // falling guns land and lie on their sides, turned as they fell
    float dt = 1.0f / 72.0f;
    for (auto& g : r.guns) {
        if (g.fall <= 0) continue;
        g.fall += 9.8f * dt;
        g.pos.y -= g.fall * dt;
        float rr = std::sqrt(g.pos.x * g.pos.x + g.pos.z * g.pos.z);
        float rest = rr > kTableIn && rr < kTableOut && g.pos.y > kTableHigh ? kTableHigh + 0.04f : 0.04f;
        if (g.pos.y <= rest) {
            g.pos.y = rest;
            g.fall = 0;
            V3 ahead = Rotate(g.rot, V3{0, 0, -1});
            float yaw = std::atan2(-ahead.x, -ahead.z);
            g.rot = AxisAngle({0, 1, 0}, yaw) * AxisAngle({0, 0, 1}, -1.5708f);
        }
    }
    // the laser against the room: how far, and where the shots mark
    if (f.laser[3] > 0.5f) {
        V3 from{f.laser[0], f.laser[1], f.laser[2]}, dir{f.laser[4], f.laser[5], f.laser[6]};
        V3 hit, n;
        float t = RoomRay(from, dir, hit, n);
        g_laser_hit.store(t, std::memory_order_relaxed);
        r.hit = hit;
        r.hit_normal = n;
        r.hit_valid = true;
    } else {
        r.hit_valid = false;
    }
    // the models: the room, the guns lying about, the marks
    auto add = [&](int model, const M4& world, uint32_t parts = ~0u, float glow = 0) {
        if (model < 0 || f.body.count >= kMaxModelDraws) return;
        ModelDraw& d = f.body.draws[f.body.count++];
        d = ModelDraw{};
        d.model = model;
        d.parts = parts;
        d.glow = glow;
        std::memcpy(d.world, world.m, sizeof(d.world));
    };
    add(ModelIndex("room"), Compose(r.origin, r.heading));
    for (size_t i = 0; i < r.guns.size(); i++) {
        if (int(i) == r.held) continue;
        const Model& gm = Models()[size_t(r.guns[i].model)];
        add(r.guns[i].model, Compose(RoomToWorld(r.guns[i].pos), r.heading * r.guns[i].rot), gm.shell >= 0 ? ~(1u << gm.shell) : ~0u);
    }
    int hole = ModelIndex("hole");
    for (int k = 0; k < std::min(r.hole_count, kHoles); k++)
        add(hole, Compose(r.holes[k], Between({0, 0, 1}, r.hole_normals[k])));
}

}  // namespace

void GameFrame(uint64_t frame, const float* game_projection, float game_aspect) {
    if (!Wanted()) return;
    Frame f = Plain(game_aspect);
    struct Out {
        uint64_t n;
        Frame& f;
        ~Out() { Publish(n, f); }
    } out{frame, f};
    if (!Active()) return;
    static const bool plain = std::getenv("BLACK_VR_PLAIN") != nullptr;  // debug: no stereo, to compare
    if (plain) return;

    Head h;
    {
        std::lock_guard lock(g_head_lock);
        h = g_head;
    }
    std::lock_guard lock(g_look_lock);
    auto now = std::chrono::steady_clock::now();
    // the test room: drawn in place of the game, as if in play, through a camera of its own (metres, 5 cm to 200 m)
    const bool room = RoomActive();
    if (room != g_room_was.exchange(room)) {
        g_room_state.ready = false;
        g_game.was_in_play = false;
        g_game.pending_yaw = 0;
        LOGR("vr: test room %s", room ? "entered" : "left");
    }
    static const float kRoomProjection[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 200.0f / 199.95f, 1, 0, 0, -0.05f * 200.0f / 199.95f, 0};
    bool in_play = h.valid && (room || (now - g_game.last_look < kPlayTimeout && now - g_game.last_scene < kPlayTimeout));
    // the play camera's projection as the world was drawn, else what the caller read
    if (room) game_projection = kRoomProjection;
    else if (g_game.has_scene) game_projection = g_game.scene;
    // the game's camera to clip, D3D row vectors: c = v M, here M transposed
    Mat game;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) game.m[j][i] = game_projection[i * 4 + j];
    bool perspective = game.m[3][2] == 1 && game.m[3][3] == 0;
    Mat game_inv;
    static int g_last_reason = -1;
    int reason = !h.valid ? 1 : !in_play ? 2 : !perspective ? 3 : !Invert(game, game_inv) ? 4 : 0;
    if (reason != g_last_reason) {
        g_last_reason = reason;
        LOGR("vr: frame %llu %s (projection %g %g %g %g / %g %g %g %g)", (unsigned long long)frame,
             reason == 0 ? "in play" : reason == 1 ? "no head" : reason == 2 ? "out of play" : reason == 3 ? "not perspective" : "no inverse",
             game_projection[0], game_projection[5], game_projection[8], game_projection[9], game_projection[10],
             game_projection[11], game_projection[14], game_projection[15]);
    }
    if (reason) {
        {
            std::lock_guard mag_lock(g_mag_lock);
            g_mag.in_play = false;
        }
        g_game.was_in_play = false;
        g_game.look = false;
        return;
    }

    // the debug head holds its hand where the game holds the gun
    if (DebugMode() && !h.hand_valid) {
        h.hand = h.view;
        Mat hr = Rotation(h.view.orientation);
        const float* g = GunOffset();
        for (int i = 0; i < 3; i++) {
            (&h.hand.position.x)[i] += hr.m[i][0] * g[0] + hr.m[i][1] * g[1] + hr.m[i][2] * g[2];
        }
        // BLACK_VR_HAND="yaw,pitch,x,y,z": the debug hand turned (degrees, left and up) and moved (metres) from there
        if (const char* v = std::getenv("BLACK_VR_HAND")) {
            float hy = 0, hp = 0, dx = 0, dy = 0, dz = 0;
            std::sscanf(v, "%f,%f,%f,%f,%f", &hy, &hp, &dx, &dy, &dz);
            float a = hy / kDeg / 2, b = hp / kDeg / 2;
            // yaw about y, then pitch about x: q = qy * qx
            XrQuaternionf qy{0, std::sin(a), 0, std::cos(a)}, qx{std::sin(b), 0, 0, std::cos(b)};
            h.hand.orientation = {qy.w * qx.x, qy.y * qx.w, -qy.y * qx.x, qy.w * qx.w};
            h.hand.position.x += dx;
            h.hand.position.y += dy;
            h.hand.position.z += dz;
        }
        h.hand_valid = true;
    }
    // Some runtimes hand the aim pose rolled half a turn (seen on Meta Link: the gun came out upside down). A hand
    // holds a gun upright nearly always: while it points level, the mean of its up says which way the pose is
    if (h.hand_valid) {
        Mat hr = Rotation(h.hand.orientation);
        if (std::fabs(hr.m[1][2]) < 0.6f) g_game.roll_up = g_game.roll_up * 0.98f + hr.m[1][1] * 0.02f;
        static int said = 0;
        bool flip = g_game.roll_up < -0.05f;
        if (said != (flip ? 2 : 1) && std::fabs(g_game.roll_up) > 0.2f) {
            said = flip ? 2 : 1;
            LOGR("vr: the aim pose is %s", flip ? "upside down, turned upright" : "upright");
        }
        if (flip) {
            // q * (half a turn about z)
            const XrQuaternionf q = h.hand.orientation;
            h.hand.orientation = {q.y, -q.x, q.w, -q.z};
        }
    }
    // A long gun with both hands: the left hand, closed on the fore end, takes it, and the gun then points from the right
    // hand through the left. A pump gun's pump follows the left hand back and forth along the barrel
    {
        using namespace math;
        int gi = HeldGun();
        const Model* g = gi >= 0 ? &Models()[size_t(gi)] : nullptr;
        float lgrip;
        {
            std::lock_guard held_lock(g_held_lock);
            lgrip = g_held.grip[kLeft];
        }
        bool can = g && g->two_handed && h.hand_valid && (h.grip_valid[0] || DebugMode());
        if (can) {
            const XrPosef& rg = h.grip_valid[1] ? h.grip[1] : h.hand;
            V3 rp{rg.position.x, rg.position.y, rg.position.z};
            const XrQuaternionf& o = h.hand.orientation;
            Q rq{o.x, o.y, o.z, o.w};
            V3 fore{g->fore[0], g->fore[1], g->fore[2]};
            V3 fore_w = rp + Rotate(rq, fore);
            V3 lp = h.grip_valid[0] ? V3{h.grip[0].position.x, h.grip[0].position.y, h.grip[0].position.z} : fore_w;
            // the debug head's left hand holds the fore end (BLACK_VR_TWOHAND), racking the pump after a shot
            static const bool debug_two = std::getenv("BLACK_VR_TWOHAND") != nullptr;
            if (DebugMode() && debug_two) {
                lgrip = 1;
                float t = float(NowMs() - g_shot_ms.load(std::memory_order_relaxed)) / 500.0f;
                float back = g->pump && t > 0.3f && t < 1.3f ? std::sin((t - 0.3f) * 3.14159f) * g->slide_travel : 0;
                lp = fore_w + Rotate(rq, V3{0, 0, back});
            }
            bool holding;
            {
                std::lock_guard mag_lock(g_mag_lock);
                holding = g_mag.state == kMagHeld;  // a hand with a magazine or shell in it holds nothing else
            }
            if (lgrip < 0.5f || holding) g_two.on = false;
            else if (!g_two.on && Length(lp - fore_w) < 0.15f) g_two.on = true;
            if (g_two.on) {
                V3 from = Rotate(rq, fore);
                V3 want = lp - rp;
                if (Length(want) > 0.1f && Length(from) > 0.05f) {
                    rq = Between(from * (1.0f / Length(from)), want * (1.0f / Length(want))) * rq;
                    h.hand.orientation = {rq.x, rq.y, rq.z, rq.w};
                }
                g_two.rot = rq;
                g_two.fore = rp + Rotate(rq, fore);
                g_two.left = lp;
                // the pump: how far back along the barrel the hand has it
                if (g->pump) {
                    V3 back = Rotate(rq, V3{0, 0, 1});
                    float d = (lp.x - g_two.fore.x) * back.x + (lp.y - g_two.fore.y) * back.y + (lp.z - g_two.fore.z) * back.z;
                    g_two.pump = std::clamp(d, 0.0f, g->slide_travel);
                    if (g_two.pump > 0.8f * g->slide_travel) g_two.racked = true;
                    if (g_two.racked && g_two.pump < 0.2f * g->slide_travel) {
                        g_two.racked = false;
                        if (g_pump_needed.exchange(false)) g_rumble.store(0x30003000u, std::memory_order_relaxed);
                    }
                }
            } else {
                g_two.pump = 0;
            }
        } else {
            g_two.on = false;
            g_two.pump = 0;
        }
    }
    // The head is the game's view, so the game draws and culls what the player looks at; the hand aims the gun, and
    // the shots leave along it (AimRay)
    const XrPosef& aim = h.hand_valid ? h.hand : h.view;
    float yaw, pitch;
    YawPitch(h.view.orientation, yaw, pitch);
    // no further than the game lets the view pitch, or the picture would tilt away from the head
    pitch = std::clamp(pitch, -g_game.pitch_hi / kDeg, -g_game.pitch_lo / kDeg);
    // the head's place on entering play is where the game's camera is: only moves from there lean the view
    if (!g_game.was_in_play) {
        g_game.last_yaw = yaw;
        g_game.origin = h.view.position;
    }
    g_game.pending_yaw += std::remainder((yaw - g_game.last_yaw) * kDeg, 360.0f);
    g_game.last_yaw = yaw;
    g_game.pitch = -pitch * kDeg;
    g_game.look = true;
    g_game.was_in_play = true;

    int e = g_game.eye;
    g_game.eye ^= 1;
    if (DebugMode() == 1 || DebugMode() == 2) e = DebugMode() - 1;
    const XrView& eye = h.eyes[e];
    // the camera the game draws with, in the headset's space: the head's yaw and pitch
    Mat camera = RotY(yaw) * RotX(pitch);
    // camera space to eye space, right handed: R_eye^T (C v - p s)
    float s = std::max(game::Opt().vr_world_scale, 1u) / 100.0f;
    Mat eye_rot_t = Transposed3(Rotation(eye.pose.orientation));
    Mat to_eye = eye_rot_t * camera;
    const XrVector3f& p = eye.pose.position;
    const XrVector3f& o = g_game.origin;
    float t[3] = {(p.x - o.x) * s, (p.y - o.y) * s, (p.z - o.z) * s};
    for (int i = 0; i < 3; i++) {
        to_eye.m[i][3] = -(eye_rot_t.m[i][0] * t[0] + eye_rot_t.m[i][1] * t[1] + eye_rot_t.m[i][2] * t[2]);
    }
    // left handed both sides: z flipped in and out
    Mat flip = Mat::Identity();
    flip.m[2][2] = -1;
    Mat eye_projection = EyeProjection(eye.fov, game);
    Mat stereo = eye_projection * flip * to_eye * flip * game_inv;
    Store(stereo, f.stereo);
    // the gun and arms, drawn in the game's camera space where the game holds them: moved into the hand.
    // world = R_hand (v - gun offset) + (hand - origin) s; eye = R_eye^T (world - (eye - origin) s)
    {
        Mat hand_rot = Rotation(aim.orientation);
        Mat to_gun = eye_rot_t * hand_rot;
        const float* g = GunOffset();
        float held[3];
        const XrVector3f& hp = aim.position;
        float rel[3] = {(hp.x - p.x) * s, (hp.y - p.y) * s, (hp.z - p.z) * s};
        for (int i = 0; i < 3; i++) {
            held[i] = rel[i] - (hand_rot.m[i][0] * g[0] + hand_rot.m[i][1] * g[1] + hand_rot.m[i][2] * g[2]) * s;
        }
        for (int i = 0; i < 3; i++) {
            to_gun.m[i][3] = eye_rot_t.m[i][0] * held[0] + eye_rot_t.m[i][1] * held[1] + eye_rot_t.m[i][2] * held[2];
        }
        Store(eye_projection * flip * to_gun * flip * game_inv, f.gun);
    }
    f.depth[0] = game_projection[14];
    f.depth[1] = game_projection[10];
    LOG_FIRST(4, "vr: game projection %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g", game_projection[0],
              game_projection[1], game_projection[2], game_projection[3], game_projection[4], game_projection[5],
              game_projection[6], game_projection[7], game_projection[8], game_projection[9], game_projection[10],
              game_projection[11], game_projection[12], game_projection[13], game_projection[14],
              game_projection[15]);

    // the HUD: a box of the eye's width share, the game's aspect, centred on the eye's forward
    float l = std::tan(eye.fov.angleLeft), r = std::tan(eye.fov.angleRight);
    float u = std::tan(eye.fov.angleUp), d = std::tan(eye.fov.angleDown);
    float size = std::clamp(game::Opt().vr_hud_size, 20u, 100u) / 100.0f;
    // x' = size x + cx w, y' = size_y y + cy w: the offset taken times w stays put after the divide
    Mat flat = Mat::Identity();
    flat.m[0][0] = size;
    flat.m[1][1] = size * (r - l) / (game_aspect * (u - d));
    // ... seen at kHudDistance: each eye sees it shifted against its offset from the head's centre
    Mat head_rot = Rotation(h.view.orientation);
    float side = (p.x - h.view.position.x) * head_rot.m[0][0] + (p.y - h.view.position.y) * head_rot.m[1][0] +
                 (p.z - h.view.position.z) * head_rot.m[2][0];
    flat.m[0][3] = -(r + l) / (r - l) - 2 * (side / kHudDistance) / (r - l);
    flat.m[1][3] = -(u + d) / (u - d);
    Store(flat, f.flat);

    const XrQuaternionf& q = eye.pose.orientation;
    const float rows[4][4] = {{float(e), 1, 0, 0},
                              {q.x, q.y, q.z, q.w},
                              {p.x, p.y, p.z, game_aspect},
                              {eye.fov.angleLeft, eye.fov.angleRight, eye.fov.angleUp, eye.fov.angleDown}};
    std::memcpy(f.rows, rows, sizeof(rows));

    // the player's own models: the headset's space (metres) to this eye's clip, through the game's units for the depth
    {
        Mat view = eye_rot_t;
        for (int i = 0; i < 3; i++) {
            view.m[i][3] = -(eye_rot_t.m[i][0] * p.x + eye_rot_t.m[i][1] * p.y + eye_rot_t.m[i][2] * p.z);
        }
        Mat units = Mat::Identity();
        units.m[0][0] = units.m[1][1] = units.m[2][2] = s;
        Store(eye_projection * flip * units * view, f.model_vp);
    }
    BodyInput body;
    auto tracked = [](const XrPosef& pose) {
        return Tracked{true, {pose.position.x, pose.position.y, pose.position.z},
                       {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w}};
    };
    body.head = tracked(h.view);
    body.right_aim = tracked(aim);
    body.right_grip = h.grip_valid[1] ? tracked(h.grip[1]) : tracked(aim);
    // the left hand on the fore end (or the pump, where it is) while it holds it
    const bool two_hands = g_two.on;
    const math::V3 two_left = g_two.fore + math::Rotate(g_two.rot, math::V3{0, 0, g_two.pump});
    const math::Q two_rot = g_two.rot;
    if (h.grip_valid[0]) {
        body.left_grip = tracked(h.grip[0]);
    } else if (DebugMode()) {
        // the debug head's left hand, low and ahead
        XrPosef left = h.view;
        Mat hr = Rotation(h.view.orientation);
        const float off[3] = {-0.18f, -0.35f, -0.30f};
        for (int i = 0; i < 3; i++) (&left.position.x)[i] += hr.m[i][0] * off[0] + hr.m[i][1] * off[1] + hr.m[i][2] * off[2];
        body.left_grip = tracked(left);
    }
    if (two_hands) body.left_grip = Tracked{true, two_left, two_rot};
    body.floor = g_game.origin.y - kEyeHeight;
    body.gun = HeldGun();
    const Model* gun = body.gun >= 0 ? &Models()[size_t(body.gun)] : nullptr;
    std::lock_guard mag_lock(g_mag_lock);
    g_mag.in_play = true;
    body.magazine = g_mag.state == kMagIn;
    body.left_curl = g_mag.state == kMagHeld ? 0.8f : 0.25f;
    {
        std::lock_guard held_lock(g_held_lock);
        body.trigger = g_held.trigger[kRight];
    }
    // the laser goes where the shots go: from the steady hand, before the kick
    const math::V3 steady_pos = body.right_grip.pos;
    const math::Q steady_rot = body.right_aim.rot;
    {
        using namespace math;
        float k = float(NowMs() - g_shot_ms.load(std::memory_order_relaxed)) / 110.0f;
        if (gun && k >= 0 && k < 1) {
            k = (1 - k) * (1 - k);
            body.right_grip.pos = body.right_grip.pos + Rotate(body.right_aim.rot, V3{0, 0.006f * k, 0.03f * k});
            body.right_aim.rot = body.right_aim.rot * AxisAngle({1, 0, 0}, 0.09f * k);
        }
    }
    PoseBody(body, f.body);
    if (gun && (gun->magazine >= 0 || gun->shell >= 0)) {
        using namespace math;
        // what is carried for it: the pouch is empty without; and where dropped ones are kept
        int carried = room ? Reserve() : g_mag.held_at ? g_mag.held : std::max(Reserve(), 0);
        Anchor anchor;
        if (room) {
            M4 to_room = RigidInverse(Compose(g_room_state.origin, g_room_state.heading));
            M4 from_room = Compose(g_room_state.origin, g_room_state.heading);
            anchor.to_world = [to_room](const M4& m) { return to_room * m; };
            anchor.from_world = [from_room](const M4& m) { return from_room * m; };
        } else if (g_view.valid) {
            // the headset's space to the game's world: through the camera's axes (headset) and the view's (world)
            V3 cx{camera.m[0][0], camera.m[1][0], camera.m[2][0]}, cy{camera.m[0][1], camera.m[1][1], camera.m[2][1]},
                cz{camera.m[0][2], camera.m[1][2], camera.m[2][2]};
            PlayerViewState v = g_view;
            V3 org{o.x, o.y, o.z};
            float scale = s;
            auto dot = [](V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
            auto vec_out = [=](V3 a) { return v.right * dot(cx, a) + v.up * dot(cy, a) + v.forward * -dot(cz, a); };
            auto vec_in = [=](V3 w) { return cx * dot(w, v.right) + cy * dot(w, v.up) + cz * -dot(w, v.forward); };
            auto map = [](const M4& m, auto vec, auto point) {
                M4 r = m;
                for (int c = 0; c < 3; c++) {
                    V3 col = vec(V3{m.m[0][c], m.m[1][c], m.m[2][c]});
                    r.m[0][c] = col.x;
                    r.m[1][c] = col.y;
                    r.m[2][c] = col.z;
                }
                V3 p = point(V3{m.m[0][3], m.m[1][3], m.m[2][3]});
                r.m[0][3] = p.x;
                r.m[1][3] = p.y;
                r.m[2][3] = p.z;
                return r;
            };
            anchor.to_world = [=](const M4& m) {
                return map(m, vec_out, [=](V3 p) { return v.eye + vec_out((p - org) * scale); });
            };
            anchor.from_world = [=](const M4& m) {
                return map(m, vec_in, [=](V3 w) { return org + vec_in(w - v.eye) * (1.0f / scale); });
            };
        } else {
            anchor.to_world = anchor.from_world = [](const M4& m) { return m; };
        }
        Magazine(*gun, body, f.body, carried, anchor);
    }
    // the game's own gloved hands (tools/game_hands.py) in place of the avatar's: the right one round the grip,
    // the left one at its controller. BLACK_VR_AVATAR keeps the avatar
    static const bool avatar = std::getenv("BLACK_VR_AVATAR") != nullptr;
    if (int hands = ModelIndex("g_hands"); hands >= 0 && !avatar) {
        using namespace math;
        uint32_t kept = 0;
        for (uint32_t i = 0; i < f.body.count; i++)
            if (!f.body.draws[i].skinned) f.body.draws[kept++] = f.body.draws[i];
        f.body.count = kept;
        // the fingers: the right index pulls with the trigger; the left hand opens and closes with its grip
        // (closed round a magazine it holds). Each finger joint turns about its local x, positive closing
        const Model& hm = Models()[size_t(hands)];
        // the left hand follows the controller as Touch games do: the grip curls the three fingers, the trigger the
        // index (lifted off it, it points), a thumb on a button or the stick lies down
        float trigger, left, left_index;
        bool index_on, thumb_on;
        {
            std::lock_guard held_lock(g_held_lock);
            trigger = g_held.trigger[kRight];
            left = g_held.grip[kLeft];
            left_index = g_held.trigger[kLeft];
            index_on = g_held.trigger_touch[kLeft];
            thumb_on = g_held.thumb_touch[kLeft];
        }
        if (DebugMode()) {
            trigger = g_fire.load(std::memory_order_relaxed) ? 1.0f : 0.0f;  // the debug fire
            index_on = thumb_on = true;
        }
        if (g_mag.state == kMagHeld) {
            left = std::max(left, 0.7f);
            left_index = std::max(left_index, 0.6f);
            index_on = thumb_on = true;
        }
        static const float kSegment[3] = {0.8f, 1.0f, 0.8f};
        M4 global[kMaxBones];
        for (size_t j = 0; j < hm.joints.size() && j < size_t(kMaxBones); j++) {
            const ModelJoint& jt = hm.joints[j];
            const std::string& n = jt.name;
            int seg = std::clamp(n.empty() ? 0 : n.back() - '0', 0, 2);
            float a = 0;
            if (n.rfind("R_index", 0) == 0) a = trigger * 0.35f * kSegment[seg];
            else if (n.rfind("L_index", 0) == 0) a = (index_on ? -0.2f + 1.25f * left_index : -0.35f) * kSegment[seg];
            else if (n.rfind("L_finger", 0) == 0) a = (-0.35f + 1.5f * left) * kSegment[seg];
            // the thumb curls over towards the fingertips (tools/game_hands.py): over the grip when it touches, at
            // rest (straight, not bent back) when lifted
            else if (n.rfind("L_thumb", 0) == 0) a = (thumb_on ? 0.35f : 0.0f) * kSegment[seg];
            M4 local = FromColumnMajor(jt.rest) * Compose(V3{}, AxisAngle({1, 0, 0}, a));
            global[j] = jt.parent >= 0 ? global[jt.parent] * local : local;
            M4 skin = global[j] * FromColumnMajor(jt.inverse_bind);
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 4; c++) f.body.bones[j][r][c] = skin.m[r][c];
        }
        auto put = [&](uint32_t part, const M4& base, const M4& local) {
            if (f.body.count >= kMaxModelDraws) return;
            ModelDraw& d = f.body.draws[f.body.count++];
            d = ModelDraw{};
            d.model = hands;
            d.parts = 1u << part;
            d.skinned = hm.joints.empty() ? 0 : 1;
            // a left handed player's: mirrored across the hand's own x (the gun's middle for the one on it), the
            // right glove becoming a left and the left a right, its place on the gun mirrored with it
            M4 m = base;
            if (LeftHanded())
                for (int r = 0; r < 3; r++) m.m[r][0] = -m.m[r][0];
            M4 w = m * local;
            std::memcpy(d.world, w.m, sizeof(d.world));
        };
        // each gun in the right hand as the game holds it, the left hand turned on the fore end as the game's
        const M4 identity = Compose(V3{}, Q{});
        M4 right_local = gun ? FromColumnMajor(gun->hand) : identity;
        M4 left_local = gun && two_hands ? FromColumnMajor(gun->fore_rot) : identity;
        if (body.right_grip.valid && body.right_aim.valid)
            put(0, Compose(body.right_grip.pos, body.right_aim.rot), right_local);
        if (body.left_grip.valid) put(1, Compose(body.left_grip.pos, body.left_grip.rot), left_local);
    }
    // the slide (bolt, pump) of a gun of the game's: back and home on each shot; a pump (a long travel) racked after it
    if (gun && gun->slide >= 0 && gun->slide_travel > 0) {
        using namespace math;
        float ms = float(NowMs() - g_shot_ms.load(std::memory_order_relaxed));
        bool pump = gun->slide_travel > 0.065f;
        float k = 0;
        if (gun->pump) {
            k = g_two.on ? g_two.pump / gun->slide_travel : 0;  // where the left hand has it
        } else if (pump) {
            float t = (ms - 180.0f) / 420.0f;
            if (t > 0 && t < 1) k = std::sin(t * 3.14159f);
        } else if (ms >= 0 && ms < 30) {
            k = ms / 30.0f;
        } else if (ms >= 30 && ms < 110) {
            k = 1 - (ms - 30) / 80.0f;
        }
        for (uint32_t i = 0; i < f.body.count; i++) {
            ModelDraw& d = f.body.draws[i];
            if (d.model != body.gun || !(d.parts >> gun->slide & 1) || f.body.count >= kMaxModelDraws) continue;
            d.parts &= ~(1u << gun->slide);
            ModelDraw& s = f.body.draws[f.body.count++];
            s = d;
            s.parts = 1u << gun->slide;
            M4 world;
            std::memcpy(world.m, d.world, sizeof(world.m));
            world = world * Compose(V3{0, 0, gun->slide_travel * k}, Q{});
            std::memcpy(s.world, world.m, sizeof(s.world));
            break;
        }
    }
    // the muzzle from the game's camera (the head's origin, turned as the hand aims), for the game's own effects
    {
        std::lock_guard muzzle_lock(g_muzzle_lock);
        {
            // where the hand aims, in the camera's axes: right, up, ahead
            Mat ar = Rotation(aim.orientation);
            float d[3] = {-ar.m[0][2], -ar.m[1][2], -ar.m[2][2]};
            g_aim[0] = camera.m[0][0] * d[0] + camera.m[1][0] * d[1] + camera.m[2][0] * d[2];
            g_aim[1] = camera.m[0][1] * d[0] + camera.m[1][1] * d[1] + camera.m[2][1] * d[2];
            g_aim[2] = -(camera.m[0][2] * d[0] + camera.m[1][2] * d[1] + camera.m[2][2] * d[2]);
            g_aim_valid = h.hand_valid;
            // the hand itself, for a gun that is the game's own
            float hd[3] = {(aim.position.x - o.x) * s, (aim.position.y - o.y) * s, (aim.position.z - o.z) * s};
            g_hand[0] = camera.m[0][0] * hd[0] + camera.m[1][0] * hd[1] + camera.m[2][0] * hd[2];
            g_hand[1] = camera.m[0][1] * hd[0] + camera.m[1][1] * hd[1] + camera.m[2][1] * hd[2];
            g_hand[2] = -(camera.m[0][2] * hd[0] + camera.m[1][2] * hd[1] + camera.m[2][2] * hd[2]);
        }
        g_muzzle_valid = gun != nullptr;
        if (gun) {
            using namespace math;
            V3 at = Apply(Compose(body.right_grip.pos, body.right_aim.rot), {gun->muzzle[0], gun->muzzle[1], gun->muzzle[2]});
            V3 d{(at.x - o.x) * s, (at.y - o.y) * s, (at.z - o.z) * s};
            // camera axes in the headset's space: its columns
            g_muzzle[0] = camera.m[0][0] * d.x + camera.m[1][0] * d.y + camera.m[2][0] * d.z;
            g_muzzle[1] = camera.m[0][1] * d.x + camera.m[1][1] * d.y + camera.m[2][1] * d.z;
            g_muzzle[2] = -(camera.m[0][2] * d.x + camera.m[1][2] * d.y + camera.m[2][2] * d.z);
        }
    }
    // the laser from the muzzle, instead of the HUD's aim mark
    static const bool no_laser = std::getenv("BLACK_VR_NOLASER") != nullptr;
    int laser = ModelIndex("laser");
    f.rows[0][2] = 0;
    if (gun && !no_laser && laser >= 0 && h.hand_valid && f.body.count < kMaxModelDraws) {
        using namespace math;
        ModelDraw& d = f.body.draws[f.body.count++];
        d.model = laser;
        d.parts = ~0u;
        d.skinned = 0;
        d.glow = 1.0f;
        M4 at = Compose(steady_pos, steady_rot) * Compose({gun->muzzle[0], gun->muzzle[1], gun->muzzle[2]}, Q{});
        // as far as the scene lets it (the renderer's probe, a few frames old), with a spot where it stops
        float hit = g_laser_hit.load(std::memory_order_relaxed);
        float length = hit > 0.05f && hit < 79.0f ? hit : 80.0f;
        M4 drawn = at;
        for (int r = 0; r < 3; r++) drawn.m[r][2] *= length / 80.0f;
        std::memcpy(d.world, drawn.m, sizeof(d.world));
        f.rows[0][2] = 1;
        V3 from{at.m[0][3], at.m[1][3], at.m[2][3]};
        V3 dir{-at.m[0][2], -at.m[1][2], -at.m[2][2]};
        const float laser[8] = {from.x, from.y, from.z, 1, dir.x, dir.y, dir.z, 0};
        std::memcpy(f.laser, laser, sizeof(laser));
        int dot = ModelIndex("dot");
        if (dot >= 0 && length < 79.0f && f.body.count < kMaxModelDraws) {
            ModelDraw& s = f.body.draws[f.body.count++];
            s = ModelDraw{};
            s.model = dot;
            s.glow = 1.2f;
            float k = 0.5f + length * 1.2f;  // about as big to the eye near and far (a third of a degree)
            M4 spot = Compose(from + dir * (length - 0.01f), Q{});
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++) spot.m[r][c] *= k;
            std::memcpy(s.world, spot.m, sizeof(s.world));
        }
    }
    if (room) RoomFrame(h, body, f);
    // the muzzle flash, turned about the barrel a little differently each frame
    static const bool always_flash = std::getenv("BLACK_VR_FLASH") != nullptr;  // debug
    int flash = ModelIndex("flash");
    if (gun && flash >= 0 && f.body.count < kMaxModelDraws && (always_flash || NowMs() < g_flash_ms.load(std::memory_order_relaxed))) {
        using namespace math;
        ModelDraw& d = f.body.draws[f.body.count++];
        d.model = flash;
        d.parts = ~0u;
        d.skinned = 0;
        d.glow = 1.6f;
        M4 at = Compose(body.right_grip.pos, body.right_aim.rot) * Compose({gun->muzzle[0], gun->muzzle[1], gun->muzzle[2]}, AxisAngle({0, 0, 1}, float(frame % 7) * 0.9f));
        std::memcpy(d.world, at.m, sizeof(d.world));
    }
}

// BLACK_VR_DEBUG set: no headset, a still head with Quest 3 like eyes, so the stereo shows on the desktop.
// BLACK_VR_DEBUG=1 left eye only, 2 right eye only, 3 both in turn
int DebugMode() {
    static const int mode = [] {
        const char* v = std::getenv("BLACK_VR_DEBUG");
        return v ? std::atoi(v) : 0;
    }();
    return mode;
}

bool Start(ID3D12Device* dev, ID3D12CommandQueue* queue) {
    if (DebugMode()) {
        Head h;
        h.valid = true;
        // BLACK_VR_HEAD="pitch": the debug head looks that many degrees up (negative down)
        float pitch = 0;
        if (const char* v = std::getenv("BLACK_VR_HEAD")) pitch = float(std::atof(v)) / kDeg;
        h.view.orientation = {std::sin(pitch / 2), 0, 0, std::cos(pitch / 2)};
        for (int e = 0; e < kEyes; e++) {
            h.eyes[e] = {XR_TYPE_VIEW};
            h.eyes[e].pose.orientation = h.view.orientation;
            h.eyes[e].pose.position.x = e ? 0.032f : -0.032f;
            float out = -0.942f, in = 0.698f;  // 54 and 40 degrees
            h.eyes[e].fov = {e ? -in : out, e ? -out : in, 0.768f, -0.873f};
            // BLACK_VR_THIRD="metres,degrees,height": the eyes stand that far from the head, that far around it
            // (0 ahead, 90 to its right) and above it, and look at its chest
            if (const char* v = std::getenv("BLACK_VR_THIRD")) {
                float dist = 2, around = 0, up = 0;
                std::sscanf(v, "%f,%f,%f", &dist, &around, &up);
                float a = around / kDeg;
                XrVector3f pos{std::sin(a) * dist, up, -std::cos(a) * dist};
                float fx = -pos.x, fy = -0.35f - pos.y, fz = -pos.z;
                float len = std::sqrt(fx * fx + fy * fy + fz * fz);
                float yw = std::atan2(-fx, -fz) / 2, pt = std::asin(fy / len) / 2;
                XrQuaternionf qy{0, std::sin(yw), 0, std::cos(yw)}, qx{std::sin(pt), 0, 0, std::cos(pt)};
                h.eyes[e].pose.orientation = {qy.w * qx.x, qy.y * qx.w, -qy.y * qx.x, qy.w * qx.w};
                h.eyes[e].pose.position = pos;
            }
        }
        std::lock_guard lock(g_head_lock);
        g_head = h;
        g_active = true;
        LOGR("vr: debug head, no headset (mode %d)", DebugMode());
        return true;
    }
    if (g_xr.instance) return true;
    if (!CreateInstance() || !CreateSession(dev, queue) || !CreateActions() || !CreateSwapchains()) {
        Stop();
        return false;
    }
    g_active = true;
    LOGR("vr: OpenXR session created");
    return true;
}

void Stop() {
    g_active = false;
    for (auto& sc : g_xr.chains) {
        if (sc.handle) xrDestroySwapchain(sc.handle);
        sc = {};
    }
    if (g_ctl.aim_space) xrDestroySpace(g_ctl.aim_space);
    if (g_ctl.aim_space_left) xrDestroySpace(g_ctl.aim_space_left);
    for (XrSpace s : g_ctl.grip_space)
        if (s) xrDestroySpace(s);
    if (g_ctl.set) xrDestroyActionSet(g_ctl.set);
    g_ctl = {};
    {
        std::lock_guard lock(g_held_lock);
        g_held = {};
    }
    if (g_xr.view) xrDestroySpace(g_xr.view);
    if (g_xr.local) xrDestroySpace(g_xr.local);
    if (g_xr.session) xrDestroySession(g_xr.session);
    if (g_xr.instance) xrDestroyInstance(g_xr.instance);
    g_xr = {};
    std::lock_guard lock(g_head_lock);
    g_head = {};
}

bool BeginFrame() {
    if (!g_xr.instance) return false;
    PollEvents();
    if (!g_xr.running) return false;
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    g_xr.frame = {XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(g_xr.session, &wi, &g_xr.frame))) return false;
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(xrBeginFrame(g_xr.session, &bi))) return false;
    g_xr.in_frame = true;
    // the actions first: the hand's pose is located from them
    SyncControls();
    LocateHead(g_xr.frame.predictedDisplayTime);
    return true;
}

void FrameDrawn(const float rows[4][4]) { std::memcpy(g_xr.drawn, rows, sizeof(g_xr.drawn)); }

int DrawnView() {
    bool play = g_xr.drawn[0][1] != 0;
    int e = int(g_xr.drawn[0][0]);
    return play && (e == 0 || e == 1) ? e : kScreen;
}

Image Acquire(int view) {
    Swapchain& sc = g_xr.chains[view];
    if (!g_xr.in_frame || !sc.handle) return {};
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(sc.handle, &ai, &sc.acquired))) return {};
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(sc.handle, &wi))) return {};
    return {sc.images[sc.acquired].texture, sc.w, sc.h};
}

void Release(int view) {
    Swapchain& sc = g_xr.chains[view];
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(sc.handle, &ri);
    if (view == kScreen) {
        g_xr.screen_image = true;
        g_xr.screen_aspect = g_xr.drawn[2][3] > 0 ? g_xr.drawn[2][3] : 16.0f / 9.0f;
        return;
    }
    g_xr.has_image[view] = true;
    g_xr.pose[view].orientation = {g_xr.drawn[1][0], g_xr.drawn[1][1], g_xr.drawn[1][2], g_xr.drawn[1][3]};
    g_xr.pose[view].position = {g_xr.drawn[2][0], g_xr.drawn[2][1], g_xr.drawn[2][2]};
    g_xr.fov[view] = {g_xr.drawn[3][0], g_xr.drawn[3][1], g_xr.drawn[3][2], g_xr.drawn[3][3]};
}

void EndFrame() {
    if (!g_xr.in_frame) return;
    g_xr.in_frame = false;
    XrCompositionLayerProjectionView pv[kEyes]{};
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader* layers[1] = {};
    uint32_t count = 0;
    bool play = DrawnView() != kScreen;
    if (g_xr.frame.shouldRender && play && g_xr.has_image[0] && g_xr.has_image[1]) {
        for (int e = 0; e < kEyes; e++) {
            pv[e] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            pv[e].pose = g_xr.pose[e];
            pv[e].fov = g_xr.fov[e];
            pv[e].subImage.swapchain = g_xr.chains[e].handle;
            pv[e].subImage.imageRect = {{0, 0}, {int32_t(g_xr.chains[e].w), int32_t(g_xr.chains[e].h)}};
        }
        proj.space = g_xr.local;
        proj.viewCount = kEyes;
        proj.views = pv;
        layers[count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
    } else if (g_xr.frame.shouldRender && g_xr.screen_image) {
        const Swapchain& sc = g_xr.chains[kScreen];
        quad.space = g_xr.local;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = sc.handle;
        quad.subImage.imageRect = {{0, 0}, {int32_t(sc.w), int32_t(sc.h)}};
        // the screen goes where the head faces as it appears, level, and stays there
        if (!g_xr.screen_placed) {
            float yaw = 0, pitch = 0, height = 0;
            {
                std::lock_guard lock(g_head_lock);
                if (g_head.valid) YawPitch(g_head.view.orientation, yaw, pitch);
                if (g_head.valid) height = g_head.view.position.y;
            }
            g_xr.screen_pose.orientation = {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
            g_xr.screen_pose.position = {-std::sin(yaw) * kScreenDistance, height, -std::cos(yaw) * kScreenDistance};
            g_xr.screen_placed = true;
        }
        quad.pose = g_xr.screen_pose;
        quad.size = {kScreenWidth, kScreenWidth / g_xr.screen_aspect};
        layers[count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
    }
    // leaving play: both eyes start over, so a stale eye is never shown beside a new one
    if (!play && g_xr.last_in_play) g_xr.has_image[0] = g_xr.has_image[1] = false;
    if (play) g_xr.screen_placed = false;  // placed again on the next menu
    g_xr.last_in_play = play;
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = g_xr.frame.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = count;
    ei.layers = layers;
    XrResult r = xrEndFrame(g_xr.session, &ei);
    if (XR_FAILED(r)) LOG_FIRST(8, "vr: xrEndFrame failed (%d)", int(r));
}

}  // namespace vr
