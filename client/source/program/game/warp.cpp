#include "warp.hpp"

#include <cmath>
#include <cstring>

#include <lib.hpp>
#include <nn/os.hpp>

#include "../net/client.hpp"
#include "anim.hpp"
#include "autostart.hpp"
#include "ghost.hpp"
#include "notify.hpp"
#include "offsets.hpp"
#include "player.hpp"

namespace coop::game::warp {

    namespace {
        uintptr_t Base() { return exl::util::GetMainModuleInfo().m_Total.m_Start; }
        template<typename T> T Read(uintptr_t a) { return *reinterpret_cast<const T*>(a); }

        /* Hold the left stick click for 1.5 s (the input hook keeps a long
           hold from the game, so Link doesn't crouch). Tried first: both
           stick clicks (the right one opens the Scope) and holding D-pad Down
           (it whistles, calling the horse and showing its own banner). */
        constexpr s64 GestureMs = 1500;
        constexpr s64 CooldownMs = 5000;
        constexpr s64 StaleMs = 3000;  /* a partner we haven't heard from this long isn't a target */
        constexpr float Behind = 2.0f; /* land this far behind the partner */

        /* The world only streams in around Link if he moves at most ~1.5 km
           at once: after 4 km the game keeps him frozen with the loading
           icon up for good (tested: 500 m and 1.5 km load in a few seconds,
           4 km never). Longer warps go in hops, high above the ground so no
           hop lands inside a mountain. While an area loads the game holds
           Link perfectly still; once he moves (starts to fall) it's loaded. */
        constexpr float HopMax = 250.0f;
        constexpr float SkyHeight = 1100.0f; /* above every peak */
        constexpr s64 HopTimeoutMs = 30000;

        s64 g_ReadyAt = 0;
        bool g_Fired = false;
        u8 g_LastTarget = 0xff;

        /* A warp in progress. */
        bool g_Active = false;
        u8 g_TargetId = 0;
        char g_TargetName[32];
        float g_Hop[3];
        s64 g_HopSince = 0;
        s64 g_LoadedAt = 0; /* the current hop's area loaded; held here a moment */
        int g_Hops = 0;
        constexpr s64 SettleMs = 500;

        /* Link's Havok position (see offsets.hpp), or null. */
        float* HavokPos() {
            uintptr_t g = Read<uintptr_t>(Base() + offsets::HavokPos_Global);
            if (!g) return nullptr;
            uintptr_t a = Read<uintptr_t>(g + offsets::HavokPos_A);
            if (!a) return nullptr;
            uintptr_t b = Read<uintptr_t>(a + offsets::HavokPos_B);
            if (!b) return nullptr;
            return reinterpret_cast<float*>(b + offsets::HavokPos_Pos);
        }

        s64 NowMsLocal() { return nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds(); }

        uintptr_t PlayerActor() {
            uintptr_t info = Read<uintptr_t>(Base() + offsets::PlayerInfoInstance);
            return info ? Read<uintptr_t>(info + offsets::PlayerInfo_PlayerActor) : 0;
        }

        /* Moves Link: his body in Havok, his matrix, PlayerInfo's copy, and
           the height a fall counts from (no fall damage from the warp). */
        bool Place(const float* to) {
            float* hk = HavokPos();
            uintptr_t actor = PlayerActor();
            uintptr_t info = Read<uintptr_t>(Base() + offsets::PlayerInfoInstance);
            if (!hk || !actor || !info) return false;
            hk[0] = to[0], hk[1] = to[1], hk[2] = to[2];
            /* And stop him: held in the air, Havok keeps adding fall speed
               (it reached the -64 m/s cap), which he'd land with. */
            float* vel = hk + offsets::HavokPos_ToVelocity / 4;
            vel[0] = vel[1] = vel[2] = 0;
            float* m = reinterpret_cast<float*>(actor + offsets::Actor_Mtx);
            m[3] = to[0], m[7] = to[1], m[11] = to[2];
            *reinterpret_cast<float*>(actor + offsets::Player_FallStartY) = to[1];
            float* ip = reinterpret_cast<float*>(info + offsets::PlayerInfo_PlayerPos);
            ip[0] = to[0], ip[1] = to[1], ip[2] = to[2];
            return true;
        }

        s32* LifePtr() {
            uintptr_t actor = PlayerActor();
            if (!actor) return nullptr;
            uintptr_t vt = Read<uintptr_t>(actor);
            return reinterpret_cast<s32* (*)(uintptr_t)>(Read<uintptr_t>(vt + offsets::ActorVt_GetLife))(actor);
        }

        /* A warp only moves you: hearts lost on the way (the cold up high,
           a hard landing) come back, until a moment after arriving. */
        s32 g_StartLife = 0;
        s64 g_GuardUntil = 0;
        void GuardLife(s64 now) {
            if (now > g_GuardUntil) return;
            s32* life = LifePtr();
            if (life && *life > 0 && *life < g_StartLife) *life = g_StartLife;
        }

        /* The partner we're warping to, if still around and in our area. */
        bool TargetSpot(float (&out)[3]) {
            net::Peer peers[net::MaxPeers];
            net::GetPeers(peers);
            for (auto& p : peers) {
                if (!p.present || p.id != g_TargetId || !p.has_state || p.state.map_area != LocalArea()) continue;
                float r = p.state.rot_y;
                out[0] = p.state.pos[0] - std::sin(r) * Behind;
                out[1] = p.state.pos[1] + 0.5f;
                out[2] = p.state.pos[2] - std::cos(r) * Behind;
                return true;
            }
            return false;
        }

