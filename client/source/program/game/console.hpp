#pragma once

#include <common.hpp>

/* Test helper: a line of text written into g_Cmd from outside the game (a
   debugger, or Savage Wind Recomp's `mem write`) is run on the next tick and
   cleared. Nothing writes it in normal play.

     spawn <actor> [metres ahead]   create an actor in front of Link
     near [metres]                  list the actors around Link
     life <actor address> <value>   set an actor's life
     kill <actor address>           delete an actor
     probe <part of a name>|...     log actors the game creates whose name contains one of these, with parameters */
namespace coop::game::console {
    extern char g_Cmd[128];
    void Tick(s64 now_ms);
    void InstallProbe(); /* development only: a hook on actor creation, see `probe` */
    /* From the hook on the game's "start a sequence": logs it for the actor named with `aswatch <actor address>`. */
    void OnStartAnimation(uintptr_t as_list, const char* name, int target, float a, float b, int bank, bool flag);
}
