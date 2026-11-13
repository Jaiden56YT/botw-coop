#include "enemies.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <lib.hpp>
#include <nn/os.hpp>

#include "../net/client.hpp"
#include "anim.hpp"
#include "ghost.hpp"
#include "offsets.hpp"
#include "player.hpp"
#include "revive.hpp"

namespace coop::game::enemies {

    namespace {
        using CsFn = void (*)(uintptr_t);
        using CsTryFn = bool (*)(uintptr_t);
        using GetLifeFn = s32* (*)(uintptr_t);

        uintptr_t Base() { return exl::util::GetMainModuleInfo().m_Total.m_Start; }
        template<typename T> T Read(uintptr_t a) { return *reinterpret_cast<const T*>(a); }
        s64 NowMs() { return nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds(); }

        constexpr s64 SendMs = 100;        /* owner updates, 10 a second */
        constexpr float OwnRange = 60.0f;  /* we only send enemies this close to us */
        constexpr s64 RemoteHoldMs = 1000; /* a partner's enemy follows them this long after their last update */
        constexpr s64 YieldMs = 1500;      /* we stop sending an enemy a lower-id partner sends */
        constexpr float SnapDist = 4.0f;   /* farther off than this: put it there at once */
        constexpr float SteerGain = 8.0f;      /* else move toward it at (distance x this) per second */
        constexpr float MaxSpeed = 12.0f;
        constexpr float HeightSlack = 0.75f;
        /* An enemy stays with its owner until another player is clearly
           closer: nearer than this share of the owner's distance, less a
           margin. Two players in one fight would otherwise swap it back and
           forth, and with it who it's fighting. */
        constexpr float TakeRatio = 0.6f, TakeMargin = 1.0f;

        /* ---- what partners send (network thread writes, game thread reads) ---- */
        struct Remote {
            u32 hash;
            u8 from;
            float pos[3];
            float rot_y;
            s64 at;
        };
        constexpr int RemoteSize = 256;
        Remote g_Remote[RemoteSize];
        u32 g_RemoteArea = 0;
        nn::os::MutexType g_Mutex;
        bool g_MutexReady = false;

        /* ---- per enemy, on the game thread ---- */
        struct Local {
            u32 hash;
            uintptr_t actor;
            uintptr_t body;   /* its Havok rigid body, 0 = not found yet */
            float off[3];     /* body position minus actor position (big enemies: not zero) */
            s64 yield_until;  /* a lower-id partner owns it */
            s64 body_tried;
            int tries;
            uintptr_t cand;   /* during a search: the nearest body seen so far, 0 = none */
            float cand_d;     /* and its distance, squared */
            u8 mode;          /* this tick: 1 = ours (we send it), 2 = a partner's (we follow), 3 = nobody's while we're down */
            bool mine;        /* we were its owner last tick */
            u8 last_mode;
            u32 cur;          /* NameId of its body sequence (see Watch::cur) */
            s64 hot_at;       /* see Watch::hot_at */
            s64 fed_at;       /* see Watch::fed_at */
            s64 their_hot_at; /* when a partner last said it's fighting in their world */
            char cur_name[40]; /* ours: the last sequence its AI started, to repeat to partners */
            s64 refresh_at;
        };
        constexpr int LocalSize = 128;
        Local g_Local[LocalSize];
        int g_LocalCount = 0;
        u32 g_Area = 0;
        s64 g_NextSend = 0;
        s64 g_NextScanAt = 0;
        int g_SendStart = 0;
        s64 g_LogAt = 0;
        int g_Steered = 0, g_Snapped = 0;
        float g_ErrSum = 0, g_ErrMaxY = 0; /* how far copies were from their owners' (for the log) */
        int g_ErrCount = 0;

        /* ---- sequences (see OnStartAnimation) ---- */
        /* Which sequence list belongs to which enemy, for the hook: written
           by the game thread each tick, read on any thread. */
        struct Watch {
            std::atomic<uintptr_t> as_list{0};
            std::atomic<u32> hash{0};
            std::atomic<u8> mode{0};
            std::atomic<u32> cur{0};    /* NameId of the body sequence it's playing as far as we know, 0 = unknown */
            std::atomic<u32> logged{0};
            std::atomic<u32> react{0};  /* NameId of the last thing that happened to it here (a hit...), and when */
            std::atomic<s64> react_at{0};
            std::atomic<s64> hot_at{0}; /* when its AI here last asked for a fighting sequence (played or dropped) */
            std::atomic<s64> fed_at{0}; /* when its owner last told us what it's doing */
        };
        Watch g_Watch[LocalSize];
        std::atomic<int> g_WatchCount{0};
        /* When the table was last written. A tick writes it every frame while the world runs; when ticks stop
           (a loading screen) the actors it names are going away, and their addresses go to other actors, so the
           hook only believes a fresh table. */
        std::atomic<s64> g_WatchAt{0};
        constexpr s64 WatchFreshMs = 400;
        std::atomic<int> g_AnimMode{0};
        std::atomic<bool> g_AnimLog{false};
        std::atomic<bool> g_Replaying{false}; /* the game thread is starting an owner's sequence */
        constexpr int AnimQueue = 64;
        AnimEvent g_AnimOut[AnimQueue]; /* ours, to send (hook threads write, game thread reads) */
        int g_AnimOutCount = 0;
        std::atomic_flag g_AnimOutLock = ATOMIC_FLAG_INIT;
        AnimEvent g_AnimIn[AnimQueue]; /* partners', to play (network thread writes, under g_Mutex) */
        int g_AnimInCount = 0;
        u32 g_AnimInArea = 0;
        int g_AnimSent = 0, g_AnimPlayed = 0, g_AnimDropped = 0;

