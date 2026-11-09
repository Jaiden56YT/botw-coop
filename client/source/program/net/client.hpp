#pragma once

#include <common.hpp>
#include "protocol.hpp"

namespace coop::net {

    struct Config {
        const char* host;      /* IPv4 dotted address of the relay server */
        u16 port;
        const char* name;      /* player name shown to others */
        const char* password;  /* session / room password */
    };

    constexpr int MaxPeers = 4;

    struct Peer {
        bool present;
        u8 id;
        char name[32];
        bool has_state;
        PlayerState state;
        u64 state_tick; /* nn::os tick when the last state arrived */
    };

    /* Starts the background network thread. Safe to call once. */
    void Start(const Config& config, const u8 (&build_id)[16]);

    bool IsConnected();
    u8 MyId();
    u8 HostId();

    /* Latest-wins: only the newest state is sent each network tick. Game thread. */
    void SubmitPlayerState(const PlayerState& state);

    /* Sends a line to the server log. Dropped if the queue is full. Any thread. */
    void SendLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

    /* Copies the current peer table out. Game thread. */
    void GetPeers(Peer (&out)[MaxPeers]);
}
