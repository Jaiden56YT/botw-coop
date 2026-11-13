#include "status.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <lib.hpp>
#include <nn/os.hpp>

#include "../net/client.hpp"
#include "anim.hpp"
#include "autostart.hpp"
#include "notify.hpp"
#include "player.hpp"

namespace coop::game::status {

    namespace {
        /* Low: at most a quarter of their hearts, or 2 hearts or fewer (but
           not someone who's simply full at 2 hearts). Fine again above 40%
           and 2 hearts, so hovering at the line doesn't repeat it. */
        bool IsLow(u16 life, u16 max) { return life > 0 && (life * 4 <= max || (life <= 8 && life < max)); }
        bool IsFine(u16 life, u16 max) { return life * 10 > max * 4 && (life > 8 || life >= max); }

        constexpr s64 RepeatMs = 30000; /* at most one warning per partner this often */
        constexpr s64 StaleMs = 3000;
        constexpr s64 TravelSettleMs = 2500; /* a warp's hops are about a second apart */

        struct Watch {
            u8 id;
            bool used;
            bool low;
            s64 warned_at;
            /* Where they were last time, to notice travel. */
            bool has_last;
            u32 last_area;
            float last_pos[3];
            s64 travel_at; /* a jump was seen: announce it at this time, unless they jump again */
            bool apart;    /* they've been out of sight (far, or in another area) since we last met */
            s64 met_at;
        };
        constexpr float ApartDist = 60.0f, MeetDist = 20.0f;
        constexpr s64 MeetRepeatMs = 60000;
        Watch g_Watch[net::MaxPeers];

        Watch& For(u8 id) {
            for (auto& w : g_Watch)
                if (w.used && w.id == id) return w;
            for (auto& w : g_Watch)
                if (!w.used) {
                    w = {id, true, false, -RepeatMs, false, 0, {0, 0, 0}, 0, true, -MeetRepeatMs};
                    return w;
                }
            return g_Watch[0];
        }

        /* "2.5 hearts", "1 heart", "3.25 hearts". */
        void Hearts(u16 quarters, char* out, size_t cap) {
            u16 whole = quarters / 4, part = quarters % 4;
            static const char* const frac[] = {"", ".25", ".5", ".75"};
            std::snprintf(out, cap, "%u%s heart%s", whole, frac[part], quarters == 4 ? "" : "s");
        }

        s64 g_NextAt = 0;

        constexpr s64 CallRepeatMs = 8000; /* whistling again and again is one call */
        std::atomic<bool> g_Whistled{false};
        s64 g_CalledAt = -CallRepeatMs;
        struct Call {
            std::atomic<bool> pending{false};
            float pos[3];
        };
        Call g_Calls[256]; /* by player id */

        const char* Compass(float dx, float dz) {
            /* +x is east, +z is south. */
            static const char* const names[] = {"north", "north-east", "east", "south-east", "south", "south-west", "west", "north-west"};
            float a = std::atan2(dx, -dz); /* 0 = north, clockwise */
            int i = static_cast<int>(std::floor(a / 0.7853982f + 0.5f));
            return names[((i % 8) + 8) % 8];
        }
        u32 g_SeenTaps = 0;

        void Distance(const float* a, const float* b, char* out, size_t cap) {
            float dx = a[0] - b[0], dz = a[2] - b[2];
            float d = std::sqrt(dx * dx + dz * dz);
            if (d >= 1000.0f) std::snprintf(out, cap, "%.1f km", d / 1000.0f);
            else std::snprintf(out, cap, "%.0f m", d);
        }

        /* A partner fast-traveled, warped, or went into or out of a shrine:
           say so, with how to follow. Area 0 is a loading screen: skipped. */
        void Travel(Watch& w, const net::Peer& p, const float* me, u32 my_area, s64 now) {
            u32 area = p.state.map_area;
            if (area == 0) return;
            const float* pos = p.state.pos;
            if (w.has_last) {
                u32 ow = OverworldArea();
                char dist[24];
                Distance(pos, me, dist, sizeof(dist));
                float jx = pos[0] - w.last_pos[0], jz = pos[2] - w.last_pos[2];
                bool jumped = jx * jx + jz * jz > 300.0f * 300.0f;
                /* Beside me, height included: a partner passing overhead in a
                   warp's sky hops hasn't arrived. */
                float mx = pos[0] - me[0], my = pos[1] - me[1], mz = pos[2] - me[2];
                bool by_me = mx * mx + my * my + mz * mz < 30.0f * 30.0f && area == my_area;
                float lx = w.last_pos[0] - me[0], ly = w.last_pos[1] - me[1], lz = w.last_pos[2] - me[2];
                if (area != w.last_area) {
                    if (w.last_area == ow && area != ow) notify::Show("%s went into a shrine", p.name);
                    else if (area == ow && w.last_area != ow) {
                        if (my_area == ow) notify::Show("%s is back outside, %s away", p.name, dist);
                        else notify::Show("%s is back outside", p.name);
                    }
                } else if (by_me && area == my_area && lx * lx + ly * ly + lz * lz > 100.0f * 100.0f) {
                    /* A warp over 250 m comes in hops, none of them a "jump": what
                       shows it is being far away one moment and beside you the next. */
                    notify::Show("%s warped to you", p.name);
                } else if (jumped) {
                    /* A warp comes in hops, one jump each: wait until they
                       stay put, then say where they ended up, once. */
                    if (by_me) notify::Show("%s warped to you", p.name);
                    else w.travel_at = now + TravelSettleMs;
                }
                if (w.travel_at && now >= w.travel_at) {
                    w.travel_at = 0;
                    if (by_me) {
                    } else if (area == my_area) {
                        notify::Show("%.10s is %s away (hold L3 to follow)", p.name, dist);
                    } else {
                        notify::Show("%s traveled", p.name);
                    }
                }
            }
            w.has_last = true;
            w.last_area = area;
            std::memcpy(w.last_pos, pos, sizeof(w.last_pos));
        }
    }

