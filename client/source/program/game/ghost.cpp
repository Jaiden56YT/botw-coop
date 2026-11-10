#include "ghost.hpp"

#include <atomic>
#include <cmath>
#include <cstring>

#include <lib.hpp>
#include <nn/os.hpp>

#include "../net/client.hpp"
#include "offsets.hpp"
#include "player.hpp"
#include "world.hpp"
#include "progress.hpp"
#include "autostart.hpp"
#include "anim.hpp"
#include "map.hpp"
#include "combat.hpp"
#include "revive.hpp"
#include "gear.hpp"
#include "items.hpp"
#include "warp.hpp"
#include "notify.hpp"
#include "status.hpp"
#include "research.hpp"
#include "enemies.hpp"
#include "meals.hpp"
#include "weapons.hpp"

namespace coop::game {

    namespace {

        using CreateActorFn = uintptr_t (*)(uintptr_t creator, const char* name, uintptr_t heap, void* params,
                                            bool sleep_after_init, bool block_other_spawns);
        using CsFn = void (*)(uintptr_t cs);
        using CsTryFn = bool (*)(uintptr_t cs);

        /* ksys::act::InstParamPack, built by hand. */
        struct ParamPack {
            uintptr_t vtable;
            uintptr_t proc;
            u16 count;
            s16 position;
            u8 data[0xc0];

            void Add(const char* key, u8 type, const void* value, size_t size) {
                if (position + sizeof(key) + 1 + size > sizeof(data)) return;
                std::memcpy(&data[position], &key, sizeof(key));
                position += sizeof(key);
                data[position++] = type;
                std::memcpy(&data[position], value, size);
                position += size;
                count++;
            }
        };
        static_assert(sizeof(ParamPack) == 0xd8);

        constexpr u8 ParamTypeVec3 = 4;
        constexpr u8 ParamTypeString = 5; /* stored inline, NUL-terminated */

        struct Ghost {
            bool used;
            u8 peer_id;           /* 255 = echo ghost */
            uintptr_t actor;      /* Actor*, or 0 while not spawned */
            s64 retry_after_ms;   /* back-off after a failed spawn */
            float shown[3];       /* smoothed position actually displayed */
            float shown_rot;
            float written[3];     /* translation we wrote last frame */
            u32 frames, overridden; /* how often the game changed it before our next write */
            float game_pos[3];    /* last position the game itself put it at */
            s64 report_ms;
            const char* anim;     /* animation sequence currently playing */
            s64 anim_retry_at;    /* when to ask again for one that failed to start */
            int anim_fails;
            s64 spawned_ms;       /* when the puppet actor was created */
            float speed, vspeed;  /* smoothed horizontal / vertical speed, m/s */
            s64 last_ms;
            bool hidden;          /* parked out of sight: the player is in another area */
            /* Worn armor, shown as CoopGear_<model> actors moved and animated with the body. */
            char want_gear[3][64]; /* gear actor names wanted per slot ("" = none) */
            char gear_name[3][64];
            uintptr_t gear[3];
            const char* gear_anim[3];
            uintptr_t parked[6];   /* gear taken off, kept out of sight for reuse */
            char parked_name[6][64];
            /* Carried weapons: CoopWeapon_<model> actors bound to the puppet's bones. */
            net::WeaponPose want_weapons[net::MaxWeapons];
            int want_weapon_count;
            uintptr_t weapon[net::MaxWeapons];
            char weapon_actor[net::MaxWeapons][64];
            weapons::Bind weapon_bind[net::MaxWeapons];
            net::WeaponPose weapon_pose[net::MaxWeapons];
            /* A horse under them while they ride. */
            uintptr_t horse;
            const char* horse_anim;
            bool horse_shown;
        };

        constexpr u8 EchoId = 255;
        constexpr int MaxGhosts = net::MaxPeers + 1;

        GhostSettings g_Settings = {"CoopPlayer", false, ""};
        uintptr_t g_TestSpawnedFor = 0; /* player actor the test row was spawned for */
        bool g_Configured = false;
        Ghost g_Ghosts[MaxGhosts];
        uintptr_t g_LastPlayerActor = 0;
        u32 g_LoggedArea = 0xFFFFFFFF;
        s64 g_LastTickMs = 0;
        std::atomic<bool> g_InTick{false};
        std::atomic<bool> g_LogPlayerAS{false}; /* debug: log the local player's animation sequences */
        std::atomic<u32> g_LocalAnim{0};        /* anim::Category of the local player's body */
        std::atomic<u32> g_LocalAction{0};      /* anim::Action in progress, and until when: */
        std::atomic<s64> g_LocalActionUntil{0};
        /* Note: no function-local statics with runtime initialisers in this
           module. They compile to __cxa_guard_acquire, which hung the game. */
        s64 g_MoveTestAt = 0;
        s64 g_JumpAt = 0;
        int g_ActionStep = 0;

        /* Echo mode: ring of our own recent positions. */
        constexpr int EchoSlots = 64;
        struct EchoSample { s64 ms; float pos[3]; float rot; };
        EchoSample g_Echo[EchoSlots];
        int g_EchoHead = 0;

