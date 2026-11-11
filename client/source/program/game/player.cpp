#include "player.hpp"

#include <atomic>
#include <cmath>
#include <cstring>

#include <lib.hpp>

#include "offsets.hpp"

namespace coop::game {

    namespace {
        bool LooksLikePointer(uintptr_t p) { return p > 0x10000 && (p & 7) == 0; }
        std::atomic<int> g_BodyVia{0}; /* 1 = actor chain, 2 = global chain (logged when it changes) */
    }

    uintptr_t LinkRigidBody() {
        uintptr_t base = exl::util::GetMainModuleInfo().m_Total.m_Start;
        auto rd = [](uintptr_t a) { return *reinterpret_cast<const uintptr_t*>(a); };
        uintptr_t vtable = base + offsets::HkRigidBody_Vtable;
        uintptr_t body = 0;
        int via = 0;
        uintptr_t info = rd(base + offsets::PlayerInfoInstance);
        uintptr_t actor = info ? rd(info + offsets::PlayerInfo_PlayerActor) : 0;
        if (actor) {
            uintptr_t phys = rd(actor + offsets::Actor_Physics);
            uintptr_t chr = LooksLikePointer(phys) ? rd(phys + offsets::Physics_ToCharacter) : 0;
            uintptr_t b = LooksLikePointer(chr) ? rd(chr + offsets::Character_ToBody) : 0;
            if (LooksLikePointer(b) && rd(b) == vtable) body = b, via = 1;
        }
        if (!body) {
            uintptr_t g = rd(base + offsets::HavokPos_Global);
            uintptr_t a = g ? rd(g + offsets::HavokPos_A) : 0;
            uintptr_t b = a ? rd(a + offsets::HavokPos_B) : 0;
            if (LooksLikePointer(b)) {
                b += offsets::HavokPos_Pos - offsets::HkBody_Pos;
                if (rd(b) == vtable) body = b, via = 2;
            }
        }
        if (body && g_BodyVia.exchange(via) != via) {
            /* What the global table holds, to compare (it's the same body on a console). */
            uintptr_t g = rd(base + offsets::HavokPos_Global);
            uintptr_t a = g ? rd(g + offsets::HavokPos_A) : 0;
            uintptr_t t = a ? rd(a + offsets::HavokPos_B) : 0;
            if (t) t += offsets::HavokPos_Pos - offsets::HkBody_Pos;
            Logging.Log("[coop] player: Link's body %p, found from %s (the global table has %p)", reinterpret_cast<void*>(body),
                        via == 1 ? "his actor" : "the global table", reinterpret_cast<void*>(t));
        }
        return body;
    }

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

    namespace {
        const char* StageString(uintptr_t off) {
            uintptr_t base = exl::util::GetMainModuleInfo().m_Total.m_Start;
            const char* s = *reinterpret_cast<const char* const*>(base + off + 8);
            return s ? s : "";
        }

        u32 Crc32(const char* a, const char* b) {
            u32 crc = 0xFFFFFFFF;
            for (const char* parts[2] = {a, b}; const char* p : parts) {
                for (; p && *p; p++) {
                    crc ^= static_cast<u8>(*p);
                    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
                }
            }
            return ~crc;
        }
    }

    const char* LocalMapType() { return StageString(offsets::StageInfo_CurrentMapType); }
    const char* LocalMapName() { return StageString(offsets::StageInfo_CurrentMapName); }

    void LocalLife(u16* life, u16* max_life) {
        *life = *max_life = 0;
        uintptr_t base = exl::util::GetMainModuleInfo().m_Total.m_Start;
        auto info = *reinterpret_cast<const uintptr_t*>(base + offsets::PlayerInfoInstance);
        auto actor = info ? *reinterpret_cast<const uintptr_t*>(info + offsets::PlayerInfo_PlayerActor) : 0;
        if (!actor) return;
        auto vt = *reinterpret_cast<const uintptr_t*>(actor);
        auto get_life = reinterpret_cast<s32* (*)(uintptr_t)>(*reinterpret_cast<const uintptr_t*>(vt + offsets::ActorVt_GetLife));
        auto get_max = reinterpret_cast<s32 (*)(uintptr_t)>(*reinterpret_cast<const uintptr_t*>(vt + offsets::ActorVt_GetMaxLife));
        s32* l = get_life(actor);
        s32 m = get_max(actor);
        if (l && *l >= 0 && *l < 0x10000) *life = static_cast<u16>(*l);
        if (m >= 0 && m < 0x10000) *max_life = static_cast<u16>(m);
    }

    u32 OverworldArea() { return Crc32("MainField", nullptr); }

    u32 LocalArea() {
        const char* type = LocalMapType();
        if (!type[0]) return 0;
        /* The overworld's map name is just the grid section Link is in. */
        if (std::strcmp(type, "MainField") == 0) return OverworldArea();
        u32 h = Crc32(type, LocalMapName());
        return h ? h : 1;
    }

    bool IsAtTitle() {
        uintptr_t base = exl::util::GetMainModuleInfo().m_Total.m_Start;
        auto info = *reinterpret_cast<const uintptr_t*>(base + offsets::PlayerInfoInstance);
        if (info == 0) return false;
        auto actor = *reinterpret_cast<const uintptr_t*>(info + offsets::PlayerInfo_PlayerActor);
        if (actor == 0) return false;
        const auto* m = reinterpret_cast<const float*>(actor + offsets::Actor_Mtx);
        return m[3] == -1060.0f && m[7] == 250.0f && m[11] == 1830.0f;
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
