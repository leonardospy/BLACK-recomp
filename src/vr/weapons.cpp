// The weapon the player holds, from the game's look at it each frame of play; which of the player's own gun models
// shows it. With BLACK_VR_DEBUG set, each new weapon data is described in the log, to learn its layout.
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/log.h"
#include "kernel/kernel.h"
#include "vr/vr.h"

namespace vr {

namespace {

std::atomic<uint32_t> g_weapon{0};
std::atomic<uint32_t> g_data{0};
std::atomic<int> g_gun{-1};
std::atomic<int> g_probe{0};  // debug: rounds a reload just loaded (BLACK_VR_RESERVEFIND)        // the model for it, GunForCode
constexpr uint32_t kDataName = 0x11E;  // char[]: "FTAK_"; the class follows at +0x130, "ASR"

constexpr uint32_t kGuestEnd = 0x4000000;  // the console's 64 MB
const uint32_t kScanBase = std::getenv("BLACK_VR_AMMOFIND") ? uint32_t(std::strtoul(std::getenv("BLACK_VR_AMMOFIND"), nullptr, 16)) : 0;

// the low 64 MB, or its view at 0x80000000 that the kernel hands out
bool Guest(uint32_t va) {
    return (va >= 0x10000 && va + 64 < kGuestEnd) || (va >= 0x80000000u && va + 64 < 0x80000000u + kGuestEnd);
}

// printable text at va, empty when it is not one
std::string Text(uint32_t va) {
    if (!Guest(va)) return {};
    std::string s;
    for (uint32_t i = 0; i < 48; i++) {
        char c = kernel::G<char>(va + i);
        if (!c) break;
        if (!std::isprint(static_cast<unsigned char>(c))) return {};
        s += c;
    }
    return s.size() >= 3 ? s : std::string{};
}

void Describe(const char* what, uint32_t base, uint32_t bytes) {
    for (uint32_t off = 0; off < bytes; off += 4) {
        uint32_t v = kernel::R32(base + off);
        std::string inline_text = Text(base + off);
        std::string pointed = Text(v);
        if (!pointed.empty()) LOGR("vr: %s+%03X -> \"%s\"", what, off, pointed.c_str());
        if (!inline_text.empty()) LOGR("vr: %s+%03X \"%s\"", what, off, inline_text.c_str());
    }
}

}  // namespace

// the rounds in the gun: a word of the player's weapon object (found with BLACK_VR_AMMOFIND)
constexpr uint32_t kRounds = 0x3238;

int Rounds() {
    if (RoomActive()) return RoomRounds();
    uint32_t weapon = g_weapon.load(std::memory_order_relaxed);
    if (!weapon || !Guest(weapon + kRounds)) return -1;
    uint32_t n = kernel::R32(weapon + kRounds);
    return n < 1000 ? int(n) : -1;
}

// the rounds carried for it: the player's count for its kind of round, 16 bits in the list the weapon points to
// (+0xFC), at the kind its round type gives (+0xF4 -> +8 -> +0x64), as the game's own fire check reads it (0x1BB186)
constexpr uint32_t kReservePointer = 0xFC;
constexpr uint32_t kRoundType = 0xF4;

uint32_t ReserveAt() {
    if (RoomActive()) return 0;
    uint32_t weapon = g_weapon.load(std::memory_order_relaxed);
    if (!weapon || !Guest(weapon + kRoundType)) return 0;
    uint32_t list = kernel::R32(weapon + kReservePointer);
    uint32_t type_obj = kernel::R32(weapon + kRoundType);
    if (!Guest(list) || !Guest(type_obj)) return 0;
    uint32_t kind_obj = kernel::R32(type_obj + 8);
    if (!Guest(kind_obj)) return 0;
    uint32_t kind = kernel::R32(kind_obj + 0x64);
    if (kind > 64) return 0;
    return list + kind * 2;
}

int Reserve() {
    if (RoomActive()) return RoomReserve();
    uint32_t at = ReserveAt();
    return at ? int(kernel::R16(at)) : -1;
}

void SetReserve(int n) {
    if (RoomActive()) return RoomSetReserve(n);
    uint32_t at = ReserveAt();
    if (at && n >= 0) kernel::W16(at, uint16_t(std::min(n, 0xFFFF)));
}

void WriteGuest16(uint32_t va, uint16_t v) {
    if (Guest(va)) kernel::W16(va, v);
}

void ReloadSeen(int loaded) { g_probe.store(loaded); }

void SetRounds(int n) {
    if (RoomActive()) return RoomSetRounds(n);
    uint32_t weapon = g_weapon.load(std::memory_order_relaxed);
    if (weapon && Guest(weapon + kRounds) && n >= 0) kernel::W32(weapon + kRounds, uint32_t(n));
}

uint32_t HeldData() { return g_data.load(std::memory_order_relaxed); }


int HeldGun() {
    if (RoomActive()) return RoomHeldGun();
    // debug: BLACK_VR_GUN_MODEL names the model held whatever the weapon
    static const char* forced = std::getenv("BLACK_VR_GUN_MODEL");
    if (forced && std::strcmp(forced, "cycle") == 0) {
        // each gun in turn, 1.5 s each
        static const char* kAll[] = {"g_pst", "g_gk1", "g_mgn", "g_shg", "g_rm1", "g_snr", "g_smg",
                                     "g_sm3", "g_p90", "g_mp1", "g_asr", "g_m16", "g_ak1"};
        static const auto start = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        return ModelIndex(kAll[(ms / 1500) % std::size(kAll)]);
    }
    if (forced) return ModelIndex(forced);
    return g_gun.load(std::memory_order_relaxed);
}

void HeldWeapon(uint32_t weapon, uint32_t data) {
    g_weapon.store(weapon, std::memory_order_relaxed);
    // the weapon's name in its data (GlobData.bin's table): "FTAK_", "FTP_"
    if (data != g_data.exchange(data, std::memory_order_relaxed)) {
        char name[16] = {};
        if (data && Guest(data)) {
            for (int i = 0; i < 15; i++) {
                char c = kernel::G<char>(data + kDataName + uint32_t(i));
                if (!std::isprint(static_cast<unsigned char>(c))) break;
                name[i] = c;
            }
        }
        g_gun.store(GunForCode(name), std::memory_order_relaxed);
    }
    // debug, BLACK_VR_RESERVEFIND=<base hex>: as the rounds in the gun go up from none (a reload), the bytes that
    // went down by as many (the rounds left to load)
    static const char* reserve_find = std::getenv("BLACK_VR_RESERVEFIND");
    if (reserve_find) {
        const uint32_t base_va = uint32_t(std::strtoul(reserve_find, nullptr, 16));
        static std::vector<uint8_t> before, now;
        static int last_rounds = -1;
        auto snap = [&](std::vector<uint8_t>& out) {
            out.assign(kGuestEnd, 0);
            uint8_t* base = static_cast<uint8_t*>(kernel::HostPtr(base_va));
            for (uint8_t* p = base; p < base + kGuestEnd;) {
                MEMORY_BASIC_INFORMATION mi{};
                if (!VirtualQuery(p, &mi, sizeof(mi))) break;
                uint8_t* end = std::min(static_cast<uint8_t*>(mi.BaseAddress) + mi.RegionSize, base + kGuestEnd);
                if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                    std::memcpy(out.data() + (p - base), p, size_t(end - p));
                p = end;
            }
        };
        int r = Rounds();
        int loaded = g_probe.exchange(0);
        if (r > 0 && last_rounds == 0) loaded = r;
        if (loaded > 0 && !before.empty()) {
            // each reload: the bytes that went down by the rounds loaded, those of every reload kept
            static std::vector<uint32_t> cand;
            static bool first = true;
            snap(now);
            std::vector<uint32_t> next;
            if (first) {
                for (uint32_t at = 0; at < kGuestEnd; at++)
                    if (before[at] >= loaded && before[at] - now[at] == loaded) next.push_back(at);
            } else {
                for (uint32_t at : cand)
                    if (before[at] >= loaded && before[at] - now[at] == loaded) next.push_back(at);
            }
            first = false;
            cand.swap(next);
            LOGR("vr: reservefind %zu candidates (%d rounds loaded)", cand.size(), loaded);
            if (cand.size() < 40)
                for (uint32_t at : cand) LOGR("vr: reservefind %08X %u -> %u", base_va + at, before[at], now[at]);
        }
        if (r == 0) snap(before);
        last_rounds = r;
        // where the weapon and its data point near the reserve found (BLACK_VR_RESERVENEAR=<address hex>)
        static const char* near_env = std::getenv("BLACK_VR_RESERVENEAR");
        static uint32_t near_data = 0;
        if (near_env && data != near_data && weapon && data) {
            near_data = data;
            uint32_t target = uint32_t(std::strtoul(near_env, nullptr, 16));
            for (uint32_t base : {weapon, data})
                for (uint32_t off = 0; off < 0x4000; off += 4) {
                    uint32_t v = kernel::R32(base + off);
                    if (v <= target && target - v < 0x800)
                        LOGR("vr: reservenear %s+%04X = %08X (target +%X)", base == weapon ? "weapon" : "data", off, v,
                             target - v);
                }
        }
    }
    // debug, BLACK_VR_AMMOFIND: the 16 bit values in the whole of memory that drop by one with each shot
    static const bool find = std::getenv("BLACK_VR_AMMOFIND") != nullptr;
    if (find) {
        static std::vector<uint16_t> last;
        static std::vector<uint32_t> cand;
        static uint32_t seen_shots = 0, wait = 0, rounds = 0;
        uint32_t shots = Shots();
        if (shots != seen_shots) {
            seen_shots = shots;
            wait = 20;  // frames after the shot: the count has gone down
        }
        if (wait && --wait == 0) {
            constexpr uint32_t kWords = kGuestEnd / 2;
            // the committed pages only: the rest of the guest's range is reserved, unreadable
            static std::vector<uint16_t> now_mem(kWords);
            {
                uint8_t* base = static_cast<uint8_t*>(kernel::HostPtr(kScanBase));
                std::fill(now_mem.begin(), now_mem.end(), uint16_t(0));
                for (uint8_t* p = base; p < base + kGuestEnd;) {
                    MEMORY_BASIC_INFORMATION mi{};
                    if (!VirtualQuery(p, &mi, sizeof(mi))) break;
                    uint8_t* end = std::min(static_cast<uint8_t*>(mi.BaseAddress) + mi.RegionSize, base + kGuestEnd);
                    bool readable = mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
                    if (readable) std::memcpy(reinterpret_cast<uint8_t*>(now_mem.data()) + (p - base), p, size_t(end - p));
                    p = end;
                }
            }
            const uint16_t* mem = now_mem.data();
            if (last.empty()) {
                last.assign(mem, mem + kWords);
            } else {
                // a candidate: kind << 28 | byte offset; kinds: 0 a byte, 1 a float, 2 a byte that dropped by one
                // or more (the game may count more than a shot at once)
                const uint8_t* now8 = reinterpret_cast<const uint8_t*>(mem);
                const uint8_t* was8 = reinterpret_cast<const uint8_t*>(last.data());
                auto value = [](const uint8_t* m, uint32_t c) {
                    uint32_t at = c & 0x0FFFFFFF;
                    if ((c >> 28) == 1) {
                        float f;
                        std::memcpy(&f, m + at, 4);
                        return f;
                    }
                    return float(m[at]);
                };
                auto keeps = [&](uint32_t c) {
                    float was = value(was8, c), is = value(now8, c);
                    return was > 0 && was < 500 && is == was - 1;
                };
                std::vector<uint32_t> next;
                if (rounds == 0) {
                    for (uint32_t at = 0x10000; at < kGuestEnd; at++) {
                        if (keeps(at)) next.push_back(at);
                        if (at % 4 == 0 && keeps(1u << 28 | at)) next.push_back(1u << 28 | at);
                    }
                } else {
                    for (uint32_t c : cand)
                        if (keeps(c)) next.push_back(c);
                }
                cand.swap(next);
                rounds++;
                last.assign(mem, mem + kWords);
                LOGR("vr: ammofind after shot %u: %zu candidates", rounds, cand.size());
                if (cand.size() < 40)
                    for (uint32_t c : cand)
                        LOGR("vr: ammofind %s %08X = %g (weapon %08X)", (c >> 28) ? "float" : "byte", kScanBase + (c & 0x0FFFFFFF),
                             value(now8, c), weapon);
            }
        }
    }
    // debug, BLACK_VR_AMMOSCAN: every word of the weapon and its data that changes, to find the rounds in the gun
    static const bool scan = std::getenv("BLACK_VR_AMMOSCAN") != nullptr;
    if (scan && weapon && Guest(weapon) && data && Guest(data)) {
        static uint32_t last_w = 0, prev[0x400 / 4 + 0x200 / 4];
        bool fresh = last_w != weapon;
        last_w = weapon;
        for (uint32_t i = 0; i < 0x400 / 4 + 0x200 / 4; i++) {
            uint32_t va = i < 0x100 ? weapon + 0x3100 + i * 4 : data + (i - 0x100) * 4;  // the rounds at +0x3238
            uint32_t v = kernel::R32(va);
            if (!fresh && v != prev[i] && v < 0x10000 && prev[i] < 0x10000)
                LOGR("vr: ammoscan %s+%03X %u -> %u", i < 0x100 ? "weapon" : "data", (i < 0x100 ? i : i - 0x100) * 4,
                     prev[i], v);
            prev[i] = v;
        }
    }
    static const bool debug = std::getenv("BLACK_VR_DEBUG") != nullptr;
    static std::unordered_set<uint32_t> seen;
    if (debug && data && Guest(data) && seen.insert(data).second) {
        LOGR("vr: weapon %08X data %08X", weapon, data);
        Describe("data", data, 0x200);
        Describe("weapon", weapon, 0x400);
    }
}

}  // namespace vr