        std::atomic<bool> g_Mute{false}; /* test: treat every enemy as "nobody's while we're down" */

        bool Starts(const char* s, const char* prefix) { return std::strncmp(s, prefix, std::strlen(prefix)) == 0; }

        bool Has(const char* s, const char* part) { return std::strstr(s, part) != nullptr; }

        /* What happens TO an enemy - it's hit, knocked down, set on fire,
           shocked, frozen, it falls, it dies - by the sequence's name. (All
           908 names in the 200 enemy packs were looked through for this.)
           These are played where they happen and sent to everyone. */
        bool Reaction(const char* n) {
            if (Starts(n, "Attack") || Starts(n, "Throw")) return false; /* AttackDamage, ThrowElectricBall... are attacks */
            static const char* const parts[] = {
                "Damage", "Blown", "Burn", "Electric", "Freez", "Die", "Dead", "Killed", "Stun", "Stagger", "Scrap", "Drown",
                "Suffer", "Groggy", "Break", "Shock", "GetUp", "Recover", "Rebound", "ThunderHit", "HeadShot",
            };
            for (const char* p : parts)
                if (Has(n, p)) return true;
            if (Has(n, "Attack")) return false; /* DownSwingAttack, FallAttack */
            return Starts(n, "Down") || Has(n, "Fall") || std::strcmp(n, "Wait_Down") == 0;
        }

        /* Everything else is what its AI chose to do: standing, moving,
           looking, attacking, guarding, camp life. These a copy takes from
           its owner. */
        bool Voluntary(const char* n) { return !Reaction(n); }

        /* It has someone to fight: it noticed a player, or it's attacking,
           guarding, aiming. An enemy belongs to the world where that's so,
           before distance is asked. */
        bool Fighting(const char* n) {
            static const char* const parts[] = {
                "Attack", "Battle", "Notice", "Arrow", "Bow", "Guard", "Shoot", "Angry", "Rush", "Beam", "Magic", "Tackle",
                "Encounter", "Find", "Throw", "Spin", "Slash", "Kick", "Fire",
            };
            for (const char* p : parts)
                if (Has(n, p)) return true;
            return false;
        }
        /* Ours counts for a short while (its AI asks again every second while it's kept waiting); a partner's for
           longer, so an enemy that runs after its owner between two swings isn't taken from them. */
        constexpr s64 HotMs = 2500, TheirHotMs = 6000;

        /* Its death isn't sent: its life is shared, so it dies in every world by itself. */
        bool Death(const char* n) { return Has(n, "Die") || Has(n, "Dead") || Has(n, "Killed"); }

        void QueueOut(u32 hash, float a, float b, const char* name, int target, int bank, u8 flag) {
            while (g_AnimOutLock.test_and_set(std::memory_order_acquire)) {}
            if (g_AnimOutCount < AnimQueue) {
                AnimEvent& e = g_AnimOut[g_AnimOutCount++];
                e.hash = hash, e.a = a, e.b = b;
                e.slot = static_cast<s8>(target), e.bank = static_cast<s8>(bank), e.flag = flag;
                std::snprintf(e.name, sizeof(e.name), "%s", name);
            }
            g_AnimOutLock.clear(std::memory_order_release);
        }

        constexpr u32 NameId(const char* s) {
            u32 h = 2166136261u;
            for (; *s; s++) h = (h ^ static_cast<u8>(*s)) * 16777619u;
            return h ? h : 1;
        }
        constexpr u32 WaitId = NameId("Wait");
        constexpr s64 RefreshMs = 2000; /* an owner repeats what each of its enemies is doing this often */

        Local* FindLocal(u32 hash) {
            for (int i = 0; i < g_LocalCount; i++)
                if (g_Local[i].hash == hash) return &g_Local[i];
            return nullptr;
        }

        /* ---- finding an enemy's rigid body ---- */
        struct Region {
            uintptr_t start, end;
        };
        constexpr int MaxRegions = 64;
        Region g_BodyRegions[MaxRegions]; /* where bodies have been found: scanned first */
        int g_BodyRegionCount = 0;

        void NoteRegion(uintptr_t a) {
            for (int i = 0; i < g_BodyRegionCount; i++)
                if (a >= g_BodyRegions[i].start && a < g_BodyRegions[i].end) return;
            MemoryInfo mi{};
            u32 page = 0;
            if (R_FAILED(svcQueryMemory(&mi, &page, a)) || g_BodyRegionCount >= MaxRegions) return;
            g_BodyRegions[g_BodyRegionCount++] = {mi.addr, mi.addr + mi.size};
        }

        const float* BodyPos(uintptr_t body) { return reinterpret_cast<const float*>(body + offsets::HkBody_Pos); }

