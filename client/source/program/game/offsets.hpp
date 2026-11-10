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

    /* Animation sequences (AS). Found from the NullASPlay AI action: its
       loadParams_ (main+0x639d98) reads "TargetIdx"/"SeqBankIdx"/"IsIgnoreSame"/
       "ASName"; its enter_ (main+0x639cc8) tail-calls ActionBase's play helper
       (main+0x151e42c), which calls ASList::startAnimationMaybe. */
    constexpr uintptr_t Actor_ASList = 0x560;              /* ksys::as::ASList* (same as the decomp) */
    constexpr uintptr_t ASList_StartAnimation = 0x149f24c; /* bool (ASList*, f32 s0, f32 s1, const SafeString&, int target, int seqbank, bool) */
    constexpr uintptr_t Got_SafeStringVtableBase = 0x1d66260; /* GOT slot; sead::SafeString vtable = *slot + 0x10 */

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
    /* TriggerParam members (same as the 1.5.0 decomp; the getXIdx stubs at
       0x1143950.. add 0x8, 0x18, .. 0x78 for the plain tables, and the array
       search at 0x1143990 reads the bool-array table at +0x98). */
    constexpr uintptr_t TriggerParam_S32ArrayFlags = 0xa8;   /* PtrArray<PtrArray<FlagS32>> */
    constexpr uintptr_t TriggerParam_Vec3fArrayFlags = 0x108; /* PtrArray<PtrArray<FlagVector3f>> */
    /* Flag<T>: vtable, type u8, properties u8, config {init, min, max}, value, hash, debug data. */
    constexpr uintptr_t FlagS32_Value = 0x18;
    constexpr uintptr_t FlagS32_Hash = 0x1c;
    constexpr uintptr_t FlagVec3f_Value = 0x30;
    constexpr uintptr_t FlagVec3f_Hash = 0x3c;

    /* Map stamps. The map manager (its instance pointer is the global below,
       loaded by the map's own code at 0xd81d4c) owns a pool of stamp objects;
       MapApp_MapIconNo/Pos are only copies of them (loader 0xd868d0, writer
       0xd8a034). The stamp menu (0xd8a960) places icons 27..35 and 37..41; 36 is never drawn. */
    /* ksys::StageInfo statics (FixedSafeString<32>; text pointer at +8). From the
       getters at 0x1208304 / 0x12082f8 (MapMgr compares the first with "MainField"). */
    constexpr uintptr_t StageInfo_CurrentMapType = 0x1dae3a0; /* "MainField", "CDungeon", "MainFieldDungeon", .. */
    constexpr uintptr_t StageInfo_CurrentMapName = 0x1dae3d8; /* "Dungeon000", "RemainsWater", field section, .. */

    /* Actor / BaseProc (same layout as the 1.5.0 decomp; Actor_ASList at 0x560 matches). */
    constexpr uintptr_t BaseProc_NameStr = 0x10;   /* mName (FixedSafeString<64> at 0x8): text pointer */
    constexpr uintptr_t Actor_HashId = 0x640;      /* u32 map placement id, same in every player's world */
    constexpr uintptr_t ActorVt_GetMaxLife = 0xf0;  /* m30: s32 getMaxLife() */
    constexpr uintptr_t ActorVt_Kill = 0x1b0;       /* m54: killWithDropsAndEffects(int emit_type) */
    constexpr uintptr_t ActorVt_GetLife = 0x2b8;    /* m87: s32* getLife() */

    /* bool tryRevive(Player*): the player's damage handler (0xca8f44, at 0xca93f8)
       calls it when life is <= 0 and goes to the game-over path if it returns
       false. Tries Mipha's Grace (player vtable +0xa78) then a fairy (+0xb50,
       "Animal_Insect_F", "FairyResurrection"). */
    constexpr uintptr_t Player_TryRevive = 0xcead24;

    /* ModelBindInfo: keeps an actor on a bone of another actor (how Link's
       weapons follow him). Recipe from 0x96b28c: construct, acquire the parent
       link at +8, set the bone name at +0x28 and index 0xffff at +0x60, then
       Actor::setModelBindInfo. calc (0x1097300) resolves the bone by name. */
    constexpr uintptr_t ModelBindInfo_Ctor = 0x1096f68;    /* (info) */
    constexpr uintptr_t ModelBindInfo_Calc = 0x1097300;    /* bool (info, child actor): writes the child's mMtx */
    constexpr uintptr_t ModelBindInfo_BoneName = 0x28;     /* const char* */
    constexpr uintptr_t ModelBindInfo_BoneIdx = 0x60;      /* u16 unit, u16 bone; 0xffff = look up */
    constexpr uintptr_t ModelBindInfo_Offset = 0x68;       /* Matrix34f */
    constexpr uintptr_t ActorLink_Acquire = 0x1517e60;     /* (link, BaseProc*, bool) */
    constexpr uintptr_t Actor_SetModelBindInfo = 0x1533750; /* (actor, info): Actor+0x4d0 */
    constexpr uintptr_t Actor_ClearModelBindInfo = 0x1533760; /* (actor) */

    /* ksys::world::Manager (GOT slot from the weather event action at 0x651a78).
       Field offsets match the decomp: mWeatherTypeTimer 0x794 is what the setter writes. */
    constexpr uintptr_t Got_WorldMgrInstance = 0x1d66460;
    constexpr uintptr_t WorldMgr_SetWeatherType = 0x14315dc; /* (mgr, u32 type, bool, bool, bool for_demo); holds ~4 frames */
    constexpr uintptr_t WorldMgr_ForcedWeather = 0x7d1;      /* u8 mWeatherType, 0xFF = not forced */
    constexpr uintptr_t WorldMgr_CurrentClimate = 0x780;     /* u8 Climate */
    constexpr uintptr_t WeatherMgr_Weather = 0x28;           /* u8 mWeather (WeatherMgr = mMgrs[3]) */

    constexpr uintptr_t MapMgr_Instance = 0x1e17ef8;   /* global: MapMgr* */
    constexpr uintptr_t MapMgr_AddStamp = 0xd8aaa0;   /* void (mgr, s32 icon, const Vec3f* pos, bool) */
    constexpr uintptr_t MapMgr_StampPool = 0x610;     /* PtrArray<Stamp> (200) */
    constexpr uintptr_t MapStamp_Icon = 0x1c;         /* s32 */
    constexpr uintptr_t MapStamp_Pos = 0x28;          /* Vec3f */
    constexpr uintptr_t MapStamp_Flags = 0x3c;        /* u32 */
    constexpr u32 MapStampFlag_Active = 0x10;         /* 0x20: active changed, not yet synced */
    constexpr uintptr_t MapStamp_VtSetPos = 0xb8;     /* vtable 0x1cdd340: 0xd7b528 (stamp, const Vec3f*) */
    constexpr uintptr_t MapStamp_SetVisible = 0xd7b8e4;  /* (stamp, bool): flag 0x200; a change queues it in MapMgr+0x2b0 */
    constexpr uintptr_t MapStamp_VtSetActive = 0xc8;  /* 0xd81d04 (stamp, bool): also (un)registers it and syncs game data */
    constexpr uintptr_t GdtManager_GetBool = 0x6b6980;    /* bool (mgr, bool* out, handle) */
    constexpr uintptr_t GdtManager_SetBool = 0x64af38;    /* bool (mgr, bool value, handle) */
    constexpr uintptr_t GdtManager_GetS32 = 0xb4042c;     /* bool (mgr, s32* out, handle) */
    constexpr uintptr_t GdtManager_SetS32 = 0x6b6b3c;     /* bool (mgr, s32 value, handle) */
    constexpr uintptr_t GdtManager_GetF32 = 0xb366a0;     /* bool (mgr, f32* out, handle) */
    constexpr uintptr_t GdtManager_SetF32 = 0xb36c80;     /* bool (mgr, f32 value, handle) */

    /* TriggerParam::setBool(param, bool value, s32 idx, bool, bool): the write
       every bool flag change ends in. setBool(handle) (above) checks the handle
       and calls main+0x64b134, which calls this for the live table and a mirror. */
    constexpr uintptr_t TriggerParam_SetBool = 0x1138814;

    /* uking::ui::PauseMenuDataMgr::itemGet(this, const SafeString& name, int count,
       const WeaponModifierInfo*): adds an item to the pouch and saves it, as when
       Link picks something up. Found from the "PouchGet" play report it sends.
       The game's own give-one-item helper (main+0xbe0f10) calls it with count 1
       and no modifier, loading the manager through this GOT slot. */
    constexpr uintptr_t PauseMenuDataMgr_ItemGet = 0xd921bc;
    constexpr uintptr_t Got_PauseMenuDataMgrInstance = 0x1d67af0;
}
