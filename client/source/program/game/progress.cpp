#include "progress.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>

#include <lib.hpp>
#include <nn/fs.hpp>
#include <nn/os.hpp>

#include "../net/client.hpp"
#include "gdt.hpp"
#include "offsets.hpp"
#include "player.hpp"
#include "autostart.hpp"

namespace coop::game::progress {

    namespace {
        constexpr int MaxFlags = 2048;

        /* The flag list: one name per line, kept in a static buffer. */
        char g_NameBuf[64 * 1024];
        const char* g_Names[MaxFlags];
        int g_NameCount = 0;

        /* idx (low 24 bits of a handle) -> name, sorted by idx. Two buffers so
           the hook (any thread) always reads a complete table. */
        struct Entry { u32 idx; u16 name; };

        /* Queued remote flag: list index + whether it was earned live (gives a reward). */
        struct Pending { u16 name; bool live; };
        Entry g_Tables[2][MaxFlags];
        std::atomic<int> g_Live{-1};      /* which table the hook reads, -1 = none */
        std::atomic<int> g_LiveCount{0};
        u32 g_BuiltForCounter = 0xFFFFFFFF;

        /* Set while the game thread applies a remote flag, so the hook doesn't echo it. */
        std::atomic<bool> g_Applying{false};

        /* Per flag: already true here (sent or received). The hook only touches
           this, never the game's own flag API, so it can't deadlock on its locks. */
        std::atomic<u8> g_Known[MaxFlags];

        /* Remote flags waiting for the game thread. */
        nn::os::MutexType g_Mutex;
        constexpr int QueueSize = 1024;
        Pending g_Queue[QueueSize];
        int g_QHead = 0, g_QCount = 0;
        std::atomic<bool> g_SnapshotWanted{false};

        /* Link is in the world and g_Known matches the save. Outside the world
           (title, loading screens) the hook leaves g_Known alone: the save's
           flags arrive during loading, and must not look like new progress. */
        std::atomic<bool> g_InWorld{false};
        /* The next world entry is a different save (or the same one reloaded):
           take its flags as they are instead of sharing the difference. */
        bool g_Fresh = true;
        char g_TestFlag[64];
        bool g_TestDone = false;
        bool g_TestMenu = false;
        s64 g_OpenMenuAt = 0; /* test: open the pause menu to show rewards */

        uintptr_t Base() { return exl::util::GetMainModuleInfo().m_Total.m_Start; }

        uintptr_t Manager() {
            uintptr_t slot = *reinterpret_cast<const uintptr_t*>(Base() + offsets::Got_GdtManagerInstance);
            return slot ? *reinterpret_cast<const uintptr_t*>(slot) : 0;
        }

        uintptr_t LiveParam() {
            uintptr_t mgr = Manager();
            if (!mgr) return 0;
            uintptr_t p = *reinterpret_cast<const uintptr_t*>(mgr + offsets::GdtManager_ParamBypass);
            return p ? *reinterpret_cast<const uintptr_t*>(p) : 0;
        }

        u32 ResetCounter() {
            uintptr_t mgr = Manager();
            return mgr ? *reinterpret_cast<const u32*>(mgr + offsets::GdtManager_ResetCounter) : 0xFFFFFFFF;
        }

        int FindByIdx(u32 idx) {
            int live = g_Live.load(std::memory_order_acquire);
            if (live < 0) return -1;
            const Entry* t = g_Tables[live];
            int lo = 0, hi = g_LiveCount.load(std::memory_order_acquire) - 1;
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                if (t[mid].idx == idx) return t[mid].name;
                if (t[mid].idx < idx) lo = mid + 1;
                else hi = mid - 1;
            }
            return -1;
        }

