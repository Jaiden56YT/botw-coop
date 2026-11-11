#include "weapons.hpp"

#include <cstdio>
#include <cmath>
#include <cstring>

#include <lib.hpp>
#include <nn/fs.hpp>
#include <nn/os.hpp>

#include "../net/client.hpp"
#include "ghost.hpp"
#include "notify.hpp"
#include "offsets.hpp"
#include "pouch.hpp"
#include "player.hpp"

namespace coop::game::weapons {

    namespace {
        uintptr_t Base() { return exl::util::GetMainModuleInfo().m_Total.m_Start; }
        template<typename T> T Read(uintptr_t addr) { return *reinterpret_cast<const T*>(addr); }

        /* weapon-map.txt: "Weapon_Sword_070 Weapon_Sword_070\n", sorted. */
        char g_MapBuf[16 * 1024];
        const char* g_Weapon[320];
        const char* g_Model[320];
        int g_MapCount = 0;
        bool g_MapTried = false;

        void LoadMap() {
            g_MapTried = true;
            nn::fs::FileHandle file;
            if (nn::fs::OpenFile(&file, "content:/botw-coop/weapon-map.txt", nn::fs::OpenMode_Read) != 0) {
                Logging.Log("[coop] weapons: no weapon-map.txt");
                return;
            }
            long size = 0;
            nn::fs::GetFileSize(&size, file);
            if (size > static_cast<long>(sizeof(g_MapBuf)) - 1) size = sizeof(g_MapBuf) - 1;
            nn::fs::ReadFile(file, 0, g_MapBuf, static_cast<ulong>(size));
            nn::fs::CloseFile(file);
            g_MapBuf[size] = '\0';
            char* p = g_MapBuf;
            while (*p && g_MapCount < 320) {
                char* eol = std::strchr(p, '\n');
                if (eol) *eol = '\0';
                if (char* sp = std::strchr(p, ' ')) {
                    *sp = '\0';
                    g_Weapon[g_MapCount] = p;
                    g_Model[g_MapCount] = sp + 1;
                    g_MapCount++;
                }
                if (!eol) break;
                p = eol + 1;
            }
            Logging.Log("[coop] weapons: %d weapons can be shown on partners", g_MapCount);
        }

        using CsFn = void (*)(uintptr_t);
        using CsTryFn = bool (*)(uintptr_t);

        /* Bones a worn weapon can sit on: hands, back holsters, spine, waist. */
        constexpr const char* Bones[] = {"Weapon_R", "Weapon_L", "Pod_A", "Pod_B", "Pod_C", "Spine_2", "Waist"};
        constexpr int NumBones = sizeof(Bones) / sizeof(Bones[0]);
        Bind g_BoneBind[NumBones];
        alignas(16) u8 g_Scratch[0x800]; /* stand-in "actor" the bone matrix is written into */
        uintptr_t g_BoundTo = 0;         /* Link actor the bone binds were made for */

        /* World matrix of one of Link's bones (identity-offset bind, computed into a scratch actor). */
        bool BoneMatrix(uintptr_t link, int b, float (&out)[12]) {
            if (g_BoundTo != link) {
                for (int i = 0; i < NumBones; i++) {
                    uintptr_t info = reinterpret_cast<uintptr_t>(g_BoneBind[i].info);
                    if (!g_BoneBind[i].made) {
                        std::memset(g_BoneBind[i].info, 0, sizeof(g_BoneBind[i].info));
                        reinterpret_cast<void (*)(uintptr_t)>(Base() + offsets::ModelBindInfo_Ctor)(info);
                        g_BoneBind[i].made = true;
                    }
                    reinterpret_cast<bool (*)(uintptr_t, uintptr_t, bool)>(Base() + offsets::ActorLink_Acquire)(info + 8, link, false);
                    *reinterpret_cast<const char**>(info + offsets::ModelBindInfo_BoneName) = Bones[i];
                    *reinterpret_cast<u32*>(info + offsets::ModelBindInfo_BoneIdx) = 0xFFFFFFFF;
                }
                g_BoundTo = link;
            }
            uintptr_t scratch = reinterpret_cast<uintptr_t>(g_Scratch);
            if (!reinterpret_cast<bool (*)(uintptr_t, uintptr_t)>(Base() + offsets::ModelBindInfo_Calc)(
                    reinterpret_cast<uintptr_t>(g_BoneBind[b].info), scratch))
                return false;
            std::memcpy(out, g_Scratch + offsets::Actor_Mtx, sizeof(out));
            return true;
        }

