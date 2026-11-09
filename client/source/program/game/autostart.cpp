#include "autostart.hpp"

#include <atomic>

#include <lib.hpp>
#include <nn/os.hpp>

#include "player.hpp"

namespace nn::hid {
    /* Every Npad*State has this layout. */
    struct NpadState {
        s64 sampling_number;
        u64 buttons;
        s32 stick_l[2];
        s32 stick_r[2];
        u32 attributes;
        u32 reserved;
    };
    static_assert(sizeof(NpadState) == 0x28);

    struct NpadFullKeyState : NpadState {};
    struct NpadHandheldState : NpadState {};
    struct NpadJoyDualState : NpadState {};

    void GetNpadStates(NpadFullKeyState* out, int count, const u32& id);
    void GetNpadStates(NpadHandheldState* out, int count, const u32& id);
    void GetNpadStates(NpadJoyDualState* out, int count, const u32& id);
}

namespace coop::game {

    namespace {
        std::atomic<bool> g_Enabled{false};
        std::atomic<s64> g_PlayerSeenMs{0};
        std::atomic<u32> g_HookCalls[3];
        std::atomic<u32> g_Presses{0};

        s64 NowMs() { return nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds(); }

        bool ShouldPressA() {
            if (!g_Enabled.load(std::memory_order_relaxed)) return false;
            s64 now = NowMs();
            /* Keep pressing until Link has been placed at the save position for
               3 s, then stop for good. (Stopping the moment he's placed, or
               never stopping, both left the loading screen up.) */
            if (IsPlayerPlaced()) {
                s64 seen = g_PlayerSeenMs.load();
                if (seen == 0) g_PlayerSeenMs.store(now);
                else if (now - seen > 3000) {
                    g_Enabled.store(false);
                    Logging.Log("[coop] autostart: save is loading, stopping");
                    return false;
                }
                return now % 1500 < 120;
            }
            g_PlayerSeenMs.store(0);
            return now % 1500 < 120;
        }

        template<typename State>
        void Apply(int which, State* out, int count) {
            g_HookCalls[which].fetch_add(1, std::memory_order_relaxed);
            if (!ShouldPressA()) return;
            g_Presses.fetch_add(1, std::memory_order_relaxed);
            for (int i = 0; i < count; i++) out[i].buttons |= 1; /* A */
        }
    }

    HOOK_DEFINE_TRAMPOLINE(FullKeyHook) {
        static void Callback(nn::hid::NpadFullKeyState* out, int count, const u32& id) {
            Orig(out, count, id);
            Apply(0, out, count);
        }
    };

    HOOK_DEFINE_TRAMPOLINE(HandheldHook) {
        static void Callback(nn::hid::NpadHandheldState* out, int count, const u32& id) {
            Orig(out, count, id);
            Apply(1, out, count);
        }
    };

    HOOK_DEFINE_TRAMPOLINE(JoyDualHook) {
        static void Callback(nn::hid::NpadJoyDualState* out, int count, const u32& id) {
            Orig(out, count, id);
            Apply(2, out, count);
        }
    };

    void InstallAutostart() {
        FullKeyHook::InstallAtFuncPtr(
            static_cast<void (*)(nn::hid::NpadFullKeyState*, int, const u32&)>(nn::hid::GetNpadStates));
        HandheldHook::InstallAtFuncPtr(
            static_cast<void (*)(nn::hid::NpadHandheldState*, int, const u32&)>(nn::hid::GetNpadStates));
        JoyDualHook::InstallAtFuncPtr(
            static_cast<void (*)(nn::hid::NpadJoyDualState*, int, const u32&)>(nn::hid::GetNpadStates));
    }

    void LogAutostartStats() {
        if (!g_Enabled.load()) return;
        Logging.Log("[coop] autostart: hid calls fullkey=%u handheld=%u joydual=%u, A injected %u times",
                    g_HookCalls[0].load(), g_HookCalls[1].load(), g_HookCalls[2].load(), g_Presses.load());
    }

    void EnableAutostart(bool enabled) {
        g_Enabled.store(enabled);
        if (enabled) Logging.Log("[coop] autostart: pressing A until the game loads");
    }
}