        int FindByName(const char* name, size_t len) {
            /* g_Names is sorted (the file is written sorted). */
            int lo = 0, hi = g_NameCount - 1;
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                int c = std::strncmp(g_Names[mid], name, len);
                if (c == 0 && g_Names[mid][len] != '\0') c = 1;
                if (c == 0) return mid;
                if (c < 0) lo = mid + 1;
                else hi = mid - 1;
            }
            return -1;
        }

        void LoadList() {
            nn::fs::FileHandle file;
            if (nn::fs::OpenFile(&file, "content:/botw-coop/sync-flags.txt", nn::fs::OpenMode_Read) != 0) {
                Logging.Log("[coop] progress: no sync-flags.txt; progress sharing is off");
                return;
            }
            long size = 0;
            nn::fs::GetFileSize(&size, file);
            if (size > static_cast<long>(sizeof(g_NameBuf)) - 1) size = sizeof(g_NameBuf) - 1;
            nn::fs::ReadFile(file, 0, g_NameBuf, static_cast<ulong>(size));
            nn::fs::CloseFile(file);
            g_NameBuf[size] = '\0';

            char* p = g_NameBuf;
            while (*p && g_NameCount < MaxFlags) {
                char* eol = std::strchr(p, '\n');
                if (eol) *eol = '\0';
                if (*p) g_Names[g_NameCount++] = p;
                if (!eol) break;
                p = eol + 1;
            }
            Logging.Log("[coop] progress: sharing %d flags", g_NameCount);
        }

        /* Resolves every name to its flag index for the current flag tables. */
        void BuildTable() {
            int next = g_Live.load() == 0 ? 1 : 0;
            Entry* t = g_Tables[next];
            int n = 0;
            for (int i = 0; i < g_NameCount; i++) {
                gdt::Handle h = gdt::BoolHandle(g_Names[i]);
                if (h == gdt::InvalidHandle) continue;
                t[n++] = {h & 0xFFFFFF, static_cast<u16>(i)};
            }
            std::sort(t, t + n, [](const Entry& a, const Entry& b) { return a.idx < b.idx; });
            g_LiveCount.store(0, std::memory_order_release);
            g_Live.store(next, std::memory_order_release);
            g_LiveCount.store(n, std::memory_order_release);
            Logging.Log("[coop] progress: resolved %d of %d flags", n, g_NameCount);
        }

        /* Sets a bool flag the way Manager::setBool does (main+0x64b134), but
           with the permission check off. The manager's normal reference checks
           permissions (checkPerms=1), and progress flags aren't writable from
           program code (only from events), so the plain setter refuses them. */
        bool ForceSetBool(gdt::Handle h) {
            uintptr_t mgr = Manager();
            if (!mgr || h == gdt::InvalidHandle) return false;
            uintptr_t bypass = mgr + 0xbd0; /* TriggerParamRef: +8 param1**, +16 param**, +24 checkPerms, +25 propagate, +26 changeOnlyOnce */

            auto set = reinterpret_cast<bool (*)(uintptr_t, bool, s32, bool, bool)>(Base() + offsets::TriggerParam_SetBool);
            s32 idx = static_cast<s32>(h & 0xFFFFFF);
            uintptr_t live = *reinterpret_cast<const uintptr_t*>(*reinterpret_cast<const uintptr_t*>(bypass + 8));
            bool ok = set(live, true, idx, false, false);
            /* Write both tables so reads see it whichever one they use. */
            uintptr_t other = *reinterpret_cast<const uintptr_t*>(*reinterpret_cast<const uintptr_t*>(bypass + 16));
            bool ok2 = other && other != live ? set(other, true, idx, false, false) : ok;
            return ok || ok2;
        }

        /* What a partner's live progress earns you. */
        const char* RewardFor(const char* flag) {
            if (std::strncmp(flag, "Clear_Dungeon", 13) == 0) return "Obj_DungeonClearSeal";  /* Spirit Orb */
            if (std::strncmp(flag, "MainField_Npc_HiddenKorok", 25) == 0) return "Obj_KorokNuts"; /* Korok seed */
            if (std::strcmp(flag, "Clear_RemainsWater") == 0) return "Obj_HeroSoul_Zora";     /* Mipha's Grace */
            if (std::strcmp(flag, "Clear_RemainsFire") == 0) return "Obj_HeroSoul_Goron";     /* Daruk's Protection */
            if (std::strcmp(flag, "Clear_RemainsWind") == 0) return "Obj_HeroSoul_Rito";      /* Revali's Gale */
            if (std::strcmp(flag, "Clear_RemainsElectric") == 0) return "Obj_HeroSoul_Gerudo"; /* Urbosa's Fury */
            return nullptr;
        }

        struct SafeString { uintptr_t vtable; const char* str; };

        /* Adds one item to the pouch, like picking it up. Game thread. */
        void GiveItem(const char* item) {
            uintptr_t slot = *reinterpret_cast<const uintptr_t*>(Base() + offsets::Got_PauseMenuDataMgrInstance);
            uintptr_t mgr = slot ? *reinterpret_cast<const uintptr_t*>(slot) : 0;
            if (!mgr) return;
            uintptr_t vt = *reinterpret_cast<const uintptr_t*>(Base() + offsets::Got_SafeStringVtableBase) + 0x10;
            SafeString name{vt, item};
            auto item_get = reinterpret_cast<void (*)(uintptr_t, const SafeString&, int, const void*)>(
                Base() + offsets::PauseMenuDataMgr_ItemGet);
            item_get(mgr, name, 1, nullptr);
        }

        void Queue(u16 name, bool live) {
            nn::os::LockMutex(&g_Mutex);
            if (g_QCount < QueueSize) {
                g_Queue[(g_QHead + g_QCount) % QueueSize] = {name, live};
                g_QCount++;
            }
            nn::os::UnlockMutex(&g_Mutex);
        }

        bool IsSet(u16 name) {
            bool v = false;
            gdt::GetBool(gdt::BoolHandle(g_Names[name]), &v);
            return v;
        }
    }

    /* Link (re)entered the world: sync g_Known with the save. After a loading
       screen in the same save, flags that turned true meanwhile are progress
       made during the transition, and are shared like any other. */
    static void EnterWorld() {
        int known = 0, shared = 0;
        for (int i = 0; i < g_NameCount; i++) {
            bool set = IsSet(static_cast<u16>(i));
            if (set) known++;
            if (set && !g_Fresh && !g_Known[i].load()) {
                Logging.Log("[coop] progress: %s (during loading)", g_Names[i]);
                net::SendFlag(g_Names[i], true);
                shared++;
            }
            g_Known[i].store(set ? 1 : 0);
        }
        if (g_Fresh) Logging.Log("[coop] progress: save has %d of %d shared flags", known, g_NameCount);
        else if (shared) Logging.Log("[coop] progress: shared %d flags set during loading", shared);
        g_Fresh = false;
        g_InWorld.store(true);
    }

    /* TriggerParam::setBool(param, value, idx, ...): every bool flag write. */
    HOOK_DEFINE_TRAMPOLINE(SetBoolHook) {
        static bool Callback(uintptr_t param, bool value, s32 idx, bool a, bool b) {
            int name = -1;
            bool was = true;
            if (value && g_InWorld.load(std::memory_order_relaxed) && !g_Applying.load(std::memory_order_relaxed) &&
                idx >= 0 && param == LiveParam()) {
                name = FindByIdx(static_cast<u32>(idx));
                if (name >= 0) was = g_Known[name].exchange(1) != 0;
            }
            bool ok = Orig(param, value, idx, a, b);
            if (name >= 0 && !was && !ok) g_Known[name].store(0); /* refused: not done after all */
            if (name >= 0 && !was && ok) {
                Logging.Log("[coop] progress: %s", g_Names[name]);
                net::SendFlag(g_Names[name], true);
            }
            return ok;
        }
    };

    void Install() {
        nn::os::InitializeMutex(&g_Mutex, false, 0);
        SetBoolHook::InstallAtOffset(offsets::TriggerParam_SetBool);
    }

    void Tick() {
        if (g_NameCount == 0) {
            static bool tried = false;
            if (!tried) { tried = true; LoadList(); }
            if (g_NameCount == 0) return;
        }
        if (!gdt::Ready()) return;

        u32 counter = ResetCounter();
        if (counter != g_BuiltForCounter) {
            g_BuiltForCounter = counter;
            BuildTable();
            /* Flag tables were reset: a save is (re)loading. Its flags are read
               once Link is in the world (they aren't there yet). */
            g_InWorld.store(false);
            g_Fresh = true;
        }
        if (IsAtTitle()) {
            g_InWorld.store(false);
            g_Fresh = true;
        }

        /* Debug: set a flag through the game's normal write path, as if the
           game did it, to exercise the hook that shares local progress. */
        float pos[3];
        if (g_TestFlag[0] && !g_TestDone && net::IsConnected() && GetLocalPlayerPos(pos)) {
            g_TestDone = true;
            gdt::Handle h = gdt::BoolHandle(g_TestFlag);
            Logging.Log("[coop] progress: test-setting %s locally", g_TestFlag);
            if (h != gdt::InvalidHandle) ForceSetBool(h); /* not marked as applying: the hook should share it */
        }

        /* Flag writes during loading and on the title screen don't stick, so
           received flags and snapshots wait until Link is in the world. */
        if (!GetLocalPlayerPos(pos)) {
            g_InWorld.store(false);
            return;
        }
        if (!g_InWorld.load()) EnterWorld();

        /* Apply flags other players set. */
        Pending batch[64];
        int n = 0;
        nn::os::LockMutex(&g_Mutex);
        while (g_QCount > 0 && n < 64) {
            batch[n++] = g_Queue[g_QHead];
            g_QHead = (g_QHead + 1) % QueueSize;
            g_QCount--;
        }
        nn::os::UnlockMutex(&g_Mutex);
        for (int i = 0; i < n; i++) {
            u16 name = batch[i].name;
            gdt::Handle h = gdt::BoolHandle(g_Names[name]);
            bool v = false;
            if (h == gdt::InvalidHandle) continue;
            if (gdt::GetBool(h, &v) && v) { g_Known[name].store(1); continue; }
            g_Known[name].store(1);
            g_Applying.store(true);
            ForceSetBool(h);
            g_Applying.store(false);
            bool now_set = false;
            gdt::GetBool(h, &now_set);
            if (!now_set) {
                /* Not accepted (e.g. mid-transition): try again next tick. */
                g_Known[name].store(0);
                Queue(name, batch[i].live);
                continue;
            }
            const char* reward = batch[i].live ? RewardFor(g_Names[name]) : nullptr;
            if (reward) {
                GiveItem(reward);
                if ((g_TestFlag[0] || g_TestMenu) && !g_OpenMenuAt)
                    g_OpenMenuAt = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds() + 2000;
            }
            Logging.Log("[coop] progress: got %s from a partner%s%s", g_Names[name], reward ? ", reward " : "",
                        reward ? reward : "");
        }

        if (g_OpenMenuAt > 0 && nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds() > g_OpenMenuAt) {
            g_OpenMenuAt = -1;
            PressButtons(1ull << 10, 150); /* Plus: pause menu / inventory */
        }

        /* Someone joined: send them everything already done here. */
        if (g_SnapshotWanted.exchange(false)) {
            int sent = 0;
            for (int i = 0; i < g_NameCount; i++) {
                if (g_Known[i].load()) {
                    net::SendFlag(g_Names[i], false);
                    sent++;
                }
            }
            Logging.Log("[coop] progress: sent %d completed flags to a new player", sent);
        }
    }

    void OnRemoteFlag(const char* name, size_t len, bool live) {
        int i = FindByName(name, len);
        if (i < 0) return; /* not a shared flag: ignore */
        Queue(static_cast<u16>(i), live);
    }

    void OnPlayerJoined() { g_SnapshotWanted.store(true); }

    void SetTestMenu(bool on) { g_TestMenu = on; }
    bool TestMenu() { return g_TestMenu; }

    void SetTestFlag(const char* name) {
        std::strncpy(g_TestFlag, name, sizeof(g_TestFlag) - 1);
    }
}