        /* out = inverse(a) * b, for rigid 3x4 row-major matrices. */
        void Relative(const float (&a)[12], const float* b, float (&out)[12]) {
            for (int r = 0; r < 3; r++) {
                for (int c = 0; c < 3; c++)
                    out[r * 4 + c] = a[0 * 4 + r] * b[0 * 4 + c] + a[1 * 4 + r] * b[1 * 4 + c] + a[2 * 4 + r] * b[2 * 4 + c];
                float t[3] = {b[3] - a[3], b[7] - a[7], b[11] - a[11]};
                out[r * 4 + 3] = a[0 * 4 + r] * t[0] + a[1 * 4 + r] * t[1] + a[2 * 4 + r] * t[2];
            }
        }

        /* Weapons we just dropped or threw: given to a partner if they land near one. */
        struct Loose { uintptr_t actor; char name[32]; s64 since; };
        Loose g_Loose[4];
        uintptr_t g_Given[8];
        int g_GivenHead = 0;

        /* Actors we spawned for gifts we got: never ours to give back. */
        uintptr_t g_Received[16];
        int g_ReceivedHead = 0;

        void TrackLoose(uintptr_t actor, const char* name, bool was_ours, s64 now) {
            for (uintptr_t r : g_Received)
                if (r == actor) return;
            for (auto& l : g_Loose)
                if (l.actor == actor) return;
            for (uintptr_t given : g_Given)
                if (given == actor) return;
            if (!was_ours) return;
            for (auto& l : g_Loose) {
                if (l.actor) continue;
                l.actor = actor;
                std::strncpy(l.name, name, sizeof(l.name) - 1);
                l.since = now;
                Logging.Log("[coop] weapons: you let go of %s", name);
                return;
            }
        }

        /* Hands a loose weapon to the partner it landed near, and takes ours away. */
        void CheckGifts(s64 now, const uintptr_t* alive, int nalive, const uintptr_t* carried, int ncarried) {
            net::Peer peers[net::MaxPeers];
            net::GetPeers(peers);
            u32 area = LocalArea();
            for (auto& l : g_Loose) {
                if (!l.actor) continue;
                bool live = false;
                for (int i = 0; i < nalive; i++) live |= alive[i] == l.actor;
                bool held = false; /* back on a bone: it never really left */
                for (int i = 0; i < ncarried; i++) held |= carried[i] == l.actor;
                if (!live || held || now - l.since > 6000) {
                    l.actor = 0;
                    continue;
                }
                if (now - l.since < 400) continue; /* let it leave the hand first */
                const float* m = reinterpret_cast<const float*>(l.actor + offsets::Actor_Mtx);
                for (auto& p : peers) {
                    if (!p.present || !p.has_state || p.state.map_area != area) continue;
                    float dx = m[3] - p.state.pos[0], dy = m[7] - p.state.pos[1], dz = m[11] - p.state.pos[2];
                    if (dx * dx + dz * dz > 3.0f * 3.0f || dy * dy > 4.0f * 4.0f) continue;
                    /* Its bonus and wear, as they were in the inventory it left. */
                    pouch::WeaponInfo info{};
                    bool known = pouch::TakeRemoved(l.name, now, &info);
                    net::GiftBonus bonus{info.flags, info.value, info.durability};
                    net::SendGift(p.id, l.name, known ? &bonus : nullptr);
                    Logging.Log("[coop] weapons: gave %s to %s (bonus flags %x, value %d, durability %d%s)", l.name, p.name, info.flags,
                                info.value, info.durability, known ? "" : ": not known");
                    notify::Gift(false, p.name, l.name);
                    /* Ours is gone: it's theirs now. (Moving it away isn't enough:
                       its physics puts it back.) Never hand the same actor over twice. */
                    g_Given[g_GivenHead++ % 8] = l.actor;
                    uintptr_t vt = Read<uintptr_t>(l.actor);
                    reinterpret_cast<void (*)(uintptr_t, int)>(Read<uintptr_t>(vt + offsets::ActorVt_Kill))(l.actor, 0);
                    l.actor = 0;
                    break;
                }
            }
        }

