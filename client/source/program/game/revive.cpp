#include "revive.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <lib.hpp>
#include <nn/os.hpp>

#include "../net/client.hpp"
#include "notify.hpp"
#include "offsets.hpp"
#include "player.hpp"
#include "warp.hpp"

namespace coop::game::revive {

    namespace {
        constexpr s32 ReviveLife = 12;      /* 3 hearts, in quarter hearts */
        constexpr s32 DownLife = 1;         /* a quarter heart while down */
        constexpr s64 DownMs = 30000;       /* how long a partner has to reach you */
        constexpr float ReachDist = 2.5f;   /* a partner this close... */
        constexpr s64 ReachMs = 1500;       /* ...for this long gets you up */
        constexpr s64 CooldownMs = 120000;  /* between two times going down */
        constexpr s64 PartnerStaleMs = 5000;

        uintptr_t Base() { return exl::util::GetMainModuleInfo().m_Total.m_Start; }

        uintptr_t LinkActor() {
            uintptr_t info = *reinterpret_cast<const uintptr_t*>(Base() + offsets::PlayerInfoInstance);
            return info ? *reinterpret_cast<const uintptr_t*>(info + offsets::PlayerInfo_PlayerActor) : 0;
        }

        s32* LifeOf(uintptr_t actor) {
            uintptr_t vt = *reinterpret_cast<const uintptr_t*>(actor);
            return reinterpret_cast<s32* (*)(uintptr_t)>(*reinterpret_cast<const uintptr_t*>(vt + offsets::ActorVt_GetLife))(actor);
        }

        std::atomic<bool> g_PartnerHere{false};
        std::atomic<s64> g_ReadyAt{0};
        std::atomic<s64> g_NowMs{0};

        /* Down: set by the revive hook, run by Tick. */
        std::atomic<bool> g_Down{false};
        std::atomic<s64> g_DownSince{0};
        std::atomic<int> g_DownCount{0};
        int g_DownSeen = 0;
        float g_DownAt[3];
        s64 g_ReachSince = 0;
        s64 g_NextSend = 0;

        bool g_Test = false;
        int g_TestLife = 0;       /* test: drop to this life, then let a real hit bring Link down */
        s64 g_TestLifeAt = 0;
        bool g_TestSkipGame = false;
        s64 g_TestAt = 0;
    }

    /* Player::tryRevive(player): the damage handler (0xca8f44) calls it when
       life reaches 0 and goes to game over if it returns false. */
    HOOK_DEFINE_TRAMPOLINE(TryReviveHook) {
        static bool Callback(uintptr_t player) {
            s32* life = LifeOf(player);
            /* Already down: hits can't finish you while a partner comes. */
            if (g_Down.load()) {
                if (life) *life = DownLife;
                return true;
            }
            /* A fairy or Mipha's Grace first (the test skips them to reach ours). */
            if (!g_TestSkipGame && Orig(player)) return true;
            s64 now = g_NowMs.load();
            if (!g_PartnerHere.load() || now < g_ReadyAt.load() || !life) return false;
            *life = DownLife;
            g_ReadyAt.store(now + CooldownMs);
            g_DownSince.store(now);
            g_Down.store(true);
            g_DownCount.fetch_add(1);
            if (g_TestLifeAt < 0) g_TestSkipGame = false;
            return true;
        }
    };

    void Install() { TryReviveHook::InstallAtOffset(offsets::Player_TryRevive); }

    void SetTest(bool on) { g_Test = on; }
    void SetTestLife(int life) { g_TestLife = life; }

    void OnPartnerRevived(u8 player_id) {
        /* Older versions revived on the spot and sent this. */
        Logging.Log("[coop] revive: player %d went down and was revived", player_id);
    }

    void OnPartnerDowned(u8 player_id, u8 state, const float* pos, u8 seconds) {
        net::Peer peers[net::MaxPeers];
        net::GetPeers(peers);
        const net::Peer* p = nullptr;
        for (auto& q : peers)
            if (q.present && q.id == player_id) p = &q;
        const char* name = p ? p->name : "Your partner";
        if (state == 1) {
            float me[3];
            char where[32] = "";
            if (GetLocalPlayerPos(me)) {
                float dx = pos[0] - me[0], dz = pos[2] - me[2];
                float d = std::sqrt(dx * dx + dz * dz);
                if (d >= 1000.0f) std::snprintf(where, sizeof(where), ", %.1f km away", d / 1000.0f);
                else std::snprintf(where, sizeof(where), ", %.0f m away", d);
            }
            Logging.Log("[coop] revive: %s is down at (%.1f, %.1f, %.1f), %u s left", name, pos[0], pos[1], pos[2], seconds);
            notify::Show("%s is down%s! Reach them within %u s", name, where, seconds);
        } else if (state == 0) {
            Logging.Log("[coop] revive: %s is back up", name);
            notify::Show("%s is back up", name);
        } else {
            Logging.Log("[coop] revive: nobody reached %s in time", name);
            notify::Show("Nobody reached %s in time", name);
        }
    }

