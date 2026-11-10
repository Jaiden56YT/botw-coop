#pragma once

#include <common.hpp>
#include "protocol.hpp"

namespace coop::net {

    constexpr int MaxPeers = 4;

    /* A carried weapon: which of Link's bones it sits on, and where relative to it. */
    constexpr int MaxWeapons = 6;
    struct WeaponPose {
        char name[32];
        char bone[16];
        float offset[12]; /* 3x4 row-major, bone space */
    };

    struct Peer {
        bool present;
        u8 id;
        char name[32];
        bool has_state;
        PlayerState state;
        u64 state_tick; /* nn::os tick when the last state arrived */
        char gear[3][32]; /* worn armor actor names: head, upper, lower ("" = none) */
        WeaponPose weapons[MaxWeapons];
        u8 weapon_count;
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
    void SendWorldState(float time, s32 days, u8 weather, u8 climate, u8 bloody_day, float bloody_timer);

    /* Shares a progress flag that just became true. Any thread; the name must
       stay valid (it points into progress's static list). */
    void SendFlag(const char* name, bool live);

    /* Queues "this enemy's life is now `life`" for the room. Any thread. */
    void SendEnemyLife(u32 area, u32 hash_id, s32 life);

    /* Tells the room we went down and were revived. Any thread. */
    void SendRevived();

    /* Tells the room what we're wearing. Any thread. */
    void SendGear(const char* head, const char* upper, const char* lower);

    /* Tells the room which weapons we carry and where. Any thread. */
    void SendWeapons(const WeaponPose* poses, int count);

    /* Gives a weapon to player `to`. Any thread. */
    void SendGift(u8 to, const char* weapon);

    /* Sends a line to the server log. Dropped if the queue is full. Any thread. */
    void SendLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

    /* Copies the current peer table out. Game thread. */
    void GetPeers(Peer (&out)[MaxPeers]);
}