        uintptr_t Base() { return exl::util::GetMainModuleInfo().m_Total.m_Start; }
        template<typename T> T Read(uintptr_t addr) { return *reinterpret_cast<const T*>(addr); }

        s64 NowMs() { return nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds(); }

        uintptr_t ActorCreator() {
            uintptr_t slot = Read<uintptr_t>(Base() + offsets::Got_ActorCreatorInstance);
            return slot ? Read<uintptr_t>(slot) : 0;
        }

        /* True if `actor` is still in ActorCreator's live actor list. If the
           list lock is busy we can't tell; report "unknown" via *known=false. */
        bool IsActorAlive(uintptr_t creator, uintptr_t actor, bool* known) {
            uintptr_t cs = creator + offsets::ActorCreator_ActorListCS;
            auto try_lock = reinterpret_cast<CsTryFn>(Base() + offsets::CriticalSection_TryLock);
            auto unlock = reinterpret_cast<CsFn>(Base() + offsets::CriticalSection_Unlock);
            if (!try_lock(cs)) { *known = false; return false; }
            *known = true;

            uintptr_t head = creator + offsets::ActorCreator_ActorList;
            s32 node_off = Read<s32>(creator + offsets::ActorCreator_ActorListOffset);
            bool found = false;
            int guard = 0;
            for (uintptr_t node = Read<uintptr_t>(head + 8); node != head && node != 0 && guard < 20000; guard++) {
                if (node - node_off == actor) { found = true; break; }
                node = Read<uintptr_t>(node + 8);
            }
            unlock(cs);
            return found;
        }

        uintptr_t SpawnNamed(const char* name, const float (&pos)[3], float rot_y, const char* model_user = nullptr) {
            uintptr_t creator = ActorCreator();
            uintptr_t heap_slot = Read<uintptr_t>(Base() + offsets::Got_SpawnHeapOwner);
            uintptr_t heap_owner = heap_slot ? Read<uintptr_t>(heap_slot) : 0;
            if (!creator || !heap_owner) return 0;
            uintptr_t heap = Read<uintptr_t>(heap_owner + offsets::SpawnHeapOwner_Heap);

            static const char KeyPos[] = "@P";
            static const char KeyRot[] = "@R";
            ParamPack pack{};
            pack.vtable = Base() + offsets::InstParamPack_Vtable;
            float rot[3] = {0, rot_y, 0};
            pack.Add(KeyPos, ParamTypeVec3, pos, sizeof(pos));
            pack.Add(KeyRot, ParamTypeVec3, rot, sizeof(rot));
            /* "@MU": show another actor's model list (gear on the puppet). */
            static const char KeyModelUser[] = "@MU";
            if (model_user && model_user[0])
                pack.Add(KeyModelUser, ParamTypeString, model_user, std::strlen(model_user) + 1);

            auto create = reinterpret_cast<CreateActorFn>(Base() + offsets::ActorCreator_CreateActor);
            uintptr_t actor = create(creator, name, heap, &pack, false, false);
            if (actor) {
                /* Keep it loaded however far away the other player goes. */
                auto* flags = reinterpret_cast<u32*>(actor + offsets::Actor_Flags2);
                *flags |= offsets::ActorFlag2_NoDistanceCheck;
            }
            return actor;
        }

        uintptr_t Spawn(const float (&pos)[3], float rot_y) { return SpawnNamed(g_Settings.actor_name, pos, rot_y); }

        /* Debug: spawn each actor in g_Settings.test_spawn once, in a ring 3 m
           around Link, and never touch them again. Shows which
           actors render when spawned this way. */
        void SpawnTestRow(const float (&me)[3], float rot) {
            float fx = std::sin(rot), fz = std::cos(rot); /* forward */
            float rx = std::cos(rot), rz = -std::sin(rot); /* right */
            char names[sizeof(g_Settings.test_spawn)];
            std::memcpy(names, g_Settings.test_spawn, sizeof(names));
            int count = 1;
            for (char* c = names; *c; c++) if (*c == ',') count++;
            int i = 0;
            char* start = names;
            while (start && *start) {
                char* comma = std::strchr(start, ',');
                if (comma) *comma = 0;
                while (*start == ' ') start++;
                char* end = start + std::strlen(start);
                while (end > start && end[-1] == ' ') *--end = 0;
                if (*start) {
                    /* A ring 3 m around Link, 1 m up, so several are on screen
                       whichever way the camera faces. */
                    float ang = rot + 6.2831853f * i / count;
                    float p[3] = {me[0] + std::sin(ang) * 3.0f, me[1] + 1.0f, me[2] + std::cos(ang) * 3.0f};
                    (void)fx; (void)fz; (void)rx; (void)rz;
                    /* "Actor:ModelUser" spawns Actor showing ModelUser's model. */
                    char* colon = std::strchr(start, ':');
                    if (colon) *colon = '\0';
                    uintptr_t a = SpawnNamed(start, p, rot + 3.14159265f, colon ? colon + 1 : nullptr);
                    Logging.Log("[coop] test spawn %d '%s' at (%.1f, %.1f, %.1f): %s %p", i + 1, start, p[0], p[1], p[2],
                                a ? "ok" : "FAILED", reinterpret_cast<void*>(a));
                    i++;
                }
                start = comma ? comma + 1 : nullptr;
            }
        }