        /* Link's own body: never an enemy's. */
        uintptr_t LinkBody() { return LinkRigidBody(); }

        /* Looking through memory for the rigid bodies of the enemies in
           `want` (those with none yet). Only characters have a body of this
           class (30 m around a camp held Link's and the Bokoblins', nothing
           else), so the nearest free one within 2 m sideways and 3 m in
           height is the enemy's own; the offset is kept for bigger enemies
           whose body isn't at their feet.

           Memory is big (a full pass took 170 ms on a fast Mac, and a real
           Switch is several times slower), so the search is spread over
           frames: each call looks for at most SweepBudgetMs and carries on
           where the last one stopped. Order: around Link's own body (bodies
           are made close together), then the regions where bodies were found
           before, then - at most every 30 s - everything else. It ends early
           once every wanted enemy has a body right at its position. */
        constexpr float BodyNear = 2.0f, BodyNearY = 3.0f, ExactDist = 0.25f;
        constexpr s64 SweepBudgetMs = 3;
        constexpr uintptr_t SweepChunk = 0x40000;      /* looked at between two looks at the clock */
        constexpr uintptr_t LinkWindow = 0x800000;     /* this far either side of Link's body first */
        s64 g_LastFullScan = -100000;
        struct Sweep {
            bool active;
            int phase;          /* 0 = around Link, 1 = known regions, 2 = everything */
            int region;         /* phase 1: next known region */
            u64 query;          /* phase 2: next address to ask the kernel about */
            uintptr_t pos, end; /* what's left of the piece being looked at */
            s64 started;
            int calls;
        } g_Sweep;

        /* The next piece of memory to look at; false when there's no more. */
        bool SweepNext(s64 now) {
            Sweep& sw = g_Sweep;
            if (sw.phase == 0) {
                sw.phase = 1;
                uintptr_t link = LinkBody();
                MemoryInfo mi{};
                u32 page = 0;
                if (link && R_SUCCEEDED(svcQueryMemory(&mi, &page, link)) && (mi.perm & Perm_Rw) == Perm_Rw) {
                    sw.pos = link > mi.addr + LinkWindow ? link - LinkWindow : mi.addr;
                    sw.end = link + LinkWindow < mi.addr + mi.size ? link + LinkWindow : mi.addr + mi.size;
                    return true;
                }
            }
            if (sw.phase == 1) {
                if (sw.region < g_BodyRegionCount) {
                    sw.pos = g_BodyRegions[sw.region].start;
                    sw.end = g_BodyRegions[sw.region].end;
                    sw.region++;
                    return true;
                }
                /* Everything else only if nothing has turned up, and not often. */
                for (int i = 0; i < g_LocalCount; i++)
                    if (g_Local[i].cand) return false;
                if (g_BodyRegionCount != 0 && now - g_LastFullScan < 30000) return false;
                g_LastFullScan = now;
                sw.phase = 2;
                sw.query = 0;
            }
            for (int guard = 0; guard < 4096; guard++) {
                MemoryInfo mi{};
                u32 page = 0;
                if (R_FAILED(svcQueryMemory(&mi, &page, sw.query))) return false;
                u64 next = mi.addr + mi.size;
                if (next <= sw.query) return false;
                sw.query = next;
                if ((mi.perm & Perm_Rw) != Perm_Rw || mi.type == MemType_CodeMutable || mi.type == MemType_Io) continue;
                bool known = false;
                for (int i = 0; i < g_BodyRegionCount && !known; i++)
                    known = g_BodyRegions[i].start == mi.addr && g_BodyRegions[i].end == mi.addr + mi.size;
                if (known) continue; /* looked at in phase 1 */
                sw.pos = mi.addr;
                sw.end = mi.addr + mi.size;
                return true;
            }
            return false;
        }

        void SweepFinish(Local** want, int n, s64 now) {
            uintptr_t vt = Base() + offsets::HkRigidBody_Vtable;
            int got = 0;
            for (int i = 0; i < n; i++) {
                Local* l = want[i];
                l->body_tried = now;
                uintptr_t c = l->cand;
                l->cand = 0;
                if (l->tries < 100) l->tries++;
                if (!c || Read<uintptr_t>(c) != vt) continue;
                /* Two enemies standing together can both be nearest to one
                   body: the closer one gets it, the other tries again. */
                bool lost = false;
                for (int j = 0; j < n; j++)
                    if (j != i && want[j]->cand == c && want[j]->cand_d < l->cand_d) lost = true;
                for (int k = 0; k < g_LocalCount; k++)
                    if (g_Local[k].body == c) lost = true;
                if (lost) continue;
                const float* p = BodyPos(c);
                const float* m = reinterpret_cast<const float*>(l->actor + offsets::Actor_Mtx);
                l->body = c;
                l->tries = 0;
                l->off[0] = p[0] - m[3], l->off[1] = p[1] - m[7], l->off[2] = p[2] - m[11];
                NoteRegion(c);
                got++;
                if (l->cand_d > 0.25f * 0.25f)
                    Logging.Log("[coop] enemies: %08x's body is offset by (%.2f, %.2f, %.2f)", l->hash, l->off[0], l->off[1], l->off[2]);
            }
            /* Not found is normal for some (a sleeping Stone Talus has no such
               body): those are asked for less often. */
            Logging.Log("[coop] enemies: found %d of %d bodies in %lld ms over %d frames%s", got, n, now - g_Sweep.started,
                        g_Sweep.calls, g_Sweep.phase == 2 ? " (looked everywhere)" : "");
            g_Sweep.active = false;
        }