        /* Gifts for us, waiting for the game thread (network thread fills). */
        constexpr int MaxIncoming = 32;
        char g_Incoming[MaxIncoming][32];
        char g_IncomingFrom[MaxIncoming][32];
        pouch::WeaponInfo g_IncomingInfo[MaxIncoming]; /* flags 0 and durability 0 = plain */
        int g_IncomingCount = 0;
        nn::os::MutexType g_GiftMutex;
        bool g_GiftMutexReady = false;

        /* What we last sent. */
        net::WeaponPose g_Sent[net::MaxWeapons];
        int g_SentCount = 0;
        s64 g_NextScan = 0, g_LastSend = 0;
        s64 g_InWorldSince = 0;
        int g_FewerScans = 0;

        void ScanLocal(s64 now) {
            if (now < g_NextScan) return;
            g_NextScan = now + 250;
            uintptr_t pinfo = Read<uintptr_t>(Base() + offsets::PlayerInfoInstance);
            uintptr_t link = pinfo ? Read<uintptr_t>(pinfo + offsets::PlayerInfo_PlayerActor) : 0;
            float me[3];
            if (!link || !GetLocalPlayerPos(me)) {
                g_InWorldSince = 0;
                return;
            }
            /* Just after a load, weapons aren't on their bones yet. */
            if (g_InWorldSince == 0) g_InWorldSince = now;
            bool settled = now - g_InWorldSince > 5000;

            /* Weapon actors near Link: the carried ones sit on a bone; loose ones
               with the name of something we just carried were dropped or thrown. */
            constexpr int MaxNear = 24;
            uintptr_t found[MaxNear];
            int nfound = 0;
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
                if (!name || std::strncmp(name, "Weapon_", 7) != 0 || nfound >= MaxNear) continue;
                const float* m = reinterpret_cast<const float*>(a + offsets::Actor_Mtx);
                float dx = m[3] - me[0], dy = m[7] - me[1], dz = m[11] - me[2];
                if (dx * dx + dy * dy + dz * dz > 100.0f) continue; /* 10 m: thrown ones fly */
                found[nfound++] = a;
            }
            reinterpret_cast<CsFn>(Base() + offsets::CriticalSection_Unlock)(cs);

            float bones[NumBones][12];
            bool have[NumBones];
            for (int b = 0; b < NumBones; b++) have[b] = BoneMatrix(link, b, bones[b]);
            /* No bone could be read: Link's skeleton isn't there for the asking
               right now. It comes and goes (for over a second at a time with two
               geared partners standing by, seen with three players), and the
               scan then says nothing about what he carries: sending "no
               weapons" made every partner take them off the puppet and make
               them again every few seconds. */
            bool any_bone = false;
            for (int b = 0; b < NumBones; b++) any_bone |= have[b];
            if (!any_bone) return;

