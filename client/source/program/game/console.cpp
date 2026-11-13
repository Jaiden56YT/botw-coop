#include "console.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <lib.hpp>

#include "ghost.hpp"
#include "offsets.hpp"
#include "player.hpp"

namespace coop::game::console {

    char g_Cmd[128];

    namespace {
        using CsFn = void (*)(uintptr_t);
        using CsTryFn = bool (*)(uintptr_t);
        using GetLifeFn = s32* (*)(uintptr_t);

        uintptr_t Base() { return exl::util::GetMainModuleInfo().m_Total.m_Start; }
        template<typename T> T Read(uintptr_t a) { return *reinterpret_cast<const T*>(a); }

        s32* Life(uintptr_t a) {
            return reinterpret_cast<GetLifeFn>(Read<uintptr_t>(Read<uintptr_t>(a) + offsets::ActorVt_GetLife))(a);
        }

        void Near(float radius) {
            float me[3];
            if (!GetLocalPlayerPos(me)) return;
            uintptr_t slot = Read<uintptr_t>(Base() + offsets::Got_ActorCreatorInstance);
            uintptr_t creator = slot ? Read<uintptr_t>(slot) : 0;
            if (!creator) return;
            uintptr_t cs = creator + offsets::ActorCreator_ActorListCS;
            if (!reinterpret_cast<CsTryFn>(Base() + offsets::CriticalSection_TryLock)(cs)) {
                Logging.Log("[coop] console: the actor list is busy, try again");
                return;
            }
            uintptr_t head = creator + offsets::ActorCreator_ActorList;
            s32 node_off = Read<s32>(creator + offsets::ActorCreator_ActorListOffset);
            int guard = 0, shown = 0;
            for (uintptr_t node = Read<uintptr_t>(head + 8); node != head && node && guard < 20000; guard++) {
                uintptr_t a = node - node_off;
                node = Read<uintptr_t>(node + 8);
                const char* name = Read<const char*>(a + offsets::BaseProc_NameStr);
                if (!name) continue;
                const float* m = reinterpret_cast<const float*>(a + offsets::Actor_Mtx);
                float dx = m[3] - me[0], dy = m[7] - me[1], dz = m[11] - me[2];
                float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (d > radius) continue;
                s32* life = Life(a);
                Logging.Log("[coop] console: %p %s  %.1f m  (%.1f, %.1f, %.1f)  hash %08x  life %d", reinterpret_cast<void*>(a), name,
                            d, m[3], m[7], m[11], Read<u32>(a + offsets::Actor_HashId), life ? *life : -1);
                if (++shown >= 60) break;
            }
            reinterpret_cast<CsFn>(Base() + offsets::CriticalSection_Unlock)(cs);
            Logging.Log("[coop] console: %d actors within %.0f m", shown, radius);
        }
    }

    /* ---- probe (development only): what the game creates, and with which parameters ---- */
    namespace {
        char g_ProbeNames[64]; /* "Explode|Remote": substrings of actor names to report; empty = off */

        bool ProbeWants(const char* name) {
            if (!g_ProbeNames[0] || !name) return false;
            char list[sizeof(g_ProbeNames)];
            std::memcpy(list, g_ProbeNames, sizeof(list));
            for (char* tok = std::strtok(list, "|"); tok; tok = std::strtok(nullptr, "|"))
                if (std::strstr(name, tok)) return true;
            return false;
        }

