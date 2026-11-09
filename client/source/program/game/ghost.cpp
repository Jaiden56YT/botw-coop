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
        };

        constexpr u8 EchoId = 255;
        constexpr int MaxGhosts = net::MaxPeers + 1;

        GhostSettings g_Settings = {"CoopGhost", false, ""};
        uintptr_t g_TestSpawnedFor = 0; /* player actor the test row was spawned for */
        bool g_Configured = false;
        Ghost g_Ghosts[MaxGhosts];
        uintptr_t g_LastPlayerActor = 0;
        s64 g_LastTickMs = 0;
        std::atomic<bool> g_InTick{false};

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

        uintptr_t SpawnNamed(const char* name, const float (&pos)[3], float rot_y) {
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
                    uintptr_t a = SpawnNamed(start, p, rot + 3.14159265f);
                    Logging.Log("[coop] test spawn %d '%s' at (%.1f, %.1f, %.1f): %s %p", i + 1, start, p[0], p[1], p[2],
                                a ? "ok" : "FAILED", reinterpret_cast<void*>(a));
                    i++;
                }
                start = comma ? comma + 1 : nullptr;
            }
        }

        void Place(uintptr_t actor, const float (&pos)[3], float rot_y) {
            auto* m = reinterpret_cast<float*>(actor + offsets::Actor_Mtx);
            float c = std::cos(rot_y), s = std::sin(rot_y);
            /* Row-major 3x4: rotation about Y, translation in column 3. */
            m[0] = c;  m[1] = 0; m[2] = s;  m[3] = pos[0];
            m[4] = 0;  m[5] = 1; m[6] = 0;  m[7] = pos[1];
            m[8] = -s; m[9] = 0; m[10] = c; m[11] = pos[2];
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

        void Drive(Ghost& g, const float (&target)[3], float target_rot, s64 now) {
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
                if (!g.actor) {
                    g.retry_after_ms = now + 3000;
                    Logging.Log("[coop] couldn't spawn ghost actor '%s'", g_Settings.actor_name);
                    return;
                }
                std::memcpy(g.shown, target, sizeof(g.shown));
                g.shown_rot = target_rot;
                Logging.Log("[coop] spawned ghost for player %d (%s) at %p", g.peer_id, g_Settings.actor_name,
                            reinterpret_cast<void*>(g.actor));
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
            Place(g.actor, g.shown, g.shown_rot);
            std::memcpy(g.written, g.shown, sizeof(g.written));
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
                    if (Ghost* g = FindOrAdd(EchoId)) Drive(*g, target, pick->rot + orbit + 1.5708f, now);
                }
            }

            net::Peer peers[net::MaxPeers];
            net::GetPeers(peers);
            s64 tick_hz = nn::os::GetSystemTickFrequency();
            u64 tick_now = nn::os::GetSystemTick().GetInt64Value();
            for (auto& p : peers) {
                if (!p.present || !p.has_state) continue;
                /* Extrapolate a little using velocity so motion looks continuous. */
                float age = static_cast<float>(tick_now - p.state_tick) / static_cast<float>(tick_hz);
                if (age > 0.25f) age = 0.25f;
                float target[3];
                for (int i = 0; i < 3; i++) target[i] = p.state.pos[i] + p.state.vel[i] * age;
                if (Ghost* g = FindOrAdd(p.id)) Drive(*g, target, p.state.rot_y, now);
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
                    g.used = false;
                    g.actor = 0;
                }
            }
        }
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
                g_InTick.store(false);
            }
            return result;
        }
    };

    void InstallGhosts() {
        GetPlayerPosHook::InstallAtOffset(offsets::PlayerInfo_GetPlayerPos);
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
