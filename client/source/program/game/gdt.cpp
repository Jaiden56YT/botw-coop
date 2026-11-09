#include "gdt.hpp"

#include <lib.hpp>

#include "offsets.hpp"

namespace coop::game::gdt {

    namespace {
        uintptr_t Base() { return exl::util::GetMainModuleInfo().m_Total.m_Start; }

        uintptr_t Manager() {
            uintptr_t slot = *reinterpret_cast<const uintptr_t*>(Base() + offsets::Got_GdtManagerInstance);
            return slot ? *reinterpret_cast<const uintptr_t*>(slot) : 0;
        }

        /* TimeMgr::init does ldr x8,[mgr,#0xbd8]; ldr x0,[x8]: two loads. */
        uintptr_t Param(uintptr_t mgr) {
            uintptr_t p = *reinterpret_cast<const uintptr_t*>(mgr + offsets::GdtManager_ParamBypass);
            return p ? *reinterpret_cast<const uintptr_t*>(p) : 0;
        }

        Handle Lookup(uintptr_t idx_fn, const char* name) {
            uintptr_t mgr = Manager();
            if (!mgr) return InvalidHandle;
            uintptr_t param = Param(mgr);
            if (!param) return InvalidHandle;
            auto hash = reinterpret_cast<u32 (*)(const char*)>(Base() + offsets::Gdt_HashName)(name);
            s32 idx = reinterpret_cast<s32 (*)(uintptr_t, u32)>(Base() + idx_fn)(param, hash);
            if (idx < 0) return InvalidHandle;
            u32 counter = *reinterpret_cast<const u32*>(mgr + offsets::GdtManager_ResetCounter);
            return static_cast<u32>(idx) | (counter << 24);
        }

        template<typename T>
        bool Get(uintptr_t fn, Handle h, T* out) {
            uintptr_t mgr = Manager();
            if (!mgr || h == InvalidHandle) return false;
            return reinterpret_cast<bool (*)(uintptr_t, T*, Handle)>(Base() + fn)(mgr, out, h);
        }

        template<typename T>
        bool Set(uintptr_t fn, Handle h, T value) {
            uintptr_t mgr = Manager();
            if (!mgr || h == InvalidHandle) return false;
            return reinterpret_cast<bool (*)(uintptr_t, T, Handle)>(Base() + fn)(mgr, value, h);
        }
    }

    bool Ready() {
        uintptr_t mgr = Manager();
        return mgr && Param(mgr);
    }

    Handle BoolHandle(const char* name) { return Lookup(offsets::TriggerParam_GetBoolIdx, name); }
    Handle S32Handle(const char* name) { return Lookup(offsets::TriggerParam_GetS32Idx, name); }
    Handle F32Handle(const char* name) { return Lookup(offsets::TriggerParam_GetF32Idx, name); }

    bool GetBool(Handle h, bool* out) { return Get(offsets::GdtManager_GetBool, h, out); }
    bool SetBool(Handle h, bool value) { return Set(offsets::GdtManager_SetBool, h, value); }
    bool GetS32(Handle h, s32* out) { return Get(offsets::GdtManager_GetS32, h, out); }
    bool SetS32(Handle h, s32 value) { return Set(offsets::GdtManager_SetS32, h, value); }
    bool GetF32(Handle h, float* out) { return Get(offsets::GdtManager_GetF32, h, out); }
    bool SetF32(Handle h, float value) { return Set(offsets::GdtManager_SetF32, h, value); }
}
