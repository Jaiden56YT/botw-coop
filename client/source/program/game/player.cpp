#include "player.hpp"

#include <cmath>
#include <cstring>

#include <lib.hpp>

#include "offsets.hpp"

namespace coop::game {

    bool GetLocalPlayer(float (&pos)[3], float* rot_y) {
        uintptr_t base = exl::util::GetMainModuleInfo().m_Total.m_Start;
        auto info = *reinterpret_cast<const uintptr_t*>(base + offsets::PlayerInfoInstance);
        if (info == 0) return false;
        auto actor = *reinterpret_cast<const uintptr_t*>(info + offsets::PlayerInfo_PlayerActor);
        if (actor == 0) return false;

        /* PlayerInfo::mPlayerPos stays (0,0,0) until the world has finished
           loading and gameplay starts. Spawning actors before that stalls the
           loading screen, so this is the "in the world" signal. */
        const auto* info_pos = reinterpret_cast<const float*>(info + offsets::PlayerInfo_PlayerPos);
        if (info_pos[0] == 0.0f && info_pos[1] == 0.0f && info_pos[2] == 0.0f) return false;

        /* The actor's own transform. (PlayerInfo::mPlayerPos is only refreshed
           now and then, so it can be stale or still zero.) */
        const auto* m = reinterpret_cast<const float*>(actor + offsets::Actor_Mtx);
        pos[0] = m[3];
        pos[1] = m[7];
        pos[2] = m[11];
        if (rot_y) *rot_y = std::atan2(m[2], m[10]);
        for (float v : pos)
            if (!std::isfinite(v)) return false;
        /* (0,0,0) means the actor exists but hasn't been placed yet. */
        if (pos[0] == 0.0f && pos[1] == 0.0f && pos[2] == 0.0f) return false;
        /* The title screen renders the Great Plateau with a Link actor parked at
           exactly this spot. A loaded save never puts Link on these exact floats. */
        if (pos[0] == -1060.0f && pos[1] == 250.0f && pos[2] == 1830.0f) return false;
        return true;
    }

    bool GetLocalPlayerPos(float (&out)[3]) { return GetLocalPlayer(out, nullptr); }

    bool IsPlayerPlaced() {
        uintptr_t base = exl::util::GetMainModuleInfo().m_Total.m_Start;
        auto info = *reinterpret_cast<const uintptr_t*>(base + offsets::PlayerInfoInstance);
        if (info == 0) return false;
        auto actor = *reinterpret_cast<const uintptr_t*>(info + offsets::PlayerInfo_PlayerActor);
        if (actor == 0) return false;
        const auto* m = reinterpret_cast<const float*>(actor + offsets::Actor_Mtx);
        if (m[3] == 0.0f && m[7] == 0.0f && m[11] == 0.0f) return false;
        if (m[3] == -1060.0f && m[7] == 250.0f && m[11] == 1830.0f) return false; /* title screen */
        return true;
    }
}

namespace coop::game {
    void LogPlayerDebug() {
        uintptr_t base = exl::util::GetMainModuleInfo().m_Total.m_Start;
        auto info = *reinterpret_cast<const uintptr_t*>(base + offsets::PlayerInfoInstance);
        if (!info) { Logging.Log("[coop] debug: no PlayerInfo"); return; }
        auto actor = *reinterpret_cast<const uintptr_t*>(info + offsets::PlayerInfo_PlayerActor);
        auto* p = reinterpret_cast<const float*>(info + offsets::PlayerInfo_PlayerPos);
        if (!actor) { Logging.Log("[coop] debug: PlayerInfo %p, no player actor", (void*)info); return; }
        auto* m = reinterpret_cast<const float*>(actor + 0x398);
        Logging.Log("[coop] debug: player actor %p, PlayerInfo pos (%.1f, %.1f, %.1f), actor mtx pos (%.1f, %.1f, %.1f)",
                    (void*)actor, p[0], p[1], p[2], m[3], m[7], m[11]);
    }
}