        /* The next hop from `me` toward the partner; true if it's the last. */
        bool NextHop(const float* me, const float* to, float (&hop)[3]) {
            float dx = to[0] - me[0], dz = to[2] - me[2];
            float d = std::sqrt(dx * dx + dz * dz);
            if (d <= HopMax) {
                std::memcpy(hop, to, sizeof(hop));
                return true;
            }
            hop[0] = me[0] + dx / d * HopMax;
            hop[1] = SkyHeight;
            hop[2] = me[2] + dz / d * HopMax;
            return false;
        }

        void Finish(bool ok) {
            g_Active = false;
            g_GuardUntil = NowMsLocal() + 3000;
            if (ok) notify::Show("Warped to %s", g_TargetName);
        }

        void Step(s64 now) {
            g_GuardUntil = now + 3000;
            float me[3];
            if (!GetLocalPlayerPos(me)) return;
            if (g_LoadedAt) {
                if (g_Hop[1] == SkyHeight) Place(g_Hop); /* hold him up here while things catch up */
                if (now - g_LoadedAt < SettleMs) return;
                std::memcpy(me, g_Hop, sizeof(me));
            }
            float dx = me[0] - g_Hop[0], dy = me[1] - g_Hop[1], dz = me[2] - g_Hop[2];
            bool loaded = g_LoadedAt || dx * dx + dy * dy + dz * dz > 0.05f * 0.05f;

            if (!loaded) {
                if (now - g_HopSince > HopTimeoutMs) {
                    Logging.Log("[coop] warp: hop %d didn't load in %lld s", g_Hops, HopTimeoutMs / 1000);
                    if (g_Hop[1] != SkyHeight) {
                        Finish(true); /* he's there; he just hasn't moved */
                    } else {
                        notify::Show("Warp stopped: the area didn't load");
                        g_Active = false;
                    }
                }
                return;
            }
            if (!g_LoadedAt && g_Hop[1] == SkyHeight) {
                g_LoadedAt = now;
                Logging.Log("[coop] warp: hop %d loaded after %lld ms", g_Hops, now - g_HopSince);
                return;
            }
            g_LoadedAt = 0;
            float to[3];
            if (!TargetSpot(to)) {
                notify::Show("%s isn't here any more", g_TargetName);
                g_Active = false;
                return;
            }
            float tx = to[0] - me[0], tz = to[2] - me[2];
            if (tx * tx + tz * tz < 30.0f * 30.0f && g_Hop[1] != SkyHeight) { /* landed by the partner */
                Logging.Log("[coop] warp: arrived after %d hop(s)", g_Hops);
                Finish(true);
                return;
            }
            bool last = NextHop(g_Hop, to, g_Hop);
            Place(g_Hop);
            g_HopSince = now;
            g_Hops++;
            Logging.Log("[coop] warp: hop %d to (%.0f, %.0f, %.0f)%s", g_Hops, g_Hop[0], g_Hop[1], g_Hop[2], last ? ", last" : "");
        }

        void TryWarp(s64 now) {
            float me[3], rot;
            if (!GetLocalPlayer(me, &rot)) return;
            if ((LocalAnimCategory() & 0xFF) == anim::Horse) {
                notify::Show("Get off your horse to warp");
                return;
            }
            net::Peer peers[net::MaxPeers];
            net::GetPeers(peers);
            s64 tick_now = nn::os::GetSystemTick().GetInt64Value();
            s64 stale = static_cast<s64>(nn::os::GetSystemTickFrequency()) * StaleMs / 1000;
            u32 area = LocalArea();

            /* Each use goes to the next partner (by id), so with several you can pick. */
            const net::Peer* pick = nullptr;
            const net::Peer* first = nullptr;
            const net::Peer* elsewhere = nullptr;
            int fresh = 0;
            for (auto& p : peers) {
                if (!p.present || !p.has_state || tick_now - static_cast<s64>(p.state_tick) > stale) continue;
                fresh++;
                if (p.state.map_area != area) {
                    elsewhere = &p;
                    continue;
                }
                if (!first || p.id < first->id) first = &p;
                if (p.id > g_LastTarget && (!pick || p.id < pick->id)) pick = &p;
            }
            if (!pick) pick = first;
            if (!pick) {
                if (!fresh) notify::Show("No partner to warp to");
                else if (area == OverworldArea())
                    notify::Show("%s is in a shrine or Divine Beast", elsewhere->name);
                else
                    notify::Show("%s is outside: leave first", elsewhere->name);
                return;
            }
            g_LastTarget = g_TargetId = pick->id;
            std::memcpy(g_TargetName, pick->name, sizeof(g_TargetName));
            float to[3];
            if (!TargetSpot(to)) return;
            bool last = NextHop(me, to, g_Hop);
            if (!Place(g_Hop)) return;
            g_Active = true;
            g_LoadedAt = 0;
            if (s32* life = LifePtr()) g_StartLife = *life;
            g_GuardUntil = now + 3000;
            g_HopSince = now;
            g_Hops = 1;
            float dx = to[0] - me[0], dz = to[2] - me[2];
            float dist = std::sqrt(dx * dx + dz * dz);
            Logging.Log("[coop] warp: to %s at (%.1f, %.1f, %.1f), %.0f m from (%.1f, %.1f, %.1f)%s", pick->name, to[0], to[1],
                        to[2], dist, me[0], me[1], me[2], last ? "" : ", in hops");
            if (!last) notify::Show("Warping to %s (%.1f km)...", pick->name, dist / 1000.0f);
        }
    }

    void Tick(s64 now) {
        if (g_Active) Step(now);
        GuardLife(now);

        s64 held = StickClickHeldMs();
        if (held == 0) {
            g_Fired = false;
            return;
        }
        if (g_Fired || held < GestureMs) return;
        g_Fired = true;
        if (g_Active || now < g_ReadyAt) return;
        g_ReadyAt = now + CooldownMs;
        TryWarp(now);
    }
}