        struct SafeString {
            uintptr_t vtable;
            const char* str;
        };

    }
    bool PlayAS(uintptr_t actor, const char* name, int target);
    namespace {

        void Place(uintptr_t actor, const float (&pos)[3], float rot_y) {
            auto* m = reinterpret_cast<float*>(actor + offsets::Actor_Mtx);
            float c = std::cos(rot_y), s = std::sin(rot_y);
            /* Row-major 3x4: rotation about Y, translation in column 3. */
            m[0] = c;  m[1] = 0; m[2] = s;  m[3] = pos[0];
            m[4] = 0;  m[5] = 1; m[6] = 0;  m[7] = pos[1];
            m[8] = -s; m[9] = 0; m[10] = c; m[11] = pos[2];
        }

        /* Picks an animation from how the puppet is actually moving and plays it
           when it changes. Actors without these sequences just ignore the call. */
        void Animate(Ghost& g, const float (&before)[3], s64 now, u32 category) {
            if (g.last_ms != 0 && now > g.last_ms && g.frames > 1) {
                float dt = (now - g.last_ms) / 1000.0f;
                float dx = g.shown[0] - before[0], dy = g.shown[1] - before[1], dz = g.shown[2] - before[2];
                float h = std::sqrt(dx * dx + dz * dz) / dt;
                float v = dy / dt;
                if (h < 50.0f) g.speed += (h - g.speed) * 0.2f; /* ignore teleports */
                if (std::fabs(v) < 50.0f) g.vspeed += (v - g.vspeed) * 0.2f;
            }
            g.last_ms = now;

            u32 body = category & 0xFF, action = (category >> 8) & 0xFF;
            const char* want = action ? anim::PuppetActionAS(static_cast<anim::Action>(action)) : nullptr;
            if (!want && body) want = anim::PuppetAS(static_cast<anim::Category>(body), g.speed);
            if (want) {
                /* The partner told us what they're doing. */
            } else if (g.vspeed < -9.0f) want = "Fall";
            else if (g.vspeed < -0.8f && g.speed > 4.0f) want = "Glide";
            else if (g.speed < 0.4f) want = "Wait";
            else if (g.speed < 2.6f) want = "Walk";
            else if (g.speed < 6.5f) want = "Run";
            else want = "Dash";

            /* Start it once when it changes. The call's result says nothing
               (it reported "no" for sequences that visibly played, 'Wait' and
               'Down' among them), and asking again restarts the clip: a puppet
               asked every 200 ms never got past a collapse's first frames.
               A puppet that was only just created can still be setting up
               and ignore the call, so in its first seconds it's asked a few
               more times. */
            if (want != g.anim) {
                PlayAS(g.actor, want, 0);
                g.anim = want;
                g.anim_fails = 0;
                g.anim_retry_at = now + 500;
            } else if (g.anim_fails < 3 && now - g.spawned_ms < 5000 && now >= g.anim_retry_at) {
                PlayAS(g.actor, want, 0);
                g.anim_fails++;
                g.anim_retry_at = now + 1000;
            }
        }

        float LerpAngle(float a, float b, float t) {
            float d = std::fmod(b - a + 3.14159265f * 3, 3.14159265f * 2) - 3.14159265f;
            return a + d * t;
        }

        Ghost* FindOrAdd(u8 id) {
            for (auto& g : g_Ghosts) if (g.used && g.peer_id == id) return &g;
            for (auto& g : g_Ghosts) if (!g.used) { g = {}; g.used = true; g.peer_id = id; return &g; }
            return nullptr;
        }

        void ParkGear(Ghost& g, int s) {
            if (!g.gear[s]) return;
            float away[3] = {g.shown[0], -2000.0f, g.shown[2]};
            bool known = false;
            if (IsActorAlive(ActorCreator(), g.gear[s], &known) && known) {
                Place(g.gear[s], away, 0);
                for (int i = 0; i < 6; i++) {
                    if (!g.parked[i]) {
                        g.parked[i] = g.gear[s];
                        std::memcpy(g.parked_name[i], g.gear_name[s], sizeof(g.parked_name[i]));
                        break;
                    }
                }
            }
            g.gear[s] = 0;
            g.gear_name[s][0] = '\0';
            g.gear_anim[s] = nullptr;
        }

