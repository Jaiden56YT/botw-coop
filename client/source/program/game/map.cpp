#include "map.hpp"

#include <cstring>

#include <lib.hpp>
#include <nn/os.hpp>

#include "../net/client.hpp"
#include "autostart.hpp"
#include "offsets.hpp"
#include "player.hpp"
#include "progress.hpp"

/* The map keeps its own stamp objects (a pool of 200 in the map manager); the
   MapApp_MapIcon* game data is only a copy made when stamps change or the game
   saves, and read back when a save loads. So partner markers are made through
   the map's own functions (see docs/reverse-engineering.md, "Map stamps"):
   add with the map's stamp-add function, move with the stamp's setPos virtual,
   remove with its setActive(false) virtual, which also updates the game data. */
namespace coop::game::map {

    namespace {
        /* Stamps the player can place are icons 27..35 (and 37..41); 36 is never drawn. */
        constexpr s32 PartnerIcon = 34;
        /* Low mantissa bits put on every coordinate of the mod's stamps. */
        constexpr u32 TagMask = 0xF, TagBits = 0xA;
        /* A partner whose state is older than this is gone from the map. */
        constexpr s64 StaleMs = 5000;
        constexpr s64 UpdateMs = 1000;

        struct PtrArray {
            s32 num;
            s32 max;
            uintptr_t* ptrs;
        };

        uintptr_t Base() { return exl::util::GetMainModuleInfo().m_Total.m_Start; }

        uintptr_t MapMgr() { return *reinterpret_cast<const uintptr_t*>(Base() + offsets::MapMgr_Instance); }

        const PtrArray& Pool(uintptr_t mgr) {
            return *reinterpret_cast<const PtrArray*>(mgr + offsets::MapMgr_StampPool);
        }

        u32 Flags(uintptr_t s) { return *reinterpret_cast<const u32*>(s + offsets::MapStamp_Flags); }
        s32 Icon(uintptr_t s) { return *reinterpret_cast<const s32*>(s + offsets::MapStamp_Icon); }
        const float* Pos(uintptr_t s) { return reinterpret_cast<const float*>(s + offsets::MapStamp_Pos); }
        bool Active(uintptr_t s) { return (Flags(s) & offsets::MapStampFlag_Active) != 0; }

        float Tag(float v) {
            u32 bits;
            std::memcpy(&bits, &v, 4);
            bits = (bits & ~TagMask) | TagBits;
            std::memcpy(&v, &bits, 4);
            return v;
        }

        bool IsTagged(float v) {
            u32 bits;
            std::memcpy(&bits, &v, 4);
            return (bits & TagMask) == TagBits;
        }

        bool IsOurs(uintptr_t s) {
            const float* p = Pos(s);
            return Active(s) && Icon(s) == PartnerIcon && IsTagged(p[0]) && IsTagged(p[1]) && IsTagged(p[2]);
        }

        void SetPos(uintptr_t s, const float (&pos)[3]) {
            auto vt = *reinterpret_cast<const uintptr_t*>(s);
            reinterpret_cast<void (*)(uintptr_t, const float*)>(
                *reinterpret_cast<const uintptr_t*>(vt + offsets::MapStamp_VtSetPos))(s, pos);
        }

        void Remove(uintptr_t s) {
            auto vt = *reinterpret_cast<const uintptr_t*>(s);
            reinterpret_cast<void (*)(uintptr_t, bool)>(
                *reinterpret_cast<const uintptr_t*>(vt + offsets::MapStamp_VtSetActive))(s, false);
        }

        /* Adds a stamp the way the map does when a save loads; returns it, or 0. */
        uintptr_t Add(uintptr_t mgr, const float (&pos)[3]) {
            const PtrArray& pool = Pool(mgr);
            bool any_free = false;
            for (s32 i = 0; i < pool.num; i++)
                if (pool.ptrs[i] && !Active(pool.ptrs[i])) any_free = true;
            if (!any_free) return 0; /* all stamps in use */
            reinterpret_cast<void (*)(uintptr_t, s32, const float*, bool)>(Base() + offsets::MapMgr_AddStamp)(
                mgr, PartnerIcon, pos, false);
            for (s32 i = 0; i < pool.num; i++) {
                uintptr_t s = pool.ptrs[i];
                if (s && IsOurs(s) && std::memcmp(Pos(s), pos, sizeof(pos)) == 0) {
                    /* The add path shows the stamp with setVisible(true), but a reused
                       pool stamp is often still marked visible from before, so it isn't
                       queued for the map screen (MapMgr+0x2b0). Toggle it. */
                    auto set_visible = reinterpret_cast<void (*)(uintptr_t, bool)>(Base() + offsets::MapStamp_SetVisible);
                    set_visible(s, false);
                    set_visible(s, true);
                    return s;
                }
            }
            return 0;
        }

