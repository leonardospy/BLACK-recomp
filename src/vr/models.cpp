// The VR player's models: vr_assets\*.bvr, and the avatar's pose from the head and the hands. See models.h.
#include "vr/models.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>

#include "app/app.h"
#include "core/log.h"
#include "vr/body.h"

namespace vr {

namespace {

// the muzzle flash: three blades crossed along the barrel (-z) and a star across it, in metres
Model Flash() {
    Model m;
    m.name = "flash";
    auto quad = [&](const float a[3], const float b[3], const float c[3], const float d[3], float r, float g, float bl) {
        uint32_t base = uint32_t(m.vertices.size());
        for (const float* p : {a, b, c, d}) {
            ModelVertex v{};
            std::memcpy(v.pos, p, 12);
            v.normal[1] = 1;
            v.colour[0] = r;
            v.colour[1] = g;
            v.colour[2] = bl;
            v.weights[0] = 1;
            m.vertices.push_back(v);
        }
        for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) m.indices.push_back(base + i);
    };
    constexpr float kLength = 0.22f, kWide = 0.05f, kStar = 0.07f;
    for (int k = 0; k < 3; k++) {
        float a = float(k) * 1.0472f, x = std::cos(a) * kWide, y = std::sin(a) * kWide;
        const float p0[3] = {-x, -y, 0}, p1[3] = {x, y, 0}, p2[3] = {x * 0.3f, y * 0.3f, -kLength}, p3[3] = {-x * 0.3f, -y * 0.3f, -kLength};
        quad(p0, p1, p2, p3, 1.0f, 0.75f, 0.3f);
    }
    for (int k = 0; k < 2; k++) {
        float a = float(k) * 0.7854f, x = std::cos(a) * kStar, y = std::sin(a) * kStar;
        const float p0[3] = {-x, -y, -0.02f}, p1[3] = {y, -x, -0.02f}, p2[3] = {x, y, -0.02f}, p3[3] = {-y, x, -0.02f};
        quad(p0, p1, p2, p3, 1.0f, 0.9f, 0.6f);
    }
    m.parts.push_back({"flash", 0, uint32_t(m.indices.size())});
    return m;
}

// the aiming laser: two thin red blades crossed along the barrel (-z), wider far away so it stays a line
Model Laser() {
    Model m;
    m.name = "laser";
    constexpr float kLength = 80.0f, kNear = 0.0005f, kFar = 0.004f;
    for (int k = 0; k < 2; k++) {
        uint32_t base = uint32_t(m.vertices.size());
        const float pts[4][3] = {{k ? 0 : -kNear, k ? -kNear : 0, 0}, {k ? 0 : kNear, k ? kNear : 0, 0},
                                 {k ? 0 : kFar, k ? kFar : 0, -kLength}, {k ? 0 : -kFar, k ? -kFar : 0, -kLength}};
        for (const auto& p : pts) {
            ModelVertex v{};
            std::memcpy(v.pos, p, 12);
            v.normal[1] = 1;
            v.colour[0] = 1.0f;
            v.colour[1] = 0.05f;
            v.colour[2] = 0.03f;
            v.weights[0] = 1;
            m.vertices.push_back(v);
        }
        for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) m.indices.push_back(base + i);
    }
    m.parts.push_back({"laser", 0, uint32_t(m.indices.size())});
    return m;
}

// the laser's spot where it hits: a small diamond, 1 cm across
Model Dot() {
    Model m;
    m.name = "dot";
    const float r = 0.006f;
    const float p[6][3] = {{r, 0, 0}, {-r, 0, 0}, {0, r, 0}, {0, -r, 0}, {0, 0, r}, {0, 0, -r}};
    for (const auto& q : p) {
        ModelVertex v{};
        std::memcpy(v.pos, q, 12);
        v.normal[1] = 1;
        v.colour[0] = 1.0f;
        v.colour[1] = 0.1f;
        v.colour[2] = 0.08f;
        v.weights[0] = 1;
        m.vertices.push_back(v);
    }
    const uint32_t t[8][3] = {{0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4}, {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {0, 3, 5}};
    for (const auto& f : t)
        for (uint32_t i : f) m.indices.push_back(i);
    m.parts.push_back({"dot", 0, uint32_t(m.indices.size())});
    return m;
}

// a flat quad of colour c, corners a b c d (counter clockwise seen from the side the normal n faces)
void Quad(Model& m, const float a[3], const float b[3], const float c[3], const float d[3], const float n[3],
          float r, float g, float bl) {
    uint32_t base = uint32_t(m.vertices.size());
    for (const float* p : {a, b, c, d}) {
        ModelVertex v{};
        std::memcpy(v.pos, p, 12);
        std::memcpy(v.normal, n, 12);
        v.colour[0] = r;
        v.colour[1] = g;
        v.colour[2] = bl;
        v.weights[0] = 1;
        m.vertices.push_back(v);
    }
    for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) m.indices.push_back(base + i);
}