        void FindBodies(Local** want, int n, s64 now) {
            if (n == 0) return;
            Sweep& sw = g_Sweep;
            if (!sw.active) {
                sw = {true, 0, 0, 0, 0, 0, now, 0};
                for (int i = 0; i < g_LocalCount; i++) g_Local[i].cand = 0;
            }
            sw.calls++;
            uintptr_t vt = Base() + offsets::HkRigidBody_Vtable;
            uintptr_t link = LinkBody();
            s64 t0 = NowMs();
            for (;;) {
                if (sw.pos >= sw.end && !SweepNext(now)) {
                    SweepFinish(want, n, now);
                    return;
                }
                uintptr_t stop = sw.pos + SweepChunk < sw.end ? sw.pos + SweepChunk : sw.end;
                for (uintptr_t a = (sw.pos + 15) & ~uintptr_t(15); a < stop && a + 0x300 <= sw.end; a += 16) {
                    if (Read<uintptr_t>(a) != vt || a == link) continue;
                    bool taken = false;
                    for (int k = 0; k < g_LocalCount && !taken; k++) taken = g_Local[k].body == a;
                    if (taken) continue;
                    const float* p = BodyPos(a);
                    for (int i = 0; i < n; i++) {
                        const float* m = reinterpret_cast<const float*>(want[i]->actor + offsets::Actor_Mtx);
                        float dx = p[0] - m[3], dy = p[1] - m[7], dz = p[2] - m[11];
                        float d = dx * dx + dz * dz;
                        if (!(d < BodyNear * BodyNear && dy * dy < BodyNearY * BodyNearY)) continue;
                        d += dy * dy;
                        if (!want[i]->cand || d < want[i]->cand_d) want[i]->cand = a, want[i]->cand_d = d;
                    }
                }
                sw.pos = stop;
                /* Every wanted enemy has a body right at its position: done. */
                bool all = true;
                for (int i = 0; i < n && all; i++) all = want[i]->cand && want[i]->cand_d < ExactDist * ExactDist;
                if (all) {
                    SweepFinish(want, n, now);
                    return;
                }
                if (NowMs() - t0 >= SweepBudgetMs) return; /* the rest next frame */
            }
        }

        bool BodyOk(const Local& l) {
            if (!l.body || Read<uintptr_t>(l.body) != Base() + offsets::HkRigidBody_Vtable) return false;
            const float* p = BodyPos(l.body);
            const float* m = reinterpret_cast<const float*>(l.actor + offsets::Actor_Mtx);
            float dx = p[0] - l.off[0] - m[3], dz = p[2] - l.off[2] - m[11];
            return dx * dx + dz * dz < 1.0f;
        }

        /* Moves our copy toward where its owner has it. */
        void Steer(Local& l, const Remote& r, float dt) {
            float* p = reinterpret_cast<float*>(l.body + offsets::HkBody_Pos);
            float* v = reinterpret_cast<float*>(l.body + offsets::HkBody_Vel); /* zeroed on a snap */
            float* m = reinterpret_cast<float*>(l.actor + offsets::Actor_Mtx);
            /* Where the body should be: the owner's actor position plus our offset. */
            float want[3] = {r.pos[0] + l.off[0], r.pos[1] + l.off[1], r.pos[2] + l.off[2]};
            float dx = want[0] - p[0], dy = want[1] - p[1], dz = want[2] - p[2];
            float d = std::sqrt(dx * dx + dz * dz);
            g_ErrSum += d;
            g_ErrCount++;
            if (std::fabs(dy) > g_ErrMaxY) g_ErrMaxY = std::fabs(dy);
            if (d > SnapDist || dy * dy > SnapDist * SnapDist) {
                p[0] = want[0], p[1] = want[1], p[2] = want[2];
                m[3] = r.pos[0], m[7] = r.pos[1], m[11] = r.pos[2];
                v[0] = v[1] = v[2] = 0;
                g_Snapped++;
            } else {
                /* Setting its velocity does nothing (its AI drives it), but its
                   position sticks: move it there, at most MaxSpeed. */
                if (d > 0.02f) {
                    float step = d * SteerGain * dt;
                    if (step > MaxSpeed * dt) step = MaxSpeed * dt;
                    if (step > d) step = d;
                    p[0] += dx / d * step;
                    p[2] += dz / d * step;
                    m[3] = p[0] - l.off[0], m[11] = p[2] - l.off[2];
                    g_Steered++;
                }
                /* Height is left to gravity and the ground while it's close
                   (the same slope in both worlds). Past HeightSlack it's a
                   jump or a flier: follow that too. */
                if (dy > HeightSlack || dy < -HeightSlack) {
                    float step = dy * SteerGain * dt;
                    float cap = MaxSpeed * dt;
                    if (step > cap) step = cap;
                    if (step < -cap) step = -cap;
                    p[1] += step;
                    m[7] = p[1] - l.off[1];
                    if (dy > 0 && v[1] < 0) v[1] = 0; /* don't fight a fall we're lifting it out of */
                }
            }
            /* Facing: the actor's rotation (yaw only). */
            float c = std::cos(r.rot_y), sn = std::sin(r.rot_y);
            m[0] = c, m[1] = 0, m[2] = sn;
            m[4] = 0, m[5] = 1, m[6] = 0;
            m[8] = -sn, m[9] = 0, m[10] = c;
        }
    }

