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
    };

    /* Loads sd:/botw-coop/config.ini (on Ryujinx: <Ryujinx dir>/sdcard/botw-coop/config.ini).
       Missing keys keep their compiled-in defaults; if no name is set, the
       Switch profile's nickname is used. Lines look like `key = value`; `#`
       starts a comment. Keys: name, server, port, password, ghost_actor, echo, autostart, test_spawn. */
    void LoadSettings(Settings& out);
}
