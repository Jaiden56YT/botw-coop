#pragma once

/* Addresses in BotW v1.9.0's main executable (build id cd57b23f...), relative
   to the module base. Each entry notes how it was found so it can be found
   again for another version. Tools: tools/nso.py + tools/xref.py. */

#include <common.hpp>

namespace coop::game::offsets {

    /* uking::act::PlayerInfo::sInstance (PlayerInfo*), in .bss.
       Found from getPlayerPos (main+0xcc7598, references the debug string
       "getPlayerPos"); its callers load the instance from this global. */
    constexpr uintptr_t PlayerInfoInstance = 0x1e13ed0;

    /* Fields of PlayerInfo in 1.9.0 (getPlayerUnchecked / getPlayerPos /
       getPlayerPosForPostCalc return these offsets). */
    constexpr uintptr_t PlayerInfo_PlayerActor = 0x60;  /* PlayerBase* */
    constexpr uintptr_t PlayerInfo_PlayerPos = 0x88;    /* sead::Vector3f */

    /* uking::act::PlayerInfo::getPlayerPos(). Called by game logic every frame;
       we hook it to get a regular tick on a thread that already uses the actor
       system. */
    constexpr uintptr_t PlayerInfo_GetPlayerPos = 0xcc7598;

    /* ksys::act::Actor fields (same as the decomp's 1.5.0 layout). */
    constexpr uintptr_t Actor_Mtx = 0x398; /* sead::Matrix34f, row-major, translation in column 3 */

    /* ksys::act::ActorCreator. Found from enableDistanceUnloadChecks (uses the
       string "ArmorExtra") and the three callers of ActorTemplate::getActorClass
       (main+0x13d12dc, which logs "[%s] classの定義がありません"). */
    constexpr uintptr_t ActorCreator_CreateActor = 0x1535900; /* Actor* (this, name, heap, params, sleep_after_init, block) */
    constexpr uintptr_t Got_ActorCreatorInstance = 0x1d66550; /* GOT slot -> &ActorCreator::sInstance */
    constexpr uintptr_t Got_SpawnHeapOwner = 0x1d66558;       /* GOT slot -> &X::sInstance; heap at X+40 (as createActor's callers do) */
    constexpr uintptr_t SpawnHeapOwner_Heap = 40;

    /* ksys::act::InstParamPack: vtable, BaseProc* mProc, then Buffer
       { u16 count; s16 position; u8 data[0xc0]; }. Entries are written as
       const char* key, u8 type, value. */
    constexpr uintptr_t InstParamPack_Vtable = 0x1bc5540;

    /* ActorCreator fields (from enableDistanceUnloadChecks at main+0x153619c). */
    constexpr uintptr_t ActorCreator_ActorList = 0x38;       /* sead::OffsetList<Actor>: head node {prev,next} */
    constexpr uintptr_t ActorCreator_ActorListOffset = 0x4c; /* s32: node offset inside Actor */
    constexpr uintptr_t ActorCreator_ActorListCS = 0xa8;     /* sead::CriticalSection */

    /* sead::CriticalSection lock / tryLock / unlock thunks. */
    constexpr uintptr_t CriticalSection_Lock = 0xeaff08;
    constexpr uintptr_t CriticalSection_TryLock = 0xeaff10;
    constexpr uintptr_t CriticalSection_Unlock = 0xeaff18;

    /* Actor::mActorFlags2 and its NoDistanceCheck bit (set by
       enableDistanceUnloadChecks). */
    constexpr uintptr_t Actor_Flags2 = 0x518;
    constexpr u32 ActorFlag2_NoDistanceCheck = 0x80;

    /* ksys::gdt::Manager (game data flags). Found from TimeMgr::init, which looks
       up "WM_Time" / "WM_NumberOfDays" (main+0x1425664), and TimeMgr::calc_
       (main+0x14260b4). Handle = index | (Manager[+0xc28] << 24). */
    constexpr uintptr_t Got_GdtManagerInstance = 0x1d66478;
    constexpr uintptr_t GdtManager_ParamBypass = 0xbd8;   /* -> TriggerParam** (dereference twice) */
    constexpr uintptr_t GdtManager_ResetCounter = 0xc28;  /* u32, top byte of every handle */
    constexpr uintptr_t Gdt_HashName = 0xeb7a84;          /* u32 (const char* name) */
    constexpr uintptr_t TriggerParam_GetBoolIdx = 0x1143950; /* s32 (param, u32 hash) */
    constexpr uintptr_t TriggerParam_GetS32Idx = 0x1143958;
    constexpr uintptr_t TriggerParam_GetF32Idx = 0x1143960;
    constexpr uintptr_t GdtManager_GetBool = 0x6b6980;    /* bool (mgr, bool* out, handle) */
    constexpr uintptr_t GdtManager_SetBool = 0x64af38;    /* bool (mgr, bool value, handle) */
    constexpr uintptr_t GdtManager_GetS32 = 0xb4042c;     /* bool (mgr, s32* out, handle) */
    constexpr uintptr_t GdtManager_SetS32 = 0x6b6b3c;     /* bool (mgr, s32 value, handle) */
    constexpr uintptr_t GdtManager_GetF32 = 0xb366a0;     /* bool (mgr, f32* out, handle) */
    constexpr uintptr_t GdtManager_SetF32 = 0xb36c80;     /* bool (mgr, f32 value, handle) */
}
