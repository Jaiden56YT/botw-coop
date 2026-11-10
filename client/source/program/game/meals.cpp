#include "meals.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

#include <lib.hpp>
#include <nn/os.hpp>

#include "../net/client.hpp"
#include "items.hpp"
#include "notify.hpp"
#include "offsets.hpp"
#include "player.hpp"

namespace coop::game::meals {

    namespace {
        uintptr_t Base() { return exl::util::GetMainModuleInfo().m_Total.m_Start; }
        template<typename T> T Read(uintptr_t a) { return *reinterpret_cast<const T*>(a); }

        constexpr float ShareRange = 6.0f;
        constexpr s64 StaleMs = 3000;

        uintptr_t PlayerActor() {
            uintptr_t info = Read<uintptr_t>(Base() + offsets::PlayerInfoInstance);
            return info ? Read<uintptr_t>(info + offsets::PlayerInfo_PlayerActor) : 0;
        }
        s32* LifePtr(uintptr_t actor) {
            uintptr_t vt = Read<uintptr_t>(actor);
            return reinterpret_cast<s32* (*)(uintptr_t)>(Read<uintptr_t>(vt + offsets::ActorVt_GetLife))(actor);
        }
        s32 MaxLife(uintptr_t actor) {
            uintptr_t vt = Read<uintptr_t>(actor);
            return reinterpret_cast<s32 (*)(uintptr_t)>(Read<uintptr_t>(vt + offsets::ActorVt_GetMaxLife))(actor);
        }

        /* Healing partners sent us, waiting for the game thread. */
        struct Incoming {
            u8 from;
            u16 quarters;
        };
        Incoming g_In[8];
        int g_InCount = 0;
        nn::os::MutexType g_Mutex;
        bool g_MutexReady = false;

        s32 g_LastLife = -1;
        uintptr_t g_LastActor = 0;
        s32 g_Eaten = 0;     /* healing seen in this menu visit, not sent yet */
        s64 g_EatenAt = 0;

        int g_TestLife = 0;
        bool g_TestDone = false;

        void Hearts(int quarters, char* out, size_t cap) {
            static const char* const frac[] = {"", ".25", ".5", ".75"};
            std::snprintf(out, cap, "%d%s heart%s", quarters / 4, frac[quarters % 4], quarters == 4 ? "" : "s");
        }

        /* Partners close enough to share with: names, for the message. */
        int NearPartners(const float* me, u32 area, char* names, size_t cap, const net::Peer* peers) {
            s64 tick_now = nn::os::GetSystemTick().GetInt64Value();
            s64 stale = static_cast<s64>(nn::os::GetSystemTickFrequency()) * StaleMs / 1000;
            int n = 0;
            names[0] = '\0';
            for (int i = 0; i < net::MaxPeers; i++) {
                const net::Peer& p = peers[i];
                if (!p.present || !p.has_state || p.state.map_area != area || tick_now - static_cast<s64>(p.state_tick) > stale)
                    continue;
                float dx = p.state.pos[0] - me[0], dy = p.state.pos[1] - me[1], dz = p.state.pos[2] - me[2];
                if (dx * dx + dy * dy + dz * dz > ShareRange * ShareRange) continue;
                size_t len = std::strlen(names);
                std::snprintf(names + len, cap - len, "%s%s", n ? " and " : "", p.name);
                n++;
            }
            return n;
        }
    }

    void Init() {
        if (!g_MutexReady) {
            nn::os::InitializeMutex(&g_Mutex, false, 0);
            g_MutexReady = true;
        }
    }

    void SetTestLife(int quarters) { g_TestLife = quarters; }

    void OnPartnerHeal(u8 from, u16 quarters) {
        if (!g_MutexReady || quarters == 0) return;
        nn::os::LockMutex(&g_Mutex);
        if (g_InCount < 8) g_In[g_InCount++] = {from, quarters};
        nn::os::UnlockMutex(&g_Mutex);
    }

    void Tick(s64 now) {
        float me[3];
        uintptr_t actor = PlayerActor();
        if (!actor || !GetLocalPlayerPos(me)) {
            g_LastLife = -1;
            return;
        }
        s32* life = LifePtr(actor);
        if (!life) return;
        s32 max = MaxLife(actor);
        if (actor != g_LastActor) {
            g_LastActor = actor;
            g_LastLife = -1;
        }
        u32 area = LocalArea();
        net::Peer peers[net::MaxPeers];
        net::GetPeers(peers);

        /* Test: drop low once a partner is near, so their meal shows. */
        if (g_TestLife > 0 && !g_TestDone) {
            char names[96];
            if (NearPartners(me, area, names, sizeof(names), peers) > 0) {
                g_TestDone = true;
                *life = g_TestLife;
                g_LastLife = g_TestLife;
                Logging.Log("[coop] meals test: life set to %d", g_TestLife);
            }
        }

        /* A partner's meal: heal by the same amount (to our full hearts). */
        Incoming in[8];
        int nin = 0;
        if (g_MutexReady) {
            nn::os::LockMutex(&g_Mutex);
            nin = g_InCount;
            for (int i = 0; i < nin; i++) in[i] = g_In[i];
            g_InCount = 0;
            nn::os::UnlockMutex(&g_Mutex);
        }
        for (int i = 0; i < nin; i++) {
            const net::Peer* from = nullptr;
            for (auto& p : peers)
                if (p.present && p.id == in[i].from) from = &p;
            if (!from || !from->has_state || from->state.map_area != area) continue;
            float dx = from->state.pos[0] - me[0], dz = from->state.pos[2] - me[2];
            /* They checked we're near; allow a little more for lag. */
            if (dx * dx + dz * dz > (ShareRange + 4) * (ShareRange + 4)) continue;
            if (*life <= 0 || *life >= max) {
                Logging.Log("[coop] meals: %s shared a meal, but we're %s", from->name, *life <= 0 ? "down" : "already full");
                continue;
            }
            s32 before = *life;
            s32 after = before + in[i].quarters;
            if (after > max) after = max;
            *life = after;
            g_LastLife = after; /* not our own meal */
            char hearts[24];
            Hearts(after - before, hearts, sizeof(hearts));
            Logging.Log("[coop] meals: %s shared a meal: %d -> %d", from->name, before, after);
            notify::Show("%s shared a meal with you: +%s", from->name, hearts);
        }

        /* Our own meal: hearts went up while the menu is open. */
        s32 cur = *life;
        if (g_LastLife >= 0 && cur > g_LastLife) {
            bool menu = items::InMenu(now);
            Logging.Log("[coop] meals: hearts %d -> %d%s", g_LastLife, cur, menu ? " in the menu (a meal)" : "");
            if (menu) {
                g_Eaten += cur - g_LastLife;
                g_EatenAt = now;
            }
        }
        g_LastLife = cur;
        /* Send once a moment has passed (eating several dishes adds up). */
        if (g_Eaten > 0 && now - g_EatenAt > 300) {
            char names[96];
            if (NearPartners(me, area, names, sizeof(names), peers) > 0) {
                net::SendHeal(static_cast<u16>(g_Eaten > 0xffff ? 0xffff : g_Eaten));
                char hearts[24];
                Hearts(g_Eaten, hearts, sizeof(hearts));
                Logging.Log("[coop] meals: ate %d quarter hearts next to %s, shared", g_Eaten, names);
                notify::Show("You shared your meal with %s: +%s", names, hearts);
            }
            g_Eaten = 0;
        }
    }
}