        void DumpPack(const char* what, const char* name, uintptr_t pack) {
            if (!pack) {
                Logging.Log("[coop] probe: %s %s (no parameters)", what, name);
                return;
            }
            u16 count = Read<u16>(pack + 0x10);
            s16 pos = Read<s16>(pack + 0x12);
            Logging.Log("[coop] probe: %s %s: %u parameters, %d bytes", what, name, count, pos);
            const u8* d = reinterpret_cast<const u8*>(pack + 0x14);
            int at = 0;
            for (int i = 0; i < count && at + 9 <= pos && at < 0xc0; i++) {
                const char* key;
                std::memcpy(&key, d + at, 8);
                u8 type = d[at + 8];
                at += 9;
                char val[96] = "";
                if (type == 4 && at + 12 <= 0xc0) {
                    float v[3];
                    std::memcpy(v, d + at, 12);
                    std::snprintf(val, sizeof(val), "vec3 (%.2f, %.2f, %.2f)", v[0], v[1], v[2]);
                    at += 12;
                } else if (type == 5) {
                    std::snprintf(val, sizeof(val), "string '%.60s'", reinterpret_cast<const char*>(d + at));
                    at += static_cast<int>(std::strlen(reinterpret_cast<const char*>(d + at))) + 1;
                } else if (type == 0 || type == 2) {
                    s32 iv;
                    float fv;
                    std::memcpy(&iv, d + at, 4);
                    std::memcpy(&fv, d + at, 4);
                    std::snprintf(val, sizeof(val), "%s %d / %.3f", type == 0 ? "int" : "float", iv, fv);
                    at += 4;
                } else {
                    /* unknown size: show what follows and stop */
                    char hex[64] = "";
                    int n = 0;
                    for (int k = 0; k < 16 && at + k < 0xc0; k++) n += std::snprintf(hex + n, sizeof(hex) - n, "%02x", d[at + k]);
                    Logging.Log("[coop] probe:   %s: type %u, then %s (stopping)", key ? key : "?", type, hex);
                    return;
                }
                Logging.Log("[coop] probe:   %s: %s", key ? key : "?", val);
            }
        }
    }

    HOOK_DEFINE_TRAMPOLINE(CreateActorProbe) {
        static uintptr_t Callback(uintptr_t creator, const char* name, uintptr_t heap, uintptr_t pack, bool sleep, bool block) {
            if (ProbeWants(name)) DumpPack("create", name, pack);
            return Orig(creator, name, heap, pack, sleep, block);
        }
    };

    void InstallProbe() { CreateActorProbe::InstallAtOffset(offsets::ActorCreator_CreateActor); }

    namespace {
        std::atomic<uintptr_t> g_WatchAS{0};
    }

    void OnStartAnimation(uintptr_t as_list, const char* name, int target, float a, float b, int bank, bool flag) {
        if (as_list && as_list == g_WatchAS.load(std::memory_order_relaxed))
            Logging.Log("[coop] console: watched actor starts '%s' slot %d (a %.2f, b %.2f, bank %d, flag %d)", name, target, a, b,
                        bank, (int)flag);
    }

