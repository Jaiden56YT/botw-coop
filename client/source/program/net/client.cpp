#include "client.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <lib.hpp>
#include <nn/os.hpp>
#include <nn/socket.hpp>

#include "../config.hpp"
#include "../game/player.hpp"
#include "../game/ghost.hpp"
#include "../game/autostart.hpp"
#include "../game/world.hpp"

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

        void Sleep(s64 ms) { nn::os::SleepThread(nn::TimeSpan::FromMilliSeconds(ms)); }

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
                    for (u8 i = 0; i < count && r.Ok(); i++) {
                        u8 id = r.Get<u8>();
                        AddPeer(id, r.Str());
                    }
                    g_Connected = true;
                    Logging.Log("[coop] joined as player %d (%s), %d other(s) here",
                                g_MyId, g_MyId == g_HostId ? "host" : "guest", count);
                    return r.Ok();
                }
                case MsgType::Reject: {
                    auto reason = r.Str();
                    Logging.Log("[coop] server refused: %.*s", (int)reason.size(), reason.data());
                    return false;
                }
                case MsgType::PlayerJoined: {
                    ScopedLock lock;
                    u8 id = r.Get<u8>();
                    auto name = r.Str();
                    AddPeer(id, name);
                    Logging.Log("[coop] %.*s joined", (int)name.size(), name.data());
                    return r.Ok();
                }
                case MsgType::PlayerLeft: {
                    ScopedLock lock;
                    if (Peer* p = FindPeer(r.Get<u8>())) {
                        Logging.Log("[coop] %s left", p->name);
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
                    bool from_host;
                    {
                        ScopedLock lock;
                        from_host = from == g_HostId;
                    }
                    if (from_host) game::OnHostWorldState({t, d});
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
            char logs[LogSlots][LogLen];
            int nlogs = 0;
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
                    g_WorldDirty = false;
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
            if (send_world) {
                Writer w(g_SendBuf, sizeof(g_SendBuf), MsgType::WorldState);
                w.Bytes(&world_time, 4);
                w.Bytes(&world_days, 4);
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
                Logging.Log("[coop] no network. In Ryujinx, turn on Guest Internet Access.");
                return false;
            }
            Result rc = nn::socket::Initialize(g_SocketPool, SocketPoolSize, SocketAllocPoolSize, 14);
            if (rc != 0) {
                Logging.Log("[coop] socket init failed: 0x%x", rc);
                return false;
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
            {
                game::GhostSettings gs{};
                std::memcpy(gs.actor_name, g_Config.ghost_actor, sizeof(gs.actor_name));
                gs.echo = g_Config.echo;
                std::memcpy(gs.test_spawn, g_Config.test_spawn, sizeof(gs.test_spawn));
                game::ConfigureGhosts(gs);
                game::EnableAutostart(g_Config.autostart);
            }
            while (!InitNetwork()) Sleep(5000);

            s64 retry_ms = 1000;
            while (true) {
                Logging.Log("[coop] connecting to %s:%d", g_Config.host, g_Config.port);
                if (!Connect()) {
                    char why[48];
                    std::snprintf(why, sizeof(why), "connect failed (errno %d)", (int)nn::socket::GetLastError());
                    Disconnect(why);
                    Sleep(retry_ms);
                    retry_ms = retry_ms * 2 > 10000 ? 10000 : retry_ms * 2;
                    continue;
                }
                retry_ms = 1000;

                s64 last_ping = NowMs();
                s64 last_sample = 0, last_report = NowMs();
                while (true) {
                    s64 now = NowMs();
                    if (now - last_sample >= 33) { last_sample = now; SamplePlayer(); }
                    if (now - last_report >= 5000) { last_report = now; LogPeers(); }
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

    void SendWorldState(float time, s32 days) {
        ScopedLock lock;
        g_WorldTime = time;
        g_WorldDays = days;
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
