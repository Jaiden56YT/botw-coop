#include "client.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <lib.hpp>
#include <nn/os.hpp>
#include <nn/socket.hpp>
#include <program/filelog.hpp>

#include "../config.hpp"
#include "../game/player.hpp"
#include "../game/ghost.hpp"
#include "../game/autostart.hpp"
#include "../game/world.hpp"
#include "../game/progress.hpp"
#include "../game/combat.hpp"
#include "../game/revive.hpp"
#include "../game/weapons.hpp"
#include "../game/items.hpp"
#include "../game/warp.hpp"
#include "../game/notify.hpp"
#include "../game/research.hpp"
#include "../game/meals.hpp"
#include "../game/map.hpp"

namespace coop::net {

    namespace {

        /* nn::socket needs a page-aligned transfer memory pool. */
        constexpr size_t SocketPoolSize = 0x600000;
        constexpr size_t SocketAllocPoolSize = 0x20000;
        alignas(0x1000) u8 g_SocketPool[SocketPoolSize];

        constexpr size_t ThreadStackSize = 0x8000;
        alignas(0x1000) u8 g_ThreadStack[ThreadStackSize];
        nn::os::ThreadType g_Thread;

        Settings g_Config;
        u8 g_BuildId[16];

        /* Shared between the game thread and the network thread. */
        nn::os::MutexType g_Mutex;
        bool g_Connected = false;
        u8 g_MyId = 0;
        u8 g_HostId = 0;
        Peer g_Peers[MaxPeers];

        bool g_WorldDirty = false;
        float g_WorldTime = 0;
        s32 g_WorldDays = 0;
        u8 g_WorldWeather = 0xFF, g_WorldClimate = 0, g_WorldBloodyDay = 0xFF;
        float g_WorldBloodyTimer = 0;

        constexpr int FlagQueueSize = 2048;
        const char* g_FlagQueue[FlagQueueSize];
        bool g_FlagLive[FlagQueueSize];
        int g_FlagHead = 0, g_FlagCount = 0;

        struct EnemyLifeMsg { u32 area, hash; s32 life; };
        constexpr int EnemyQueueSize = 256;
        EnemyLifeMsg g_EnemyQueue[EnemyQueueSize];
        int g_EnemyHead = 0, g_EnemyCount = 0;

        game::enemies::Pose g_EnemyPosOut[game::enemies::MaxPerMessage];
        int g_EnemyPosCount = 0;
        u32 g_EnemyPosArea = 0;

        bool g_RevivedPending = false;
        u32 g_HealPending = 0;
        bool g_RejectShown = false;
        SharedStamp g_StampsOut[MaxSharedStamps];
        int g_StampsOutCount = -1; /* -1 = nothing to send */
        struct DownedOut { u8 state; float pos[3]; u8 left; };
        DownedOut g_DownedOut[4];
        int g_DownedOutCount = 0;
        char g_GearOut[3][32];
        WeaponPose g_WeaponsOut[MaxWeapons];
        int g_WeaponsOutCount = 0;
        bool g_WeaponsPending = false;
        struct GiftOut { u8 to; char name[32]; bool has_bonus; GiftBonus bonus; };
        GiftOut g_GiftsOut[4];
        int g_GiftsOutCount = 0;
        bool g_GearPending = false;

        bool g_StateDirty = false;
        PlayerState g_OutState;

        constexpr int LogSlots = 16;
        constexpr int LogLen = 200;
        char g_LogQueue[LogSlots][LogLen];
        int g_LogHead = 0, g_LogCount = 0;

        /* Network-thread only. */
        s32 g_Fd = -1;
        u8 g_RecvBuf[MaxMessageSize + 8];
        size_t g_RecvLen = 0;
        u8 g_SendBuf[1024];

        struct ScopedLock {
            ScopedLock() { nn::os::LockMutex(&g_Mutex); }
            ~ScopedLock() { nn::os::UnlockMutex(&g_Mutex); }
        };

        /* Every wait on this thread is also a chance to write the log file. */
        void Sleep(s64 ms) {
            filelog::Flush();
            nn::os::SleepThread(nn::TimeSpan::FromMilliSeconds(ms));
        }

        s64 NowMs() {
            return nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
        }

        bool SendAll(const void* data, size_t len) {
            auto* p = static_cast<const u8*>(data);
            while (len > 0) {
                s64 n = nn::socket::Send(g_Fd, p, len, nn::socket::MsgFlag::Msg_None);
                if (n <= 0) return false;
                p += n;
                len -= n;
            }
            return true;
        }

        bool SendMsg(Writer& w) {
            size_t len = w.Finish();
            return len != 0 && SendAll(g_SendBuf, len);
        }

        /* For the "can't reach the server" banner (a Switch shows no log). */
        int g_FailedTries = 0;
        bool g_FailShown = false;

