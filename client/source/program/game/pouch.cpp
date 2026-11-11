#include "pouch.hpp"

#include <cstdio>
#include <cstring>

#include <lib.hpp>

#include "offsets.hpp"

namespace coop::game::pouch {

    namespace {
        uintptr_t Base() { return exl::util::GetMainModuleInfo().m_Total.m_Start; }
        template<typename T> T Read(uintptr_t a) { return *reinterpret_cast<const T*>(a); }

        uintptr_t Manager() {
            uintptr_t slot = Read<uintptr_t>(Base() + offsets::Got_PauseMenuDataMgrInstance);
            return slot ? Read<uintptr_t>(slot) : 0;
        }

        bool IsWeaponType(s32 type) { return type == 0 || type == 1 || type == 3; } /* sword, bow, shield (2 = arrows) */

        struct Entry {
            uintptr_t item;
            char name[32];
            WeaponInfo info;
        };
        constexpr int MaxEntries = 80; /* 20 weapons + 14 bows + 20 shields at most */
        Entry g_Now[MaxEntries], g_Before[MaxEntries];
        int g_NowCount = 0, g_BeforeCount = 0;
        bool g_HaveBefore = false;
        s64 g_NextAt = 0;

        struct Removed {
            char name[32];
            WeaponInfo info;
            s64 at;
            bool used;
        };
        constexpr int MaxRemoved = 16;
        Removed g_Removed[MaxRemoved];
        int g_RemovedHead = 0;

        /* The weapons in the inventory. No lock is taken; instead every node
           has to be one of the manager's 420 item slots, so a list caught
           mid-change can't lead anywhere bad. -1 if it didn't read cleanly. */
        int Collect(uintptr_t mgr, Entry* out, int max) {
            uintptr_t head = mgr + offsets::PauseMenuDataMgr_ItemList;
            uintptr_t first = mgr + offsets::PauseMenuDataMgr_Items;
            uintptr_t last = first + 420 * offsets::PouchItem_Size;
            s32 count = Read<s32>(head + 0x10), node_off = Read<s32>(head + 0x14);
            if (count < 0 || count > 420 || node_off != 8) return -1;
            int n = 0, seen = 0;
            for (uintptr_t node = Read<uintptr_t>(head + 8); node != head; seen++) {
                uintptr_t it = node - node_off;
                if (seen > 420 || it < first || it >= last || (it - first) % offsets::PouchItem_Size != 0) return -1;
                node = Read<uintptr_t>(node + 8);
                if (!IsWeaponType(Read<s32>(it + offsets::PouchItem_Type))) continue;
                const char* name = Read<const char*>(it + offsets::PouchItem_Name);
                /* The name is the item's own buffer, a little further in. */
                if (reinterpret_cast<uintptr_t>(name) != it + offsets::PouchItem_Name + 0xc) return -1;
                if (std::strncmp(name, "Weapon_", 7) != 0 || n >= max) continue;
                Entry& e = out[n++];
                e.item = it;
                std::snprintf(e.name, sizeof(e.name), "%.31s", name);
                e.info.flags = Read<u32>(it + offsets::PouchItem_ModifierFlags);
                e.info.value = Read<s32>(it + offsets::PouchItem_ModifierValue);
                e.info.durability = Read<s32>(it + offsets::PouchItem_Value);
            }
            return seen == count ? n : -1;
        }
    }

    void Tick(s64 now) {
        if (now < g_NextAt) return;
        g_NextAt = now + 200;
        uintptr_t mgr = Manager();
        if (!mgr) return;
        int n = Collect(mgr, g_Now, MaxEntries);
        if (n < 0) return;
        g_NowCount = n;
        if (g_HaveBefore) {
            for (int i = 0; i < g_BeforeCount; i++) {
                const Entry& b = g_Before[i];
                if (b.info.durability <= 0) continue; /* already noted as gone */
                /* Gone from the list, or still listed with its durability at
                   0: that's how a weapon dropped from the menu sits there
                   until the menu closes. */
                const Entry* cur = nullptr;
                for (int k = 0; k < g_NowCount && !cur; k++)
                    if (g_Now[k].item == b.item && std::strcmp(g_Now[k].name, b.name) == 0) cur = &g_Now[k];
                if (cur && cur->info.durability > 0) continue;
                Logging.Log("[coop] pouch: %s left the inventory (bonus flags %x, value %d, durability %d)", b.name, b.info.flags,
                            b.info.value, b.info.durability);
                Removed& r = g_Removed[g_RemovedHead++ % MaxRemoved];
                std::memcpy(r.name, b.name, sizeof(r.name));
                r.info = b.info;
                r.at = now;
                r.used = false;
            }
        }
        std::memcpy(g_Before, g_Now, sizeof(Entry) * g_NowCount);
        g_BeforeCount = g_NowCount;
        g_HaveBefore = true;
    }

    bool TakeRemoved(const char* name, s64 now, WeaponInfo* out) {
        Removed* best = nullptr;
        for (auto& r : g_Removed) {
            if (r.used || !r.name[0] || now - r.at > 120000 || std::strcmp(r.name, name) != 0) continue;
            if (!best || r.at > best->at) best = &r;
        }
        if (!best) return false;
        best->used = true;
        *out = best->info;
        return true;
    }

    bool Give(const char* name, const WeaponInfo& info) {
        uintptr_t mgr = Manager();
        if (!mgr || info.durability <= 0) return false;
        uintptr_t head = mgr + offsets::PauseMenuDataMgr_ItemList;
        s32 before = Read<s32>(head + 0x10);
        struct SafeString {
            uintptr_t vtable;
            const char* str;
        };
        struct Modifier {
            u32 flags;
            s32 value;
        };
        uintptr_t vt = Read<uintptr_t>(Base() + offsets::Got_SafeStringVtableBase) + 0x10;
        SafeString item{vt, name};
        Modifier mod{info.flags, info.value};
        /* For a weapon the count argument is its durability. */
        reinterpret_cast<void (*)(uintptr_t, const SafeString&, int, const Modifier*)>(Base() + offsets::PauseMenuDataMgr_ItemGet)(
            mgr, item, info.durability, info.flags ? &mod : nullptr);
        return Read<s32>(head + 0x10) > before;
    }

    void Describe(const WeaponInfo& info, char* out, size_t cap) {
        if (cap == 0) return;
        out[0] = '\0';
        u32 f = info.flags & 0x7fffffff; /* the top bit marks the stronger (yellow) kind */
        if (f & 0x1) std::snprintf(out, cap, "Attack Up +%d", info.value);
        else if (f & 0x2) std::snprintf(out, cap, "Durability Up");
        else if (f & 0x4) std::snprintf(out, cap, "Critical Hit");
        else if (f & 0x8) std::snprintf(out, cap, "Long Throw");
        else if (f & 0x10) std::snprintf(out, cap, "Multi-Shot x%d", info.value);
        else if (f & 0x20) std::snprintf(out, cap, "Zoom");
        else if (f & 0x40) std::snprintf(out, cap, "Quick Shot");
        else if (f & 0x80) std::snprintf(out, cap, "Shield Surf Up");
        else if (f & 0x100) std::snprintf(out, cap, "Shield Guard Up +%d", info.value);
    }
}