        /* Puts on / takes off gear pieces to match want_gear, then moves them with the body. */
        void DriveGear(Ghost& g, s64 now) {
            for (int s = 0; s < 3; s++) {
                if (std::strcmp(g.want_gear[s], g.gear_name[s]) == 0) continue;
                ParkGear(g, s);
                if (!g.want_gear[s][0]) continue;
                /* Reuse a parked piece of the same model if we have one. */
                for (int i = 0; i < 6 && !g.gear[s]; i++) {
                    if (g.parked[i] && std::strcmp(g.parked_name[i], g.want_gear[s]) == 0) {
                        bool known = false;
                        if (IsActorAlive(ActorCreator(), g.parked[i], &known) && known) g.gear[s] = g.parked[i];
                        g.parked[i] = 0;
                    }
                }
                if (!g.gear[s]) g.gear[s] = SpawnNamed(g.want_gear[s], g.shown, g.shown_rot);
                if (g.gear[s]) {
                    std::memcpy(g.gear_name[s], g.want_gear[s], sizeof(g.gear_name[s]));
                    Logging.Log("[coop] player %d puts on %s", g.peer_id, g.gear_name[s]);
                } else {
                    /* Unknown or failed: don't retry every frame. */
                    std::memcpy(g.gear_name[s], g.want_gear[s], sizeof(g.gear_name[s]));
                }
            }
            for (int s = 0; s < 3; s++) {
                if (!g.gear[s]) continue;
                Place(g.gear[s], g.shown, g.shown_rot);
                if (g.anim && g.gear_anim[s] != g.anim && PlayAS(g.gear[s], g.anim, 0)) g.gear_anim[s] = g.anim;
            }
            (void)now;
        }

        void ParkWeapon(Ghost& g, int i) {
            if (!g.weapon[i]) return;
            float away[3] = {g.shown[0], -2000.0f, g.shown[2]};
            bool known = false;
            if (IsActorAlive(ActorCreator(), g.weapon[i], &known) && known) {
                reinterpret_cast<void (*)(uintptr_t)>(Base() + offsets::Actor_ClearModelBindInfo)(g.weapon[i]);
                Place(g.weapon[i], away, 0);
            }
            g.weapon[i] = 0;
            g.weapon_actor[i][0] = '\0';
        }

        /* Spawns, binds and moves the partner's weapons onto the puppet's bones. */
        void DriveWeapons(Ghost& g) {
            for (int i = 0; i < net::MaxWeapons; i++) {
                bool want = i < g.want_weapon_count;
                char actor_name[64] = {};
                if (want && !weapons::WeaponActorFor(g.want_weapons[i].name, actor_name)) want = false;
                if (!want) {
                    ParkWeapon(g, i);
                    continue;
                }
                const net::WeaponPose& p = g.want_weapons[i];
                if (std::strcmp(actor_name, g.weapon_actor[i]) != 0) {
                    ParkWeapon(g, i);
                    g.weapon[i] = SpawnNamed(actor_name, g.shown, g.shown_rot);
                    std::memcpy(g.weapon_actor[i], actor_name, sizeof(actor_name));
                    g.weapon_pose[i].bone[0] = '\0'; /* force a bind */
                    if (g.weapon[i]) Logging.Log("[coop] player %d carries %s", g.peer_id, p.name);
                }
                if (!g.weapon[i]) continue;
                bool rebind = std::strcmp(p.bone, g.weapon_pose[i].bone) != 0 ||
                              std::memcmp(p.offset, g.weapon_pose[i].offset, sizeof(p.offset)) != 0;
                if (rebind) {
                    weapons::BindTo(g.weapon_bind[i], g.weapon[i], g.actor, p.bone, p.offset);
                    g.weapon_pose[i] = p;
                }
                weapons::Update(g.weapon_bind[i], g.weapon[i]);
            }
        }

        /* Where the rider is on the horse. Horse_Link.bfres's Saddle_Root bone sits
           2.10 m above the horse's origin and 0.05 m forward (rest pose), and
           the riding animations keep Link's hips on his origin, so a rider's
           position is the saddle. */
        constexpr float SaddleHeight = 2.10f;
        constexpr float SaddleForward = 0.05f;

        void DriveHorse(Ghost& g, u32 category) {
            bool riding = (category & 0xFF) == anim::Horse;
            if (!riding) {
                if (g.horse && g.horse_shown) {
                    float away[3] = {g.shown[0], -2000.0f, g.shown[2]};
                    bool known = false;
                    if (IsActorAlive(ActorCreator(), g.horse, &known) && known) Place(g.horse, away, 0);
                    g.horse_shown = false;
                }
                return;
            }
            float at[3] = {g.shown[0] - std::sin(g.shown_rot) * SaddleForward, g.shown[1] - SaddleHeight,
                           g.shown[2] - std::cos(g.shown_rot) * SaddleForward};
            if (g.horse) {
                bool known = false;
                if (!IsActorAlive(ActorCreator(), g.horse, &known) && known) g.horse = 0;
            }
            if (!g.horse) {
                g.horse = SpawnNamed("CoopHorse", at, g.shown_rot);
                g.horse_anim = nullptr;
                if (!g.horse) return;
                Logging.Log("[coop] player %d is riding: horse %p", g.peer_id, reinterpret_cast<void*>(g.horse));
            }
            g.horse_shown = true;
            Place(g.horse, at, g.shown_rot);
            /* Gait from the rider's speed (m/s). */
            const char* want = g.speed < 0.5f ? "Wait" : g.speed < 3.0f ? "Move_Gear1" : g.speed < 7.0f ? "Move_Gear2"
                             : g.speed < 12.0f ? "Move_Gear3" : "Move_Gear4";
            if (want != g.horse_anim && PlayAS(g.horse, want, 0)) g.horse_anim = want;
        }

