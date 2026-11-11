#include "items.hpp"

#include <atomic>
#include <cstring>

#include <lib.hpp>

#include "../net/client.hpp"
#include "gdt.hpp"
#include "notify.hpp"
#include "ghost.hpp"
#include "offsets.hpp"
#include "player.hpp"
#include "weapons.hpp"

namespace coop::game::items {

    namespace {
        uintptr_t Base() { return exl::util::GetMainModuleInfo().m_Total.m_Start; }
        template<typename T> T Read(uintptr_t addr) { return *reinterpret_cast<const T*>(addr); }
        using CsFn = void (*)(uintptr_t);
        using CsTryFn = bool (*)(uintptr_t);

        constexpr s64 ScanMs = 200;
        constexpr s64 DropWindowMs = 3000; /* a drop shows up this soon after the menu closes */
        constexpr float NearLink = 2.5f;   /* ...and this close to Link */
        constexpr float NearLinkWeapon = 4.0f;
        constexpr float GiveRange = 3.0f;  /* settled this close to a partner = theirs */

        /* Items that can be dropped from the inventory. Dishes and elixirs
           can't (and would lose their effects), arrows can't either. */
        bool Droppable(const char* name) {
            if (std::strncmp(name, "Item_Cook_", 10) == 0) return false;
            /* Not a thing you carry: it turned up beside Link on its own and was
               taken for a drop (seen once, after the game had been frozen). */
            if (std::strcmp(name, "Item_Conductor") == 0) return false;
            return std::strncmp(name, "Item_", 5) == 0 || std::strncmp(name, "Animal_Insect_", 14) == 0 ||
                   std::strcmp(name, "BeeHome") == 0 || std::strcmp(name, "Obj_FireWoodBundle") == 0;
        }

        /* The pause menu stops the world, and with it the clock (WM_Time); our
           ticks keep running. A clock that stands still isn't always the menu,
           though: the game pins it in play too (a new game until Link has
           left the Shrine of Resurrection, 11:00 until the first tower is
           activated, a forced hour in some places). So when the clock stops
           we nudge it by a hair. A running world writes its own value back
           over ours within a frame (TimeMgr::calc_ reads the flag, applies
           its rules and writes it back); a paused one leaves ours alone.
           While an event runs (talking, a cutscene) the game keeps whatever
           it reads, so those count as paused too.
           The clock also stands still for a moment now and then in play, so
           only a stop of MenuMinMs or more counts. */
        constexpr s64 MenuMinMs = 1200;
        constexpr s64 ProbeEveryMs = 300;
        constexpr s64 ProbeStillMs = 150;   /* the clock hasn't moved for this long (it only moves once a frame) */
        constexpr s64 ProbeSettleMs = 350;  /* a write shows up in what we read within this (a frame, measured), if nothing overwrites it */
        constexpr float ProbeStep = 0.002f; /* half a second of game time */
        gdt::Handle g_TimeH = gdt::InvalidHandle;
        s64 g_HandleAt = 0;
        float g_LastTime = -1;
        s64 g_TimeMovedAt = 0;    /* last moment the world was seen running */
        s64 g_PausedAt = -100000; /* last moment we were in the menu */
        std::atomic<bool> g_OurWrite{false};
        float g_SyncValue = 0;
        /* The nudge: we wrote g_ProbeValue over g_ProbeOld at g_ProbeAt. */
        bool g_Probing = false;
        bool g_ProbeSeen = false; /* our value has shown up in what we read */
        float g_ProbeValue = 0, g_ProbeOld = 0;
        s64 g_ProbeAt = 0;
        s64 g_NextProbe = 0;
        s64 g_StillSince = 0;
        bool g_ProbeDebug = false; /* test_research = 5: log what the clock reads after each nudge */
        int g_ProbeLogLeft = 0;

