#include "warp.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <lib.hpp>
#include <nn/fs.hpp>
#include <nn/os.hpp>

#include "../net/client.hpp"
#include "anim.hpp"
#include "autostart.hpp"
#include "ghost.hpp"
#include "items.hpp"
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
        /* A far warp in one jump (see GroundLoadingHook and Direct). */
        constexpr int LandReady = 12;           /* the game says "ready" this many updates in a row: the ground is in */
        constexpr s64 GateFarMinMs = 2500;      /* held at least this long: its first answers can still be about the old spot */
        constexpr s64 GateFarQuietMs = 25000;   /* this long without a "ready"... */
        constexpr s64 GateFarSilentMs = 8000;   /* ...and not asked for this long: go on anyway */
        constexpr s64 GateTimeoutMs = 120000;   /* a whole new part of the map on a slow machine */
        constexpr s64 SettleQuietMs = 4000;     /* "ready" for this long after landing: the gate can go */

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

        /* The gate: while a warp runs, the game is told the ground is ready. */
        std::atomic<bool> g_Gate{false};
        std::atomic<int> g_ReadyRun{0}; /* the game's own "ready" answers in a row */
        std::atomic<s64> g_GateAt{0};   /* when it last asked */
        s64 g_TakenAt = 0;              /* when Link first read as being at the hop (0: not yet) */
        std::atomic<u32> g_GateCalls{0}; /* how often it has asked; 0: the hook isn't reached here */

        /* The game only asks in some states (not while Link stands on solid
           ground, it seems), so "it asked once" is what shows the hook works. */
        bool Gated() { return g_GateCalls.load(std::memory_order_relaxed) != 0; }

        /* How a far warp travels (config.ini, warp_direct):
           - false: in hops of 250 m through the sky, each waiting for the
             game to un-pause the world (it pauses while the ground loads);
           - true: in one jump, with the game told "ready" so the world
             runs, Link held at the landing spot until the ground is really
             in. Reliable and quicker in Savage Wind Recomp (15 of 15, 8 to
             20 s for 2 to 9 km). In Ryujinx the ground often never came,
             so it's off unless asked for. */
        bool g_Direct = false;
        bool g_Far = false; /* this warp goes farther than one hop */
        /* Only for far warps: a near one lands in what's already loaded,
           as it always did. */
        bool Direct() { return g_Direct && g_Far && Gated(); }

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

        /* The game takes fall damage from the highest point since Link left
           the ground, and a hop through the sky leaves that 1,100 m up: the
           landing then costs most of his hearts, or all of them. It was
           hidden where the area loads within the guard's 3 s, and wasn't
           where it takes longer. So the height is forgotten on the way and
           until he has landed. */
        void ForgetFall() {
            uintptr_t actor = PlayerActor();
            if (!actor) return;
            float y = reinterpret_cast<const float*>(actor + offsets::Actor_Mtx)[7];
            for (uintptr_t off : {offsets::Player_FallTop, offsets::Player_FallTop2}) {
                float* top = reinterpret_cast<float*>(actor + off);
                if (std::isfinite(*top) && *top > y && *top < y + 20000.0f) *top = y;
            }
        }
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
        s64 g_SettleUntil = 0, g_SettleEnd = 0; /* the gate stays on after a far warp until the ground stays ready */
        float g_Land[3];
        s64 g_LandUntil = 0, g_LandEnd = 0, g_LandNextAt = 0;
        int g_LandTries = 0;
        /* A shrine's hollow and platform were solid only 2 to 4 s after the
           ground around them: retries are spread over that time. */
        constexpr int LandTriesMax = 12;
        constexpr s64 LandRetryMs = 2000;
        constexpr s64 LandHoldMs = 1200; /* kept on the spot this long after being put back */
        s64 g_LandHoldUntil = 0;
        constexpr s64 LandWatchMaxMs = 20000;

        void Finish(bool ok) {
            g_Active = false;
            /* The cold of the sky still bit once after the 3 s (half a heart
               per far warp): longer after those. */
            g_GuardUntil = NowMsLocal() + (g_Far ? 8000 : 3000);
            if (ok) {
                std::memcpy(g_Land, g_Hop, sizeof(g_Land));
                g_LandUntil = NowMsLocal() + 5000;
                g_LandEnd = NowMsLocal() + LandWatchMaxMs;
                g_LandNextAt = 0;
                g_LandHoldUntil = 0;
                g_LandTries = 0;
            }
            if (ok && g_Far && Direct()) {
                g_SettleUntil = NowMsLocal() + SettleQuietMs;
                g_SettleEnd = NowMsLocal() + GateTimeoutMs;
            }
            if (ok && g_Announce) notify::Show("Warped to %s", g_TargetName);
        }

        /* After a far jump the map keeps loading in stages, and the game's
           answer can go back to "still loading" seconds after the ground was
           ready. Left alone it then pauses the world, which is the freeze
           the gate is there to prevent (seen: Link stuck in the air in a
           T-pose with the loading icon, for good). So the gate stays on until
           the answer has been "ready" for a while, and while it isn't, Link
           alone is paused: held where he is. */
        void Settle(s64 now) {
            if (g_Active || !g_SettleUntil) return;
            if (now > g_SettleEnd || now > g_SettleUntil) {
                g_SettleUntil = 0;
                return;
            }
            bool asked = now - g_GateAt.load(std::memory_order_relaxed) < 500;
            if (!asked || g_ReadyRun.load(std::memory_order_relaxed) >= 3) return;
            g_SettleUntil = now + SettleQuietMs;
            g_GuardUntil = now + 3000;
            if (g_LandUntil) g_LandUntil = now + 5000;
            float me[3];
            if (GetLocalPlayerPos(me)) Place(me);
        }

        /* He ended up well off the height he was put at while the partner
           stayed there: what he should stand on wasn't solid yet. The ground
           loads a moment after the scenery; a shrine's platform loads later
           still, and he drops through it, or is thrown 17 m up when it
           arrives around him. Put him back on the partner's spot, a few
           times at most. */
        void WatchLanding(s64 now) {
            if (g_Active || now > g_LandUntil || now > g_LandEnd || now < g_LandNextAt) return;
            float me[3];
            if (!GetLocalPlayerPos(me)) return;
            float feet = g_Land[1] - LandAbove; /* where the partner stood */
            bool below = me[1] < feet - 1.0f, above = me[1] > feet + 4.0f;
            if (!below && !above) return;
            float to[3];
            bool steady = false;
            if (g_LandTries >= LandTriesMax || !TargetSpot(to, &steady) || std::fabs(to[1] - g_Land[1]) > 1.0f) {
                g_LandUntil = 0; /* they moved too (a cliff, a glide, a climb), or left: it's his own doing */
                return;
            }
            if (!steady) return; /* they're dropping or jumping right now: look again in a moment */
            g_LandTries++;
            Logging.Log("[coop] warp: %.1f m %s the landing spot; back to %s (try %d)", std::fabs(me[1] - feet),
                        below ? "below" : "above", g_TargetName, g_LandTries);
            if (!Place(to)) return;
            std::memcpy(g_Land, to, sizeof(g_Land));
            g_LandUntil = now + 5000;
            g_LandNextAt = now + LandRetryMs;
            g_LandHoldUntil = now + LandHoldMs;
            g_GuardUntil = now + 3000;
        }

        void Step(s64 now) {
            g_GuardUntil = now + 3000;
            float me[3];
            if (!GetLocalPlayerPos(me)) return;
            bool sky = g_Hop[1] == SkyHeight;
            bool gate = Direct();
            if (g_LoadedAt) {
                if (sky) Place(g_Hop); /* hold him up here while things catch up */
                if (now - g_LoadedAt < SettleMs) return;
                std::memcpy(me, g_Hop, sizeof(me));
            } else {
                /* Right after a placement his position can still read as the
                   old spot for a tick: only judge the hop once he's on it. */
                float hx = me[0] - g_Hop[0], hz = me[2] - g_Hop[2];
                bool taken = hx * hx + hz * hz <= 2.0f * 2.0f;
                if (gate) {
                    /* The world runs on under the gate: he's kept here until
                       the ground is in. While a new part of the map loads the
                       game can go seconds without updating him, and his
                       position (and its answers about the ground) are still
                       about the old spot: nothing counts until he reads as here. */
                    Place(g_Hop);
                    if (!taken) {
                        g_TakenAt = 0;
                        if (now - g_HopSince > GateTimeoutMs) {
                            Logging.Log("[coop] warp: hop %d didn't take in %lld s", g_Hops, (now - g_HopSince) / 1000);
                            notify::Show("Warp stopped");
                            g_Active = false;
                        }
                        return;
                    }
                    if (!g_TakenAt) {
                        g_TakenAt = now;
                        g_ReadyRun.store(0);
                    }
                } else if (!taken) {
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
            bool loaded;
            if (g_LoadedAt) loaded = true;
            else if (gate) {
                /* Ready for long enough, or it isn't asking at all (it
                   doesn't pause the world then either). */
                s64 since = now - g_TakenAt, silent = now - g_GateAt.load();
                /* After a far jump it can take 15 s to start asking (seen),
                   so its silence means little until much later. */
                bool quiet = since >= GateFarQuietMs && silent >= GateFarSilentMs;
                loaded = since >= GateFarMinMs && (g_ReadyRun.load() >= LandReady || quiet);
            }
            else loaded = dx * dx + dy * dy + dz * dz > 0.05f * 0.05f; /* he moves once the world runs */

            if (!loaded) {
                if (now - g_HopSince > (gate ? GateTimeoutMs : HopTimeoutMs)) {
                    Logging.Log("[coop] warp: hop %d didn't load in %lld s", g_Hops, (now - g_HopSince) / 1000);
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
                Logging.Log("[coop] warp: arrived after %d hop(s), the last loaded in %lld ms", g_Hops, now - g_HopSince);
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
            if (!last && gate) {
                std::memcpy(g_Hop, to, sizeof(g_Hop));
                last = true;
            }
            Place(g_Hop);
            g_ReadyRun.store(0);
            g_TakenAt = 0;
            g_HopSince = now;
            g_Retries = 0;
            g_Hops++;
            Logging.Log("[coop] warp: hop %d to (%.0f, %.0f, %.0f)%s", g_Hops, g_Hop[0], g_Hop[1], g_Hop[2], last ? ", last" : "");
        }

        bool Begin(const net::Peer& peer, const float* me, s64 now, bool announce);

        /* ---- travel: the game's own stage change ----
           A far-away partner, or one in a shrine, is reached the way the
           game's fast travel does it: a stage change with a loading screen
           to the fast-travel point nearest them (or to their shrine's
           entrance), then the ordinary warp for the rest of the way. Hops
           through the sky over several kilometres got stuck (the game waits
           for map it never loads); right after a real load they don't. */
        struct TravelPoint {
            char stage[16];
            char start[32];
            float at[3];
        };
        constexpr int MaxPoints = 200;
        TravelPoint g_Points[MaxPoints]; /* romfs botw-coop/travel-points.txt, built from the player's own game files */
        int g_PointCount = 0;
        bool g_PointsTried = false;
        constexpr float TravelMinDist = 1500.0f; /* nearer than this, the warp alone (hops that far have always loaded) */
        constexpr float TravelGain = 300.0f;    /* and only if the point is at least this much nearer to them than we are */
        /* How long to wait for the game to start the stage change, and then for the load itself
           (in Ryujinx a load took 20 to 50 s; in Savage Wind Recomp 10 s). */
        constexpr s64 TravelStartTimeoutMs = 120000, TravelLoadTimeoutMs = 240000;
        /* A load that hasn't finished by now is stuck (in Ryujinx one in eight was, asked for from the main
           thread; three in eight from any thread: the loading screen stayed up for good, with a hundred actors
           made and no more coming). Holding Link on his partner's spot with the game's question about the
           ground answered "ready" didn't help (the game isn't asking). What's tried now: the same stage change
           once more. */
        constexpr s64 TravelStuckMs = 60000;
        struct {
            bool active;
            s64 started;
            u8 target;
            char name[32];
            u32 area;
            float at[3];
            bool has_at;
            char stage[32], start[40];
            int asks;
            s64 asked_at;
            s64 moved_at; /* when Link's actor was first seen on the new stage: the load is under way */
            s64 told_at;
            u32 gate_calls;
            s64 due_at;
            int again; /* times the stage change was asked for once more because the load was stuck */
            float from[3];
        } g_Travel;
        /* The game drops a stage change asked for while the world is still
           settling after a load (in Ryujinx, for 20 s after a save loaded:
           nothing happened), so it's only asked once the world has run
           without a stop for a few seconds, and asked again if no loading
           screen follows. Never twice close together: two requests 1 ms
           apart left Ryujinx on the new stage with the world stopped. */
        constexpr s64 TravelQuietMs = 3000, TravelAskAgainMs = 10000;
        constexpr int TravelAsksMax = 5;

        /* Link's own position (his actor), which is right during a load too; PlayerInfo's copy isn't. */
        bool ActorPos(float (&out)[3]) {
            uintptr_t info = Read<uintptr_t>(Base() + offsets::PlayerInfoInstance);
            uintptr_t actor = info ? Read<uintptr_t>(info + offsets::PlayerInfo_PlayerActor) : 0;
            if (!actor) return false;
            const float* m = reinterpret_cast<const float*>(actor + offsets::Actor_Mtx);
            out[0] = m[3], out[1] = m[7], out[2] = m[11];
            return true;
        }

        void LoadPoints() {
            g_PointsTried = true;
            nn::fs::FileHandle file;
            if (nn::fs::OpenFile(&file, "content:/botw-coop/travel-points.txt", nn::fs::OpenMode_Read) != 0) {
                Logging.Log("[coop] warp: no travel-points.txt (run the setup again): far warps go in hops");
                return;
            }
            static char buf[12 * 1024];
            long size = 0;
            nn::fs::GetFileSize(&size, file);
            if (size > static_cast<long>(sizeof(buf)) - 1) size = sizeof(buf) - 1;
            nn::fs::ReadFile(file, 0, buf, static_cast<ulong>(size));
            nn::fs::CloseFile(file);
            buf[size] = 0;
            for (char* line = buf; line && *line && g_PointCount < MaxPoints;) {
                char* eol = std::strchr(line, '\n');
                if (eol) *eol = 0;
                TravelPoint& t = g_Points[g_PointCount];
                if (std::sscanf(line, "%15s %31s %f %f %f", t.stage, t.start, &t.at[0], &t.at[1], &t.at[2]) == 5) g_PointCount++;
                line = eol ? eol + 1 : nullptr;
            }
            Logging.Log("[coop] warp: %d fast-travel points", g_PointCount);
        }

        /* The mod's tick runs on whichever game thread asked for Link's position first that frame: the main
           thread mostly, a worker thread now and then. The game's own stage changes come from its events, on the
           main thread; ours is only asked for from there too. */
        bool OnMainThread() {
            const char* name = nn::os::GetThreadNamePointer(nn::os::GetCurrentThread());
            return name && std::strcmp(name, "MainThread") == 0;
        }

        void ChangeStage(const char* stage, const char* start) {
            struct Str {
                uintptr_t vtable;
                const char* str;
            };
            static char s_stage[32], s_start[40]; /* (kept: the game may read them after the call returns) */
            std::snprintf(s_stage, sizeof(s_stage), "%s", stage);
            std::snprintf(s_start, sizeof(s_start), "%s", start);
            uintptr_t vt = Read<uintptr_t>(Base() + offsets::Got_SafeStringVtableBase) + 0x10;
            Str a{vt, s_stage}, b{vt, s_start};
            reinterpret_cast<void (*)(const Str*, const Str*, int, bool)>(Base() + offsets::StageInfo_ChangeScene)(&a, &b, 0, false);
        }

        /* Starts a stage change toward this partner if that's the way to reach them. */
        bool g_TravelOn = true;
        int g_TravelChain = 0; /* stage changes in a row for one warp (the partner traveled on while we loaded) */

        bool StartTravel(const net::Peer& p, const float* me, u32 my_area, s64 now) {
            if (g_Travel.active) return true;
            if (!g_TravelOn) return false;
            if (!g_PointsTried) LoadPoints();
            char stage[32], start[40];
            float at[3] = {0, 0, 0};
            bool has_at = false;
            if (p.state.map_area == OverworldArea()) {
                const TravelPoint* best = nullptr;
                float best_d = 0;
                for (int i = 0; i < g_PointCount; i++) {
                    float dx = g_Points[i].at[0] - p.state.pos[0], dz = g_Points[i].at[2] - p.state.pos[2];
                    float d = dx * dx + dz * dz;
                    if (!best || d < best_d) best = &g_Points[i], best_d = d;
                }
                if (!best) return false;
                if (my_area == OverworldArea()) {
                    float dx = me[0] - p.state.pos[0], dz = me[2] - p.state.pos[2];
                    float mine = std::sqrt(dx * dx + dz * dz);
                    if (mine < TravelMinDist || std::sqrt(best_d) + TravelGain > mine) return false;
                }
                std::snprintf(stage, sizeof(stage), "%s", best->stage);
                std::snprintf(start, sizeof(start), "%s", best->start);
                std::memcpy(at, best->at, sizeof(at));
                has_at = true;
            } else {
                /* A shrine is "CDungeon/DungeonNNN", entered at "Entrance_1". */
                if (p.state.map_area == my_area) return false;
                int found = -1;
                char name[16];
                for (int i = 0; i < 140 && found < 0; i++) {
                    std::snprintf(name, sizeof(name), "Dungeon%03d", i);
                    if (AreaOf("CDungeon", name) == p.state.map_area) found = i;
                }
                if (found < 0) return false;
                std::snprintf(stage, sizeof(stage), "CDungeon/Dungeon%03d", found);
                std::snprintf(start, sizeof(start), "Entrance_1");
            }
            Logging.Log("[coop] warp: traveling to %s by the game's own stage change: %s at %s", p.name, stage, start);
            notify::Show("Traveling to %.10s...", p.name);
            g_Travel.active = true;
            g_Travel.started = now;
            g_Travel.target = p.id;
            std::snprintf(g_Travel.name, sizeof(g_Travel.name), "%s", p.name);
            g_Travel.area = p.state.map_area;
            std::memcpy(g_Travel.at, at, sizeof(at));
            g_Travel.has_at = has_at;
            std::snprintf(g_Travel.stage, sizeof(g_Travel.stage), "%s", stage);
            std::snprintf(g_Travel.start, sizeof(g_Travel.start), "%s", start);
            g_Travel.asks = 0;
            g_Travel.asked_at = 0;
            g_Travel.moved_at = 0;
            g_Travel.due_at = 0;
            g_Travel.again = 0;
            std::memcpy(g_Travel.from, me, sizeof(g_Travel.from));
            return true; /* (TravelTick asks the game, when the world is ready for it) */
        }

        bool Begin(const net::Peer& peer, const float* me, s64 now, bool announce);

        /* After the loading screen: the rest of the way to the partner. */
        void TravelTick(s64 now) {
            if (!g_Travel.active) return;
            if (g_Travel.moved_at ? now - g_Travel.moved_at > TravelLoadTimeoutMs : now - g_Travel.started > TravelStartTimeoutMs) {
                g_Travel.active = false;
                Logging.Log("[coop] warp: the stage change to %s %s", g_Travel.name,
                            g_Travel.moved_at ? "started, but the stage didn't finish loading" : "never started");
                notify::Show("Travel to %.10s didn't finish", g_Travel.name);
                return;
            }
            float at[3], me[3], rot;
            if (!ActorPos(at)) return;
            float fx = at[0] - g_Travel.from[0], fz = at[2] - g_Travel.from[2];
            if (LocalArea() != g_Travel.area || (g_Travel.has_at && fx * fx + fz * fz < 30.0f * 30.0f)) {
                /* Still where we started. Ask (again) once the world has been running for a while. */
                bool due = now - items::LastPausedMs() > TravelQuietMs && g_Travel.asks < TravelAsksMax &&
                           (g_Travel.asks == 0 || now - g_Travel.asked_at > TravelAskAgainMs);
                if (!due) g_Travel.due_at = 0;
                else if (!g_Travel.due_at) g_Travel.due_at = now;
                /* (Not on the main thread within 2 s: wherever this runs, the main thread isn't called that here.) */
                if (due && (OnMainThread() || now - g_Travel.due_at > 2000)) {
                    g_Travel.asks++;
                    g_Travel.asked_at = now;
                    g_Travel.due_at = 0;
                    Logging.Log("[coop] warp: asking the game for the stage change (%d)%s", g_Travel.asks,
                                OnMainThread() ? "" : ", not from the main thread");
                    ChangeStage(g_Travel.stage, g_Travel.start);
                }
                return;
            }
            /* On the new stage: the load is under way. It's done once the world runs again and Link is live
               (PlayerInfo's copy of his position follows his actor again; during the load it stays behind). */
            if (g_Travel.moved_at && now - g_Travel.told_at >= 5000) {
                u32 calls = g_GateCalls.load(std::memory_order_relaxed);
                Logging.Log("[coop] warp: loading for %lld s: the game asked about the ground %u times in the last 5 s (%d ready in a row), "
                            "its clock last stood still %lld ms ago",
                            (now - g_Travel.moved_at) / 1000, calls - g_Travel.gate_calls, g_ReadyRun.load(std::memory_order_relaxed),
                            now - items::LastPausedMs());
                g_Travel.told_at = now;
                g_Travel.gate_calls = calls;
            }
            if (g_Travel.moved_at && now - g_Travel.moved_at > TravelStuckMs) {
                if (g_Travel.again >= 1) {
                    g_Travel.active = false;
                    Logging.Log("[coop] warp: the stage is stuck loading for the second time: giving up");
                    notify::Show("Travel is stuck: please restart the game");
                    return;
                }
                if (!OnMainThread()) return;
                g_Travel.again++;
                g_Travel.moved_at = now;
                Logging.Log("[coop] warp: the load is stuck after %lld s: asking the game for the same stage change once more",
                            TravelStuckMs / 1000);
                ChangeStage(g_Travel.stage, g_Travel.start);
                return;
            }
            if (!g_Travel.moved_at) {
                g_Travel.moved_at = now;
                g_Travel.told_at = now;
                g_Travel.gate_calls = g_GateCalls.load(std::memory_order_relaxed);
                Logging.Log("[coop] warp: the stage change started after %lld ms (%d request%s)", now - g_Travel.started, g_Travel.asks,
                            g_Travel.asks == 1 ? "" : "s");
            }
            if (now - items::LastPausedMs() < 1500 || !GetLocalPlayer(me, &rot)) return;
            float lx = me[0] - at[0], lz = me[2] - at[2];
            if (lx * lx + lz * lz > 5.0f * 5.0f) return;
            if (g_Travel.has_at) {
                float dx = me[0] - g_Travel.at[0], dz = me[2] - g_Travel.at[2];
                if (dx * dx + dz * dz > 80.0f * 80.0f) return;
            }
            g_Travel.active = false;
            Logging.Log("[coop] warp: stage loaded after %lld ms, at (%.1f, %.1f, %.1f)", now - g_Travel.started, me[0], me[1], me[2]);
            net::Peer peers[net::MaxPeers];
            net::GetPeers(peers);
            for (auto& p : peers) {
                if (!p.present || p.id != g_Travel.target || !p.has_state) continue;
                /* They went on somewhere far while we were loading: once more, then that's it. */
                if (g_TravelChain < 1) {
                    g_TravelChain++;
                    if (StartTravel(p, me, LocalArea(), now)) return;
                }
                if (p.state.map_area != LocalArea()) {
                    notify::Show("%.10s has moved on", p.name);
                    return;
                }
                Begin(p, me, now, true);
                return;
            }
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
                else if (g_TravelChain = 0, StartTravel(*elsewhere, me, area, now)) g_LastTarget = elsewhere->id;
                else if (area == OverworldArea())
                    notify::Show("%s is in a shrine or Divine Beast", elsewhere->name);
                else
                    notify::Show("%s is outside: leave first", elsewhere->name);
                return;
            }
            g_TravelChain = 0;
            if (StartTravel(*pick, me, area, now)) {
                g_LastTarget = pick->id;
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
            g_Far = !last;
            /* With the game's answer about the ground there's no need to
               creep up on the place: one jump, and he's held there while
               the world (kept running) loads around him. */
            if (!last && Direct()) {
                std::memcpy(g_Hop, to, sizeof(g_Hop));
                last = true;
            }
            if (!Place(g_Hop)) return false;
            g_Active = true;
            g_ReadyRun.store(0);
            g_TakenAt = 0;
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
            if (dist > HopMax && announce) notify::Show("Warping to %s (%.1f km)...", pick->name, dist / 1000.0f);
            return true;
        }
    }

    /* The game asks this on every update, and pauses the world while the
       answer is yes. But the world's own update is what loads the map, so a
       Link moved too far ahead of it waits for himself: the world stays
       frozen (seen on a busy Mac after 11 hops of 250 m; one jump of 800 m
       always does it). During a warp the caller is told "ready", the world
       keeps running and loads, and the real answer tells the warp when the
       ground is there. */
    HOOK_DEFINE_TRAMPOLINE(GroundLoadingHook) {
        static u64 Callback(void* self) {
            bool loading = Orig(self) & 1;
            g_GateAt.store(NowMsLocal(), std::memory_order_relaxed);
            g_GateCalls.fetch_add(1, std::memory_order_relaxed);
            if (loading) g_ReadyRun.store(0, std::memory_order_relaxed);
            else g_ReadyRun.fetch_add(1, std::memory_order_relaxed);
            return g_Gate.load(std::memory_order_relaxed) ? 0 : loading;
        }
    };

    void Install() { GroundLoadingHook::InstallAtOffset(offsets::World_GroundLoading); }

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

    bool GoToPartner(u8 id, s64 now, bool travel) {
        float me[3];
        if (g_Active || !GetLocalPlayerPos(me)) return false;
        net::Peer peers[net::MaxPeers];
        net::GetPeers(peers);
        u32 area = LocalArea();
        for (auto& p : peers) {
            if (!p.present || p.id != id || !p.has_state) continue;
            g_TravelChain = 0;
            if (travel && StartTravel(p, me, area, now)) return true;
            return p.state.map_area == area && Begin(p, me, now, false);
        }
        return false;
    }

    namespace {
        int g_TestFollow = 0;
        char g_TestLeader[32] = ""; /* follow only this player (else the nearest) */
        s64 g_NextFollow = 0;

        void FollowTest(s64 now) {
            if (g_TestFollow <= 0 || g_Active || g_Travel.active || now < g_NextFollow) return;
            g_NextFollow = now + 3000;
            float me[3];
            if (!GetLocalPlayerPos(me)) return;
            net::Peer peers[net::MaxPeers];
            net::GetPeers(peers);
            const net::Peer* nearest = nullptr;
            float nearest_d = 0;
            for (auto& p : peers) {
                if (!p.present || !p.has_state) continue;
                if (g_TestLeader[0] && std::strcmp(p.name, g_TestLeader) != 0) continue;
                float dx = p.state.pos[0] - me[0], dy = p.state.pos[1] - me[1], dz = p.state.pos[2] - me[2];
                float d = p.state.map_area == LocalArea() ? dx * dx + dy * dy + dz * dz : 1e12f; /* another area: far */
                if (!nearest || d < nearest_d) nearest = &p, nearest_d = d;
            }
            if (nearest && nearest_d > static_cast<float>(g_TestFollow) * g_TestFollow && GoToPartner(nearest->id, now, true))
                g_NextFollow = now + 8000;
        }
    }

    void SetDirect(bool on) { g_Direct = on; }
    void SetTravel(bool on) { g_TravelOn = on; }
    bool Traveling() { return g_Travel.active && g_Travel.asks > 0; }

    void SetTestFollow(int metres, const char* leader) {
        g_TestFollow = metres;
        std::snprintf(g_TestLeader, sizeof(g_TestLeader), "%s", leader ? leader : "");
    }

    bool Guarding(s64 now, s32* start_life) {
        if (!g_Active && now > g_GuardUntil) return false;
        if (start_life) *start_life = g_StartLife;
        return true;
    }

    bool Busy(s64 now) { return g_Active || now - g_LastActiveAt < 3000; }

    bool Loading(s64 now) {
        return g_Active || g_Travel.active || g_SettleUntil != 0 || now < g_LandHoldUntil || now - g_LastActiveAt < 5000;
    }

    void Tick(s64 now) {
        if (g_Active) {
            g_LastActiveAt = now;
            Step(now);
        }
        Settle(now);
        if (!g_Active && now < g_LandHoldUntil) Place(g_Land);
        g_Gate.store(Direct() && (g_Active || g_SettleUntil != 0), std::memory_order_relaxed);
        /* The 3 s after arriving count from when the world runs again: the
           last hop's area can still be loading (the clock stands still). */
        if (!g_Active && now <= g_GuardUntil && now - items::LastPausedMs() < 500) {
            if (g_GuardUntil < now + 3000) g_GuardUntil = now + 3000;
            if (g_LandUntil) g_LandUntil = now + 5000;
        }
        if (g_Active || now <= g_GuardUntil || now <= g_LandUntil) ForgetFall();
        GuardLife(now);
        WatchLanding(now);
        TravelTick(now);
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