        /* Debug: while the local Link rides, log where he is relative to his
           horse (to measure SaddleHeight). */
        s64 g_MountLogAt = 0;
        void LogMountedOffset(s64 now) {
            if ((LocalAnimCategory() & 0xFF) != anim::Horse || now < g_MountLogAt) return;
            g_MountLogAt = now + 2000;
            float me[3];
            if (!GetLocalPlayerPos(me)) return;
            uintptr_t creator = ActorCreator();
            if (!creator) return;
            uintptr_t cs = creator + offsets::ActorCreator_ActorListCS;
            if (!reinterpret_cast<bool (*)(uintptr_t)>(Base() + offsets::CriticalSection_TryLock)(cs)) return;
            uintptr_t head = creator + offsets::ActorCreator_ActorList;
            s32 node_off = Read<s32>(creator + offsets::ActorCreator_ActorListOffset);
            uintptr_t best = 0;
            float best_d = 25.0f;
            int guard = 0;
            for (uintptr_t node = Read<uintptr_t>(head + 8); node != head && node != 0 && guard < 20000; guard++) {
                uintptr_t a = node - node_off;
                node = Read<uintptr_t>(node + 8);
                const char* name = Read<const char*>(a + offsets::BaseProc_NameStr);
                if (!name || std::strncmp(name, "GameRomHorse", 12) != 0) continue;
                const float* m = reinterpret_cast<const float*>(a + offsets::Actor_Mtx);
                float dx = m[3] - me[0], dy = m[7] - me[1], dz = m[11] - me[2];
                float d = dx * dx + dy * dy + dz * dz;
                if (d < best_d) { best_d = d; best = a; }
            }
            if (best) {
                const float* m = reinterpret_cast<const float*>(best + offsets::Actor_Mtx);
                float dx = me[0] - m[3], dy = me[1] - m[7], dz = me[2] - m[11];
                /* Along the horse's forward (its local z axis) and right (x axis). */
                float fwd = dx * m[2] + dy * m[6] + dz * m[10];
                float right = dx * m[0] + dy * m[4] + dz * m[8];
                Logging.Log("[coop] riding: Link is %.3f m above his horse, %.3f m forward, %.3f m right (%s)", dy, fwd,
                            right, Read<const char*>(best + offsets::BaseProc_NameStr));
            }
            reinterpret_cast<void (*)(uintptr_t)>(Base() + offsets::CriticalSection_Unlock)(cs);
        }

        void Drive(Ghost& g, const float (&target)[3], float target_rot, s64 now, u32 category) {
            uintptr_t creator = ActorCreator();
            if (g.actor) {
                bool known = false;
                bool alive = IsActorAlive(creator, g.actor, &known);
                if (!known) return; /* list busy this frame; try again next frame */
                if (!alive) {
                    Logging.Log("[coop] ghost for player %d was removed by the game; respawning", g.peer_id);
                    g.actor = 0;
                }
            }
            if (!g.actor) {
                if (now < g.retry_after_ms) return;
                g.actor = Spawn(target, target_rot);
                g.spawned_ms = now;
                if (!g.actor) {
                    g.retry_after_ms = now + 3000;
                    Logging.Log("[coop] couldn't spawn ghost actor '%s'", g_Settings.actor_name);
                    return;
                }
                std::memcpy(g.shown, target, sizeof(g.shown));
                g.shown_rot = target_rot;
                g.anim = nullptr;
                Logging.Log("[coop] spawned ghost for player %d (%s) at %p, ASList %p", g.peer_id, g_Settings.actor_name,
                            reinterpret_cast<void*>(g.actor),
                            reinterpret_cast<void*>(Read<uintptr_t>(g.actor + offsets::Actor_ASList)));
            }
            if (g.hidden) {
                /* Back in our area: appear where they are, not where they left. */
                g.hidden = false;
                std::memcpy(g.shown, target, sizeof(g.shown));
                Logging.Log("[coop] player %d is back in this area", g.peer_id);
            }
            /* Ease toward the target; snap if it jumped (fast travel, respawn). */
            float dx = target[0] - g.shown[0], dy = target[1] - g.shown[1], dz = target[2] - g.shown[2];
            if (dx * dx + dy * dy + dz * dz > 50.0f * 50.0f) {
                std::memcpy(g.shown, target, sizeof(g.shown));
            } else {
                for (int i = 0; i < 3; i++) g.shown[i] += (target[i] - g.shown[i]) * 0.35f;
            }
            g.shown_rot = LerpAngle(g.shown_rot, target_rot, 0.35f);

            /* Does the game move the actor between our writes? If it does, the
               ghost would fight us (snap back to its spawn point, play an
               animation that moves it, ...). */
            const float* m = reinterpret_cast<const float*>(g.actor + offsets::Actor_Mtx);
            if (g.frames > 0) {
                float ex = m[3] - g.written[0], ey = m[7] - g.written[1], ez = m[11] - g.written[2];
                if (ex * ex + ey * ey + ez * ez > 0.0001f) {
                    g.overridden++;
                    g.game_pos[0] = m[3]; g.game_pos[1] = m[7]; g.game_pos[2] = m[11];
                }
            }
            g.frames++;
            if (now - g.report_ms > 10000) {
                g.report_ms = now;
                Logging.Log("[coop] ghost %d at (%.1f, %.1f, %.1f); game moved it in %u of %u frames, last to (%.1f, %.1f, %.1f)",
                            g.peer_id, g.shown[0], g.shown[1], g.shown[2], g.overridden, g.frames,
                            g.game_pos[0], g.game_pos[1], g.game_pos[2]);
            }
            float before[3];
            std::memcpy(before, g.written, sizeof(before));
            Place(g.actor, g.shown, g.shown_rot);
            std::memcpy(g.written, g.shown, sizeof(g.written));
            Animate(g, before, now, category);
            DriveGear(g, now);
            DriveWeapons(g);
            DriveHorse(g, category);
        }