        void WatchPause(s64 now) {
            if (!gdt::Ready()) return;
            if (g_TimeH == gdt::InvalidHandle || now - g_HandleAt > 5000) {
                g_TimeH = gdt::F32Handle("WM_Time");
                g_HandleAt = now;
            }
            float t;
            if (!gdt::GetF32(g_TimeH, &t)) return;
            if (g_ProbeLogLeft > 0) {
                g_ProbeLogLeft--;
                Logging.Log("[coop] probe: read %.5f, %lld ms after", t, now - g_ProbeAt);
            }
            /* Our own time sync just wrote the clock: that write tells us the
               same as a nudge does. */
            bool synced = g_OurWrite.exchange(false);
            if (synced) {
                g_ProbeOld = t; /* what it reads now: the old time, or already ours */
                g_ProbeValue = g_SyncValue;
                g_ProbeAt = now;
                g_ProbeSeen = false;
                g_Probing = true;
            }
            if (t != g_LastTime && !g_Probing) g_StillSince = now;
            bool ran = false;
            if (synced) {
                /* wait for the next reads */
            } else if (g_Probing) {
                if (t == g_ProbeValue) {
                    g_ProbeSeen = true; /* still ours: nothing is writing the clock */
                } else if (g_ProbeSeen || t != g_ProbeOld || now - g_ProbeAt > ProbeSettleMs) {
                    /* Ours was there and is gone, or the clock moved on, or
                       ours never showed: the world wrote over it. */
                    ran = true;
                    g_Probing = false;
                    g_StillSince = now;
                }
            } else if (t != g_LastTime) {
                ran = true;
            } else if (now - g_StillSince >= ProbeStillMs && now >= g_NextProbe) {
                g_NextProbe = now + ProbeEveryMs;
                if (gdt::SetF32(g_TimeH, t + ProbeStep)) {
                    g_ProbeOld = t;
                    g_ProbeValue = t + ProbeStep;
                    g_ProbeAt = now;
                    g_ProbeSeen = false;
                    g_Probing = true;
                    if (g_ProbeDebug) {
                        float back = -1;
                        gdt::GetF32(g_TimeH, &back);
                        Logging.Log("[coop] probe: wrote %.5f over %.5f, reads back %.5f", g_ProbeValue, t, back);
                        g_ProbeLogLeft = 8;
                    }
                }
            }
            if (!g_Probing) g_LastTime = t;
            if (!ran) {
                if (now - g_TimeMovedAt >= MenuMinMs) g_PausedAt = now;
                return;
            }
            if (now - g_TimeMovedAt >= MenuMinMs && g_TimeMovedAt > 0)
                Logging.Log("[coop] items: back from the menu (%.1f s)", (now - g_TimeMovedAt) / 1000.0f);
            g_TimeMovedAt = now;
        }

        /* Item actors near Link in the last scan (to spot new ones). */
        constexpr int MaxNear = 96;
        uintptr_t g_Prev[MaxNear];
        int g_PrevCount = 0;
        bool g_HavePrev = false;
        u32 g_PrevArea = 0;
        float g_PrevMe[3];
        s64 g_QuietUntil = 0;

        /* Items we dropped, until they're given away or picked up again. */
        struct Dropped {
            uintptr_t actor;
            char name[32];
            s64 since, still_since;
            float pos[3];
            bool released;
        };
        Dropped g_Dropped[16];
        s64 g_NextScan = 0;
        s64 g_DebugAt = 0;

        char g_Test[64];
        s64 g_TestAt = 0;
        bool g_TestDone = false;

        bool InPrev(uintptr_t a) {
            for (int i = 0; i < g_PrevCount; i++)
                if (g_Prev[i] == a) return true;
            return false;
        }

        void Track(uintptr_t a, const char* name, s64 now) {
            for (auto& d : g_Dropped)
                if (d.actor == a) return;
            for (auto& d : g_Dropped) {
                if (d.actor) continue;
                d.actor = a;
                std::strncpy(d.name, name, sizeof(d.name) - 1);
                d.name[sizeof(d.name) - 1] = '\0';
                d.since = d.still_since = now;
                d.released = false;
                const float* m = reinterpret_cast<const float*>(a + offsets::Actor_Mtx);
                d.pos[0] = m[3], d.pos[1] = m[7], d.pos[2] = m[11];
                Logging.Log("[coop] items: you dropped %s", name);
                return;
            }
        }