    void Tick(s64 now) {
        g_NowMs.store(now);
        float me[3];
        if (!GetLocalPlayerPos(me)) {
            g_PartnerHere.store(false);
            return;
        }

        bool here = false;
        net::Peer peers[net::MaxPeers];
        net::GetPeers(peers);
        s64 tick_now = nn::os::GetSystemTick().GetInt64Value();
        s64 stale = static_cast<s64>(nn::os::GetSystemTickFrequency()) * PartnerStaleMs / 1000;
        u32 area = LocalArea();
        for (auto& p : peers)
            if (p.present && p.has_state && tick_now - static_cast<s64>(p.state_tick) < stale && p.state.map_area == area)
                here = true;
        g_PartnerHere.store(here);

        /* Just went down. */
        int downs = g_DownCount.load();
        if (downs != g_DownSeen) {
            g_DownSeen = downs;
            std::memcpy(g_DownAt, me, sizeof(me));
            g_ReachSince = 0;
            g_NextSend = 0;
            Logging.Log("[coop] revive: you're down at (%.1f, %.1f, %.1f); a partner has %lld s to reach you", me[0], me[1], me[2],
                        DownMs / 1000);
            notify::Show("You're down! A partner has %d s to reach you", static_cast<int>(DownMs / 1000));
        }

        if (g_Down.load()) {
            uintptr_t link = LinkActor();
            s32* life = link ? LifeOf(link) : nullptr;
            /* Stay put (and on a quarter heart) until someone comes. */
            warp::PlaceLink(g_DownAt);
            if (life && *life > DownLife) *life = DownLife;
            s64 left = DownMs - (now - g_DownSince.load());
            if (now >= g_NextSend) { /* tell partners, and keep telling late joiners */
                g_NextSend = now + 5000;
                net::SendDowned(1, g_DownAt, static_cast<u8>(left > 0 ? left / 1000 : 0));
            }
            /* A partner standing by me gets me up. */
            const char* who = nullptr;
            for (auto& p : peers) {
                if (!p.present || !p.has_state || p.state.map_area != area) continue;
                float dx = p.state.pos[0] - me[0], dy = p.state.pos[1] - me[1], dz = p.state.pos[2] - me[2];
                if (dx * dx + dy * dy + dz * dz <= ReachDist * ReachDist) who = p.name;
            }
            if (who) {
                if (!g_ReachSince) g_ReachSince = now;
                if (now - g_ReachSince >= ReachMs) {
                    g_Down.store(false);
                    if (life) *life = ReviveLife;
                    net::SendDowned(0, g_DownAt, 0);
                    Logging.Log("[coop] revive: %s got you back up (3 hearts)", who);
                    notify::Show("%s got you back up", who);
                }
            } else {
                g_ReachSince = 0;
            }
            if (g_Down.load() && left <= 0) {
                /* Out of time: on your own, on a quarter heart. */
                g_Down.store(false);
                net::SendDowned(2, g_DownAt, 0);
                Logging.Log("[coop] revive: nobody reached you in time");
                notify::Show("Nobody reached you in time. Careful: the next hit ends it");
            }
        }

        /* Test: a real lethal hit. Low life, and fairies/Mipha's Grace skipped,
           so the next hit that would end the game reaches our revive. */
        if (g_TestLife > 0 && here && g_TestLifeAt == 0) g_TestLifeAt = now + 8000;
        if (g_TestLifeAt > 0 && now > g_TestLifeAt) {
            g_TestLifeAt = -1;
            uintptr_t link = LinkActor();
            if (s32* life = link ? LifeOf(link) : nullptr) {
                Logging.Log("[coop] revive test: life %d -> %d; the next lethal hit skips fairies", *life, g_TestLife);
                *life = g_TestLife;
                g_TestSkipGame = true;
            }
        }

        if (g_Test && here && g_TestAt == 0) g_TestAt = now + 4000;
        if (g_TestAt > 0 && now > g_TestAt) {
            g_TestAt = -1;
            uintptr_t link = LinkActor();
            if (s32* life = link ? LifeOf(link) : nullptr) {
                Logging.Log("[coop] revive test: life %d -> 0, running the game's revive check", *life);
                *life = 0;
                g_TestSkipGame = true;
                bool ok = reinterpret_cast<bool (*)(uintptr_t)>(Base() + offsets::Player_TryRevive)(link);
                g_TestSkipGame = false;
                Logging.Log("[coop] revive test: revive check returned %d, life now %d", ok, *life);
            }
        }
    }
}