        void Disconnect(const char* why) {
            if (g_Fd >= 0) {
                nn::socket::Close(g_Fd);
                g_Fd = -1;
            }
            g_RecvLen = 0;
            ScopedLock lock;
            Logging.Log("[coop] %s: %s", g_Connected ? "disconnected" : "connection failed", why);
            g_Connected = false;
            g_MyId = g_HostId = 0;
            std::memset(g_Peers, 0, sizeof(g_Peers));
        }

        bool Connect() {
            nn::socket::SockAddr addr{};
            addr.len = sizeof(addr);
            addr.family = nn::socket::Family::Af_Inet;
            addr.port = nn::socket::InetHtons(g_Config.port);
            if (nn::socket::InetAton(g_Config.host, &addr.addr) == 0) {
                Logging.Log("[coop] bad server address '%s'", g_Config.host);
                return false;
            }

            g_Fd = nn::socket::Socket(nn::socket::Family::Af_Inet, nn::socket::Type::Sock_Stream,
                                      nn::socket::Protocol::IpProto_Tcp);
            if (g_Fd < 0) return false;

            if (nn::socket::Connect(g_Fd, &addr, sizeof(addr)) != 0) {
                nn::socket::Close(g_Fd);
                g_Fd = -1;
                return false;
            }

            int one = 1;
            nn::socket::SetSockOpt(g_Fd, nn::socket::Level::Sol_Tcp, nn::socket::Option::Tcp_NoDelay, &one, sizeof(one));

            Writer w(g_SendBuf, sizeof(g_SendBuf), MsgType::Hello);
            w.U16(ProtocolVersion);
            w.Bytes(g_BuildId, 16);
            w.Str(g_Config.name);
            w.Str(g_Config.password);
            return SendMsg(w);
        }

        Peer* FindPeer(u8 id) {
            for (auto& p : g_Peers)
                if (p.present && p.id == id) return &p;
            return nullptr;
        }

        Peer* AddPeer(u8 id, std::string_view name) {
            Peer* slot = FindPeer(id);
            for (auto& p : g_Peers)
                if (!slot && !p.present) slot = &p;
            if (!slot) return nullptr;
            *slot = {};
            slot->present = true;
            slot->id = id;
            size_t n = name.size() < sizeof(slot->name) - 1 ? name.size() : sizeof(slot->name) - 1;
            std::memcpy(slot->name, name.data(), n);
            return slot;
        }

