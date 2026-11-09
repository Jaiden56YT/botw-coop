#pragma once

/* Declarations for the subset of nn::socket and nn::nifm the mod uses. These
   resolve against the game's own SDK module at load time; the names and types
   below must match the mangled symbols exported by `sdk`. */

#include <common.hpp>

namespace nn::socket {

    enum class Family : u8 { Af_Inet = 2 };
    enum class Type : u32 { Sock_Stream = 1 };
    enum class Protocol : u32 { IpProto_Tcp = 6 };
    enum class MsgFlag : u32 { Msg_None = 0, Msg_DontWait = 0x80 };
    enum class Level : u32 { Sol_Socket = 0xFFFF, Sol_Tcp = 6 };
    enum class Option : u32 { Tcp_NoDelay = 1, So_RcvTimeo = 0x1006 };

    struct InAddr {
        u32 addr;
    };

    /* BSD-style sockaddr with the leading length byte. */
    struct SockAddr {
        u8 len;
        Family family;
        u16 port;  /* network byte order */
        InAddr addr;
        u8 zero[8];
    };
    static_assert(sizeof(SockAddr) == 16);

    struct PollFd {
        s32 fd;
        s16 events;
        s16 revents;
    };
    constexpr s16 PollIn = 0x1, PollErr = 0x8, PollHup = 0x10, PollNval = 0x20;

    Result Initialize(void* pool, u64 pool_size, u64 alloc_pool_size, int concurrency_limit);
    s32 GetLastError();

    s32 Socket(Family family, Type type, Protocol protocol);
    s32 Connect(s32 fd, const SockAddr* addr, u32 addr_len);
    s64 Send(s32 fd, const void* buf, u64 len, MsgFlag flags);
    s64 Recv(s32 fd, void* buf, u64 len, MsgFlag flags);
    s32 SetSockOpt(s32 fd, Level level, Option option, const void* value, u32 value_len);
    s32 Close(s32 fd);
    s32 Poll(PollFd* fds, u64 count, s32 timeout_ms);

    u16 InetHtons(u16 v);
    s32 InetAton(const char* str, InAddr* out);
}

namespace nn::nifm {
    Result Initialize();
    void SubmitNetworkRequest();
    bool IsNetworkRequestOnHold();
    bool IsNetworkAvailable();
}
