#include "revive.hpp"

#include <atomic>

#include <lib.hpp>
#include <nn/os.hpp>

#include "../net/client.hpp"
#include "notify.hpp"
#include "offsets.hpp"
#include "player.hpp"

namespace coop::game::revive {

    namespace {
        constexpr s32 ReviveLife = 12;     /* 3 hearts, in quarter hearts */
        constexpr s64 CooldownMs = 120000;
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
        std::atomic<int> g_Revived{0};
        int g_Logged = 0;
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
            /* A fairy or Mipha's Grace first (the test skips them to reach ours). */
            if (!g_TestSkipGame && Orig(player)) return true;
            s64 now = g_NowMs.load();
            if (!g_PartnerHere.load() || now < g_ReadyAt.load()) return false;
            s32* life = LifeOf(player);
            if (!life) return false;
            *life = ReviveLife;
            g_ReadyAt.store(now + CooldownMs);
            g_Revived.fetch_add(1);
            if (g_TestLifeAt < 0) g_TestSkipGame = false;
            return true;
        }
    };

    void Install() { TryReviveHook::InstallAtOffset(offsets::Player_TryRevive); }

    void SetTest(bool on) { g_Test = on; }
    void SetTestLife(int life) { g_TestLife = life; }

    void OnPartnerRevived(u8 player_id) {
        Logging.Log("[coop] revive: player %d went down and was revived by being near you", player_id);
        net::Peer peers[net::MaxPeers];
        net::GetPeers(peers);
        for (auto& p : peers)
            if (p.present && p.id == player_id) notify::Show("%s went down. You got them back up", p.name);
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

        int revived = g_Revived.load();
        if (revived != g_Logged) {
            g_Logged = revived;
            Logging.Log("[coop] revive: you went down; your partner got you back up (3 hearts). Next one in %lld s",
                        CooldownMs / 1000);
            net::SendRevived();
            notify::Show("Your partner got you back up");
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
