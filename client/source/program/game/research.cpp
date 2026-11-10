#include "research.hpp"

#include <cmath>
#include <cstring>

#include <lib.hpp>

#include "offsets.hpp"
#include "player.hpp"

/* DEBUG ONLY: finding things at runtime. Not part of normal play. */
namespace coop::game::research {

    namespace {
        uintptr_t Base() { return exl::util::GetMainModuleInfo().m_Total.m_Start; }
        template<typename T> T Read(uintptr_t a) { return *reinterpret_cast<const T*>(a); }

        struct Region {
            uintptr_t start, end;
        };
        Region g_Regions[1024];
        int g_RegionCount = 0;
        void LoadRegions() {
            g_RegionCount = 0;
            u64 addr = 0;
            while (g_RegionCount < 1024) {
                MemoryInfo mi{};
                u32 page = 0;
                if (R_FAILED(svcQueryMemory(&mi, &page, addr))) break;
                if ((mi.perm & Perm_R) && mi.type != MemType_Unmapped && mi.type != MemType_Io)
                    g_Regions[g_RegionCount++] = {mi.addr, mi.addr + mi.size};
                u64 next = mi.addr + mi.size;
                if (next <= addr) break;
                addr = next;
            }
        }
        bool Readable(uintptr_t a, size_t len) {
            if (a & 7) return false;
            int lo = 0, hi = g_RegionCount - 1;
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                if (a < g_Regions[mid].start) hi = mid - 1;
                else if (a >= g_Regions[mid].end) lo = mid + 1;
                else return a + len <= g_Regions[mid].end;
            }
            return false;
        }

        int g_Mode = 0;
        s64 g_At = 0;
        bool g_Done = false;

        uintptr_t PlayerActor() {
            uintptr_t info = Read<uintptr_t>(Base() + offsets::PlayerInfoInstance);
            return info ? Read<uintptr_t>(info + offsets::PlayerInfo_PlayerActor) : 0;
        }

