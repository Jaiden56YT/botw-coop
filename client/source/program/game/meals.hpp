#pragma once

#include <common.hpp>

/* Shared meals. Dishes can't be dropped or held, so they can't be traded;
   instead, eating next to a partner feeds them too. When your hearts go up
   while the pause menu is open (you ate a dish or drank an elixir) and a
   partner is within 6 m of you, they get the same healing (up to their full
   hearts), and both of you see a message. */
namespace coop::game::meals {

    /* Call once at startup, before the network thread runs. */
    void Init();

    /* Game thread, every tick. */
    void Tick(s64 now_ms);

    /* Network thread: a partner ate `quarters` of healing next to us. */
    void OnPartnerHeal(u8 from, u16 quarters);

    /* Debug: once a partner is near, drop to this life (quarter hearts) so a
       partner's meal has something to heal. */
    void SetTestLife(int quarters);
}
