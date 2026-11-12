#pragma once

#include <common.hpp>

namespace coop {

    struct Settings {
        char host[64];
        u16 port;
        char name[32];
        char password[64];
        char ghost_actor[64]; /* actor shown in place of other players */
        bool echo;            /* debug: show a ghost of yourself, 1 s behind */
        bool autostart;       /* test: press A at the title/menus until in game */
        char test_spawn[256]; /* debug: actors spawned once in a row in front of Link */
        char test_flag[64];   /* debug: progress flag set locally once connected */
        bool test_move;       /* debug: move the local Link and log its animations */
        bool test_menu;       /* debug: open the pause menu after a partner's reward */
        bool test_revive;     /* debug: once a partner is near, set life to 0 and run the revive check */
        char test_bind[64];   /* debug: "Weapon:Bone", bind that weapon to Link's bone */
        char test_keys[256];  /* debug: buttons to press once in the world, e.g. "Plus,L,L,A:300,Plus" */
        char test_drop[64];   /* debug: spawn this item at Link's feet as if dropped from the inventory */
        bool log_file;        /* also write the log to sd:/botw-coop/log.txt (a Switch has no other way to show it) */
        bool warp_direct;     /* a far warp goes in one jump, not in hops through the sky (see warp.cpp) */
        bool beasts_anywhere; /* share Divine Beast clears with partners who weren't inside with you */
        char test_gate[64];   /* debug: flag that stands in for "has the paraglider" (progress is held until it's set) */
        int test_heal;        /* debug: once a partner is near, drop to this life (quarter hearts) to test shared meals */
        int test_follow;      /* debug: warp to the nearest partner whenever they're more than this many metres away */
        char test_leader[32]; /* debug: test_follow follows only the player with this name */
        int test_research;    /* debug: run a reverse-engineering search (research.cpp) */
        int test_life;        /* debug: drop to this life near a partner and let a real hit test the revive */
    };

    /* Loads sd:/botw-coop/config.ini (on Ryujinx: <Ryujinx dir>/sdcard/botw-coop/config.ini).
       Missing keys keep their compiled-in defaults; if no name is set, the
       Switch profile's nickname is used. Lines look like `key = value`; `#`
       starts a comment. Keys: name, server, port, password, ghost_actor, echo, autostart, test_spawn. */
    void LoadSettings(Settings& out);
}