        uintptr_t g_Stamp[net::MaxPeers];
        /* Where each partner last was in the overworld. Inside a shrine or a
           Divine Beast their coordinates are that stage's, so the marker stays
           here: at the shrine they went into. */
        float g_LastOutside[net::MaxPeers][3];
        bool g_HaveOutside[net::MaxPeers];
        s64 g_NextUpdate = 0;
        u32 g_Counter = 0xFFFFFFFF;
        bool g_Swept = false;
        bool g_Warned = false;
        s64 g_OpenMapAt = 0; /* test: open the map to show the marker */
    }

    void Tick(s64 now) {
        if (g_OpenMapAt > 0 && now > g_OpenMapAt) {
            g_OpenMapAt = -1;
            PressButtons(1ull << 11, 150); /* Minus: map */
        }
        if (now < g_NextUpdate) return;
        g_NextUpdate = now + UpdateMs;

        /* A save (re)loading rebuilds the map's stamps from its game data. */
        uintptr_t slot = *reinterpret_cast<const uintptr_t*>(Base() + offsets::Got_GdtManagerInstance);
        uintptr_t gdt = slot ? *reinterpret_cast<const uintptr_t*>(slot) : 0;
        if (!gdt) return;
        u32 counter = *reinterpret_cast<const u32*>(gdt + offsets::GdtManager_ResetCounter);
        if (counter != g_Counter) {
            g_Counter = counter;
            g_Swept = false;
            for (auto& s : g_Stamp) s = 0;
        }

        float me[3];
        if (!GetLocalPlayerPos(me)) return;
        uintptr_t mgr = MapMgr();
        const PtrArray* pool = mgr ? &Pool(mgr) : nullptr;
        if (!pool || pool->num <= 0 || pool->num > 1000 || !pool->ptrs) {
            if (!g_Warned) Logging.Log("[coop] map: no map manager; partner markers are off");
            g_Warned = true;
            return;
        }

        if (!g_Swept) {
            /* Remove partner markers that were saved into this save last time. */
            g_Swept = true;
            int removed = 0, used = 0;
            for (s32 i = 0; i < pool->num; i++) {
                uintptr_t s = pool->ptrs[i];
                if (!s || !Active(s)) continue;
                if (IsOurs(s)) {
                    Remove(s);
                    removed++;
                } else {
                    used++;
                }
            }
            Logging.Log("[coop] map: %d of %d stamps used, %d old partner markers removed", used, pool->num,
                        removed);
        }

        net::Peer peers[net::MaxPeers];
        net::GetPeers(peers);
        s64 tick_now = nn::os::GetSystemTick().GetInt64Value();
        s64 stale_ticks = static_cast<s64>(nn::os::GetSystemTickFrequency()) * StaleMs / 1000;
        for (int p = 0; p < net::MaxPeers; p++) {
            const net::Peer& peer = peers[p];
            bool fresh = peer.present && peer.has_state && tick_now - static_cast<s64>(peer.state_tick) < stale_ticks;
            uintptr_t& stamp = g_Stamp[p];
            /* Deleted on the map by the player, or reused: ours no longer. */
            if (stamp && !IsOurs(stamp)) stamp = 0;
            if (!fresh) {
                g_HaveOutside[p] = false;
                if (stamp) {
                    Remove(stamp);
                    Logging.Log("[coop] map: removed the marker for player %d", p);
                    stamp = 0;
                }
                continue;
            }
            if (peer.state.map_area == OverworldArea()) {
                std::memcpy(g_LastOutside[p], peer.state.pos, sizeof(g_LastOutside[p]));
                g_HaveOutside[p] = true;
            }
            if (!g_HaveOutside[p]) continue;
            const float* at = g_LastOutside[p];
            const float pos[3] = {Tag(at[0]), Tag(at[1]), Tag(at[2])};
            if (stamp) {
                SetPos(stamp, pos);
                continue;
            }
            stamp = Add(mgr, pos);
            if (!stamp) continue;
            Logging.Log("[coop] map: marker for %s", peer.name);
            if (progress::TestMenu() && g_OpenMapAt == 0) g_OpenMapAt = now + 15000;
        }
    }
}
