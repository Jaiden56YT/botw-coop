#include "enemies.hpp"

#include <cmath>
#include <cstring>

#include <lib.hpp>
#include <nn/os.hpp>

#include "../net/client.hpp"
#include "offsets.hpp"
#include "player.hpp"

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
            s64 yield_until;  /* a lower-id partner owns it */
            s64 body_tried;
        };
        constexpr int LocalSize = 128;
        Local g_Local[LocalSize];
        int g_LocalCount = 0;
        u32 g_Area = 0;
        s64 g_NextSend = 0;
        s64 g_NextScanAt = 0;
        s64 g_LogAt = 0;
        int g_Steered = 0, g_Snapped = 0;

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

        /* One pass over memory: for each enemy in `want` (no body yet), the
           rigid body sitting at its position. Learned regions first; the
           whole address space only when those find nothing. */
        void FindBodies(Local** want, int n, s64 now) {
            if (n == 0) return;
            uintptr_t vt = Base() + offsets::HkRigidBody_Vtable;
            int left = n;
            auto scan = [&](uintptr_t start, uintptr_t end) {
                for (uintptr_t a = (start + 15) & ~uintptr_t(15); a + 0x300 <= end && left > 0; a += 16) {
                    if (Read<uintptr_t>(a) != vt) continue;
                    const float* p = BodyPos(a);
                    for (int i = 0; i < n; i++) {
                        Local* l = want[i];
                        if (l->body) continue;
                        const float* m = reinterpret_cast<const float*>(l->actor + offsets::Actor_Mtx);
                        float dx = p[0] - m[3], dy = p[1] - m[7], dz = p[2] - m[11];
                        if (!(dx * dx + dz * dz < 0.25f * 0.25f && dy * dy < 1.0f)) continue;
                        l->body = a;
                        left--;
                        NoteRegion(a);
                        break;
                    }
                }
            };
            s64 t0 = NowMs();
            for (int r = 0; r < g_BodyRegionCount && left > 0; r++) scan(g_BodyRegions[r].start, g_BodyRegions[r].end);
            bool full = false;
            if (left == n) {
                /* Nothing from the known places: look everywhere once. */
                full = true;
                u64 addr = 0;
                for (int guard = 0; guard < 4096 && left > 0; guard++) {
                    MemoryInfo mi{};
                    u32 page = 0;
                    if (R_FAILED(svcQueryMemory(&mi, &page, addr))) break;
                    if ((mi.perm & Perm_Rw) == Perm_Rw && mi.type != MemType_CodeMutable && mi.type != MemType_Io)
                        scan(mi.addr, mi.addr + mi.size);
                    u64 next = mi.addr + mi.size;
                    if (next <= addr) break;
                    addr = next;
                }
            }
            for (int i = 0; i < n; i++) want[i]->body_tried = now;
            Logging.Log("[coop] enemies: found %d of %d bodies in %lld ms%s", n - left, n, NowMs() - t0,
                        full ? " (full scan)" : "");
        }

        bool BodyOk(const Local& l) {
            if (!l.body || Read<uintptr_t>(l.body) != Base() + offsets::HkRigidBody_Vtable) return false;
            const float* p = BodyPos(l.body);
            const float* m = reinterpret_cast<const float*>(l.actor + offsets::Actor_Mtx);
            float dx = p[0] - m[3], dz = p[2] - m[11];
            return dx * dx + dz * dz < 1.0f;
        }

        /* Moves our copy toward where its owner has it. */
        void Steer(Local& l, const Remote& r, float dt) {
            float* p = reinterpret_cast<float*>(l.body + offsets::HkBody_Pos);
            float* v = reinterpret_cast<float*>(l.body + offsets::HkBody_Vel); /* zeroed on a snap */
            float* m = reinterpret_cast<float*>(l.actor + offsets::Actor_Mtx);
            float dx = r.pos[0] - p[0], dy = r.pos[1] - p[1], dz = r.pos[2] - p[2];
            float d = std::sqrt(dx * dx + dz * dz);
            if (d > SnapDist || dy * dy > SnapDist * SnapDist) {
                p[0] = r.pos[0], p[1] = r.pos[1], p[2] = r.pos[2];
                m[3] = r.pos[0], m[7] = r.pos[1], m[11] = r.pos[2];
                v[0] = v[1] = v[2] = 0;
                g_Snapped++;
            } else if (d > 0.02f) {
                /* Setting its velocity does nothing (its AI drives it), but its
                   position sticks: move it there, at most MaxSpeed. Horizontal
                   only: gravity and the ground handle height. */
                float step = d * SteerGain * dt;
                if (step > MaxSpeed * dt) step = MaxSpeed * dt;
                if (step > d) step = d;
                p[0] += dx / d * step;
                p[2] += dz / d * step;
                m[3] = p[0], m[11] = p[2];
                g_Steered++;
            }
            /* Facing: the actor's rotation (yaw only). */
            float c = std::cos(r.rot_y), sn = std::sin(r.rot_y);
            m[0] = c, m[1] = 0, m[2] = sn;
            m[4] = 0, m[5] = 1, m[6] = 0;
            m[8] = -sn, m[9] = 0, m[10] = c;
            (void)dt;
        }
    }

    void Init() {
        if (!g_MutexReady) {
            nn::os::InitializeMutex(&g_Mutex, false, 0);
            g_MutexReady = true;
        }
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
                *l = {hash, a, 0, 0, 0};
            }
            if (!l) continue;
            if (l->actor != a) *l = {hash, a, 0, 0, 0};
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
        bool send = now >= g_NextSend;
        if (send) g_NextSend = now + SendMs;
        Local* need_body[16];
        int nneed = 0;

        for (int i = 0; i < g_LocalCount; i++) {
            Local& l = g_Local[i];
            const float* m = reinterpret_cast<const float*>(l.actor + offsets::Actor_Mtx);
            float mdx = m[3] - me[0], mdz = m[11] - me[2];
            float my_d = mdx * mdx + mdz * mdz;

            /* Am I the closest player to it? */
            bool closest = my_d < OwnRange * OwnRange;
            for (auto& p : peers) {
                if (!closest) break;
                if (!p.present || !p.has_state || p.state.map_area != area ||
                    tick_now - static_cast<s64>(p.state_tick) > stale)
                    continue;
                float dx = m[3] - p.state.pos[0], dz = m[11] - p.state.pos[2];
                if (dx * dx + dz * dz < my_d) closest = false;
            }

            if (have_remote[i]) {
                const Remote& r = remote[i];
                /* Both of us think we're closest: the lower id wins. */
                if (closest && my_id < r.from) {
                    /* ours: ignore theirs */
                } else {
                    l.yield_until = now + YieldMs;
                    if (!BodyOk(l)) {
                        l.body = 0;
                        if (now - l.body_tried > 2000 && nneed < 16) need_body[nneed++] = &l;
                        continue;
                    }
                    Steer(l, r, dt);
                    continue;
                }
            }
            if (closest && now >= l.yield_until && send && nout < MaxPerMessage) {
                Pose& o = out[nout++];
                o.hash = l.hash;
                o.pos[0] = m[3], o.pos[1] = m[7], o.pos[2] = m[11];
                o.rot_y = std::atan2(m[2], m[10]);
            }
        }
        if (nneed && now >= g_NextScanAt) {
            g_NextScanAt = now + 500;
            FindBodies(need_body, nneed, now);
        }
        if (send && nout) net::SendEnemyPoses(area, out, nout);

        if (now >= g_LogAt && (g_Steered || g_Snapped)) {
            g_LogAt = now + 30000;
            Logging.Log("[coop] enemies: %d tracked, following partners: %d steers, %d snaps in the last 30 s", g_LocalCount,
                        g_Steered, g_Snapped);
            g_Steered = g_Snapped = 0;
        }
    }
}