        /* Returns false if the connection should be dropped. */
        bool Handle(MsgType type, Reader r) {
            switch (type) {
                case MsgType::Welcome: {
                    ScopedLock lock;
                    g_MyId = r.Get<u8>();
                    g_HostId = r.Get<u8>();
                    u8 count = r.Get<u8>();
                    char first[32] = "a partner";
                    for (u8 i = 0; i < count && r.Ok(); i++) {
                        u8 id = r.Get<u8>();
                        Peer* p = AddPeer(id, r.Str());
                        if (i == 0 && p) std::snprintf(first, sizeof(first), "%s", p->name);
                    }
                    g_Connected = true;
                    Logging.Log("[coop] joined as player %d (%s), %d other(s) here",
                                g_MyId, g_MyId == g_HostId ? "host" : "guest", count);
                    /* Progress merges both ways: the others send us theirs (they see
                       PlayerJoined), and we send them ours. */
                    if (count > 0) game::progress::OnPlayerJoined();
                    if (count == 1) game::notify::Show("Co-op: %s is here", first);
                    else if (count > 1) game::notify::Show("Co-op: %d partners are here", count);
                    else game::notify::Show("Co-op: connected. Waiting for a partner");
                    g_FailedTries = 0;
                    g_FailShown = false;
                    return r.Ok();
                }
                case MsgType::Reject: {
                    auto reason = r.Str();
                    Logging.Log("[coop] server refused: %.*s", (int)reason.size(), reason.data());
                    /* Say it on screen once (we keep retrying, e.g. until both sides update). */
                    if (!g_RejectShown) {
                        g_RejectShown = true;
                        game::notify::Show("Co-op is off: %.*s", (int)reason.size(), reason.data());
                    }
                    return false;
                }
                case MsgType::PlayerJoined: {
                    ScopedLock lock;
                    u8 id = r.Get<u8>();
                    auto name = r.Str();
                    AddPeer(id, name);
                    Logging.Log("[coop] %.*s joined", (int)name.size(), name.data());
                    game::notify::Show("%.*s joined", (int)name.size(), name.data());
                    game::progress::OnPlayerJoined();
                    return r.Ok();
                }
                case MsgType::PlayerLeft: {
                    ScopedLock lock;
                    if (Peer* p = FindPeer(r.Get<u8>())) {
                        Logging.Log("[coop] %s left", p->name);
                        game::notify::Show("%s left", p->name);
                        p->present = false;
                    }
                    return true;
                }
                case MsgType::HostChanged: {
                    ScopedLock lock;
                    g_HostId = r.Get<u8>();
                    return true;
                }
                case MsgType::PlayerState: {
                    u8 id = r.Get<u8>();
                    const u8* raw = r.Take(sizeof(PlayerState));
                    if (!raw) return false;
                    ScopedLock lock;
                    if (Peer* p = FindPeer(id)) {
                        std::memcpy(&p->state, raw, sizeof(PlayerState));
                        p->has_state = true;
                        p->state_tick = nn::os::GetSystemTick().GetInt64Value();
                    }
                    return true;
                }
                case MsgType::WorldState: {
                    u8 from = r.Get<u8>();
                    float t = r.Get<float>();
                    s32 d = r.Get<s32>();
                    if (!r.Ok()) return false;
                    u8 weather = 0xFF, climate = 0, bloody = 0xFF;
                    float bloody_timer = 0;
                    if (r.Left() >= 2) {
                        weather = r.Get<u8>();
                        climate = r.Get<u8>();
                    }
                    if (r.Left() >= 5) {
                        bloody = r.Get<u8>();
                        bloody_timer = r.Get<float>();
                    }
                    bool from_host;
                    {
                        ScopedLock lock;
                        from_host = from == g_HostId;
                    }
                    if (from_host) game::OnHostWorldState({t, d, weather, climate, bloody, bloody_timer});
                    return true;
                }
                case MsgType::Flag: {
                    u8 from = r.Get<u8>();
                    u8 value = r.Get<u8>();
                    auto name = r.Str();
                    if (!r.Ok()) return false;
                    /* value: 1 = earned just now, 2 = catch-up for a new player */
                    if (value) game::progress::OnRemoteFlag(from, name.data(), name.size(), value == 1);
                    return true;
                }
                case MsgType::EnemyLife: {
                    r.Get<u8>(); /* sender id */
                    u32 area = r.Get<u32>();
                    u32 hash = r.Get<u32>();
                    s32 life = r.Get<s32>();
                    if (!r.Ok()) return false;
                    game::combat::OnRemoteLife(area, hash, life);
                    return true;
                }
                case MsgType::EnemyPos: {
                    u8 from = r.Get<u8>();
                    u32 area = r.Get<u32>();
                    u8 count = r.Get<u8>();
                    game::enemies::Pose poses[game::enemies::MaxPerMessage];
                    int n = 0;
                    for (u8 i = 0; i < count && r.Ok(); i++) {
                        game::enemies::Pose p;
                        p.hash = r.Get<u32>();
                        for (float& f : p.pos) f = r.Get<float>();
                        p.rot_y = r.Get<float>();
                        if (n < game::enemies::MaxPerMessage) poses[n++] = p;
                    }
                    if (!r.Ok()) return false;
                    game::enemies::OnRemote(from, area, poses, n);
                    return true;
                }
                case MsgType::Gear: {
                    u8 from = r.Get<u8>();
                    std::string_view parts[3];
                    for (auto& part : parts) part = r.Str();
                    if (!r.Ok()) return false;
                    ScopedLock lock;
                    if (Peer* p = FindPeer(from)) {
                        for (int i = 0; i < 3; i++) {
                            size_t n = parts[i].size() < 31 ? parts[i].size() : 31;
                            std::memcpy(p->gear[i], parts[i].data(), n);
                            p->gear[i][n] = '\0';
                        }
                    }
                    return true;
                }
                case MsgType::Gift: {
                    u8 from = r.Get<u8>();
                    u8 to = r.Get<u8>();
                    auto name = r.Str();
                    if (!r.Ok()) return false;
                    char weapon[32] = {};
                    std::memcpy(weapon, name.data(), name.size() < 31 ? name.size() : 31);
                    char who[32] = "a partner";
                    {
                        ScopedLock lock;
                        if (to != g_MyId) return true;
                        if (Peer* p = FindPeer(from)) std::memcpy(who, p->name, sizeof(who));
                    }
                    GiftBonus bonus{};
                    bool has_bonus = r.Left() >= sizeof(GiftBonus);
                    if (has_bonus) bonus = r.Get<GiftBonus>();
                    game::weapons::OnGift(who, weapon, has_bonus ? &bonus : nullptr);
                    return true;
                }
                case MsgType::Weapons: {
                    u8 from = r.Get<u8>();
                    u8 count = r.Get<u8>();
                    WeaponPose poses[MaxWeapons] = {};
                    int n = 0;
                    for (u8 i = 0; i < count && r.Ok(); i++) {
                        auto name = r.Str();
                        auto bone = r.Str();
                        float off[12];
                        for (float& f : off) f = r.Get<float>();
                        if (!r.Ok() || n >= MaxWeapons) continue;
                        WeaponPose& p = poses[n++];
                        std::memcpy(p.name, name.data(), name.size() < 31 ? name.size() : 31);
                        std::memcpy(p.bone, bone.data(), bone.size() < 15 ? bone.size() : 15);
                        std::memcpy(p.offset, off, sizeof(off));
                    }
                    if (!r.Ok()) return false;
                    ScopedLock lock;
                    if (Peer* p = FindPeer(from)) {
                        std::memcpy(p->weapons, poses, sizeof(poses));
                        p->weapon_count = static_cast<u8>(n);
                    }
                    return true;
                }
                case MsgType::Stamps: {
                    u8 from = r.Get<u8>();
                    u8 count = r.Get<u8>();
                    SharedStamp list[MaxSharedStamps];
                    int n = 0;
                    for (u8 i = 0; i < count && r.Ok(); i++) {
                        SharedStamp st{};
                        st.icon = r.Get<u8>();
                        for (float& f : st.pos) f = r.Get<float>();
                        if (n < MaxSharedStamps) list[n++] = st;
                    }
                    if (!r.Ok()) return false;
                    game::map::OnRemoteStamps(from, list, n);
                    return true;
                }
                case MsgType::Downed: {
                    u8 from = r.Get<u8>();
                    u8 state = r.Get<u8>();
                    float pos[3];
                    for (float& f : pos) f = r.Get<float>();
                    u8 left = r.Get<u8>();
                    if (!r.Ok()) return false;
                    game::revive::OnPartnerDowned(from, state, pos, left);
                    return true;
                }
                case MsgType::Heal: {
                    u8 from = r.Get<u8>();
                    u16 q = r.Get<u16>();
                    if (!r.Ok()) return false;
                    game::meals::OnPartnerHeal(from, q);
                    return true;
                }
                case MsgType::Revived: {
                    u8 from = r.Get<u8>();
                    if (!r.Ok()) return false;
                    game::revive::OnPartnerRevived(from);
                    return true;
                }
                case MsgType::Ping: {
                    Writer w(g_SendBuf, sizeof(g_SendBuf), MsgType::Pong);
                    w.U64(r.Get<u64>());
                    return SendMsg(w);
                }
                case MsgType::Pong:
                    return true;
                default:
                    return true; /* Unknown types are skipped for forward compatibility. */
            }
        }

