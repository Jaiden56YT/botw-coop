#pragma once

#include <common.hpp>
#include "protocol.hpp"

namespace coop::net {

    constexpr int MaxPeers = 4;

    struct Peer {
        bool present;
        u8 id;
        char name[32];
        bool has_state;
        PlayerState state;
        u64 state_tick; /* nn::os tick when the last state arrived */
    };

    /* Starts the background network thread, which loads settings
       (see config.hpp), connects, and streams Link's state. Call once. */
    void Start(const u8 (&build_id)[16]);

    bool IsConnected();
    u8 MyId();
    u8 HostId();

    /* Latest-wins: only the newest state is sent each network tick. Game thread. */
    void SubmitPlayerState(const PlayerState& state);

    /* Host only: shares the world clock. Any thread. */
    void SendWorldState(float time, s32 days);

    /* Sends a line to the server log. Dropped if the queue is full. Any thread. */
    void SendLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

    /* Copies the current peer table out. Game thread. */
    void GetPeers(Peer (&out)[MaxPeers]);
}
