#include "status.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

#include <lib.hpp>
#include <nn/os.hpp>

#include "../net/client.hpp"
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

        struct Watch {
            u8 id;
            bool used;
            bool low;
            s64 warned_at;
            /* Where they were last time, to notice travel. */
            bool has_last;
            u32 last_area;
            float last_pos[3];
        };
        Watch g_Watch[net::MaxPeers];

        Watch& For(u8 id) {
            for (auto& w : g_Watch)
                if (w.used && w.id == id) return w;
            for (auto& w : g_Watch)
                if (!w.used) {
                    w = {id, true, false, -RepeatMs, false, 0, {0, 0, 0}};
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

        void Distance(const float* a, const float* b, char* out, size_t cap) {
            float dx = a[0] - b[0], dz = a[2] - b[2];
            float d = std::sqrt(dx * dx + dz * dz);
            if (d >= 1000.0f) std::snprintf(out, cap, "%.1f km", d / 1000.0f);
            else std::snprintf(out, cap, "%.0f m", d);
        }

        /* A partner fast-traveled, warped, or went into or out of a shrine:
           say so, with how to follow. Area 0 is a loading screen: skipped. */
        void Travel(Watch& w, const net::Peer& p, const float* me, u32 my_area) {
            u32 area = p.state.map_area;
            if (area == 0) return;
            const float* pos = p.state.pos;
            if (w.has_last) {
                u32 ow = OverworldArea();
                char dist[24];
                Distance(pos, me, dist, sizeof(dist));
                float jx = pos[0] - w.last_pos[0], jz = pos[2] - w.last_pos[2];
                bool jumped = jx * jx + jz * jz > 300.0f * 300.0f;
                float mx = pos[0] - me[0], mz = pos[2] - me[2];
                bool by_me = mx * mx + mz * mz < 30.0f * 30.0f && area == my_area;
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
                    if (by_me) notify::Show("%s warped to you", p.name);
                    else if (area == my_area) notify::Show("%s traveled: %s away (hold L3 to follow)", p.name, dist);
                    else notify::Show("%s traveled", p.name);
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

        for (auto& p : peers) {
            if (!p.present || !p.has_state || tick_now - static_cast<s64>(p.state_tick) > stale) continue;
            u16 life = p.state.health, max = p.state.max_hp;
            if (max == 0) continue;
            Watch& w = For(p.id);
            Travel(w, p, me, area);
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
            notify::Show("%s is low on hearts: %s left%s", p.name, hearts, where);
        }
    }
}
