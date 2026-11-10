#include "world.hpp"

#include <atomic>
#include <cmath>

#include <lib.hpp>

#include "../net/client.hpp"
#include "gdt.hpp"
#include "offsets.hpp"
#include "player.hpp"

namespace coop::game {

    namespace {
        gdt::Handle g_TimeH = gdt::InvalidHandle;
        gdt::Handle g_DaysH = gdt::InvalidHandle;
        gdt::Handle g_BloodyDayH = gdt::InvalidHandle;
        gdt::Handle g_BloodyTimerH = gdt::InvalidHandle;
        /* Blood Moon state from the host (network thread writes). */
        std::atomic<u32> g_PendingBloodyDay{0xFF};
        std::atomic<u32> g_PendingBloodyTimer{0};
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
                g_BloodyDayH = gdt::BoolHandle("WM_BloodyDay");
                g_BloodyTimerH = gdt::F32Handle("WM_BloodyMoonTimer");
                g_LastHandleMs = now;
            }
            return g_TimeH != gdt::InvalidHandle;
        }

        /* Weather: written by the network thread (weather | climate << 8 | valid << 16). */
        std::atomic<u32> g_PendingWeather{0};
        int g_MgrsOff = -1; /* offset of world::Manager::mMgrs, found at runtime */
        s64 g_LastWeatherLog = 0;
        s64 g_LastLocalLog = 0;

        uintptr_t WorldMgr() {
            uintptr_t base = exl::util::GetMainModuleInfo().m_Total.m_Start;
            uintptr_t slot = *reinterpret_cast<const uintptr_t*>(base + offsets::Got_WorldMgrInstance);
            return slot ? *reinterpret_cast<const uintptr_t*>(slot) : 0;
        }

        /* mMgrs is a PtrArray of the 9 world jobs (time, sky, .., weather is [3]). */
        uintptr_t WeatherMgr(uintptr_t mgr) {
            if (g_MgrsOff < 0) {
                for (int off = 0x400; off < 0x600; off += 8) {
                    s32 num = *reinterpret_cast<const s32*>(mgr + off);
                    s32 max = *reinterpret_cast<const s32*>(mgr + off + 4);
                    uintptr_t ptrs = *reinterpret_cast<const uintptr_t*>(mgr + off + 8);
                    if (num == 9 && max >= 9 && max < 64 && ptrs && (ptrs & 7) == 0) {
                        g_MgrsOff = off;
                        Logging.Log("[coop] world: world managers at +%x", off);
                        break;
                    }
                }
                if (g_MgrsOff < 0) return 0;
            }
            uintptr_t ptrs = *reinterpret_cast<const uintptr_t*>(mgr + g_MgrsOff + 8);
            return ptrs ? *reinterpret_cast<const uintptr_t*>(ptrs + 3 * 8) : 0;
        }

        /* The weather the world is showing now: forced (events) or the natural roll. */
        bool LocalWeather(u8* weather, u8* climate) {
            uintptr_t mgr = WorldMgr();
            if (!mgr) return false;
            u8 forced = *reinterpret_cast<const u8*>(mgr + offsets::WorldMgr_ForcedWeather);
            uintptr_t wm = WeatherMgr(mgr);
            u8 natural = wm ? *reinterpret_cast<const u8*>(wm + offsets::WeatherMgr_Weather) : 0xFF;
            *weather = forced < 9 ? forced : natural;
            *climate = *reinterpret_cast<const u8*>(mgr + offsets::WorldMgr_CurrentClimate);
            return *weather < 9;
        }

        /* Shortest signed difference between two times of day, in degrees. */
        float TimeDiff(float a, float b) {
            float d = std::fmod(a - b + 540.0f, 360.0f) - 180.0f;
            return d;
        }
    }

    void OnHostWorldState(const WorldState& state) {
        g_PendingBloodyDay.store(state.bloody_day);
        u32 tb;
        __builtin_memcpy(&tb, &state.bloody_timer, 4);
        g_PendingBloodyTimer.store(tb);
        if (state.weather < 9)
            g_PendingWeather.store(state.weather | (static_cast<u32>(state.climate) << 8) | 0x10000);
        else
            g_PendingWeather.store(0);
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
            if (!LocalWeather(&s.weather, &s.climate)) s.weather = 0xFF;
            bool bloody = false;
            s.bloody_day = gdt::GetBool(g_BloodyDayH, &bloody) ? (bloody ? 1 : 0) : 0xFF;
            if (!gdt::GetF32(g_BloodyTimerH, &s.bloody_timer)) s.bloody_day = 0xFF;
            net::SendWorldState(s.time, s.days, s.weather, s.climate, s.bloody_day, s.bloody_timer);
            if (now - g_LastWeatherLog > 30000) {
                g_LastWeatherLog = now;
                Logging.Log("[coop] world: hosting weather %d in climate %d", s.weather, s.climate);
            }
            static s64 last_log = 0;
            if (now - last_log > 30000) {
                last_log = now;
                Logging.Log("[coop] world: hosting time %.1f (%02d:%02d), day %d", s.time,
                            static_cast<int>(s.time / 15.0f), static_cast<int>(std::fmod(s.time, 15.0f) * 4.0f), s.days);
            }
            return;
        }

        /* Weather: the host's, if we're in the same climate (they'd look wrong
           elsewhere: no snow in the desert). Re-applied every tick: a forced
           weather only holds for a few frames. */
        if (now - g_LastLocalLog > 15000) {
            g_LastLocalLog = now;
            u8 w = 0xFF, c = 0xFF;
            LocalWeather(&w, &c);
            Logging.Log("[coop] world: local weather %d, climate %d", w, c);
        }
        u32 pw = g_PendingWeather.load();
        if (pw & 0x10000) {
            u8 want_weather = pw & 0xFF, want_climate = (pw >> 8) & 0xFF;
            u8 my_weather = 0xFF, my_climate = 0xFF;
            uintptr_t mgr = WorldMgr();
            if (mgr && LocalArea() == OverworldArea() && LocalWeather(&my_weather, &my_climate) &&
                my_climate == want_climate && want_weather < 9) {
                uintptr_t base = exl::util::GetMainModuleInfo().m_Total.m_Start;
                reinterpret_cast<void (*)(uintptr_t, u32, bool, bool, bool)>(base + offsets::WorldMgr_SetWeatherType)(
                    mgr, want_weather, true, false, false);
                if (now - g_LastWeatherLog > 30000) {
                    g_LastWeatherLog = now;
                    Logging.Log("[coop] world: following the host's weather %d (climate %d)", want_weather, want_climate);
                }
            }
        }

        /* Blood Moon: follow the host's timer and bloody day, so it rises the
           same night for everyone (time is shared too). */
        u32 want_bloody = g_PendingBloodyDay.load();
        if (want_bloody <= 1) {
            u32 tb = g_PendingBloodyTimer.load();
            float want_timer;
            __builtin_memcpy(&want_timer, &tb, 4);
            bool have = false;
            float have_timer = 0;
            gdt::GetBool(g_BloodyDayH, &have);
            gdt::GetF32(g_BloodyTimerH, &have_timer);
            if (have != (want_bloody == 1)) {
                gdt::SetBool(g_BloodyDayH, want_bloody == 1);
                Logging.Log("[coop] world: Blood Moon tonight: %s (the host's)", want_bloody == 1 ? "yes" : "no");
            }
            if (std::fabs(have_timer - want_timer) > 360.0f) gdt::SetF32(g_BloodyTimerH, want_timer);
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