// The test room: grey, 10 m wide, from 5 m behind to 7 m ahead of where the player stands, 3 m high, the floor at
// y 0 with a line every metre, a ring table round the player, three targets on the far wall
Model Room() {
    Model m;
    m.name = "room";
    const float X = 5, B = 5, F = -7, H = 3;
    auto quad = [&](float ax, float ay, float az, float bx, float by, float bz, float cx, float cy, float cz, float dx,
                    float dy, float dz, float nx, float ny, float nz, float k) {
        const float a[3] = {ax, ay, az}, b[3] = {bx, by, bz}, c[3] = {cx, cy, cz}, d[3] = {dx, dy, dz}, n[3] = {nx, ny, nz};
        Quad(m, a, b, c, d, n, k, k, k);
    };
    quad(-X, 0, B, X, 0, B, X, 0, F, -X, 0, F, 0, 1, 0, 0.30f);    // floor
    quad(-X, H, F, X, H, F, X, H, B, -X, H, B, 0, -1, 0, 0.55f);   // ceiling
    quad(-X, 0, F, X, 0, F, X, H, F, -X, H, F, 0, 0, 1, 0.50f);    // ahead
    quad(X, 0, B, -X, 0, B, -X, H, B, X, H, B, 0, 0, -1, 0.50f);   // behind
    quad(-X, 0, B, -X, 0, F, -X, H, F, -X, H, B, 1, 0, 0, 0.45f);  // left
    quad(X, 0, F, X, 0, B, X, H, B, X, H, F, -1, 0, 0, 0.45f);     // right
    // a darker line on the floor every metre, both ways, to judge distance by
    const float w = 0.01f, up = 0.002f;
    for (float z = F + 1; z < B; z += 1) quad(-X, up, z + w, X, up, z + w, X, up, z - w, -X, up, z - w, 0, 1, 0, 0.18f);
    for (float x = -X + 1; x < X; x += 1) quad(x - w, up, B, x + w, up, B, x + w, up, F, x - w, up, F, 0, 1, 0, 0.18f);
    // the ring table round the player: its top, and its outer and inner sides down to the floor
    {
        const float T = 0.9f, r0 = 1.75f, r1 = 2.35f;
        const int n = 48;
        for (int i = 0; i < n; i++) {
            float a0 = 6.2832f * float(i) / n, a1 = 6.2832f * float(i + 1) / n;
            float s0 = std::sin(a0), c0 = std::cos(a0), s1 = std::sin(a1), c1 = std::cos(a1);
            quad(s0 * r0, T, -c0 * r0, s1 * r0, T, -c1 * r0, s1 * r1, T, -c1 * r1, s0 * r1, T, -c0 * r1, 0, 1, 0, 0.62f);
            quad(s0 * r1, 0, -c0 * r1, s1 * r1, 0, -c1 * r1, s1 * r1, T, -c1 * r1, s0 * r1, T, -c0 * r1, s0, 0, -c0, 0.4f);
            quad(s1 * r0, 0, -c1 * r0, s0 * r0, 0, -c0 * r0, s0 * r0, T, -c0 * r0, s1 * r0, T, -c1 * r0, -s0, 0, c0, 0.4f);
        }
    }
    // targets: a white board, a red ring and a white centre, on the far wall
    for (float x : {-2.0f, 0.0f, 2.0f}) {
        const float z1 = F + 0.005f, z2 = F + 0.01f, z3 = F + 0.015f;
        quad(x - 0.5f, 0.8f, z1, x + 0.5f, 0.8f, z1, x + 0.5f, 1.8f, z1, x - 0.5f, 1.8f, z1, 0, 0, 1, 0.9f);
        const float a[3] = {x - 0.25f, 1.05f, z2}, b[3] = {x + 0.25f, 1.05f, z2}, c[3] = {x + 0.25f, 1.55f, z2}, d[3] = {x - 0.25f, 1.55f, z2};
        const float n[3] = {0, 0, 1};
        Quad(m, a, b, c, d, n, 0.8f, 0.1f, 0.08f);
        quad(x - 0.08f, 1.22f, z3, x + 0.08f, 1.22f, z3, x + 0.08f, 1.38f, z3, x - 0.08f, 1.38f, z3, 0, 0, 1, 0.95f);
    }
    m.parts.push_back({"room", 0, uint32_t(m.indices.size())});
    return m;
}