        bool LocalPlayer(uintptr_t* actor_out, float (&pos)[3], float* rot) {
            uintptr_t info = Read<uintptr_t>(Base() + offsets::PlayerInfoInstance);
            *actor_out = info ? Read<uintptr_t>(info + offsets::PlayerInfo_PlayerActor) : 0;
            return GetLocalPlayer(pos, rot);
        }

        void Tick() {
            s64 now = NowMs();
            if (now - g_LastTickMs < 15) return;
            g_LastTickMs = now;

            uintptr_t player_actor = 0;
            float me[3];
            float my_rot = 0;
            bool have_me = LocalPlayer(&player_actor, me, &my_rot);

            /* A new player actor means a stage load: every actor we spawned is gone. */
            if (player_actor != g_LastPlayerActor) {
                for (auto& g : g_Ghosts) g.actor = 0;
                g_LastPlayerActor = player_actor;
            }
            if (!have_me) return;

            /* Debug: walk forward, run, jump, so the player's own animation
               names show up in the log. */
            if (g_Settings.test_move && g_MoveTestAt == 0) g_MoveTestAt = now + 4000;
            if (g_MoveTestAt > 0 && now > g_MoveTestAt) {
                g_MoveTestAt = -1;
                g_JumpAt = now + 3000;
                g_LogPlayerAS.store(true);
                HoldStick(0, 32767, 6000);
                Logging.Log("[coop] test: moving forward");
            }
            if (g_JumpAt > 0 && now > g_JumpAt) {
                /* Then a fixed sequence of actions, logging the sequences they start. */
                struct Step { u64 mask; int ms; const char* what; };
                const Step steps[] = {
                    {1ull << 2, 150, "jump (X)"},     {1ull << 3, 150, "attack (Y)"}, {1ull << 3, 150, "attack (Y)"},
                    {1ull << 3, 150, "attack (Y)"},   {1ull << 8, 2000, "guard (ZL)"}, {1ull << 9, 2500, "bow (ZR)"},
                    {1ull << 3, 2500, "charge (hold Y)"}, {1ull << 7, 900, "throw (hold R)"},
                };
                int i = g_ActionStep++;
                if (i < static_cast<int>(sizeof(steps) / sizeof(steps[0]))) {
                    PressButtons(steps[i].mask, steps[i].ms);
                    Logging.Log("[coop] test: %s", steps[i].what);
                    g_JumpAt = now + steps[i].ms + 1500;
                } else {
                    g_JumpAt = -1;
                }
            }

            if (g_Settings.test_spawn[0] && g_TestSpawnedFor != player_actor) {
                g_TestSpawnedFor = player_actor;
                SpawnTestRow(me, my_rot);
            }

            if (g_Settings.echo) {
                g_Echo[g_EchoHead] = {now, {me[0], me[1], me[2]}, my_rot};
                g_EchoHead = (g_EchoHead + 1) % EchoSlots;
                /* Oldest sample at least 1 s old. */
                const EchoSample* pick = nullptr;
                for (int i = 0; i < EchoSlots; i++) {
                    const auto& e = g_Echo[(g_EchoHead + i) % EchoSlots];
                    if (e.ms != 0 && now - e.ms >= 1000) pick = &e;
                }
                if (pick) {
                    /* 2 m to the right of where you were, slowly circling you so
                       movement is visible even when you stand still. */
                    float orbit = (now % 12000) / 12000.0f * 6.2831853f;
                    float c = std::cos(pick->rot + orbit), s = std::sin(pick->rot + orbit);
                    float target[3] = {pick->pos[0] + c * 2.5f, pick->pos[1], pick->pos[2] - s * 2.5f};
                    if (Ghost* g = FindOrAdd(EchoId)) {
                        /* Echo carries our own weapons, to check them with one game. */
                        g->want_weapon_count = weapons::LocalPoses(g->want_weapons);
                        char worn[gear::Slots][gear::NameLen];
                        gear::LocalWorn(worn);
                        for (int s = 0; s < gear::Slots; s++) {
                            char want[64] = {};
                            if (worn[s][0] && !gear::GearActorFor(worn[s], want)) want[0] = '\0';
                            std::memcpy(g->want_gear[s], want, sizeof(want));
                        }
                        Drive(*g, target, pick->rot + orbit + 1.5708f, now, 0);
                    }
                }
            }

            net::Peer peers[net::MaxPeers];
            net::GetPeers(peers);
            s64 tick_hz = nn::os::GetSystemTickFrequency();
            u64 tick_now = nn::os::GetSystemTick().GetInt64Value();
            u32 my_area = LocalArea();
            if (my_area != g_LoggedArea) {
                g_LoggedArea = my_area;
                Logging.Log("[coop] area: %s / %s (%08x)", LocalMapType(), LocalMapName(), my_area);
            }
            for (auto& p : peers) {
                if (!p.present || !p.has_state) continue;
                /* In another area (a shrine, a Divine Beast, or the overworld while
                   we're in one): their coordinates mean nothing here. Hide them. */
                if (p.state.map_area != my_area) {
                    for (auto& g : g_Ghosts) {
                        if (!g.used || g.peer_id != p.id || !g.actor || g.hidden) continue;
                        bool known = false;
                        if (IsActorAlive(ActorCreator(), g.actor, &known) && known) {
                            float away[3] = {g.shown[0], -2000.0f, g.shown[2]};
                            Place(g.actor, away, 0);
                            for (uintptr_t piece : g.gear)
                                if (piece) Place(piece, away, 0);
                            for (int wi = 0; wi < net::MaxWeapons; wi++) ParkWeapon(g, wi);
                            if (g.horse) {
                                Place(g.horse, away, 0);
                                g.horse_shown = false;
                            }
                            g.hidden = true;
                            Logging.Log("[coop] player %d is in another area; hiding them", g.peer_id);
                        }
                    }
                    continue;
                }
                /* Extrapolate a little using velocity so motion looks continuous. */
                float age = static_cast<float>(tick_now - p.state_tick) / static_cast<float>(tick_hz);
                if (age > 0.25f) age = 0.25f;
                float target[3];
                for (int i = 0; i < 3; i++) target[i] = p.state.pos[i] + p.state.vel[i] * age;
                if (Ghost* g = FindOrAdd(p.id)) {
                    for (int s = 0; s < 3; s++) {
                        char want[64] = {};
                        if (p.gear[s][0] && !gear::GearActorFor(p.gear[s], want)) want[0] = '\0';
                        std::memcpy(g->want_gear[s], want, sizeof(want));
                    }
                    std::memcpy(g->want_weapons, p.weapons, sizeof(p.weapons));
                    g->want_weapon_count = p.weapon_count;
                    Drive(*g, target, p.state.rot_y, now, p.state.anim_hash);
                }
            }

            /* Players who left: park their ghost out of sight. */
            for (auto& g : g_Ghosts) {
                if (!g.used || g.peer_id == EchoId || !g.actor) continue;
                bool present = false;
                for (auto& p : peers) if (p.present && p.id == g.peer_id) present = true;
                if (!present) {
                    float away[3] = {g.shown[0], -2000.0f, g.shown[2]};
                    bool known = false;
                    if (IsActorAlive(ActorCreator(), g.actor, &known) && known) Place(g.actor, away, 0);
                    for (int sl = 0; sl < 3; sl++) ParkGear(g, sl);
                    for (int wi = 0; wi < net::MaxWeapons; wi++) ParkWeapon(g, wi);
                    if (g.horse && IsActorAlive(ActorCreator(), g.horse, &known) && known) Place(g.horse, away, 0);
                    g.used = false;
                    g.actor = 0;
                }
            }
        }
    }

