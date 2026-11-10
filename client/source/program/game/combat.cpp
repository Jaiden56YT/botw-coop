#include "combat.hpp"

#include <cstring>

#include <lib.hpp>
#include <nn/os.hpp>

#include "../net/client.hpp"
#include "offsets.hpp"
#include "player.hpp"

namespace coop::game::combat {

    namespace {
        using CsFn = void (*)(uintptr_t);
        using CsTryFn = bool (*)(uintptr_t);
        using GetLifeFn = s32* (*)(uintptr_t);
        using GetMaxLifeFn = s32 (*)(uintptr_t);
        using KillFn = void (*)(uintptr_t, int);

        constexpr s64 ScanMs = 100;

        uintptr_t Base() { return exl::util::GetMainModuleInfo().m_Total.m_Start; }
        template<typename T> T Read(uintptr_t addr) { return *reinterpret_cast<const T*>(addr); }

        uintptr_t ActorCreator() {
            uintptr_t slot = Read<uintptr_t>(Base() + offsets::Got_ActorCreatorInstance);
            return slot ? Read<uintptr_t>(slot) : 0;
        }

        const char* Name(uintptr_t actor) {
            const char* s = Read<const char*>(actor + offsets::BaseProc_NameStr);
            return s ? s : "";
        }

        bool IsEnemy(uintptr_t actor) { return std::strncmp(Name(actor), "Enemy_", 6) == 0; }

        template<typename Fn> uintptr_t Virtual(uintptr_t actor, uintptr_t off) {
            return Read<uintptr_t>(Read<uintptr_t>(actor) + off);
        }

        s32* LifePtr(uintptr_t actor) {
            s32* p = reinterpret_cast<GetLifeFn>(Virtual<GetLifeFn>(actor, offsets::ActorVt_GetLife))(actor);
            return p;
        }

        s32 MaxLife(uintptr_t actor) {
            return reinterpret_cast<GetMaxLifeFn>(Virtual<GetMaxLifeFn>(actor, offsets::ActorVt_GetMaxLife))(actor);
        }

        /* Calls fn(actor) for every live actor, under ActorCreator's list lock.
           Returns false if the lock was busy (try again next tick). */
        template<typename Fn> bool ForEachActor(Fn&& fn) {
            uintptr_t creator = ActorCreator();
            if (!creator) return false;
            uintptr_t cs = creator + offsets::ActorCreator_ActorListCS;
            if (!reinterpret_cast<CsTryFn>(Base() + offsets::CriticalSection_TryLock)(cs)) return false;
            uintptr_t head = creator + offsets::ActorCreator_ActorList;
            s32 node_off = Read<s32>(creator + offsets::ActorCreator_ActorListOffset);
            int guard = 0;
            for (uintptr_t node = Read<uintptr_t>(head + 8); node != head && node != 0 && guard < 20000; guard++) {
                fn(node - node_off);
                node = Read<uintptr_t>(node + 8);
            }
            reinterpret_cast<CsFn>(Base() + offsets::CriticalSection_Unlock)(cs);
            return true;
        }

        /* Last life seen for each placed enemy (open addressing on HashId). */
        struct Known { u32 hash; s32 life; };
        constexpr int TableSize = 2048;
        Known g_Table[TableSize];
        u32 g_TableArea = 0;

        Known* Lookup(u32 hash, bool insert) {
            u32 i = (hash * 2654435761u) % TableSize;
            for (int n = 0; n < TableSize; n++, i = (i + 1) % TableSize) {
                if (g_Table[i].hash == hash) return &g_Table[i];
                if (g_Table[i].hash == 0) {
                    if (!insert) return nullptr;
                    g_Table[i] = {hash, -1};
                    return &g_Table[i];
                }
            }
            return nullptr;
        }

        /* Partner updates waiting for the game thread (network thread fills it). */
        struct Remote { u32 area, hash; s32 life; };
        constexpr int RemoteSize = 256;
        Remote g_Remote[RemoteSize];
        int g_RemoteCount = 0;
        nn::os::MutexType g_Mutex;
        bool g_MutexReady = false;

        bool g_LogNearby = false;
        bool g_Logged = false;
        s64 g_NextLog = 0;
        s64 g_NextScan = 0;
    }

    void SetLogNearby(bool on) { g_LogNearby = on; }

    /* Call once before the network thread starts. */
    void Init() {
        if (!g_MutexReady) {
            nn::os::InitializeMutex(&g_Mutex, false, 0);
            g_MutexReady = true;
        }
    }