        s32 g_LastError = 0;

        /* Waits up to 5 ms for data, then reads what's there and handles every
           complete message. Sets *idle when nothing arrived. We never make a
           blocking receive: Ryujinx doesn't honour receive timeouts. */
        bool Pump(bool* idle) {
            *idle = false;
            nn::socket::PollFd pfd = {g_Fd, nn::socket::PollIn, 0};
            s32 ready = nn::socket::Poll(&pfd, 1, 5);
            if (ready < 0) {
                /* Ryujinx-Nextendo reports a plain poll timeout as -1/ETIMEDOUT (110
                   Linux, 60 BSD) rather than 0. Either way it just means no data. */
                s32 err = nn::socket::GetLastError();
                if (err == 110 || err == 60) { *idle = true; return true; }
                g_LastError = err;
                return false;
            }
            if (ready == 0) { *idle = true; return true; }
            if (pfd.revents & (nn::socket::PollErr | nn::socket::PollNval)) { g_LastError = -1; return false; }

            /* Read in small chunks: Ryujinx can hand back an empty buffer (and so a
               0-byte read) when the game passes one much larger than this. */
            size_t room = sizeof(g_RecvBuf) - g_RecvLen;
            if (room > 4096) room = 4096;
            s64 n = nn::socket::Recv(g_Fd, g_RecvBuf + g_RecvLen, room, nn::socket::MsgFlag::Msg_DontWait);
            if (n == 0) { g_LastError = 0; return false; } /* readable + 0 bytes = peer closed */
            if (n < 0) {
                /* EAGAIN/EWOULDBLOCK: 11 in Linux numbering (Ryujinx), 35 in BSD. */
                s32 err = nn::socket::GetLastError();
                if (err == 11 || err == 35) { *idle = true; return true; }
                g_LastError = err;
                return false;
            }
            g_RecvLen += n;

            size_t off = 0;
            while (g_RecvLen - off >= 4) {
                u32 len;
                std::memcpy(&len, g_RecvBuf + off, 4);
                if (len < HeaderSize || len > MaxMessageSize) return false;
                if (g_RecvLen - off < 4 + len) break;
                u16 type;
                std::memcpy(&type, g_RecvBuf + off + 4, 2);
                if (!Handle(static_cast<MsgType>(type), Reader(g_RecvBuf + off + 8, len - HeaderSize)))
                    return false;
                off += 4 + len;
            }
            std::memmove(g_RecvBuf, g_RecvBuf + off, g_RecvLen - off);
            g_RecvLen -= off;
            return true;
        }