    namespace {
        uintptr_t LocalPlayerASList() {
            uintptr_t info = Read<uintptr_t>(Base() + offsets::PlayerInfoInstance);
            uintptr_t actor = info ? Read<uintptr_t>(info + offsets::PlayerInfo_PlayerActor) : 0;
            return actor ? Read<uintptr_t>(actor + offsets::Actor_ASList) : 0;
        }
    }

    uintptr_t SpawnActor(const char* name, const float (&pos)[3], float rot_y) { return SpawnNamed(name, pos, rot_y); }

    /* Sees every animation sequence the game starts. Used to learn which
       sequence the local player is in (debug logging for now). */
    namespace {
        std::atomic<bool> g_OwnAS{false};          /* we're starting a sequence on the local player ourselves */
        std::atomic<bool> g_HoldPose{false};       /* keep the sequence we started: drop the game's own */
    }

    HOOK_DEFINE_TRAMPOLINE(StartAnimationHook) {
        static void Callback(uintptr_t as_list, float a, float b, const SafeString& name, int target, int bank, bool flag) {
            /* Lying down, waiting for a partner: Link's own state machine
               keeps asking for its standing sequences (it noticed ours and
               put WaitAttention back within a frame). Those are dropped
               until he's up; body and upper body only. */
            if (g_HoldPose.load(std::memory_order_relaxed) && target <= 1 && !g_OwnAS.load(std::memory_order_relaxed) &&
                as_list == LocalPlayerASList())
                return;
            Orig(as_list, a, b, name, target, bank, flag);
            if (!name.str || as_list != LocalPlayerASList()) return;
            if (target != 0 && g_LogPlayerAS.load(std::memory_order_relaxed))
                Logging.Log("[coop] player AS slot %d '%s'", target, name.str);
            /* Attacks, bow, throw, guard: any slot. */
            int ms = 0;
            bool ends_hold = false;
            anim::Action act = anim::ActionFromPlayerAS(name.str, target, &ms, &ends_hold);
            if (act != anim::NoAction) {
                g_LocalAction.store(act, std::memory_order_relaxed);
                g_LocalActionUntil.store(NowMs() + ms, std::memory_order_relaxed);
            } else if (ends_hold) {
                u32 cur = g_LocalAction.load(std::memory_order_relaxed);
                if (cur == anim::ChargeWait || cur == anim::ThrowAim) g_LocalActionUntil.store(0, std::memory_order_relaxed);
            }
            /* Slot 0 is the player's main body sequence. */
            if (target == 0 && name.str && as_list == LocalPlayerASList()) {
                anim::Category c = anim::FromPlayerAS(name.str);
                if (c != anim::Unknown) g_LocalAnim.store(c, std::memory_order_relaxed);
                if (g_LogPlayerAS.load(std::memory_order_relaxed))
                    Logging.Log("[coop] player AS '%s' -> category %u", name.str, (u32)c);
            }
        }
    };

