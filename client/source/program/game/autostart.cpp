#include "autostart.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>

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
        std::atomic<u64> g_PressMask{0};
        std::atomic<s64> g_PressUntil{0};
        std::atomic<s32> g_StickX{0}, g_StickY{0};
        std::atomic<s64> g_StickUntil{0};

        /* The left stick click is held back from the game while it's down: a
           tap (shorter than TapMs) is passed on when it's released, so Link
           still crouches; a long hold is ours (warp) and the game never sees it. */
        constexpr u64 LStick = 1ull << 4;
        constexpr s64 TapMs = 350;
        std::atomic<int> g_LsFrom{-1};
        std::atomic<s64> g_LsSince{0};
        std::atomic<s64> g_LsTapUntil{0};
        std::atomic<u32> g_LsTaps{0};

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
            if (g_StickUntil.load(std::memory_order_relaxed) > NowMs()) {
                for (int i = 0; i < count; i++) {
                    out[i].stick_l[0] = g_StickX.load();
                    out[i].stick_l[1] = g_StickY.load();
                }
            }
            u64 mask = g_PressMask.load(std::memory_order_relaxed);
            if (mask) {
                if (NowMs() < g_PressUntil.load()) {
                    for (int i = 0; i < count; i++) out[i].buttons |= mask;
                } else {
                    g_PressMask.store(0);
                }
            }
            if (count > 0) {
                s64 now = NowMs();
                bool down = out[0].buttons & LStick;
                int from = g_LsFrom.load(std::memory_order_relaxed);
                if (down && from < 0) {
                    g_LsSince.store(now);
                    g_LsFrom.store(which);
                } else if (from == which && !down) {
                    if (now - g_LsSince.load() < TapMs) {
                        g_LsTapUntil.store(now + 120);
                        g_LsTaps.fetch_add(1, std::memory_order_relaxed);
                    }
                    g_LsFrom.store(-1);
                }
                if (g_LsFrom.load(std::memory_order_relaxed) >= 0)
                    for (int i = 0; i < count; i++) out[i].buttons &= ~LStick;
                if (now < g_LsTapUntil.load(std::memory_order_relaxed))
                    out[0].buttons |= LStick;
            }
            if (ShouldPressA()) {
                g_Presses.fetch_add(1, std::memory_order_relaxed);
                for (int i = 0; i < count; i++) out[i].buttons |= 1; /* A */
            }
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

    u32 StickClickTaps() { return g_LsTaps.load(std::memory_order_relaxed); }

    s64 StickClickHeldMs() {
        if (g_LsFrom.load(std::memory_order_relaxed) < 0) return 0;
        return NowMs() - g_LsSince.load(std::memory_order_relaxed);
    }

    void PressButtons(u64 mask, int ms) {
        g_PressUntil.store(NowMs() + ms);
        g_PressMask.store(mask);
    }

    void HoldStick(s32 x, s32 y, int ms) {
        g_StickX.store(x);
        g_StickY.store(y);
        g_StickUntil.store(NowMs() + ms);
    }

    void EnableAutostart(bool enabled) {
        g_Enabled.store(enabled);
        if (enabled) Logging.Log("[coop] autostart: pressing A until the game loads");
    }

    namespace {
        char g_Keys[256];
        char* g_KeyNext = nullptr;
        s64 g_KeyAt = 0;

        u64 ButtonMask(const char* n) {
            struct { const char* name; int bit; } const map[] = {
                {"A", 0}, {"B", 1}, {"X", 2}, {"Y", 3}, {"L", 6}, {"R", 7}, {"ZL", 8}, {"ZR", 9}, {"Plus", 10},
                {"Minus", 11}, {"LS", 4}, {"RS", 5}, {"Left", 12}, {"Up", 13}, {"Right", 14}, {"Down", 15},
            };
            for (auto& m : map)
                if (std::strcmp(m.name, n) == 0) return 1ull << m.bit;
            return 0;
        }
    }

    void SetKeyScript(const char* script) {
        std::strncpy(g_Keys, script ? script : "", sizeof(g_Keys) - 1);
        g_KeyNext = g_Keys[0] ? g_Keys : nullptr;
    }

    void KeyScriptTick(s64 now) {
        if (!g_KeyNext) return;
        if (g_KeyAt == 0) {
            float pos[3];
            if (!GetLocalPlayerPos(pos)) return;
            g_KeyAt = now + 5000; /* settle first */
        }
        if (now < g_KeyAt) return;
        char token[32] = {};
        char* comma = std::strchr(g_KeyNext, ',');
        size_t len = comma ? static_cast<size_t>(comma - g_KeyNext) : std::strlen(g_KeyNext);
        std::memcpy(token, g_KeyNext, len < 31 ? len : 31);
        g_KeyNext = comma ? comma + 1 : nullptr;
        int ms = 150;
        if (char* colon = std::strchr(token, ':')) {
            *colon = '\0';
            ms = std::atoi(colon + 1);
        }
        /* "LS+RS" presses both at once. */
        u64 mask = 0;
        bool all_known = true;
        for (char* part = token; part && *part;) {
            char* plus = std::strchr(part, '+');
            if (plus) *plus = '\0';
            u64 m = ButtonMask(part);
            all_known &= m != 0;
            mask |= m;
            if (plus) *plus = '+';
            part = plus ? plus + 1 : nullptr;
        }
        if (!all_known) mask = 0;
        if (mask) PressButtons(mask, ms);
        /* Left stick: SL/SR/SU/SD (left, right, up, down). Anything else just waits. */
        struct { const char* name; s32 x, y; } const sticks[] = {
            {"SL", -0x7FFF, 0}, {"SR", 0x7FFF, 0}, {"SU", 0, 0x7FFF}, {"SD", 0, -0x7FFF},
        };
        bool stick = false;
        for (auto& st : sticks)
            if (std::strcmp(token, st.name) == 0) {
                HoldStick(st.x, st.y, ms);
                stick = true;
            }
        Logging.Log("[coop] keys: %s (%d ms)%s, stick taps passed on so far: %u", token, ms, mask || stick ? "" : " (wait)",
                    StickClickTaps());
        g_KeyAt = now + ms + 1200;
    }
}