        bool Flush() {
            PlayerState state;
            bool send_state = false;
            bool send_world = false;
            float world_time = 0;
            s32 world_days = 0;
            u8 world_weather = 0xFF, world_climate = 0, world_bloody = 0xFF;
            float world_bloody_timer = 0;
            char logs[LogSlots][LogLen];
            int nlogs = 0;
            const char* flags[64];
            bool flag_live[64];
            int nflags = 0;
            EnemyLifeMsg enemies[64];
            int nenemies = 0;
            game::enemies::Pose enemy_pos[game::enemies::MaxPerMessage];
            int nenemy_pos = 0;
            u32 enemy_pos_area = 0;
            bool send_revived = false;
            u16 send_heal = 0;
            DownedOut downed[4];
            int ndowned = 0;
            SharedStamp stamps_out[MaxSharedStamps];
            int nstamps_out = -1;
            char gear[3][32];
            bool send_gear = false;
            WeaponPose weapons[MaxWeapons];
            int nweapons = 0;
            bool send_weapons = false;
            GiftOut gifts[4];
            int ngifts = 0;
            {
                ScopedLock lock;
                if (g_StateDirty && g_Connected) {
                    state = g_OutState;
                    g_StateDirty = false;
                    send_state = true;
                }
                if (g_WorldDirty && g_Connected) {
                    send_world = true;
                    world_time = g_WorldTime;
                    world_days = g_WorldDays;
                    world_weather = g_WorldWeather;
                    world_climate = g_WorldClimate;
                    world_bloody = g_WorldBloodyDay;
                    world_bloody_timer = g_WorldBloodyTimer;
                    g_WorldDirty = false;
                }
                while (g_FlagCount > 0 && nflags < 64 && g_Connected) {
                    flag_live[nflags] = g_FlagLive[g_FlagHead];
                    flags[nflags++] = g_FlagQueue[g_FlagHead];
                    g_FlagHead = (g_FlagHead + 1) % FlagQueueSize;
                    g_FlagCount--;
                }
                if (g_GiftsOutCount > 0 && g_Connected) {
                    std::memcpy(gifts, g_GiftsOut, sizeof(gifts));
                    ngifts = g_GiftsOutCount;
                    g_GiftsOutCount = 0;
                }
                if (g_WeaponsPending && g_Connected) {
                    std::memcpy(weapons, g_WeaponsOut, sizeof(weapons));
                    nweapons = g_WeaponsOutCount;
                    g_WeaponsPending = false;
                    send_weapons = true;
                }
                if (g_GearPending && g_Connected) {
                    std::memcpy(gear, g_GearOut, sizeof(gear));
                    g_GearPending = false;
                    send_gear = true;
                }
                if (g_Connected && g_StampsOutCount >= 0) {
                    nstamps_out = g_StampsOutCount;
                    std::memcpy(stamps_out, g_StampsOut, sizeof(stamps_out));
                    g_StampsOutCount = -1;
                }
                if (g_Connected) {
                    ndowned = g_DownedOutCount;
                    std::memcpy(downed, g_DownedOut, sizeof(downed));
                }
                g_DownedOutCount = 0;
                if (g_HealPending && g_Connected) {
                    send_heal = g_HealPending > 0xffff ? 0xffff : static_cast<u16>(g_HealPending);
                    g_HealPending = 0;
                }
                if (g_RevivedPending && g_Connected) {
                    send_revived = true;
                    g_RevivedPending = false;
                }
                if (g_EnemyPosCount && g_Connected) {
                    std::memcpy(enemy_pos, g_EnemyPosOut, g_EnemyPosCount * sizeof(*enemy_pos));
                    nenemy_pos = g_EnemyPosCount;
                    enemy_pos_area = g_EnemyPosArea;
                }
                g_EnemyPosCount = 0;
                while (g_EnemyCount > 0 && nenemies < 64 && g_Connected) {
                    enemies[nenemies++] = g_EnemyQueue[g_EnemyHead];
                    g_EnemyHead = (g_EnemyHead + 1) % EnemyQueueSize;
                    g_EnemyCount--;
                }
                while (g_LogCount > 0) {
                    std::memcpy(logs[nlogs++], g_LogQueue[g_LogHead], LogLen);
                    g_LogHead = (g_LogHead + 1) % LogSlots;
                    g_LogCount--;
                }
            }
            for (int i = 0; i < nlogs; i++) {
                Writer w(g_SendBuf, sizeof(g_SendBuf), MsgType::Log);
                w.Str(logs[i]);
                if (!SendMsg(w)) return false;
            }
            for (int i = 0; i < nflags; i++) {
                Writer w(g_SendBuf, sizeof(g_SendBuf), MsgType::Flag);
                w.U8(flag_live[i] ? 1 : 2);
                w.Str(flags[i]);
                if (!SendMsg(w)) return false;
            }
            for (int i = 0; i < ngifts; i++) {
                Writer w(g_SendBuf, sizeof(g_SendBuf), MsgType::Gift);
                w.U8(gifts[i].to);
                w.Str(gifts[i].name);
                if (gifts[i].has_bonus) w.Bytes(&gifts[i].bonus, sizeof(GiftBonus));
                if (!SendMsg(w)) return false;
            }
            if (send_weapons) {
                Writer w(g_SendBuf, sizeof(g_SendBuf), MsgType::Weapons);
                w.U8(static_cast<u8>(nweapons));
                for (int i = 0; i < nweapons; i++) {
                    w.Str(weapons[i].name);
                    w.Str(weapons[i].bone);
                    w.Bytes(weapons[i].offset, sizeof(weapons[i].offset));
                }
                if (!SendMsg(w)) return false;
            }
            if (send_gear) {
                Writer w(g_SendBuf, sizeof(g_SendBuf), MsgType::Gear);
                for (auto& part : gear) w.Str(part);
                if (!SendMsg(w)) return false;
            }
            if (nstamps_out >= 0) {
                Writer w(g_SendBuf, sizeof(g_SendBuf), MsgType::Stamps);
                w.U8(static_cast<u8>(nstamps_out));
                for (int i = 0; i < nstamps_out; i++) {
                    w.U8(stamps_out[i].icon);
                    w.Bytes(stamps_out[i].pos, 12);
                }
                if (!SendMsg(w)) return false;
            }
            for (int i = 0; i < ndowned; i++) {
                Writer w(g_SendBuf, sizeof(g_SendBuf), MsgType::Downed);
                w.U8(downed[i].state);
                w.Bytes(downed[i].pos, 12);
                w.U8(downed[i].left);
                if (!SendMsg(w)) return false;
            }
            if (send_heal) {
                Writer w(g_SendBuf, sizeof(g_SendBuf), MsgType::Heal);
                w.Bytes(&send_heal, 2);
                if (!SendMsg(w)) return false;
            }
            if (send_revived) {
                Writer w(g_SendBuf, sizeof(g_SendBuf), MsgType::Revived);
                if (!SendMsg(w)) return false;
            }
            if (nenemy_pos) {
                Writer w(g_SendBuf, sizeof(g_SendBuf), MsgType::EnemyPos);
                w.Bytes(&enemy_pos_area, 4);
                w.U8(static_cast<u8>(nenemy_pos));
                for (int i = 0; i < nenemy_pos; i++) {
                    w.Bytes(&enemy_pos[i].hash, 4);
                    w.Bytes(enemy_pos[i].pos, 12);
                    w.Bytes(&enemy_pos[i].rot_y, 4);
                }
                if (!SendMsg(w)) return false;
            }
            for (int i = 0; i < nenemies; i++) {
                Writer w(g_SendBuf, sizeof(g_SendBuf), MsgType::EnemyLife);
                w.Bytes(&enemies[i].area, 4);
                w.Bytes(&enemies[i].hash, 4);
                w.Bytes(&enemies[i].life, 4);
                if (!SendMsg(w)) return false;
            }
            if (send_world) {
                Writer w(g_SendBuf, sizeof(g_SendBuf), MsgType::WorldState);
                w.Bytes(&world_time, 4);
                w.Bytes(&world_days, 4);
                w.U8(world_weather);
                w.U8(world_climate);
                w.U8(world_bloody);
                w.Bytes(&world_bloody_timer, 4);
                if (!SendMsg(w)) return false;
            }
            if (send_state) {
                Writer w(g_SendBuf, sizeof(g_SendBuf), MsgType::PlayerState);
                w.Bytes(&state, sizeof(state));
                if (!SendMsg(w)) return false;
            }
            return true;
        }

