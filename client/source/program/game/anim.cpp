#include "anim.hpp"

#include <cstring>

namespace coop::game::anim {

    namespace {
        bool Starts(const char* s, const char* prefix) { return std::strncmp(s, prefix, std::strlen(prefix)) == 0; }
        bool Is(const char* s, const char* v) { return std::strcmp(s, v) == 0; }
    }

    Category FromPlayerAS(const char* n) {
        if (!n || !*n) return Unknown;
        if (Is(n, "ClimbWait") || Is(n, "LadderWait")) return ClimbWait;
        if (Starts(n, "Climb") || Starts(n, "Ladder") || Is(n, "GroundDownToWall") || Starts(n, "WallDash")) return Climb;
        if (Is(n, "SwimWait")) return SwimWait;
        if (Is(n, "SwimDash")) return SwimDash;
        if (Starts(n, "Swim")) return Swim;
        if (Is(n, "ParashawlGlide") || Is(n, "SurfingOnPara")) return Glide;
        if (Starts(n, "Surfing") || Starts(n, "WakeBoard")) return Surf;
        if (Starts(n, "Horse") || Starts(n, "Rideon") || Starts(n, "AimHorse")) return Horse;
        if (Is(n, "Jump") || Is(n, "BackJump") || Is(n, "CutJumpSt") || Is(n, "WaterDiveJump")) return Jump;
        if (Is(n, "Fall") || Is(n, "CutFall") || Is(n, "SlippingDown")) return Fall;
        if (Starts(n, "Move") || Is(n, "DemoMove") || Is(n, "Stair") || Starts(n, "SquatMove") ||
            Starts(n, "SquatAtnMove") || Is(n, "Turn") || Is(n, "Brake") || Is(n, "Push") || Is(n, "Slide"))
            return Move;
        if (Starts(n, "Wait") || Is(n, "DemoWait") || Starts(n, "Squat") || Starts(n, "Talk") ||
            Starts(n, "LookAround") || Is(n, "Land") || Is(n, "StepLand") || Starts(n, "SheikPad") || Is(n, "Get"))
            return Wait;
        return Unknown; /* attacks, items, damage...: keep whatever was playing */
    }

    Action ActionFromPlayerAS(const char* n, int slot, int* ms, bool* ends_hold) {
        *ms = 0;
        *ends_hold = false;
        if (!n || !*n) return NoAction;
        if (slot == 3) {
            if (Is(n, "FaceShieldGuard")) { *ms = 1000; return Guard; }
            return NoAction;
        }
        if (Is(n, "BowCharge")) { *ms = 8000; return BowAim; }
        if (Is(n, "BowShoot")) { *ms = 500; return BowShoot; }
        if (Is(n, "WeaponThrowCharge")) { *ms = 5000; return ThrowAim; }
        if (Starts(n, "WeaponThrow")) { *ms = 600; return Throw; }
        if (Is(n, "CutChargeWait")) { *ms = 5000; return ChargeWait; }
        if (Is(n, "CutS2")) { *ms = 700; return Attack2; }
        if (Is(n, "CutS3")) { *ms = 700; return Attack3; }
        if (Is(n, "CutSF")) { *ms = 900; return AttackF; }
        if (Starts(n, "Cut") && !Is(n, "CutJumpSt") && !Is(n, "CutFall")) {
            if (std::strstr(n, "Charge")) { *ms = 900; return Spin; }
            *ms = 700;
            return Attack1;
        }
        /* Back to plain standing or moving on the body: aiming/charging is over. */
        if (slot == 0 && (Is(n, "Wait") || Starts(n, "Move"))) *ends_hold = true;
        return NoAction;
    }

    const char* PuppetActionAS(Action a) {
        switch (a) {
            case Attack1: return "Attack1";
            case Attack2: return "Attack2";
            case Attack3: return "Attack3";
            case AttackF: return "AttackF";
            case Spin: return "Spin";
            case ChargeWait: return "ChargeWait";
            case BowAim: return "BowAim";
            case BowShoot: return "BowShoot";
            case ThrowAim: return "ThrowAim";
            case Throw: return "Throw";
            case Guard: return "Guard";
            default: return nullptr;
        }
    }

    const char* PuppetAS(Category c, float speed) {
        switch (c) {
            case Wait: return "Wait";
            case Move: return speed < 2.6f ? "Walk" : speed < 6.5f ? "Run" : "Dash";
            case Climb: return "Climb";
            case ClimbWait: return "ClimbWait";
            case Swim: return "Swim";
            case SwimWait: return "SwimWait";
            case SwimDash: return "SwimDash";
            case Glide: return "Glide";
            case Jump: return "Jump";
            case Fall: return "Fall";
            /* The rider's clips follow the horse's gait (see DriveHorse). */
            case Horse: return speed < 0.5f ? "Horse" : speed < 3.0f ? "HorseGear1" : speed < 7.0f ? "HorseGear2" : "HorseGear3";
            case Surf: return "Surf";
            default: return nullptr;
        }
    }
}
