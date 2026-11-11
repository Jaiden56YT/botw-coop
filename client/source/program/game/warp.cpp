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
        /* Where a warp lands: on the partner's own spot, a little above
           their feet. That's the one place known to be free. It used to be
           2 m behind them at +0.5 m, which is inside the hillside on any
           slope over 14 degrees and inside the wall when they stand with
           their back to one: two test players following each other on a
           slope both ended up under the ground. Puppets don't collide with
           Link, so standing in the same place is harmless. */
        constexpr float LandAbove = 0.5f;

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
        s64 g_LastActiveAt = -100000;
        u8 g_TargetId = 0;
        char g_TargetName[32];
        float g_Hop[3];
        s64 g_HopSince = 0;
        s64 g_LoadedAt = 0; /* the current hop's area loaded; held here a moment */
        int g_Hops = 0;
        int g_Retries = 0;
        constexpr s64 SettleMs = 500;

        /* Link's Havok position (see offsets.hpp), or null. */
        float* HavokPos() {
            uintptr_t body = LinkRigidBody();
            return body ? reinterpret_cast<float*>(body + offsets::HkBody_Pos) : nullptr;
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
        bool TargetSpot(float (&out)[3], bool* steady = nullptr) {
            net::Peer peers[net::MaxPeers];
            net::GetPeers(peers);
            for (auto& p : peers) {
                if (!p.present || p.id != g_TargetId || !p.has_state || p.state.map_area != LocalArea()) continue;
                out[0] = p.state.pos[0];
                out[1] = p.state.pos[1] + LandAbove;
                out[2] = p.state.pos[2];
                /* Not rising or dropping: standing, walking, swimming. */
                if (steady) *steady = std::fabs(p.state.vel[1]) < 1.0f;
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

        bool g_Announce = true; /* say "Warped to X" (not for the revive's respawn) */

        /* After landing: where he was put, and until when to keep an eye on him. */
        float g_Land[3];
        s64 g_LandUntil = 0;
        int g_LandTries = 0;

        void Finish(bool ok) {
            g_Active = false;
            g_GuardUntil = NowMsLocal() + 3000;
            if (ok) {
                std::memcpy(g_Land, g_Hop, sizeof(g_Land));
                g_LandUntil = NowMsLocal() + 5000;
                g_LandTries = 0;
            }
            if (ok && g_Announce) notify::Show("Warped to %s", g_TargetName);
        }

        /* He dropped well below where he was put while the partner stayed up
           there: the ground wasn't solid yet (it loads a moment after the
           scenery). Put him back on the partner's spot, a few times at most. */
        void WatchLanding(s64 now) {
            if (g_Active || now > g_LandUntil) return;
            float me[3];
            if (!GetLocalPlayerPos(me) || me[1] > g_Land[1] - 4.0f) return;
            float to[3];
            bool steady = false;
            if (g_LandTries >= 3 || !TargetSpot(to, &steady) || !steady || to[1] < g_Land[1] - 2.0f) {
                g_LandUntil = 0; /* they went down too (a cliff, a glide), or left: it's a real fall */
                return;
            }
            g_LandTries++;
            Logging.Log("[coop] warp: fell %.1f m below the landing spot; back to %s (try %d)", g_Land[1] - me[1], g_TargetName,
                        g_LandTries);
            if (!Place(to)) return;
            std::memcpy(g_Land, to, sizeof(g_Land));
            g_LandUntil = now + 5000;
            g_GuardUntil = now + 3000;
        }

        void Step(s64 now) {
            g_GuardUntil = now + 3000;
            float me[3];
            if (!GetLocalPlayerPos(me)) return;
            bool sky = g_Hop[1] == SkyHeight;
            if (g_LoadedAt) {
                if (sky) Place(g_Hop); /* hold him up here while things catch up */
                if (now - g_LoadedAt < SettleMs) return;
                std::memcpy(me, g_Hop, sizeof(me));
            } else {
                /* Right after a placement his position can still read as the
                   old spot for a tick: only judge the hop once he's on it. */
                float hx = me[0] - g_Hop[0], hz = me[2] - g_Hop[2];
                if (hx * hx + hz * hz > 2.0f * 2.0f) {
                    if (now - g_HopSince > 1000 && g_Retries < 5) {
                        g_Retries++;
                        Place(g_Hop);
                        g_HopSince = now;
                    } else if (g_Retries >= 5 && now - g_HopSince > 1000) {
                        Logging.Log("[coop] warp: hop %d didn't take", g_Hops);
                        notify::Show("Warp stopped");
                        g_Active = false;
                    }
                    return;
                }
            }
            float dx = me[0] - g_Hop[0], dy = me[1] - g_Hop[1], dz = me[2] - g_Hop[2];
            bool loaded = g_LoadedAt || dx * dx + dy * dy + dz * dz > 0.05f * 0.05f;

            if (!loaded) {
                if (now - g_HopSince > HopTimeoutMs) {
                    Logging.Log("[coop] warp: hop %d didn't load in %lld s", g_Hops, HopTimeoutMs / 1000);
                    if (!sky) {
                        Finish(true); /* he's there; he just hasn't moved */
                    } else {
                        notify::Show("Warp stopped: the area didn't load");
                        g_Active = false;
                    }
                }
                return;
            }
            if (!sky) { /* the last hop: he's by the partner */
                Logging.Log("[coop] warp: arrived after %d hop(s)", g_Hops);
                Finish(true);
                return;
            }
            if (!g_LoadedAt) {
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
            bool last = NextHop(g_Hop, to, g_Hop);
            Place(g_Hop);
            g_HopSince = now;
            g_Retries = 0;
            g_Hops++;
            Logging.Log("[coop] warp: hop %d to (%.0f, %.0f, %.0f)%s", g_Hops, g_Hop[0], g_Hop[1], g_Hop[2], last ? ", last" : "");
        }

        bool Begin(const net::Peer& peer, const float* me, s64 now, bool announce);

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
            Begin(*pick, me, now, true);
        }

        bool Begin(const net::Peer& peer, const float* me, s64 now, bool announce) {
            const net::Peer* pick = &peer;
            g_Announce = announce;
            g_LastTarget = g_TargetId = pick->id;
            std::memcpy(g_TargetName, pick->name, sizeof(g_TargetName));
            float to[3];
            if (!TargetSpot(to)) return false;
            bool last = NextHop(me, to, g_Hop);
            if (!Place(g_Hop)) return false;
            g_Active = true;
            g_LoadedAt = 0;
            g_Retries = 0;
            if (s32* life = LifePtr()) g_StartLife = *life;
            g_GuardUntil = now + 3000;
            g_HopSince = now;
            g_Hops = 1;
            float dx = to[0] - me[0], dz = to[2] - me[2];
            float dist = std::sqrt(dx * dx + dz * dz);
            Logging.Log("[coop] warp: to %s at (%.1f, %.1f, %.1f), %.0f m from (%.1f, %.1f, %.1f)%s", pick->name, to[0], to[1],
                        to[2], dist, me[0], me[1], me[2], last ? "" : ", in hops");
            if (!last && announce) notify::Show("Warping to %s (%.1f km)...", pick->name, dist / 1000.0f);
            return true;
        }
    }

    bool PlaceLink(const float (&pos)[3]) { return Place(pos); }

    void HoldLinkXZ(const float (&pos)[3]) {
        float* hk = HavokPos();
        uintptr_t actor = PlayerActor();
        if (!hk || !actor) return;
        hk[0] = pos[0], hk[2] = pos[2];
        float* vel = hk + offsets::HavokPos_ToVelocity / 4;
        vel[0] = vel[2] = 0;
        float* m = reinterpret_cast<float*>(actor + offsets::Actor_Mtx);
        m[3] = pos[0], m[11] = pos[2];
    }

    bool GoToPartner(u8 id, s64 now) {
        float me[3];
        if (g_Active || !GetLocalPlayerPos(me)) return false;
        net::Peer peers[net::MaxPeers];
        net::GetPeers(peers);
        for (auto& p : peers)
            if (p.present && p.id == id && p.has_state && p.state.map_area == LocalArea()) return Begin(p, me, now, false);
        return false;
    }

    namespace {
        int g_TestFollow = 0;
        s64 g_NextFollow = 0;

        void FollowTest(s64 now) {
            if (g_TestFollow <= 0 || g_Active || now < g_NextFollow) return;
            g_NextFollow = now + 3000;
            float me[3];
            if (!GetLocalPlayerPos(me)) return;
            net::Peer peers[net::MaxPeers];
            net::GetPeers(peers);
            const net::Peer* nearest = nullptr;
            float nearest_d = 0;
            for (auto& p : peers) {
                if (!p.present || !p.has_state || p.state.map_area != LocalArea()) continue;
                float dx = p.state.pos[0] - me[0], dy = p.state.pos[1] - me[1], dz = p.state.pos[2] - me[2];
                float d = dx * dx + dy * dy + dz * dz;
                if (!nearest || d < nearest_d) nearest = &p, nearest_d = d;
            }
            if (nearest && nearest_d > static_cast<float>(g_TestFollow) * g_TestFollow && GoToPartner(nearest->id, now))
                g_NextFollow = now + 8000;
        }
    }

    void SetTestFollow(int metres) { g_TestFollow = metres; }

    bool Busy(s64 now) { return g_Active || now - g_LastActiveAt < 3000; }

    void Tick(s64 now) {
        if (g_Active) {
            g_LastActiveAt = now;
            Step(now);
        }
        GuardLife(now);
        WatchLanding(now);
        FollowTest(now);

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