        bool InitNetwork() {
            nn::nifm::Initialize();
            nn::nifm::SubmitNetworkRequest();
            while (nn::nifm::IsNetworkRequestOnHold()) Sleep(100);
            if (!nn::nifm::IsNetworkAvailable()) {
                Logging.Log("[coop] no network. In Ryujinx, turn on Guest Internet Access; on a Switch, connect to Wi-Fi.");
                return false;
            }
            /* Once. A failure can mean the game set sockets up itself already,
               so it isn't the end: connecting is tried anyway. */
            static bool s_tried = false;
            if (!s_tried) {
                s_tried = true;
                Result rc = nn::socket::Initialize(g_SocketPool, SocketPoolSize, SocketAllocPoolSize, 14);
                if (rc != 0) Logging.Log("[coop] socket init returned 0x%x; trying to connect anyway", rc);
            }
            return true;
        }

        /* Samples Link's position and queues it. Velocity comes from the
           previous sample; facing from the direction of travel. */
        void SamplePlayer() {
            static float last[3];
            static s64 last_ms = 0;

            float pos[3];
            float rot_y = 0;
            if (!game::GetLocalPlayer(pos, &rot_y)) { last_ms = 0; return; }

            s64 now = NowMs();
            PlayerState st{};
            std::memcpy(st.pos, pos, sizeof(pos));
            if (last_ms != 0 && now > last_ms) {
                float dt = (now - last_ms) / 1000.0f;
                for (int i = 0; i < 3; i++) st.vel[i] = (pos[i] - last[i]) / dt;
            }
            st.rot_y = rot_y;
            st.anim_hash = game::LocalAnimCategory();
            st.map_area = game::LocalArea();
            game::LocalLife(&st.health, &st.max_hp);
            std::memcpy(last, pos, sizeof(pos));
            last_ms = now;
            SubmitPlayerState(st);
        }