    void Tick(s64 now) {
        if (now < g_NextAt) return;
        g_NextAt = now + 250;
        float me[3];
        if (!GetLocalPlayerPos(me)) return;
        net::Peer peers[net::MaxPeers];
        net::GetPeers(peers);
        s64 tick_now = nn::os::GetSystemTick().GetInt64Value();
        s64 stale = static_cast<s64>(nn::os::GetSystemTickFrequency()) * StaleMs / 1000;
        u32 area = LocalArea();

        for (auto& w : g_Watch) {
            bool here = false;
            for (auto& p : peers) here |= p.present && p.id == w.id;
            if (!here) w.used = false;
        }

        /* Our whistle goes out; partners' whistles are shown. */
        if (g_Whistled.exchange(false) && now - g_CalledAt >= CallRepeatMs) {
            g_CalledAt = now;
            net::SendCall(me);
        }
        for (auto& p : peers) {
            if (!p.present || !g_Calls[p.id].pending.exchange(false)) continue;
            const float* at = g_Calls[p.id].pos;
            if (p.has_state && p.state.map_area != area) {
                notify::Show("%.10s whistles (%s)", p.name, area == OverworldArea() ? "in a shrine" : "outside");
                continue;
            }
            float dx = at[0] - me[0], dz = at[2] - me[2];
            float d = std::sqrt(dx * dx + dz * dz);
            if (d < 15.0f) notify::Show("%.10s whistles, right by you", p.name);
            else if (d >= 1000.0f) notify::Show("%.10s whistles: %.1f km, %s", p.name, d / 1000.0f, Compass(dx, dz));
            else notify::Show("%.10s whistles: %.0f m, %s", p.name, d, Compass(dx, dz));
        }

        /* Two taps on the left stick click: how every partner is doing. */
        u32 taps = StickClickDoubleTaps();
        if (taps != g_SeenTaps) {
            g_SeenTaps = taps;
            int shown = 0;
            for (auto& p : peers) {
                if (!p.present) continue;
                shown++;
                if (!p.has_state || tick_now - static_cast<s64>(p.state_tick) > stale || p.state.max_hp == 0) {
                    notify::Show("%.10s: no word (loading?)", p.name);
                    continue;
                }
                char hearts[24], where[32];
                Hearts(p.state.health, hearts, sizeof(hearts));
                if (p.state.map_area != area) {
                    std::snprintf(where, sizeof(where), area == OverworldArea() ? "in a shrine" : "outside");
                } else {
                    char dist[16];
                    Distance(p.state.pos, me, dist, sizeof(dist));
                    std::snprintf(where, sizeof(where), "%s away", dist);
                }
                bool down = (p.state.anim_hash & 0xff) == anim::Down;
                notify::Show("%.10s: %s%s of %u, %s", p.name, down ? "DOWN, " : "", hearts, (unsigned)(p.state.max_hp / 4), where);
            }
            if (!shown) notify::Show("Co-op: no partner is connected");
        }

        for (auto& p : peers) {
            if (!p.present || !p.has_state || tick_now - static_cast<s64>(p.state_tick) > stale) continue;
            u16 life = p.state.health, max = p.state.max_hp;
            if (max == 0) continue;
            Watch& w = For(p.id);
            Travel(w, p, me, area, now);
            /* Meeting up again: who it is and how they're doing. */
            {
                float dx = p.state.pos[0] - me[0], dy = p.state.pos[1] - me[1], dz = p.state.pos[2] - me[2];
                float d2 = dx * dx + dy * dy + dz * dz;
                if (p.state.map_area != area || d2 > ApartDist * ApartDist) {
                    w.apart = true;
                } else if (w.apart && d2 < MeetDist * MeetDist) {
                    w.apart = false;
                    if (now - w.met_at >= MeetRepeatMs) {
                        w.met_at = now;
                        char hearts[24];
                        Hearts(life, hearts, sizeof(hearts));
                        notify::Show("%.10s is with you (%s of %u)", p.name, hearts, (unsigned)(max / 4));
                    }
                }
            }
            if (w.low) {
                if (IsFine(life, max)) w.low = false;
                continue;
            }
            if (!IsLow(life, max)) continue;
            w.low = true;
            if (now - w.warned_at < RepeatMs) continue;
            w.warned_at = now;

            char hearts[24];
            Hearts(life, hearts, sizeof(hearts));
            char where[48] = "";
            if (p.state.map_area != area) {
                std::snprintf(where, sizeof(where), area == OverworldArea() ? " (in a shrine)" : " (outside)");
            } else {
                float dx = p.state.pos[0] - me[0], dz = p.state.pos[2] - me[2];
                float d = std::sqrt(dx * dx + dz * dz);
                if (d >= 1000.0f) std::snprintf(where, sizeof(where), ", %.1f km away", d / 1000.0f);
                else if (d >= 40.0f) std::snprintf(where, sizeof(where), ", %.0f m away", d);
            }
            Logging.Log("[coop] status: %s is low (%u/%u)", p.name, life, max);
            notify::Show("%.10s is low (%s)%s", p.name, hearts, where);
        }
    }

    void OnLocalWhistle() { g_Whistled.store(true); }

    void OnPartnerCall(u8 from, const float* pos) {
        std::memcpy(g_Calls[from].pos, pos, sizeof(g_Calls[from].pos));
        g_Calls[from].pending.store(true);
    }
}