        /* Depth-first search for `target` from `from`, logging every path. */
        uintptr_t g_Path[6];
        int g_Found = 0;
        void Search(uintptr_t node, uintptr_t target, int depth, int max_depth, size_t span) {
            if (g_Found >= 12 || !Readable(node, span)) return;
            for (size_t off = 0; off < span; off += 8) {
                uintptr_t p = Read<uintptr_t>(node + off);
                if (p == target) {
                    g_Path[depth] = off;
                    char buf[128];
                    int n = 0;
                    for (int i = 0; i <= depth; i++) n += std::snprintf(buf + n, sizeof(buf) - n, "+%#lx ", g_Path[i]);
                    Logging.Log("[coop] research: path %s-> %p (target %+ld)", buf, reinterpret_cast<void*>(p),
                                static_cast<long>(p - target));
                    g_Found++;
                    continue;
                }
                if (depth + 1 < max_depth && p && (p & 7) == 0 && Readable(p, 8)) {
                    g_Path[depth] = off;
                    Search(p, target, depth + 1, max_depth, depth + 1 < 2 ? 0x800 : 0x200);
                }
            }
        }
    }

    namespace {
        using CsFn = void (*)(uintptr_t);
        using CsTryFn = bool (*)(uintptr_t);
        constexpr uintptr_t BodyVtable = 0x1b87b20; /* hkpRigidBody (Link's body) */
        constexpr uintptr_t BodyToPos = 0x1c0;      /* motion (+0x150) transform translation (+0x70) */
        constexpr uintptr_t BodyToVel = 0x270;      /* motion linear velocity (+0x120) */

        uintptr_t g_Enemy = 0, g_Body = 0;
        float g_BodyRadius = 8.0f;
        float g_EnemyStart[3];
        s64 g_MoveAt = 0, g_LogUntil = 0, g_LogAt = 0;
        int g_Step = 0;

        uintptr_t NearestEnemy(const float* me, float* dist) {
            uintptr_t slot = Read<uintptr_t>(Base() + offsets::Got_ActorCreatorInstance);
            uintptr_t creator = slot ? Read<uintptr_t>(slot) : 0;
            if (!creator) return 0;
            uintptr_t cs = creator + offsets::ActorCreator_ActorListCS;
            if (!reinterpret_cast<CsTryFn>(Base() + offsets::CriticalSection_TryLock)(cs)) return 0;
            uintptr_t head = creator + offsets::ActorCreator_ActorList;
            s32 node_off = Read<s32>(creator + offsets::ActorCreator_ActorListOffset);
            uintptr_t best = 0;
            float bestd = 1e9f;
            int guard = 0;
            for (uintptr_t node = Read<uintptr_t>(head + 8); node != head && node && guard < 20000; guard++) {
                uintptr_t a = node - node_off;
                node = Read<uintptr_t>(node + 8);
                const char* n = Read<const char*>(a + offsets::BaseProc_NameStr);
                if (!n || std::strncmp(n, "Enemy_", 6) != 0) continue;
                if (Read<u32>(a + offsets::Actor_HashId) == 0) continue; /* parts and spawns */
                const float* m = reinterpret_cast<const float*>(a + offsets::Actor_Mtx);
                float dx = m[3] - me[0], dy = m[7] - me[1], dz = m[11] - me[2];
                float d = dx * dx + dy * dy + dz * dz;
                if (d < bestd) bestd = d, best = a;
            }
            reinterpret_cast<CsFn>(Base() + offsets::CriticalSection_Unlock)(cs);
            *dist = std::sqrt(bestd);
            return best;
        }

        /* Rigid bodies (by vtable) whose position is within 3 m of `at`. */
        int FindBodies(const float* at, uintptr_t* out, int max) {
            LoadRegions();
            uintptr_t vt = Base() + BodyVtable;
            int n = 0;
            for (int r = 0; r < g_RegionCount && n < max; r++) {
                MemoryInfo mi{};
                u32 page = 0;
                svcQueryMemory(&mi, &page, g_Regions[r].start);
                if ((mi.perm & Perm_Rw) != Perm_Rw || mi.type == MemType_CodeMutable) continue;
                for (uintptr_t a = g_Regions[r].start; a + BodyToVel + 16 <= g_Regions[r].end && n < max; a += 16) {
                    if (Read<uintptr_t>(a) != vt) continue;
                    const float* p = reinterpret_cast<const float*>(a + BodyToPos);
                    float dx = p[0] - at[0], dy = p[1] - at[1], dz = p[2] - at[2];
                    if (!(dx * dx + dy * dy + dz * dz <= g_BodyRadius * g_BodyRadius)) continue;
                    Logging.Log("[coop] research: body %p at (%.2f %.2f %.2f), %.2f m from the actor", reinterpret_cast<void*>(a),
                                p[0], p[1], p[2], std::sqrt(dx * dx + dy * dy + dz * dz));
                    out[n++] = a;
                }
            }
            return n;
        }

        void EnemyTest(s64 now, const float* me) {
            if (g_Step == 0) {
                float d;
                g_Enemy = NearestEnemy(me, &d);
                if (!g_Enemy || d > 40.0f) return; /* wait for the test enemy */
                Logging.Log("[coop] research: hash %08x", Read<u32>(g_Enemy + offsets::Actor_HashId));
                const float* m = reinterpret_cast<const float*>(g_Enemy + offsets::Actor_Mtx);
                std::memcpy(g_EnemyStart, (float[3]){m[3], m[7], m[11]}, 12);
                Logging.Log("[coop] research: nearest enemy %s at %.1f m (%.2f %.2f %.2f)",
                            Read<const char*>(g_Enemy + offsets::BaseProc_NameStr), d, m[3], m[7], m[11]);
                uintptr_t bodies[16];
                int n = FindBodies(g_EnemyStart, bodies, 16);
                Logging.Log("[coop] research: %d bodies near it", n);
                if (!n) {
                    /* None at the actor: what is around, then? Every body within 30 m,
                       and every enemy actor (with parts) near it. */
                    g_BodyRadius = 30.0f;
                    n = FindBodies(g_EnemyStart, bodies, 16);
                    Logging.Log("[coop] research: %d bodies within 30 m", n);
                    g_Done = true;
                    return;
                }
                /* The closest one. */
                float bestd = 1e9f;
                for (int i = 0; i < n; i++) {
                    const float* p = reinterpret_cast<const float*>(bodies[i] + BodyToPos);
                    float dx = p[0] - g_EnemyStart[0], dz = p[2] - g_EnemyStart[2];
                    if (dx * dx + dz * dz < bestd) bestd = dx * dx + dz * dz, g_Body = bodies[i];
                }
                g_Step = 1;
                g_MoveAt = now + 1000;
                LoadRegions();
                uintptr_t is = Read<uintptr_t>(g_Enemy + 0x578);
                Logging.Log("[coop] research: paths from the enemy's InstanceSet %p to its body %p:", reinterpret_cast<void*>(is),
                            reinterpret_cast<void*>(g_Body));
                g_Found = 0;
                Search(is, g_Body, 0, 4, 0x800);
                Logging.Log("[coop] research: paths to body+0x150 (motion):");
                Search(is, g_Body + 0x150, 0, 4, 0x800);
                Logging.Log("[coop] research: paths from the enemy actor:");
                Search(g_Enemy, g_Body, 0, 3, 0x2800);
                return;
            }
            if (g_Step == 1 && now >= g_MoveAt) {
                /* Move it 4 m along x: body and matrix. */
                float* p = reinterpret_cast<float*>(g_Body + BodyToPos);
                float* v = reinterpret_cast<float*>(g_Body + BodyToVel);
                float* m = reinterpret_cast<float*>(g_Enemy + offsets::Actor_Mtx);
                Logging.Log("[coop] research: moving body %p from (%.2f %.2f %.2f) by 4 m in x", reinterpret_cast<void*>(g_Body),
                            p[0], p[1], p[2]);
                p[0] += 4.0f;
                v[0] = v[1] = v[2] = 0;
                m[3] += 4.0f;
                g_Step = 2;
                g_LogUntil = now + 4000;
                return;
            }
            if (g_Step == 2) {
                if (now > g_LogUntil) {
                    g_Done = true;
                    Logging.Log("[coop] research: done");
                    return;
                }
                if (now < g_LogAt) return;
                g_LogAt = now + 500;
                const float* m = reinterpret_cast<const float*>(g_Enemy + offsets::Actor_Mtx);
                const float* p = reinterpret_cast<const float*>(g_Body + BodyToPos);
                Logging.Log("[coop] research: enemy at (%.2f %.2f %.2f), body (%.2f %.2f %.2f); started at x %.2f", m[3], m[7],
                            m[11], p[0], p[1], p[2], g_EnemyStart[0]);
            }
        }
    }

    void SetTest(int mode) { g_Mode = mode; }

    void Tick(s64 now) {
        if (!g_Mode || g_Done) return;
        float me[3];
        if (!GetLocalPlayerPos(me)) return;
        if (!g_At) g_At = now + 12000;
        if (now < g_At) return;
        if (g_Mode == 2) {
            EnemyTest(now, me);
            return;
        }
        g_Done = true;
        LoadRegions();
        uintptr_t g = Read<uintptr_t>(Base() + offsets::HavokPos_Global);
        uintptr_t a = g ? Read<uintptr_t>(g + offsets::HavokPos_A) : 0;
        uintptr_t b = a ? Read<uintptr_t>(a + offsets::HavokPos_B) : 0;
        uintptr_t actor = PlayerActor();
        Logging.Log("[coop] research: global obj %p, +0xf0 %p, +0xd28 %p (pos at +0x5e0); actor %p, actor+0x578 %p",
                    reinterpret_cast<void*>(g), reinterpret_cast<void*>(a), reinterpret_cast<void*>(b),
                    reinterpret_cast<void*>(actor), reinterpret_cast<void*>(Read<uintptr_t>(actor + 0x578)));
        if (!b || !actor) return;
        uintptr_t is = Read<uintptr_t>(actor + 0x578);
        uintptr_t body = b + 0x420;
        Logging.Log("[coop] research: body %p vtable %#lx; pointers to it from the InstanceSet:", reinterpret_cast<void*>(body),
                    Read<uintptr_t>(body) - Base());
        g_Found = 0;
        Search(is, body, 0, 3, 0x1000);
        Logging.Log("[coop] research: ... from the actor:");
        Search(actor, body, 0, 3, 0x2800);
        Logging.Log("[coop] research: done");
    }
}