        void LogPeers() {
            Peer peers[MaxPeers];
            GetPeers(peers);
            float me[3] = {};
            bool have_me = game::GetLocalPlayerPos(me);
            for (auto& p : peers) {
                if (!p.present || !p.has_state) continue;
                float dx = p.state.pos[0] - me[0], dy = p.state.pos[1] - me[1], dz = p.state.pos[2] - me[2];
                if (have_me)
                    Logging.Log("[coop] %s at (%.1f, %.1f, %.1f), %.1fm away", p.name, p.state.pos[0], p.state.pos[1],
                                p.state.pos[2], __builtin_sqrtf(dx * dx + dy * dy + dz * dz));
                else
                    Logging.Log("[coop] %s at (%.1f, %.1f, %.1f)", p.name, p.state.pos[0], p.state.pos[1], p.state.pos[2]);
            }
            if (have_me) SendLog("at (%.1f, %.1f, %.1f)", me[0], me[1], me[2]);
            if (g_Config.echo || g_Config.autostart) {
                game::LogPlayerDebug();
                game::LogAutostartStats();
            }
        }

        void ThreadMain(void*) {
            /* Give the game time to set up its file system before we touch the SD card. */
            Sleep(3000);
            LoadSettings(g_Config);
            filelog::Enable(g_Config.log_file);
            {
                game::GhostSettings gs{};
                std::memcpy(gs.actor_name, g_Config.ghost_actor, sizeof(gs.actor_name));
                gs.echo = g_Config.echo;
                std::memcpy(gs.test_spawn, g_Config.test_spawn, sizeof(gs.test_spawn));
                gs.test_move = g_Config.test_move;
                game::ConfigureGhosts(gs);
                game::EnableAutostart(g_Config.autostart);
                if (g_Config.test_flag[0]) game::progress::SetTestFlag(g_Config.test_flag);
                game::progress::SetBeastsAnywhere(g_Config.beasts_anywhere);
                game::progress::SetTestGate(g_Config.test_gate);
                if (g_Config.test_menu) game::progress::SetTestMenu(true);
                game::combat::SetLogNearby(g_Config.autostart);
                game::revive::SetTest(g_Config.test_revive);
                game::revive::SetTestLife(g_Config.test_life);
                game::research::SetTest(g_Config.test_research);
                game::items::SetProbeDebug(g_Config.test_research == 5);
                game::meals::SetTestLife(g_Config.test_heal);
                game::warp::SetTestFollow(g_Config.test_follow);
                game::weapons::SetTest(g_Config.test_bind);
                game::items::SetTest(g_Config.test_drop);
                game::SetKeyScript(g_Config.test_keys);
            }
            for (int tries = 0; !InitNetwork(); tries++) {
                if (tries == 2) game::notify::Show("Co-op: no network connection");
                Sleep(5000);
            }

            s64 retry_ms = 1000;
            while (true) {
                Logging.Log("[coop] connecting to %s:%d", g_Config.host, g_Config.port);
                if (!Connect()) {
                    char why[48];
                    std::snprintf(why, sizeof(why), "connect failed (errno %d)", (int)nn::socket::GetLastError());
                    Disconnect(why);
                    /* Said once, after a few tries; again only after a connection that worked. */
                    if (++g_FailedTries == 3 && !g_FailShown) {
                        g_FailShown = true;
                        game::notify::Show("Co-op: can't reach the server at %s:%d", g_Config.host, g_Config.port);
                    }
                    Sleep(retry_ms);
                    retry_ms = retry_ms * 2 > 10000 ? 10000 : retry_ms * 2;
                    continue;
                }
                retry_ms = 1000;

                s64 last_ping = NowMs();
                s64 last_sample = 0, last_report = NowMs(), last_flush = 0;
                while (true) {
                    s64 now = NowMs();
                    if (now - last_sample >= 33) { last_sample = now; SamplePlayer(); }
                    if (now - last_report >= 5000) { last_report = now; LogPeers(); }
                    if (now - last_flush >= 2000) { last_flush = now; filelog::Flush(); }
                    bool idle = false;
                    if (!Flush() || !Pump(&idle)) break;
                    if (NowMs() - last_ping >= 2000) {
                        last_ping = NowMs();
                        Writer w(g_SendBuf, sizeof(g_SendBuf), MsgType::Ping);
                        w.U64(static_cast<u64>(last_ping));
                        if (!SendMsg(w)) break;
                    }
                }
                char why[48];
                std::snprintf(why, sizeof(why), "connection lost (errno %d)", (int)g_LastError);
                Disconnect(why);
                g_LastError = 0;
                Sleep(retry_ms);
            }
        }
    }

