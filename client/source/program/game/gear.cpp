#include "gear.hpp"

#include <cstdio>
#include <cstring>

#include <lib.hpp>
#include <nn/fs.hpp>

#include "../net/client.hpp"
#include "offsets.hpp"
#include "player.hpp"

namespace coop::game::gear {

    namespace {
        using CsFn = void (*)(uintptr_t);
        using CsTryFn = bool (*)(uintptr_t);

        constexpr s64 ScanMs = 1000;
        constexpr s64 ResendMs = 5000;

        uintptr_t Base() { return exl::util::GetMainModuleInfo().m_Total.m_Start; }
        template<typename T> T Read(uintptr_t addr) { return *reinterpret_cast<const T*>(addr); }

        /* gear-map.txt: "Armor_151_Upper Armor_116_Upper\n", sorted by armor name. */
        char g_MapBuf[24 * 1024];
        const char* g_Armor[512];
        const char* g_Model[512];
        int g_MapCount = 0;
        bool g_MapTried = false;

        void LoadMap() {
            g_MapTried = true;
            nn::fs::FileHandle file;
            if (nn::fs::OpenFile(&file, "content:/botw-coop/gear-map.txt", nn::fs::OpenMode_Read) != 0) {
                Logging.Log("[coop] gear: no gear-map.txt; partners' armor won't show");
                return;
            }
            long size = 0;
            nn::fs::GetFileSize(&size, file);
            if (size > static_cast<long>(sizeof(g_MapBuf)) - 1) size = sizeof(g_MapBuf) - 1;
            nn::fs::ReadFile(file, 0, g_MapBuf, static_cast<ulong>(size));
            nn::fs::CloseFile(file);
            g_MapBuf[size] = '\0';
            char* p = g_MapBuf;
            while (*p && g_MapCount < 512) {
                char* eol = std::strchr(p, '\n');
                if (eol) *eol = '\0';
                char* sp = std::strchr(p, ' ');
                if (sp) {
                    *sp = '\0';
                    g_Armor[g_MapCount] = p;
                    g_Model[g_MapCount] = sp + 1;
                    g_MapCount++;
                }
                if (!eol) break;
                p = eol + 1;
            }
            Logging.Log("[coop] gear: %d armor pieces can be shown on partners", g_MapCount);
        }

        /* What we last sent. */
        char g_Sent[Slots][NameLen];
        s64 g_NextScan = 0;
        s64 g_LastSend = 0;

        int SlotOf(const char* name) {
            if (std::strncmp(name, "Armor_", 6) != 0 || std::strncmp(name, "Armor_Default", 13) == 0) return -1;
            const char* tail = std::strrchr(name, '_');
            if (!tail) return -1;
            if (std::strcmp(tail, "_Head") == 0 || std::strcmp(tail, "_B") == 0) return 0;
            if (std::strcmp(tail, "_Upper") == 0) return 1;
            if (std::strcmp(tail, "_Lower") == 0) return 2;
            return -1;
        }
    }

    void LocalWorn(char (&out)[Slots][NameLen]) { std::memcpy(out, g_Sent, sizeof(out)); }

    bool GearActorFor(const char* armor, char (&out)[64]) {
        if (!g_MapTried) LoadMap();
        int lo = 0, hi = g_MapCount - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            int c = std::strcmp(g_Armor[mid], armor);
            if (c == 0) {
                std::snprintf(out, sizeof(out), "CoopGear_%s", g_Model[mid]);
                return true;
            }
            if (c < 0) lo = mid + 1;
            else hi = mid - 1;
        }
        return false;
    }

    void Tick(s64 now) {
        if (now < g_NextScan) return;
        g_NextScan = now + ScanMs;
        if (!g_MapTried) LoadMap();
        float me[3];
        if (!GetLocalPlayerPos(me)) return;

        /* Worn armor actors sit on Link (the pieces in the world are elsewhere). */
        uintptr_t slot = Read<uintptr_t>(Base() + offsets::Got_ActorCreatorInstance);
        uintptr_t creator = slot ? Read<uintptr_t>(slot) : 0;
        if (!creator) return;
        uintptr_t cs = creator + offsets::ActorCreator_ActorListCS;
        if (!reinterpret_cast<CsTryFn>(Base() + offsets::CriticalSection_TryLock)(cs)) return;
        char worn[Slots][NameLen] = {};
        uintptr_t head = creator + offsets::ActorCreator_ActorList;
        s32 node_off = Read<s32>(creator + offsets::ActorCreator_ActorListOffset);
        int guard = 0;
        for (uintptr_t node = Read<uintptr_t>(head + 8); node != head && node != 0 && guard < 20000; guard++) {
            uintptr_t a = node - node_off;
            node = Read<uintptr_t>(node + 8);
            const char* name = Read<const char*>(a + offsets::BaseProc_NameStr);
            if (!name) continue;
            int s = SlotOf(name);
            if (s < 0) continue;
            const float* m = reinterpret_cast<const float*>(a + offsets::Actor_Mtx);
            float dx = m[3] - me[0], dy = m[7] - me[1], dz = m[11] - me[2];
            if (dx * dx + dy * dy + dz * dz > 0.3f * 0.3f) continue;
            std::strncpy(worn[s], name, NameLen - 1);
        }
        reinterpret_cast<CsFn>(Base() + offsets::CriticalSection_Unlock)(cs);

        bool changed = std::memcmp(worn, g_Sent, sizeof(worn)) != 0;
        if (!changed && now - g_LastSend < ResendMs) return;
        if (changed) Logging.Log("[coop] gear: wearing %s / %s / %s", worn[0][0] ? worn[0] : "-",
                                 worn[1][0] ? worn[1] : "-", worn[2][0] ? worn[2] : "-");
        std::memcpy(g_Sent, worn, sizeof(worn));
        g_LastSend = now;
        net::SendGear(worn[0], worn[1], worn[2]);
    }
}
