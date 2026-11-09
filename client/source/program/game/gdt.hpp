#pragma once

#include <common.hpp>

/* Read and write BotW's game data flags (ksys::gdt::Manager) by name.
   Call from the game thread. Handles stay valid until the game reloads its
   flag tables (title screen / load), so look them up again after a load. */
namespace coop::game::gdt {

    using Handle = u32;
    constexpr Handle InvalidHandle = 0xFFFFFFFF;

    bool Ready();

    Handle BoolHandle(const char* name);
    Handle S32Handle(const char* name);
    Handle F32Handle(const char* name);

    bool GetBool(Handle h, bool* out);
    bool SetBool(Handle h, bool value);
    bool GetS32(Handle h, s32* out);
    bool SetS32(Handle h, s32 value);
    bool GetF32(Handle h, float* out);
    bool SetF32(Handle h, float value);
}