            net::WeaponPose poses[net::MaxWeapons] = {};
            int n = 0;
            uintptr_t carried[MaxNear];
            int ncarried = 0;
            for (int i = 0; i < nfound; i++) {
                const float* m = reinterpret_cast<const float*>(found[i] + offsets::Actor_Mtx);
                int best = -1;
                float best_d = 1e9f;
                for (int b = 0; b < NumBones; b++) {
                    if (!have[b]) continue;
                    float dx = m[3] - bones[b][3], dy = m[7] - bones[b][7], dz = m[11] - bones[b][11];
                    float d = dx * dx + dy * dy + dz * dz;
                    if (d < best_d) { best_d = d; best = b; }
                }
                if (best < 0) continue;
                const char* wname = Read<const char*>(found[i] + offsets::BaseProc_NameStr);
                /* Stay on the bone it was on last time unless another is clearly
                   closer. A bow on Link's back sits between Pod_A and Spine_2,
                   and an idle animation flipped it between them about once a
                   second, with a new message (and a re-bind on every partner's
                   puppet) each time. */
                for (int k = 0; k < g_SentCount; k++) {
                    if (std::strcmp(g_Sent[k].name, wname) != 0) continue;
                    for (int b = 0; b < NumBones; b++) {
                        if (!have[b] || std::strcmp(Bones[b], g_Sent[k].bone) != 0) continue;
                        float dx = m[3] - bones[b][3], dy = m[7] - bones[b][7], dz = m[11] - bones[b][11];
                        float d = dx * dx + dy * dy + dz * dz;
                        if (d < 0.35f * 0.35f && best_d > d * 0.25f) best = b, best_d = d; /* under half the distance to switch */
                    }
                    break;
                }
                if (best_d > 0.35f * 0.35f) {
                    /* Loose. Ours if we carried this weapon a moment ago. */
                    bool was_ours = false;
                    for (int k = 0; k < g_SentCount; k++) was_ours |= std::strcmp(g_Sent[k].name, wname) == 0;
                    if (settled) TrackLoose(found[i], wname, was_ours, now);
                    continue;
                }
                carried[ncarried++] = found[i];
                if (n >= net::MaxWeapons) continue;
                net::WeaponPose& p = poses[n++];
                std::strncpy(p.name, Read<const char*>(found[i] + offsets::BaseProc_NameStr), sizeof(p.name) - 1);
                std::strncpy(p.bone, Bones[best], sizeof(p.bone) - 1);
                Relative(bones[best], m, p.offset);
            }

            CheckGifts(now, found, nfound, carried, ncarried);

            /* Fewer weapons than last time has to hold for three scans (0.75 s)
               before it counts: now and then a scan finds none on a bone (the
               bone matrices aren't there for a moment), and sending that made
               every partner take the weapons off the puppet and make them again. */
            if (n < g_SentCount && ++g_FewerScans < 3) return;
            g_FewerScans = 0;

            /* Send when a weapon changed, moved to another bone, or every 3 s. */
            bool changed = n != g_SentCount;
            for (int i = 0; i < n && !changed; i++) {
                if (std::strcmp(poses[i].name, g_Sent[i].name) || std::strcmp(poses[i].bone, g_Sent[i].bone)) changed = true;
                for (int k = 0; k < 12 && !changed; k++)
                    if (__builtin_fabsf(poses[i].offset[k] - g_Sent[i].offset[k]) > 0.02f) changed = true;
            }
            if (!changed && now - g_LastSend < 3000) return;
            if (changed)
                for (int i = 0; i < n; i++)
                    Logging.Log("[coop] weapons: %s on %s, offset t(%.3f %.3f %.3f) r0(%.2f %.2f %.2f)", poses[i].name, poses[i].bone,
                                poses[i].offset[3], poses[i].offset[7], poses[i].offset[11], poses[i].offset[0],
                                poses[i].offset[1], poses[i].offset[2]);
            std::memcpy(g_Sent, poses, sizeof(poses));
            g_SentCount = n;
            g_LastSend = now;
            net::SendWeapons(poses, n);
        }