        void Scan(s64 now) {
            float me[3];
            if (!GetLocalPlayerPos(me)) {
                g_HavePrev = false;
                return;
            }
            u32 area = LocalArea();
            /* A new area, or a jump (fast travel or a warp to a partner, which
               needn't change the area): everything around Link is new. */
            float jx = me[0] - g_PrevMe[0], jz = me[2] - g_PrevMe[2];
            bool jumped = g_HavePrev && jx * jx + jz * jz > 40.0f * 40.0f;
            std::memcpy(g_PrevMe, me, sizeof(me));
            if (area != g_PrevArea || jumped) {
                g_PrevArea = area;
                g_HavePrev = false;
                for (auto& d : g_Dropped) d.actor = 0;
            }
            /* After a load the world streams in around Link: nothing new is a drop yet. */
            if (!g_HavePrev) g_QuietUntil = now + 10000;

            uintptr_t found[MaxNear];
            const char* names[MaxNear];
            int n = 0;
            uintptr_t slot = Read<uintptr_t>(Base() + offsets::Got_ActorCreatorInstance);
            uintptr_t creator = slot ? Read<uintptr_t>(slot) : 0;
            if (!creator) return;
            uintptr_t cs = creator + offsets::ActorCreator_ActorListCS;
            if (!reinterpret_cast<CsTryFn>(Base() + offsets::CriticalSection_TryLock)(cs)) return;
            uintptr_t head = creator + offsets::ActorCreator_ActorList;
            s32 node_off = Read<s32>(creator + offsets::ActorCreator_ActorListOffset);
            int guard = 0;
            for (uintptr_t node = Read<uintptr_t>(head + 8); node != head && node != 0 && guard < 20000; guard++) {
                uintptr_t a = node - node_off;
                node = Read<uintptr_t>(node + 8);
                const char* name = Read<const char*>(a + offsets::BaseProc_NameStr);
                if (!name || n >= MaxNear) continue;
                bool weapon = std::strncmp(name, "Weapon_", 7) == 0;
                if (!weapon && !Droppable(name)) continue;
                const float* m = reinterpret_cast<const float*>(a + offsets::Actor_Mtx);
                float dx = m[3] - me[0], dy = m[7] - me[1], dz = m[11] - me[2];
                if (dx * dx + dy * dy + dz * dz > 12.0f * 12.0f) continue;
                found[n] = a;
                names[n] = name;
                n++;
            }
            reinterpret_cast<CsFn>(Base() + offsets::CriticalSection_Unlock)(cs);

            /* New since the last scan, right by Link, just after a pause: dropped from the menu. */
            bool after_pause = now - g_PausedAt < DropWindowMs;
            if (g_HavePrev && after_pause && now > g_QuietUntil) {
                for (int i = 0; i < n; i++) {
                    if (InPrev(found[i]) || weapons::IsGiftActor(found[i])) continue;
                    const float* m = reinterpret_cast<const float*>(found[i] + offsets::Actor_Mtx);
                    float dx = m[3] - me[0], dz = m[11] - me[2];
                    bool weapon = std::strncmp(names[i], "Weapon_", 7) == 0;
                    /* A long weapon lands farther out (a claymore: 2.7 m). */
                    float near = weapon ? NearLinkWeapon : NearLink;
                    if (dx * dx + dz * dz > near * near) continue;
                    if (weapon) {
                        weapons::TrackDropped(found[i], names[i], now);
                    } else {
                        Track(found[i], names[i], now);
                    }
                }
            }
            std::memcpy(g_Prev, found, n * sizeof(uintptr_t));
            g_PrevCount = n;
            g_HavePrev = true;

            /* Dropped items that have come to rest next to a partner go to them. */
            net::Peer peers[net::MaxPeers];
            bool have_peers = false;
            for (auto& d : g_Dropped) {
                if (!d.actor) continue;
                bool alive = false;
                for (int i = 0; i < n; i++) alive |= found[i] == d.actor;
                if (!alive || now - d.since > 120000) { /* put away, picked up again, or left lying */
                    d.actor = 0;
                    continue;
                }
                const float* m = reinterpret_cast<const float*>(d.actor + offsets::Actor_Mtx);
                float mx = m[3] - d.pos[0], my = m[7] - d.pos[1], mz = m[11] - d.pos[2];
                if (mx * mx + my * my + mz * mz > 0.03f * 0.03f) d.still_since = now;
                d.pos[0] = m[3], d.pos[1] = m[7], d.pos[2] = m[11];
                if (now >= g_DebugAt && now - d.since < 10000) {
                    g_DebugAt = now + 1000;
                    Logging.Log("[coop] items: %s at %.2f m from Link, %.2f m above his feet, still for %lld ms", d.name,
                                __builtin_sqrtf((m[3] - me[0]) * (m[3] - me[0]) + (m[11] - me[2]) * (m[11] - me[2])),
                                m[7] - me[1], static_cast<long long>(now - d.still_since));
                }
                /* Held up in Link's hands until he puts it down or throws it.
                   (Out of the menu, items are held first; the menu can't drop them.) */
                float hx = m[3] - me[0], hz = m[11] - me[2];
                if (!d.released && (m[7] - me[1] < 0.6f || hx * hx + hz * hz > 0.6f * 0.6f)) {
                    d.released = true;
                    Logging.Log("[coop] items: you put down %s", d.name);
                }
                if (!d.released) continue;
                bool near_any = false;
                if (!have_peers) {
                    net::GetPeers(peers);
                    have_peers = true;
                }
                for (auto& p : peers) {
                    if (!p.present || !p.has_state || p.state.map_area != area) continue;
                    float dx = m[3] - p.state.pos[0], dy = m[7] - p.state.pos[1], dz = m[11] - p.state.pos[2];
                    if (dx * dx + dz * dz > GiveRange * GiveRange || dy * dy > 4.0f * 4.0f) continue;
                    near_any = true;
                    net::SendGift(p.id, d.name);
                    Logging.Log("[coop] items: gave %s to %s", d.name, p.name);
                    notify::Gift(false, p.name, d.name);
                    uintptr_t vt = Read<uintptr_t>(d.actor);
                    reinterpret_cast<void (*)(uintptr_t, int)>(Read<uintptr_t>(vt + offsets::ActorVt_Kill))(d.actor, 0);
                    d.actor = 0;
                    break;
                }
                if (!near_any && now - d.still_since > 400 && now - d.still_since < 600)
                    Logging.Log("[coop] items: %s is lying at (%.1f, %.1f, %.1f), no partner within %.0f m", d.name, m[3], m[7],
                                m[11], GiveRange);
            }
        }