    void Tick(s64 now) {
        if (now < g_NextScan) return;
        g_NextScan = now + ScanMs;
        float me[3];
        if (!GetLocalPlayerPos(me)) return;
        u32 area = LocalArea();
        if (area == 0) return;
        if (area != g_TableArea) {
            /* New area or reload: enemies there are new to us. */
            std::memset(g_Table, 0, sizeof(g_Table));
            g_TableArea = area;
        }

        /* Partner updates for this area. */
        Remote remote[RemoteSize];
        int nremote = 0;
        if (g_MutexReady) {
            nn::os::LockMutex(&g_Mutex);
            for (int i = 0; i < g_RemoteCount; i++)
                if (g_Remote[i].area == area) remote[nremote++] = g_Remote[i];
            g_RemoteCount = 0;
            nn::os::UnlockMutex(&g_Mutex);
        }

        constexpr int MaxOut = 64;
        u32 out_hash[MaxOut];
        s32 out_life[MaxOut];
        int nout = 0;
        uintptr_t kill[16];
        int nkill = 0;
        int applied = 0;
        bool scanned = ForEachActor([&](uintptr_t a) {
            u32 hash = Read<u32>(a + offsets::Actor_HashId);
            if (hash == 0 || !IsEnemy(a)) return;
            s32* life = LifePtr(a);
            if (!life) return;
            Known* k = Lookup(hash, true);
            if (!k) return;
            s32 cur = *life;
            for (int i = 0; i < nremote; i++) {
                if (remote[i].hash != hash) continue;
                remote[i].hash = 0; /* used */
                if (remote[i].life < cur) {
                    /* A partner hurt this enemy: it's hurt here too. */
                    cur = remote[i].life < 0 ? 0 : remote[i].life;
                    *life = cur;
                    applied++;
                    if (cur == 0 && nkill < 16) kill[nkill++] = a;
                }
                k->life = cur;
            }
            if (k->life < 0) {
                k->life = cur; /* first sighting */
            } else if (cur < k->life) {
                /* Hurt here (by us, or anything in our world): tell the others. */
                if (nout < MaxOut) {
                    out_hash[nout] = hash;
                    out_life[nout] = cur;
                    nout++;
                }
                k->life = cur;
            } else if (cur > k->life) {
                k->life = cur; /* healed or respawned (Blood Moon) */
            }
        });
        if (!scanned) {
            /* Lock busy: keep the partner updates for next time. */
            for (int i = 0; i < nremote; i++)
                if (remote[i].hash) OnRemoteLife(remote[i].area, remote[i].hash, remote[i].life);
            return;
        }
        for (int i = 0; i < nout; i++) {
            net::SendEnemyLife(area, out_hash[i], out_life[i]);
            if (g_LogNearby) Logging.Log("[coop] combat: enemy %08x hurt here, life %d", out_hash[i], out_life[i]);
        }
        if (applied && g_LogNearby) Logging.Log("[coop] combat: applied %d partner hits", applied);
        /* Enemies a partner killed: kill them here, outside the actor-list lock. */
        for (int i = 0; i < nkill; i++) {
            uintptr_t a = kill[i];
            bool alive = false;
            ForEachActor([&](uintptr_t b) { alive |= b == a; });
            if (!alive) continue;
            reinterpret_cast<KillFn>(Virtual<KillFn>(a, offsets::ActorVt_Kill))(a, 0);
            Logging.Log("[coop] combat: %s killed by a partner", Name(a));
        }

        if (g_LogNearby && now >= g_NextLog) {
            g_NextLog = now + 10000;
            int actors = 0, enemies = 0;
            g_Logged = ForEachActor([&](uintptr_t a) {
                actors++;
                if (!IsEnemy(a)) return;
                enemies++;
                if (Read<u32>(a + offsets::Actor_HashId) == 0) return;
                const float* m = reinterpret_cast<const float*>(a + offsets::Actor_Mtx);
                float dx = m[3] - me[0], dz = m[11] - me[2];
                if (dx * dx + dz * dz > 1000.0f * 1000.0f || enemies > 25) return;
                s32* life = LifePtr(a);
                Logging.Log("[coop] combat: %s hash %08x life %d/%d (ptr +%lx) at (%.0f, %.0f, %.0f)", Name(a),
                            Read<u32>(a + offsets::Actor_HashId), life ? *life : -1, MaxLife(a),
                            life ? reinterpret_cast<uintptr_t>(life) - a : 0, m[3], m[7], m[11]);
            });
            if (g_Logged) {
                Logging.Log("[coop] combat: %d actors loaded, %d enemies", actors, enemies);
                uintptr_t info = Read<uintptr_t>(Base() + offsets::PlayerInfoInstance);
                uintptr_t link = info ? Read<uintptr_t>(info + offsets::PlayerInfo_PlayerActor) : 0;
                if (link) {
                    s32* life = LifePtr(link);
                    Logging.Log("[coop] combat: Link (%s) life %d/%d (ptr +%lx)", Name(link), life ? *life : -1, MaxLife(link),
                                life ? reinterpret_cast<uintptr_t>(life) - link : 0);
                }
            }
        }
    }

    void OnRemoteLife(u32 area, u32 hash, s32 life) {
        if (!g_MutexReady || hash == 0) return;
        nn::os::LockMutex(&g_Mutex);
        /* Keep only the lowest life per enemy. */
        bool merged = false;
        for (int i = 0; i < g_RemoteCount && !merged; i++) {
            if (g_Remote[i].hash == hash && g_Remote[i].area == area) {
                if (life < g_Remote[i].life) g_Remote[i].life = life;
                merged = true;
            }
        }
        if (!merged && g_RemoteCount < RemoteSize) g_Remote[g_RemoteCount++] = {area, hash, life};
        nn::os::UnlockMutex(&g_Mutex);
    }
}