    void Init() {
        if (!g_MutexReady) {
            nn::os::InitializeMutex(&g_Mutex, false, 0);
            g_MutexReady = true;
        }
    }

    void SetAnimSync(int mode, bool log, bool mute) {
        g_AnimMode.store(mode);
        g_AnimLog.store(log);
        g_Mute.store(mute);
    }

    int OnStartAnimation(uintptr_t as_list, float a, float b, const char* name, int target, int bank, bool flag) {
        int sync = g_AnimMode.load(std::memory_order_relaxed);
        if (!sync && !g_AnimLog.load(std::memory_order_relaxed)) return 0;
        if (g_Replaying.load(std::memory_order_relaxed)) return 0;
        if (NowMs() - g_WatchAt.load(std::memory_order_relaxed) > WatchFreshMs) return 0;
        int n = g_WatchCount.load(std::memory_order_relaxed);
        for (int i = 0; i < n; i++) {
            Watch& w = g_Watch[i];
            if (w.as_list.load(std::memory_order_relaxed) != as_list) continue;
            u8 mode = w.mode.load(std::memory_order_relaxed);
            u32 hash = w.hash.load(std::memory_order_relaxed);
            u32 id = NameId(name);
            u32 cur = w.cur.load(std::memory_order_relaxed);
            if (g_AnimLog.load(std::memory_order_relaxed) && id != w.logged.exchange(id, std::memory_order_relaxed))
                Logging.Log("[coop] enemy AS %08x slot %d '%s' (%s)", hash, target, name,
                            mode == 1 ? "ours" : mode == 2 ? "partner's" : mode == 3 ? "nobody's, we're down" : "nobody's");
            if (!sync) return 0;
            if (target != 0 || !Voluntary(name)) {
                /* Something that happened to it here (a hit, a fall): played,
                   and what it was doing before is forgotten, so its owner's
                   next word on it is played whatever it is. */
                if (target == 0) w.cur.store(0, std::memory_order_relaxed);
                /* Partners see it too (the hit was ours, on our screen). Its
                   death isn't sent: its life is shared, so it dies in every
                   world by itself. */
                bool react = target == 0 && mode != 0 && !Death(name);
                if (target == 0) {
                    w.react.store(id, std::memory_order_relaxed);
                    w.react_at.store(NowMs(), std::memory_order_relaxed);
                }
                if (react) QueueOut(hash, a, b, name, target, bank, static_cast<u8>((flag ? 1 : 0) | ReactFlag));
                return 0;
            }
            bool ours = mode == 1;
            if (Fighting(name)) w.hot_at.store(NowMs(), std::memory_order_relaxed);
            /* Its owner says what it does - as long as the owner does say (an owner on an older build of the mod
               sends nothing, and its copy here would stand frozen). Owners repeat themselves every 2 s. */
            if (sync == 2 && mode == 2 && cur != 0 && NowMs() - w.fed_at.load(std::memory_order_relaxed) < 5000) return 1;
            if (sync == 2 && mode == 3) {
                /* Nobody's while we're down: it stands still. */
                if (cur == WaitId) return 1;
                w.cur.store(WaitId, std::memory_order_relaxed);
                return id == WaitId ? 0 : 2;
            }
            /* It starts: sent if it's ours, and remembered either way (it may become ours while it still plays). */
            QueueOut(hash, a, b, name, target, bank, static_cast<u8>((flag ? 1 : 0) | (ours ? 0 : NoteFlag)));
            w.cur.store(id, std::memory_order_relaxed);
            return 0;
        }
        return 0;
    }

    void OnRemoteAnims(u8 from, u32 area, const AnimEvent* events, int count) {
        if (!g_MutexReady) return;
        (void)from;
        nn::os::LockMutex(&g_Mutex);
        if (area != g_AnimInArea) {
            g_AnimInCount = 0;
            g_AnimInArea = area;
        }
        for (int i = 0; i < count && g_AnimInCount < AnimQueue; i++) g_AnimIn[g_AnimInCount++] = events[i];
        nn::os::UnlockMutex(&g_Mutex);
    }

    void Stop() {
        g_WatchCount.store(0, std::memory_order_relaxed);
        g_WatchAt.store(0, std::memory_order_relaxed);
        g_LocalCount = 0;
    }