        void RunTest(s64 now) {
            if (!g_Test[0] || g_TestDone) return;
            float me[3], rot = 0;
            if (!GetLocalPlayer(me, &rot)) return;
            if (g_TestAt == 0) g_TestAt = now + 14000;
            if (now < g_TestAt) return;
            g_TestDone = true;
            /* As the menu would: pause, then the item by Link's feet. */
            g_PausedAt = now;
            g_TimeMovedAt = now - MenuMinMs;
            float at[3] = {me[0] + __builtin_sinf(rot) * 0.6f, me[1] + 0.8f, me[2] + __builtin_cosf(rot) * 0.6f};
            uintptr_t a = SpawnActor(g_Test, at, rot);
            Logging.Log("[coop] items test: dropped %s: %p", g_Test, reinterpret_cast<void*>(a));
        }
    }

    void NoteClockWrite(float time) {
        g_SyncValue = time;
        g_OurWrite.store(true);
    }

    void SetProbeDebug(bool on) { g_ProbeDebug = on; }

    s64 LastPausedMs() { return g_PausedAt; }

    bool InMenu(s64 now) { return now - g_PausedAt < 500; }

    void SetTest(const char* name) { std::strncpy(g_Test, name ? name : "", sizeof(g_Test) - 1); }

    void Tick(s64 now) {
        WatchPause(now);
        RunTest(now);
        if (now < g_NextScan) return;
        g_NextScan = now + ScanMs;
        Scan(now);
    }
}