        char g_TestSpec[64];
        bool g_TestDone = false;
        Bind g_TestBind;
        s64 g_TestAt = 0;
        uintptr_t g_TestActor = 0;
        s64 g_TestLogAt = 0;
    }

    bool WeaponActorFor(const char* weapon, char (&out)[64]) {
        if (!g_MapTried) LoadMap();
        int lo = 0, hi = g_MapCount - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            int c = std::strcmp(g_Weapon[mid], weapon);
            if (c == 0) {
                std::snprintf(out, sizeof(out), "CoopWeapon_%s", g_Model[mid]);
                return true;
            }
            if (c < 0) lo = mid + 1;
            else hi = mid - 1;
        }
        return false;
    }

    bool BindTo(Bind& slot, uintptr_t actor, uintptr_t parent, const char* bone, const float* offset) {
        if (!actor || !parent) return false;
        uintptr_t info = reinterpret_cast<uintptr_t>(slot.info);
        if (!slot.made) {
            std::memset(slot.info, 0, sizeof(slot.info));
            reinterpret_cast<void (*)(uintptr_t)>(Base() + offsets::ModelBindInfo_Ctor)(info);
            slot.made = true;
        }
        /* Parent: the link at +8 (ActorLinkConstDataAccess::acquire). */
        reinterpret_cast<bool (*)(uintptr_t, uintptr_t, bool)>(Base() + offsets::ActorLink_Acquire)(info + 8, parent, false);
        std::strncpy(slot.bone, bone, sizeof(slot.bone) - 1);
        *reinterpret_cast<const char**>(info + offsets::ModelBindInfo_BoneName) = slot.bone;
        *reinterpret_cast<u32*>(info + offsets::ModelBindInfo_BoneIdx) = 0xFFFFFFFF; /* look the bone up again */
        float* m = reinterpret_cast<float*>(info + offsets::ModelBindInfo_Offset);
        if (offset) {
            std::memcpy(m, offset, 12 * sizeof(float));
        } else {
            const float ident[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
            std::memcpy(m, ident, sizeof(ident));
        }
        reinterpret_cast<void (*)(uintptr_t, uintptr_t)>(Base() + offsets::Actor_SetModelBindInfo)(actor, info);
        return true;
    }

    void Init() {
        if (!g_GiftMutexReady) {
            nn::os::InitializeMutex(&g_GiftMutex, false, 0);
            g_GiftMutexReady = true;
        }
    }

    void OnGift(const char* from, const char* weapon, const net::GiftBonus* bonus) {
        if (!g_GiftMutexReady) return;
        nn::os::LockMutex(&g_GiftMutex);
        if (g_IncomingCount < MaxIncoming) {
            std::strncpy(g_Incoming[g_IncomingCount], weapon, 31);
            std::strncpy(g_IncomingFrom[g_IncomingCount], from, 31);
            g_IncomingInfo[g_IncomingCount] = bonus ? pouch::WeaponInfo{bonus->flags, bonus->value, bonus->durability}
                                                    : pouch::WeaponInfo{0, 0, 0};
            g_IncomingCount++;
        }
        nn::os::UnlockMutex(&g_GiftMutex);
    }

    void TrackDropped(uintptr_t actor, const char* name, s64 now) { TrackLoose(actor, name, true, now); }

    bool IsGiftActor(uintptr_t actor) {
        for (uintptr_t r : g_Received)
            if (r == actor) return true;
        return false;
    }

    int LocalPoses(net::WeaponPose (&out)[net::MaxWeapons]) {
        std::memcpy(out, g_Sent, sizeof(out));
        return g_SentCount;
    }

    void SetTest(const char* spec) { std::strncpy(g_TestSpec, spec ? spec : "", sizeof(g_TestSpec) - 1); }

    void Update(Bind& slot, uintptr_t actor) {
        if (!slot.made || !actor) return;
        reinterpret_cast<bool (*)(uintptr_t, uintptr_t)>(Base() + offsets::ModelBindInfo_Calc)(
            reinterpret_cast<uintptr_t>(slot.info), actor);
    }

    void Tick(s64 now) {
        ScanLocal(now);
        /* A partner gave us something: it lands a step ahead of us, a few per
           tick (a dropped stack arrives as one gift per item). */
        float me[3], rot = 0;
        /* Gifts wait while actors can't be created (a loading screen, a menu). */
        if (g_GiftMutexReady && g_IncomingCount > 0 && SafeToSpawn(now) && GetLocalPlayer(me, &rot)) {
            constexpr int PerTick = 4;
            char gifts[PerTick][32], from[PerTick][32];
            pouch::WeaponInfo info[PerTick];
            nn::os::LockMutex(&g_GiftMutex);
            int n = g_IncomingCount < PerTick ? g_IncomingCount : PerTick;
            std::memcpy(gifts, g_Incoming, n * 32);
            std::memcpy(from, g_IncomingFrom, n * 32);
            std::memcpy(info, g_IncomingInfo, n * sizeof(pouch::WeaponInfo));
            std::memmove(g_Incoming, g_Incoming + n, (g_IncomingCount - n) * 32);
            std::memmove(g_IncomingFrom, g_IncomingFrom + n, (g_IncomingCount - n) * 32);
            std::memmove(g_IncomingInfo, g_IncomingInfo + n, (g_IncomingCount - n) * sizeof(pouch::WeaponInfo));
            g_IncomingCount -= n;
            nn::os::UnlockMutex(&g_GiftMutex);
            for (int i = 0; i < n; i++) {
                bool told = false;
                /* A weapon with a bonus goes straight into the inventory: only
                   an inventory item can carry the bonus (one lying on the
                   ground would be picked up plain). No room there: it lands
                   at your feet like any other, without the bonus. */
                if (info[i].flags && info[i].durability > 0) {
                    char what[40], shown[64];
                    pouch::Describe(info[i], what, sizeof(what));
                    if (!notify::DisplayName(gifts[i], shown, sizeof(shown))) std::snprintf(shown, sizeof(shown), "%.31s", gifts[i]);
                    if (pouch::Give(gifts[i], info[i])) {
                        Logging.Log("[coop] gifts: %s gave you %s with %s (flags %x, value %d, durability %d), now in your inventory",
                                    from[i], gifts[i], what, info[i].flags, info[i].value, info[i].durability);
                        notify::Show("%s gave you %s (%s)", from[i], shown, what);
                        continue;
                    }
                    Logging.Log("[coop] gifts: no room in the inventory for %s; it lands at your feet without its bonus", gifts[i]);
                    notify::Show("%s gave you %s, but there's no room: it's at your feet, without its %s", from[i], shown, what);
                    told = true;
                }
                /* Spread around the spot ahead so a stack doesn't land in one pile. */
                float ang = rot + 0.35f * static_cast<float>((g_ReceivedHead % 5) - 2);
                float at[3] = {me[0] + std::sin(ang) * 1.5f, me[1] + 0.5f, me[2] + std::cos(ang) * 1.5f};
                uintptr_t a = SpawnActor(gifts[i], at, rot);
                g_Received[g_ReceivedHead++ % 16] = a;
                Logging.Log("[coop] gifts: %s gave you %s%s", from[i], gifts[i], a ? "" : " (couldn't spawn it)");
                if (a && !told) notify::Gift(true, from[i], gifts[i]);
            }
        }
        if (g_TestActor) Update(g_TestBind, g_TestActor);
        if (g_TestActor && now > g_TestLogAt) {
            g_TestLogAt = now + 2000;
            const float* m = reinterpret_cast<const float*>(g_TestActor + offsets::Actor_Mtx);
            uintptr_t info = reinterpret_cast<uintptr_t>(g_TestBind.info);
            Logging.Log("[coop] weapons test: actor at (%.2f, %.2f, %.2f), +4d0 %s, flags520 %x, bone idx %04x/%04x",
                        m[3], m[7], m[11], Read<uintptr_t>(g_TestActor + 0x4d0) == info ? "ours" : "OTHER",
                        Read<u32>(g_TestActor + 0x520), Read<u16>(info + 0x60), Read<u16>(info + 0x62));
        }
        if (!g_TestSpec[0] || g_TestDone) return;
        if (!GetLocalPlayerPos(me)) return;
        if (g_TestAt == 0) g_TestAt = now + 4000;
        if (now < g_TestAt) return;
        g_TestDone = true;
        char spec[64];
        std::memcpy(spec, g_TestSpec, sizeof(spec));
        char* colon = std::strchr(spec, ':');
        const char* bone = colon ? colon + 1 : "Weapon_R";
        if (colon) *colon = '\0';
        char actor_name[64];
        if (!WeaponActorFor(spec, actor_name)) {
            Logging.Log("[coop] weapons test: unknown weapon %s", spec);
            return;
        }
        uintptr_t info = Read<uintptr_t>(Base() + offsets::PlayerInfoInstance);
        uintptr_t link = info ? Read<uintptr_t>(info + offsets::PlayerInfo_PlayerActor) : 0;
        uintptr_t a = SpawnActor(actor_name, me, 0);
        g_TestActor = a;
        bool ok = BindTo(g_TestBind, a, link, bone, nullptr);
        Logging.Log("[coop] weapons test: %s %p bound to Link's %s: %d", actor_name, reinterpret_cast<void*>(a), bone, ok);
    }
}
