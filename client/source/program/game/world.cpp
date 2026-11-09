#include "world.hpp"

#include <atomic>
#include <cmath>

#include <lib.hpp>

#include "../net/client.hpp"
#include "gdt.hpp"
#include "player.hpp"

namespace coop::game {

    namespace {
        gdt::Handle g_TimeH = gdt::InvalidHandle;
        gdt::Handle g_DaysH = gdt::InvalidHandle;
        s64 g_LastSendMs = 0;
        s64 g_LastHandleMs = 0;

        /* Written by the network thread, read by the game thread. */
        std::atomic<u64> g_PendingBits{0};
        std::atomic<bool> g_Pending{false};

        u64 Pack(const WorldState& s) {
            u32 t;
            __builtin_memcpy(&t, &s.time, 4);
            return (static_cast<u64>(static_cast<u32>(s.days)) << 32) | t;
        }
        WorldState Unpack(u64 v) {
            WorldState s;
            u32 t = static_cast<u32>(v);
            __builtin_memcpy(&s.time, &t, 4);
            s.days = static_cast<s32>(v >> 32);
            return s;
        }

        bool RefreshHandles(s64 now) {
            if (!gdt::Ready()) return false;
            /* Handles change when the game reloads its flags; refresh every few seconds. */
            if (g_TimeH == gdt::InvalidHandle || now - g_LastHandleMs > 5000) {
                g_TimeH = gdt::F32Handle("WM_Time");
                g_DaysH = gdt::S32Handle("WM_NumberOfDays");
                g_LastHandleMs = now;
            }
            return g_TimeH != gdt::InvalidHandle;
        }

        /* Shortest signed difference between two times of day, in degrees. */
        float TimeDiff(float a, float b) {
            float d = std::fmod(a - b + 540.0f, 360.0f) - 180.0f;
            return d;
        }
    }

    void OnHostWorldState(const WorldState& state) {
        g_PendingBits.store(Pack(state));
        g_Pending.store(true);
    }

    void WorldTick(s64 now) {
        if (!net::IsConnected()) return;
        float pos[3];
        if (!GetLocalPlayerPos(pos)) return; /* only in the world, not menus/loading */
        if (!RefreshHandles(now)) return;

        bool host = net::MyId() == net::HostId();
        if (host) {
            if (now - g_LastSendMs < 1000) return;
            g_LastSendMs = now;
            WorldState s{};
            if (!gdt::GetF32(g_TimeH, &s.time)) return;
            gdt::GetS32(g_DaysH, &s.days);
            net::SendWorldState(s.time, s.days);
            static s64 last_log = 0;
            if (now - last_log > 30000) {
                last_log = now;
                Logging.Log("[coop] world: hosting time %.1f (%02d:%02d), day %d", s.time,
                            static_cast<int>(s.time / 15.0f), static_cast<int>(std::fmod(s.time, 15.0f) * 4.0f), s.days);
            }
            return;
        }

        if (!g_Pending.exchange(false)) return;
        WorldState want = Unpack(g_PendingBits.load());
        float have = 0;
        if (!gdt::GetF32(g_TimeH, &have)) return;
        /* 2.5 degrees = 10 in-game minutes. Small drift is left alone so the
           sky doesn't stutter; anything bigger snaps to the host. */
        float diff = TimeDiff(want.time, have);
        if (std::fabs(diff) > 2.5f) {
            gdt::SetF32(g_TimeH, want.time);
            Logging.Log("[coop] world: synced time to host (%.1f -> %.1f, %02d:%02d)", have, want.time,
                        static_cast<int>(want.time / 15.0f), static_cast<int>(std::fmod(want.time, 15.0f) * 4.0f));
        }
        s32 days = 0;
        if (gdt::GetS32(g_DaysH, &days) && days != want.days) gdt::SetS32(g_DaysH, want.days);
    }
}