// a shot's mark on a wall: a dark square 4 cm across, facing +z
Model Hole() {
    Model m;
    m.name = "hole";
    const float a[3] = {-0.02f, -0.02f, 0}, b[3] = {0.02f, -0.02f, 0}, c[3] = {0.02f, 0.02f, 0}, d[3] = {-0.02f, 0.02f, 0};
    const float n[3] = {0, 0, 1};
    Quad(m, a, b, c, d, n, 0.04f, 0.04f, 0.04f);
    m.parts.push_back({"hole", 0, uint32_t(m.indices.size())});
    return m;
}

const char* kModelNames[] = {"avatar_swat", "avatar_adventurer", "rifle_a", "rifle_b", "smg_a", "smg_b", "sniper",
                             "pistol", "shotgun",
                             // the game's own guns, when tools/game_guns.py made them from the game's files
                             "g_pst", "g_gk1", "g_mgn", "g_shg", "g_rm1", "g_snr", "g_smg", "g_sm3", "g_p90",
                             "g_mp1", "g_asr", "g_m16", "g_ak1", "g_hands"};

bool Load(const std::filesystem::path& path, Model& m) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    char magic[4];
    uint32_t counts[4];
    bool ok = fread(magic, 1, 4, f) == 4 && fread(counts, 4, 4, f) == 4;
    // version 1: vertices without uv (60 bytes); 2: with it, and a texture and slide after the joints
    int version = ok && std::memcmp(magic, "BVR1", 4) == 0 ? 1 : ok && std::memcmp(magic, "BVR2", 4) == 0 ? 2 : 0;
    ok = version != 0;
    if (ok) {
        m.vertices.resize(counts[0]);
        m.indices.resize(counts[1]);
        size_t size = version == 1 ? 60 : sizeof(ModelVertex);
        std::vector<uint8_t> raw(size * counts[0]);
        ok = fread(raw.data(), size, counts[0], f) == counts[0] && fread(m.indices.data(), 4, counts[1], f) == counts[1];
        for (uint32_t i = 0; ok && i < counts[0]; i++) {
            m.vertices[i] = {};
            std::memcpy(&m.vertices[i], raw.data() + i * size, size);
        }
    }
    for (uint32_t i = 0; ok && i < counts[3]; i++) {
        char name[32];
        uint32_t range[2];
        ok = fread(name, 1, 32, f) == 32 && fread(range, 4, 2, f) == 2;
        name[31] = 0;
        m.parts.push_back({name, range[0], range[1]});
    }
    for (uint32_t i = 0; ok && i < counts[2]; i++) {
        char name[32];
        ModelJoint j;
        ok = fread(name, 1, 32, f) == 32 && fread(&j.parent, 4, 1, f) == 1 && fread(j.inverse_bind, 4, 16, f) == 16 &&
             fread(j.rest, 4, 16, f) == 16;
        name[31] = 0;
        j.name = name;
        m.joints.push_back(j);
    }
    if (ok && version == 2) {
        uint32_t wh[2];
        ok = fread(wh, 4, 2, f) == 2 && wh[0] && wh[1] && wh[0] <= 4096 && wh[1] <= 4096;
        if (ok) {
            m.texture.resize(size_t(wh[0]) * wh[1] * 4);
            ok = fread(m.texture.data(), 1, m.texture.size(), f) == m.texture.size() &&
                 fread(&m.slide_travel, 4, 1, f) == 1;
            uint32_t flags = 0;
            if (ok && fread(m.fore, 4, 3, f) == 3 && fread(&flags, 4, 1, f) == 1) {
                m.two_handed = flags & 1;
                m.pump = flags & 2;
                float held[32];
                if (fread(held, 4, 32, f) == 32) {
                    std::memcpy(m.hand, held, sizeof(m.hand));
                    std::memcpy(m.fore_rot, held + 16, sizeof(m.fore_rot));
                }
            }
            m.texture_w = wh[0];
            m.texture_h = wh[1];
        }
        for (size_t p = 0; ok && p < m.parts.size(); p++) {
            if (m.parts[p].name == "slide") m.slide = int(p);
            if (m.parts[p].name != "shell") continue;
            m.shell = int(p);
            float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
            for (uint32_t i = m.parts[p].first; i < m.parts[p].first + m.parts[p].count; i++)
                for (int k = 0; k < 3; k++) {
                    lo[k] = std::min(lo[k], m.vertices[m.indices[i]].pos[k]);
                    hi[k] = std::max(hi[k], m.vertices[m.indices[i]].pos[k]);
                }
            for (int k = 0; k < 3; k++) m.shell_centre[k] = (lo[k] + hi[k]) / 2;
        }
    }
    fclose(f);
    // the magazine's bounds: its centre, and its top (the end nearest the gun's top, +y)
    for (size_t p = 0; ok && p < m.parts.size(); p++) {
        if (m.parts[p].name != "magazine") continue;
        float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
        for (uint32_t i = m.parts[p].first; i < m.parts[p].first + m.parts[p].count; i++) {
            const float* v = m.vertices[m.indices[i]].pos;
            for (int k = 0; k < 3; k++) {
                lo[k] = std::min(lo[k], v[k]);
                hi[k] = std::max(hi[k], v[k]);
            }
        }
        m.magazine = int(p);
        for (int k = 0; k < 3; k++) m.magazine_centre[k] = (lo[k] + hi[k]) / 2;
        m.magazine_top[0] = m.magazine_centre[0];
        m.magazine_top[1] = hi[1];
        m.magazine_top[2] = m.magazine_centre[2];
    }
    // a gun's muzzle: the middle of what lies at its far end (-z)
    if (ok && m.joints.empty() && !m.vertices.empty()) {
        float far = 1e9f;
        for (const auto& v : m.vertices) far = std::min(far, v.pos[2]);
        float sum[2] = {};
        int n = 0;
        for (const auto& v : m.vertices) {
            if (v.pos[2] > far + 0.01f) continue;
            sum[0] += v.pos[0];
            sum[1] += v.pos[1];
            n++;
        }
        m.muzzle[0] = sum[0] / float(n);
        m.muzzle[1] = sum[1] / float(n);
        m.muzzle[2] = far;
    }
    return ok;
}

}  // namespace