    void Start(const u8 (&build_id)[16]) {
        std::memcpy(g_BuildId, build_id, 16);
        nn::os::InitializeMutex(&g_Mutex, false, 0);

        nn::os::CreateThread(&g_Thread, ThreadMain, nullptr, g_ThreadStack, ThreadStackSize,
                             nn::os::DefaultThreadPriority);
        nn::os::SetThreadName(&g_Thread, "coop::net");
        nn::os::StartThread(&g_Thread);
    }

    bool IsConnected() { ScopedLock lock; return g_Connected; }
    u8 MyId() { ScopedLock lock; return g_MyId; }
    u8 HostId() { ScopedLock lock; return g_HostId; }

    void SubmitPlayerState(const PlayerState& state) {
        ScopedLock lock;
        g_OutState = state;
        g_StateDirty = true;
    }

    void SendFlag(const char* name, bool live) {
        ScopedLock lock;
        if (g_FlagCount == FlagQueueSize) return;
        int slot = (g_FlagHead + g_FlagCount) % FlagQueueSize;
        g_FlagQueue[slot] = name;
        g_FlagLive[slot] = live;
        g_FlagCount++;
    }

    void SendGift(u8 to, const char* weapon, const GiftBonus* bonus) {
        ScopedLock lock;
        if (g_GiftsOutCount >= 4) return;
        g_GiftsOut[g_GiftsOutCount].to = to;
        g_GiftsOut[g_GiftsOutCount].has_bonus = bonus != nullptr;
        if (bonus) g_GiftsOut[g_GiftsOutCount].bonus = *bonus;
        std::strncpy(g_GiftsOut[g_GiftsOutCount].name, weapon, 31);
        g_GiftsOut[g_GiftsOutCount].name[31] = '\0';
        g_GiftsOutCount++;
    }

    void SendWeapons(const WeaponPose* poses, int count) {
        ScopedLock lock;
        if (count > MaxWeapons) count = MaxWeapons;
        std::memcpy(g_WeaponsOut, poses, sizeof(WeaponPose) * count);
        g_WeaponsOutCount = count;
        g_WeaponsPending = true;
    }

    void SendGear(const char* head, const char* upper, const char* lower) {
        ScopedLock lock;
        const char* parts[3] = {head, upper, lower};
        for (int i = 0; i < 3; i++) {
            std::strncpy(g_GearOut[i], parts[i] ? parts[i] : "", 31);
            g_GearOut[i][31] = '\0';
        }
        g_GearPending = true;
    }

    void SendStamps(const SharedStamp* stamps, int count) {
        ScopedLock lock;
        if (count > MaxSharedStamps) count = MaxSharedStamps;
        std::memcpy(g_StampsOut, stamps, count * sizeof(stamps[0]));
        g_StampsOutCount = count;
    }

    void SendDowned(u8 state, const float* pos, u8 seconds_left) {
        ScopedLock lock;
        if (g_DownedOutCount == 4) g_DownedOutCount = 3; /* keep the newest */
        DownedOut& d = g_DownedOut[g_DownedOutCount++];
        d.state = state;
        std::memcpy(d.pos, pos, sizeof(d.pos));
        d.left = seconds_left;
    }

    void SendHeal(u16 quarters) {
        ScopedLock lock;
        g_HealPending += quarters;
    }

    void SendRevived() {
        ScopedLock lock;
        g_RevivedPending = true;
    }

    void SendEnemyPoses(u32 area, const game::enemies::Pose* poses, int count) {
        ScopedLock lock;
        if (count > game::enemies::MaxPerMessage) count = game::enemies::MaxPerMessage;
        std::memcpy(g_EnemyPosOut, poses, count * sizeof(*poses));
        g_EnemyPosCount = count;
        g_EnemyPosArea = area;
    }

    void SendEnemyLife(u32 area, u32 hash_id, s32 life) {
        ScopedLock lock;
        if (g_EnemyCount == EnemyQueueSize) return;
        g_EnemyQueue[(g_EnemyHead + g_EnemyCount) % EnemyQueueSize] = {area, hash_id, life};
        g_EnemyCount++;
    }

    void SendWorldState(float time, s32 days, u8 weather, u8 climate, u8 bloody_day, float bloody_timer) {
        ScopedLock lock;
        g_WorldTime = time;
        g_WorldDays = days;
        g_WorldWeather = weather;
        g_WorldClimate = climate;
        g_WorldBloodyDay = bloody_day;
        g_WorldBloodyTimer = bloody_timer;
        g_WorldDirty = true;
    }

    void SendLog(const char* fmt, ...) {
        char line[LogLen];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(line, sizeof(line), fmt, args);
        va_end(args);

        ScopedLock lock;
        if (g_LogCount == LogSlots) return;
        std::memcpy(g_LogQueue[(g_LogHead + g_LogCount) % LogSlots], line, LogLen);
        g_LogCount++;
    }

    void GetPeers(Peer (&out)[MaxPeers]) {
        ScopedLock lock;
        std::memcpy(out, g_Peers, sizeof(g_Peers));
    }
}
