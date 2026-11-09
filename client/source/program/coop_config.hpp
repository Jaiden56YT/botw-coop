#pragma once

/* Connection settings. For M0 these are compiled in; pass overrides on the
   make command line, e.g.
       make COOP_FLAGS='-DCOOP_SERVER_HOST=\"192.168.1.20\" -DCOOP_PLAYER_NAME=\"Sidon\"'
   A config file read at startup replaces this in M1. */

#ifndef COOP_SERVER_HOST
#define COOP_SERVER_HOST "127.0.0.1"
#endif

#ifndef COOP_SERVER_PORT
#define COOP_SERVER_PORT 7777
#endif

#ifndef COOP_PLAYER_NAME
#define COOP_PLAYER_NAME "Link"
#endif

#ifndef COOP_PASSWORD
#define COOP_PASSWORD "hyrule"
#endif

namespace coop {
    /* GNU build ID of BotW v1.9.0's main executable. */
    constexpr unsigned char ExpectedBuildId[16] = {
        0xcd, 0x57, 0xb2, 0x3f, 0xa4, 0xbb, 0xad, 0x65,
        0x80, 0x3d, 0x97, 0x88, 0xc0, 0x18, 0x21, 0xee,
    };
}