const std::vector<Model>& Models() {
    static std::vector<Model> models = [] {
        std::vector<Model> out;
        std::filesystem::path dir = app::ExeDir() / "vr_assets";
        for (const char* name : kModelNames) {
            Model m;
            m.name = name;
            if (Load(dir / (std::string(name) + ".bvr"), m)) {
                out.push_back(std::move(m));
            } else {
                if (std::strncmp(name, "g_", 2) != 0) LOGW("vr: model %s not found in %ls", name, dir.c_str());
            }
        }
        out.push_back(Flash());
        out.push_back(Laser());
        out.push_back(Dot());
        out.push_back(Room());
        out.push_back(Hole());
        LOGR("vr: %zu models", out.size());
        return out;
    }();
    return models;
}

int GunForCode(const char* code) {
    // the game's weapons by the names in its table (GlobData.bin); the launchers and the machine gun keep the
    // game's own models
    static const struct {
        const char* name;
        const char* model;
    } kGuns[] = {
        {"FTP_", "pistol"},    {"FTgk1_", "pistol"},  {"FTmgn_", "pistol"},  {"FTSG_", "shotgun"},
        {"FTrm1_", "shotgun"}, {"FTSR_", "sniper"},   {"FTSMG_", "smg_a"},   {"FTsm3_", "smg_a"},
        {"FTP90_", "smg_b"},   {"FTmp1_", "smg_b"},   {"FTAR_", "rifle_a"},  {"FTM16_", "rifle_a"},
        {"FTAK_", "rifle_b"},  {"FTRPG_", nullptr},   {"FTGL_", nullptr},    {"FThvy_", nullptr},
    };
    // the game's own models first, made by tools/game_guns.py
    static const struct {
        const char* name;
        const char* model;
    } kGameGuns[] = {
        {"FTP_", "g_pst"},  {"FTgk1_", "g_gk1"}, {"FTmgn_", "g_mgn"}, {"FTSG_", "g_shg"},  {"FTrm1_", "g_rm1"},
        {"FTSR_", "g_snr"}, {"FTSMG_", "g_smg"}, {"FTsm3_", "g_sm3"}, {"FTP90_", "g_p90"}, {"FTmp1_", "g_mp1"},
        {"FTAR_", "g_asr"}, {"FTM16_", "g_m16"}, {"FTAK_", "g_ak1"},
    };
    const char* model = nullptr;
    bool known = false;
    for (const auto& g : kGameGuns) {
        if (std::strcmp(g.name, code) == 0 && ModelIndex(g.model) >= 0) {
            model = g.model;
            known = true;
        }
    }
    for (const auto& g : kGuns) {
        if (!model && std::strcmp(g.name, code) == 0) {
            model = g.model;
            known = true;
        }
    }
    if (*code) LOGR("vr: weapon \"%s\": %s", code, model ? model : known ? "the game's own gun" : "unknown, the game's own gun");
    return model ? ModelIndex(model) : -1;
}

bool OwnModels() {
    static const bool own = ModelIndex("avatar_swat") >= 0 && ModelIndex("rifle_a") >= 0;
    return own;
}

int ModelIndex(const char* name) {
    const auto& models = Models();
    for (size_t i = 0; i < models.size(); i++)
        if (models[i].name == name) return int(i);
    return -1;
}

}  // namespace vr
