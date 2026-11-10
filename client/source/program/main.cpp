#include "lib.hpp"

#include <cstring>

#include "coop_config.hpp"
#include "net/client.hpp"
#include "game/ghost.hpp"
#include "game/autostart.hpp"
#include "game/progress.hpp"
#include "game/combat.hpp"
#include "game/revive.hpp"

namespace {

    /* Finds the GNU build ID note ("GNU\0" + 16 bytes) near the end of the
       main executable's .rodata. Returns false if it isn't there. */
    bool ReadMainBuildId(u8 (&out)[16]) {
        const auto& rodata = exl::util::GetMainModuleInfo().m_Rodata;
        auto* start = reinterpret_cast<const u8*>(rodata.m_Start);
        auto* end = reinterpret_cast<const u8*>(rodata.GetEnd());
        static constexpr u8 NoteHeader[] = {0x04, 0, 0, 0, 0x10, 0, 0, 0, 0x03, 0, 0, 0, 'G', 'N', 'U', 0};
        constexpr size_t NoteSize = sizeof(NoteHeader) + 16;

        auto* stop = rodata.m_Size > 0x10000 ? end - 0x10000 : start;
        for (auto* p = end - NoteSize; p >= stop; p -= 4) {
            if (std::memcmp(p, NoteHeader, sizeof(NoteHeader)) == 0) {
                std::memcpy(out, p + sizeof(NoteHeader), 16);
                return true;
            }
        }
        return false;
    }

    void StartCoop() {
        u8 build_id[16] = {};
        if (!ReadMainBuildId(build_id) || std::memcmp(build_id, coop::ExpectedBuildId, 16) != 0) {
            Logging.Log("[coop] this is not BotW v1.9.0 (build id %02x%02x%02x%02x...); co-op is off",
                        build_id[0], build_id[1], build_id[2], build_id[3]);
            return;
        }

        coop::game::InstallGhosts();
        coop::game::InstallAutostart();
        coop::game::progress::Install();
        coop::game::combat::Init();
        coop::game::revive::Install();
        coop::net::Start(build_id);
        coop::net::SendLog("hello from Hyrule: botw-coop M0 is running in v1.9.0");
    }
}

/* nnMain is the game's entry point. Starting from here (instead of exl_main)
   means the SDK is fully initialised before we create threads or sockets. */
extern "C" void nnMain();

HOOK_DEFINE_TRAMPOLINE(NnMainHook) {
    static void Callback() {
        StartCoop();
        Orig();
    }
};

extern "C" void exl_main(void* x0, void* x1) {
    exl::hook::Initialize();
    NnMainHook::InstallAtFuncPtr(nnMain);
}

extern "C" NORETURN void exl_exception_entry() {
    EXL_ABORT("Default exception handler called!");
}