    bool PlayLocalAS(const char* name) {
        uintptr_t info = Read<uintptr_t>(Base() + offsets::PlayerInfoInstance);
        uintptr_t actor = info ? Read<uintptr_t>(info + offsets::PlayerInfo_PlayerActor) : 0;
        if (!actor) return false;
        g_OwnAS.store(true);
        bool ok = PlayAS(actor, name, 0);
        g_OwnAS.store(false);
        return ok;
    }

    void HoldLocalPose(bool on) { g_HoldPose.store(on); }

    u32 LocalAnimCategory() {
        u32 body = g_LocalAnim.load(std::memory_order_relaxed);
        if (revive::IsDown()) return anim::Down;
        u32 action = NowMs() < g_LocalActionUntil.load(std::memory_order_relaxed) ? g_LocalAction.load(std::memory_order_relaxed) : 0;
        return body | (action << 8);
    }

    bool PlayAS(uintptr_t actor, const char* name, int target) {
        uintptr_t as_list = actor ? Read<uintptr_t>(actor + offsets::Actor_ASList) : 0;
        if (!as_list) return false;
        uintptr_t slot = Read<uintptr_t>(Base() + offsets::Got_SafeStringVtableBase);
        SafeString str{slot + 0x10, name};
        auto start = reinterpret_cast<bool (*)(uintptr_t, float, float, const SafeString&, int, int, bool)>(
            Base() + offsets::ASList_StartAnimation);
        return start(as_list, -1.0f, -1.0f, str, target, 0, true);
    }

    HOOK_DEFINE_TRAMPOLINE(GetPlayerPosHook) {
        static void* Callback(void* player_info) {
            void* result = Orig(player_info);
            /* Many threads call this, and spawning an actor can call it again
               on the same thread. Only one tick runs at a time; nobody waits. */
            bool expected = false;
            if (g_Configured && g_InTick.compare_exchange_strong(expected, true)) {
                Tick();
                WorldTick(NowMs());
                progress::Tick();
                map::Tick(NowMs());
                combat::Tick(NowMs());
                revive::Tick(NowMs());
                gear::Tick(NowMs());
                weapons::Tick(NowMs());
                items::Tick(NowMs());
                warp::Tick(NowMs());
                notify::Tick(NowMs());
                status::Tick(NowMs());
                research::Tick(NowMs());
                enemies::Tick(NowMs());
                meals::Tick(NowMs());
                LogMountedOffset(NowMs());
                KeyScriptTick(NowMs());
                g_InTick.store(false);
            }
            return result;
        }
    };

    void InstallGhosts() {
        GetPlayerPosHook::InstallAtOffset(offsets::PlayerInfo_GetPlayerPos);
        StartAnimationHook::InstallAtOffset(offsets::ASList_StartAnimation);
    }

    void ConfigureGhosts(const GhostSettings& settings) {
        /* Wait out a tick in progress so it never sees half-copied settings. */
        bool expected = false;
        while (!g_InTick.compare_exchange_weak(expected, true)) {
            expected = false;
            nn::os::SleepThread(nn::TimeSpan::FromMilliSeconds(1));
        }
        g_Settings = settings;
        g_Configured = true;
        g_InTick.store(false);
        Logging.Log("[coop] ghosts: actor=%s echo=%s", g_Settings.actor_name, g_Settings.echo ? "on" : "off");
    }
}