    void Tick(s64 now) {
        (void)now;
        if (!g_Cmd[0]) return;
        char cmd[sizeof(g_Cmd)];
        std::memcpy(cmd, g_Cmd, sizeof(cmd));
        cmd[sizeof(cmd) - 1] = 0;
        std::memset(g_Cmd, 0, sizeof(g_Cmd));
        char* arg = std::strchr(cmd, ' ');
        if (arg) *arg++ = 0;
        if (!std::strcmp(cmd, "near")) {
            Near(arg ? static_cast<float>(std::atof(arg)) : 8.0f);
        } else if (!std::strcmp(cmd, "spawn") && arg) {
            /* spawn <actor> [metres ahead] [Key=i<int> | Key=f<float>]... */
            static char keys[6][32];
            SpawnParam params[6];
            int nparams = 0;
            float ahead = 2.5f;
            char* rest = std::strchr(arg, ' ');
            if (rest) *rest++ = 0;
            for (char* tok = rest ? std::strtok(rest, " ") : nullptr; tok; tok = std::strtok(nullptr, " ")) {
                char* eq = std::strchr(tok, '=');
                if (!eq) {
                    ahead = static_cast<float>(std::atof(tok));
                } else if (nparams < 6) {
                    *eq = 0;
                    std::snprintf(keys[nparams], sizeof(keys[nparams]), "%s", tok);
                    params[nparams].key = keys[nparams];
                    if (eq[1] == 'f') {
                        params[nparams].type = SpawnFloat;
                        params[nparams].f = static_cast<float>(std::atof(eq + 2));
                    } else {
                        params[nparams].type = SpawnInt;
                        params[nparams].i = std::atoi(eq + 2);
                    }
                    nparams++;
                }
            }
            float me[3], rot = 0;
            if (!GetLocalPlayer(me, &rot)) return;
            float at[3] = {me[0] + std::sin(rot) * ahead, me[1] + 1.0f, me[2] + std::cos(rot) * ahead};
            uintptr_t a = SpawnActorWith(arg, at, rot, params, nparams);
            Logging.Log("[coop] console: spawn %s (%d parameters) at (%.1f, %.1f, %.1f): %p", arg, nparams, at[0], at[1], at[2],
                        reinterpret_cast<void*>(a));
        } else if (!std::strcmp(cmd, "life") && arg) {
            char* val = nullptr;
            uintptr_t a = std::strtoull(arg, &val, 16);
            s32* life = a ? Life(a) : nullptr;
            if (life && val) {
                s32 v = static_cast<s32>(std::atoi(val));
                Logging.Log("[coop] console: life of %p: %d -> %d", reinterpret_cast<void*>(a), *life, v);
                *life = v;
            }
        } else if (!std::strcmp(cmd, "kill") && arg) {
            uintptr_t a = std::strtoull(arg, nullptr, 16);
            if (a) {
                reinterpret_cast<void (*)(uintptr_t, int)>(Read<uintptr_t>(Read<uintptr_t>(a) + offsets::ActorVt_Kill))(a, 0);
                Logging.Log("[coop] console: killed %p", reinterpret_cast<void*>(a));
            }
        } else if (!std::strcmp(cmd, "aswatch")) {
            uintptr_t a = arg ? std::strtoull(arg, nullptr, 16) : 0;
            g_WatchAS.store(a ? Read<uintptr_t>(a + offsets::Actor_ASList) : 0);
            Logging.Log("[coop] console: watching the sequences of %p", reinterpret_cast<void*>(a));
        } else if (!std::strcmp(cmd, "as") && arg) {
            /* as <actor address> <sequence> [slot] */
            char* name = nullptr;
            uintptr_t a = std::strtoull(arg, &name, 16);
            while (name && *name == ' ') name++;
            char* slot = name ? std::strchr(name, ' ') : nullptr;
            if (slot) *slot++ = 0;
            if (a && name && *name) {
                uintptr_t as_list = Read<uintptr_t>(a + offsets::Actor_ASList);
                bool ok = as_list && StartAS(as_list, -1.0f, -1.0f, name, slot ? std::atoi(slot) : 0, 0, true);
                Logging.Log("[coop] console: sequence '%s' on %p: %d", name, reinterpret_cast<void*>(a), (int)ok);
            }
        } else if (!std::strcmp(cmd, "scene") && arg) {
            /* scene <map, e.g. MainField/E-4> <start position name> [number]: the game's own stage change */
            char* pos = std::strchr(arg, ' ');
            if (!pos) return;
            *pos++ = 0;
            char* num = std::strchr(pos, ' ');
            if (num) *num++ = 0;
            struct Str {
                uintptr_t vtable;
                const char* str;
            };
            uintptr_t vt = Read<uintptr_t>(Base() + offsets::Got_SafeStringVtableBase) + 0x10;
            static char map_name[64], pos_name[64];
            std::snprintf(map_name, sizeof(map_name), "%s", arg);
            std::snprintf(pos_name, sizeof(pos_name), "%s", pos);
            Str m{vt, map_name}, q{vt, pos_name};
            Logging.Log("[coop] console: stage change to '%s' at '%s' (%d)", map_name, pos_name, num ? std::atoi(num) : 0);
            reinterpret_cast<void (*)(const Str*, const Str*, int, bool)>(Base() + 0x12082e0)(&m, &q, num ? std::atoi(num) : 0, false);
        } else if (!std::strcmp(cmd, "probe")) {
            std::snprintf(g_ProbeNames, sizeof(g_ProbeNames), "%s", arg ? arg : "");
            Logging.Log("[coop] console: probing creations of '%s'", g_ProbeNames);
        } else {
            Logging.Log("[coop] console: unknown command '%s'", cmd);
        }
    }
}