    void OnRemote(u8 from, u32 area, const Pose* poses, int count) {
        if (!g_MutexReady) return;
        s64 now = NowMs();
        nn::os::LockMutex(&g_Mutex);
        if (area != g_RemoteArea) {
            std::memset(g_Remote, 0, sizeof(g_Remote));
            g_RemoteArea = area;
        }
        for (int i = 0; i < count; i++) {
            u32 h = poses[i].hash;
            if (!h) continue;
            u32 slot = (h * 2654435761u) % RemoteSize;
            for (int k = 0; k < RemoteSize; k++, slot = (slot + 1) % RemoteSize) {
                Remote& r = g_Remote[slot];
                if (r.hash != h && r.hash != 0 && now - r.at < 10000) continue;
                r.hash = h;
                r.from = from;
                std::memcpy(r.pos, poses[i].pos, sizeof(r.pos));
                r.rot_y = poses[i].rot_y;
                r.at = now;
                break;
            }
        }
        nn::os::UnlockMutex(&g_Mutex);
    }

    void Tick(s64 now) {
        static s64 s_last = 0;
        float dt = s_last ? (now - s_last) / 1000.0f : 0.033f;
        s_last = now;

        float me[3];
        if (!GetLocalPlayerPos(me)) return;
        u32 area = LocalArea();
        if (area == 0) return;
        if (area != g_Area) {
            g_Area = area;
            g_LocalCount = 0;
        }

        /* What the hook saw start since the last tick (same order as then). */
        for (int i = 0; i < g_LocalCount && i < g_WatchCount.load(std::memory_order_relaxed); i++)
            if (g_Watch[i].hash.load(std::memory_order_relaxed) == g_Local[i].hash) {
                g_Local[i].cur = g_Watch[i].cur.load(std::memory_order_relaxed);
                g_Local[i].hot_at = g_Watch[i].hot_at.load(std::memory_order_relaxed);
            }

        /* Placed enemies alive near us (actor list, under its lock). */
        uintptr_t slot = Read<uintptr_t>(Base() + offsets::Got_ActorCreatorInstance);
        uintptr_t creator = slot ? Read<uintptr_t>(slot) : 0;
        if (!creator) return;
        uintptr_t cs = creator + offsets::ActorCreator_ActorListCS;
        if (!reinterpret_cast<CsTryFn>(Base() + offsets::CriticalSection_TryLock)(cs)) return;
        bool seen[LocalSize] = {};
        uintptr_t head = creator + offsets::ActorCreator_ActorList;
        s32 node_off = Read<s32>(creator + offsets::ActorCreator_ActorListOffset);
        int guard = 0;
        for (uintptr_t node = Read<uintptr_t>(head + 8); node != head && node && guard < 20000; guard++) {
            uintptr_t a = node - node_off;
            node = Read<uintptr_t>(node + 8);
            u32 hash = Read<u32>(a + offsets::Actor_HashId);
            if (!hash) continue;
            const char* name = Read<const char*>(a + offsets::BaseProc_NameStr);
            if (!name || std::strncmp(name, "Enemy_", 6) != 0) continue;
            const float* m = reinterpret_cast<const float*>(a + offsets::Actor_Mtx);
            float dx = m[3] - me[0], dz = m[11] - me[2];
            if (dx * dx + dz * dz > 150.0f * 150.0f) continue;
            s32* life = reinterpret_cast<GetLifeFn>(Read<uintptr_t>(Read<uintptr_t>(a) + offsets::ActorVt_GetLife))(a);
            if (!life || *life <= 0) continue;
            Local* l = FindLocal(hash);
            if (!l && g_LocalCount < LocalSize) {
                l = &g_Local[g_LocalCount++];
                *l = {hash, a, 0, {0, 0, 0}, 0, 0, 0};
            }
            if (!l) continue;
            if (l->actor != a) *l = {hash, a, 0, {0, 0, 0}, 0, 0, 0};
            seen[l - g_Local] = true;
        }
        reinterpret_cast<CsFn>(Base() + offsets::CriticalSection_Unlock)(cs);
        /* Forget enemies that are gone (killed, unloaded, far). */
        for (int i = 0; i < g_LocalCount;) {
            if (seen[i]) {
                i++;
                continue;
            }
            g_Local[i] = g_Local[--g_LocalCount];
            seen[i] = seen[g_LocalCount];
        }

        /* Partners' positions, to see who's closest to each enemy. */
        net::Peer peers[net::MaxPeers];
        net::GetPeers(peers);
        u8 my_id = net::MyId();
        s64 tick_now = nn::os::GetSystemTick().GetInt64Value();
        s64 stale = static_cast<s64>(nn::os::GetSystemTickFrequency()) * 3;

        Remote remote[LocalSize];
        bool have_remote[LocalSize] = {};
        if (g_MutexReady) {
            nn::os::LockMutex(&g_Mutex);
            if (g_RemoteArea == area) {
                for (int i = 0; i < g_LocalCount; i++) {
                    u32 h = g_Local[i].hash;
                    u32 s = (h * 2654435761u) % RemoteSize;
                    for (int k = 0; k < RemoteSize; k++, s = (s + 1) % RemoteSize) {
                        if (g_Remote[s].hash == 0) break;
                        if (g_Remote[s].hash != h) continue;
                        if (now - g_Remote[s].at < RemoteHoldMs) {
                            remote[i] = g_Remote[s];
                            have_remote[i] = true;
                        }
                        break;
                    }
                }
            }
            nn::os::UnlockMutex(&g_Mutex);
        }

        Pose out[MaxPerMessage];
        int nout = 0;
        bool down = revive::IsDown() || g_Mute.load(std::memory_order_relaxed);
        bool sync_anims = g_AnimMode.load(std::memory_order_relaxed) == 2;
        bool send = now >= g_NextSend;
        if (send) g_NextSend = now + SendMs;
        Local* need_body[16];
        int nneed = 0;

        /* More owned enemies than a message holds: start a bit further along
           the list each time, so all of them get sent in turn. */
        if (send && g_LocalCount > 0) g_SendStart = (g_SendStart + MaxPerMessage / 2) % g_LocalCount;
        for (int n = 0; n < g_LocalCount; n++) {
            int i = g_LocalCount ? (n + g_SendStart) % g_LocalCount : 0;
            Local& l = g_Local[i];
            l.mode = 0;
            const float* m = reinterpret_cast<const float*>(l.actor + offsets::Actor_Mtx);
            float mdx = m[3] - me[0], mdz = m[11] - me[2];
            float my_d = mdx * mdx + mdz * mdz;

            /* Whose is it? A player who is down owns nothing. */
            float my_dist = std::sqrt(my_d);
            float other = 1e9f;
            float sender = -1.0f;
            for (auto& p : peers) {
                if (!p.present || !p.has_state || p.state.map_area != area ||
                    tick_now - static_cast<s64>(p.state_tick) > stale || (p.state.anim_hash & 0xff) == anim::Down)
                    continue;
                float dx = m[3] - p.state.pos[0], dz = m[11] - p.state.pos[2];
                float d = std::sqrt(dx * dx + dz * dz);
                if (d < other) other = d;
                if (have_remote[i] && p.id == remote[i].from) sender = d;
            }
            bool can_own = my_d < OwnRange * OwnRange && !down;
            bool want;
            bool hot = sync_anims && l.hot_at && now - l.hot_at < HotMs;
            bool their_hot = sync_anims && l.their_hot_at && now - l.their_hot_at < TheirHotMs;
            if (have_remote[i] && hot != their_hot) {
                /* It's fighting in one world and idling in the other: it belongs where it fights. */
                want = can_own && hot;
            } else if (have_remote[i]) {
                const Remote& r = remote[i];
                float theirs = sender >= 0 ? sender : other;
                want = can_own && my_dist < theirs * TakeRatio - TakeMargin;
                /* Both of us send it: the lower id keeps it, unless the other is the one clearly closer. */
                if (!want && l.mine) want = can_own && my_id < r.from && !(theirs < my_dist * TakeRatio - TakeMargin);
            } else if (l.mine) {
                want = can_own && !(other < my_dist * TakeRatio - TakeMargin);
            } else {
                want = can_own && my_dist <= other;
            }
            l.mine = want;

            if (have_remote[i] && !want) {
                const Remote& r = remote[i];
                l.yield_until = now + YieldMs;
                l.mode = 2;
                if (!BodyOk(l)) {
                    l.body = 0;
                    /* Some never have one (see FindBodies): ask less and less often. */
                    s64 wait = l.tries > 8 ? 30000 : l.tries > 3 ? 10000 : 2000;
                    if (now - l.body_tried > wait && nneed < 16) need_body[nneed++] = &l;
                    continue;
                }
                Steer(l, r, dt);
                continue;
            }
            if (want) {
                l.mode = 1;
                if (send && nout < MaxPerMessage) {
                    Pose& o = out[nout++];
                    o.hash = l.hash;
                    o.pos[0] = m[3], o.pos[1] = m[7], o.pos[2] = m[11];
                    o.rot_y = std::atan2(m[2], m[10]);
                }
            } else if (down) {
                l.mode = 3;
            }
        }
        /* A search runs a few milliseconds a frame until it's through; the
           next one starts half a second later at the earliest. */
        if (nneed && (g_Sweep.active || now >= g_NextScanAt)) {
            FindBodies(need_body, nneed, now);
            if (!g_Sweep.active) g_NextScanAt = now + 500;
        } else if (!nneed && g_Sweep.active) {
            g_Sweep.active = false; /* nobody left to look for */
        }
        if (send && nout) net::SendEnemyPoses(area, out, nout);

        /* Sequences: ours go out, the partners' are played on the copies we
           follow, then the hook's table is written for the next frame. */
        if (g_AnimMode.load(std::memory_order_relaxed)) {
            AnimEvent ev[AnimQueue];
            while (g_AnimOutLock.test_and_set(std::memory_order_acquire)) {}
            int n = g_AnimOutCount;
            std::memcpy(ev, g_AnimOut, n * sizeof(AnimEvent));
            g_AnimOutCount = 0;
            g_AnimOutLock.clear(std::memory_order_release);
            int keep = 0;
            for (int i = 0; i < n; i++) {
                Local* l = FindLocal(ev[i].hash);
                if (l && !(ev[i].flag & ReactFlag)) {
                    std::snprintf(l->cur_name, sizeof(l->cur_name), "%s", ev[i].name);
                    l->refresh_at = now + RefreshMs;
                }
                if (ev[i].flag & NoteFlag) continue; /* not ours: nothing to send */
                ev[keep++] = ev[i];
            }
            n = keep;
            /* What each of ours is doing, again: for a partner whose copy
               missed it (just arrived, or its copy was hit meanwhile). */
            for (int i = 0; i < g_LocalCount && n < AnimQueue; i++) {
                Local& l = g_Local[i];
                if (l.mode != 1 || !l.cur_name[0] || now < l.refresh_at) continue;
                l.refresh_at = now + RefreshMs;
                AnimEvent& e = ev[n++];
                e = {};
                e.hash = l.hash, e.a = -1.0f, e.b = -1.0f, e.flag = 1 | RefreshFlag;
                std::snprintf(e.name, sizeof(e.name), "%s", l.cur_name);
            }
            for (int i = 0; i < n; i += MaxAnimsPerMessage)
                net::SendEnemyAnims(area, ev + i, n - i < MaxAnimsPerMessage ? n - i : MaxAnimsPerMessage);
            g_AnimSent += n;

            n = 0;
            if (g_MutexReady) {
                nn::os::LockMutex(&g_Mutex);
                if (g_AnimInArea == area) {
                    n = g_AnimInCount;
                    std::memcpy(ev, g_AnimIn, n * sizeof(AnimEvent));
                }
                g_AnimInCount = 0;
                nn::os::UnlockMutex(&g_Mutex);
            }
            for (int i = 0; i < n; i++) {
                Local* l = FindLocal(ev[i].hash);
                bool react = (ev[i].flag & ReactFlag) != 0;
                ev[i].name[sizeof(ev[i].name) - 1] = 0;
                if (l && !react) {
                    l->fed_at = now;
                    if (Fighting(ev[i].name)) l->their_hot_at = now;
                }
                if (!l || (l->mode != 2 && !react)) {
                    g_AnimDropped++;
                    continue;
                }
                if (react) {
                    /* The same thing just happened to our copy by itself (it was knocked down in every world and
                       gets up in every world): not started a second time. */
                    Watch& w = g_Watch[l - g_Local];
                    if (w.hash.load(std::memory_order_relaxed) == l->hash && w.react.load(std::memory_order_relaxed) == NameId(ev[i].name) &&
                        now - w.react_at.load(std::memory_order_relaxed) < 1500)
                        continue;
                    w.react.store(NameId(ev[i].name), std::memory_order_relaxed);
                    w.react_at.store(now, std::memory_order_relaxed);
                }
                u32 id = react ? 0 : NameId(ev[i].name);
                if ((ev[i].flag & RefreshFlag) && l->cur == id) continue; /* already doing that */
                uintptr_t as_list = Read<uintptr_t>(l->actor + offsets::Actor_ASList);
                if (!as_list) continue;
                g_Replaying.store(true, std::memory_order_relaxed);
                StartAS(as_list, ev[i].a, ev[i].b, ev[i].name, ev[i].slot, ev[i].bank, (ev[i].flag & 1) != 0);
                g_Replaying.store(false, std::memory_order_relaxed);
                l->cur = id;
                /* What it's doing now, in case it becomes ours before its own AI says anything. */
                if (!react) std::snprintf(l->cur_name, sizeof(l->cur_name), "%s", ev[i].name);
                g_AnimPlayed++;
            }
        }
        for (int i = 0; i < g_LocalCount; i++) {
            Local& l = g_Local[i];
            if (l.mode != l.last_mode) {
                if (g_AnimLog.load(std::memory_order_relaxed))
                    Logging.Log("[coop] enemy %08x is now %s", l.hash,
                                l.mode == 1 ? "ours" : l.mode == 2 ? "a partner's" : l.mode == 3 ? "nobody's (we're down)" : "nobody's");
                if (l.mode == 1) l.refresh_at = 0; /* newly ours: partners hear what it's doing at once */
                l.last_mode = l.mode;
            }
            g_Watch[i].as_list.store(Read<uintptr_t>(l.actor + offsets::Actor_ASList), std::memory_order_relaxed);
            g_Watch[i].hash.store(l.hash, std::memory_order_relaxed);
            g_Watch[i].mode.store(l.mode, std::memory_order_relaxed);
            g_Watch[i].cur.store(l.cur, std::memory_order_relaxed);
            g_Watch[i].fed_at.store(l.fed_at, std::memory_order_relaxed);
        }
        g_WatchCount.store(g_LocalCount, std::memory_order_relaxed);
        g_WatchAt.store(now, std::memory_order_relaxed);

        if (now >= g_LogAt && (g_Steered || g_Snapped)) {
            g_LogAt = now + 10000;
            Logging.Log("[coop] enemies: %d tracked; following partners: %d moves, %d snaps, %.2f m off on average, height off by up to %.2f m",
                        g_LocalCount, g_Steered, g_Snapped, g_ErrCount ? g_ErrSum / g_ErrCount : 0.0f, g_ErrMaxY);
            if (g_AnimSent || g_AnimPlayed || g_AnimDropped)
                Logging.Log("[coop] enemies: sequences sent %d, played from partners %d, not ours to play %d", g_AnimSent,
                            g_AnimPlayed, g_AnimDropped);
            g_AnimSent = g_AnimPlayed = g_AnimDropped = 0;
            g_Steered = g_Snapped = g_ErrCount = 0;
            g_ErrSum = g_ErrMaxY = 0;
        }
    }
}
