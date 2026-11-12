#include "notify.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <lib.hpp>
#include <nn/os.hpp>

#include "items.hpp"
#include "offsets.hpp"
#include "player.hpp"

namespace coop::game::notify {

    namespace {
        uintptr_t Base() { return exl::util::GetMainModuleInfo().m_Total.m_Start; }

        /* sead::SafeString: {vtable, const char*}. */
        struct SafeString {
            uintptr_t vt;
            const char* str;
        };
        /* The game's message result: UTF-16 text and its length. */
        struct MessageString {
            const char16_t* text;
            u32 length;
        };
        using GetMessageFn = int (*)(const SafeString*, const SafeString*, MessageString*);
        using ShowInfoFn = void (*)(u32);

        constexpr int MaxText = 96;
        constexpr int QueueLen = 8;
        constexpr s64 ShowEveryMs = 3500;  /* the banner shows for about 3 s */
        constexpr s64 OverrideMs = 3300;   /* the screen loads its text within this (2 s was too short on a busy machine: the game's own line showed) */

        /* Messages waiting to be shown (any thread adds, the game thread shows). */
        char g_Queue[QueueLen][MaxText];
        int g_Head = 0, g_Tail = 0;
        std::atomic_flag g_Lock = ATOMIC_FLAG_INIT;

        /* The two banner slots we borrow, alternated so a new message replaces the last. */
        char16_t g_Text[2][MaxText];
        u32 g_TextLen[2];
        int g_Slot = 0;
        std::atomic<s64> g_OverrideUntil{0};
        std::atomic<int> g_ActiveSlot{-1};
        s64 g_NextAt = 0;

        void Lock() {
            while (g_Lock.test_and_set(std::memory_order_acquire)) {}
        }
        void Unlock() { g_Lock.clear(std::memory_order_release); }

        s64 NowMs() { return nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds(); }

        /* UTF-8 to UTF-16 (BMP only); returns the length. */
        u32 ToUtf16(const char* in, char16_t* out, int cap) {
            u32 n = 0;
            const auto* s = reinterpret_cast<const unsigned char*>(in);
            while (*s && n + 1 < static_cast<u32>(cap)) {
                u32 c = *s++;
                if (c >= 0xe0 && s[0] && s[1]) {
                    c = ((c & 0x0f) << 12) | ((s[0] & 0x3f) << 6) | (s[1] & 0x3f);
                    s += 2;
                } else if (c >= 0xc0 && s[0]) {
                    c = ((c & 0x1f) << 6) | (s[0] & 0x3f);
                    s += 1;
                }
                out[n++] = static_cast<char16_t>(c);
            }
            out[n] = 0;
            return n;
        }

        bool Is(const SafeString* s, const char* text) {
            return s && s->str && std::strcmp(s->str, text) == 0;
        }

        const char* SlotLabel(int slot) { return slot == 0 ? "0028" : "0061"; }
    }

    HOOK_DEFINE_TRAMPOLINE(GetMessageHook) {
        static int Callback(const SafeString* file, const SafeString* label, MessageString* out) {
            int slot = g_ActiveSlot.load(std::memory_order_relaxed);
            if (slot >= 0 && Is(file, "LayoutMsg/MainScreen_00") && Is(label, SlotLabel(slot)) &&
                NowMs() < g_OverrideUntil.load(std::memory_order_relaxed)) {
                out->text = g_Text[slot];
                out->length = g_TextLen[slot];
                return 0;
            }
            return Orig(file, label, out);
        }
    };

    void Install() { GetMessageHook::InstallAtOffset(offsets::Ui_GetMessage); }

    void Show(const char* fmt, ...) {
        char buf[MaxText];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        Logging.Log("[coop] notify: %s", buf);
        Lock();
        int next = (g_Tail + 1) % QueueLen;
        if (next != g_Head) { /* full: drop it (it's in the log) */
            std::memcpy(g_Queue[g_Tail], buf, sizeof(buf));
            g_Tail = next;
        }
        Unlock();
    }

    namespace {
        struct GiftNote {
            bool to_me;
            char partner[32];
            char actor[48];
            int count;
            s64 last;
        };
        GiftNote g_Gift{};

        void FlushGift() {
            if (g_Gift.count == 0) return;
            char name[64];
            if (!DisplayName(g_Gift.actor, name, sizeof(name))) std::snprintf(name, sizeof(name), "%s", g_Gift.actor);
            char what[80];
            if (g_Gift.count > 1) std::snprintf(what, sizeof(what), "%s x%d", name, g_Gift.count);
            else std::snprintf(what, sizeof(what), "%s", name);
            if (g_Gift.to_me) Show("%s gave you %s", g_Gift.partner, what);
            else Show("You gave %s to %s", what, g_Gift.partner);
            g_Gift.count = 0;
        }
    }

    void Gift(bool to_me, const char* partner, const char* actor) {
        if (g_Gift.count && (g_Gift.to_me != to_me || std::strcmp(g_Gift.partner, partner) != 0 ||
                             std::strcmp(g_Gift.actor, actor) != 0))
            FlushGift();
        g_Gift.to_me = to_me;
        std::snprintf(g_Gift.partner, sizeof(g_Gift.partner), "%s", partner);
        std::snprintf(g_Gift.actor, sizeof(g_Gift.actor), "%s", actor);
        g_Gift.count++;
        g_Gift.last = NowMs();
    }

    void Tick(s64 now) {
        if (g_Gift.count && now - g_Gift.last > 1500) FlushGift();
        if (now < g_NextAt) return;
        /* Only where a banner can be seen: Link in the world, and no menu or
           loading screen for the last 1.5 s (messages wait in the queue). */
        float me[3];
        if (!GetLocalPlayerPos(me) || now - items::LastPausedMs() < 1500) return;
        char msg[MaxText];
        Lock();
        bool have = g_Head != g_Tail;
        if (have) {
            std::memcpy(msg, g_Queue[g_Head], sizeof(msg));
            g_Head = (g_Head + 1) % QueueLen;
        }
        Unlock();
        if (!have) return;
        g_Slot ^= 1;
        g_TextLen[g_Slot] = ToUtf16(msg, g_Text[g_Slot], MaxText);
        g_ActiveSlot.store(g_Slot);
        g_OverrideUntil.store(now + OverrideMs);
        reinterpret_cast<ShowInfoFn>(Base() + offsets::Ui_ShowInfo)(g_Slot == 0 ? offsets::UiInfo_A : offsets::UiInfo_B);
        g_NextAt = now + ShowEveryMs;
    }

    bool DisplayName(const char* actor, char* out, size_t cap) {
        if (cap == 0) return false;
        out[0] = '\0';
        if (!actor || !actor[0]) return false;
        const char* file = "ActorType/Item";
        struct {
            const char* prefix;
            const char* file;
        } const files[] = {
            {"Weapon_Sword_", "ActorType/WeaponSmallSword"}, {"Weapon_Lsword_", "ActorType/WeaponLargeSword"},
            {"Weapon_Spear_", "ActorType/WeaponSpear"},      {"Weapon_Bow_", "ActorType/WeaponBow"},
            {"Weapon_Shield_", "ActorType/WeaponShield"},    {"Animal_Insect_", "ActorType/CapturedActor"},
            {"BeeHome", "ActorType/CapturedActor"},          {"Armor_", "ActorType/ArmorUpper"},
        };
        for (auto& f : files)
            if (std::strncmp(actor, f.prefix, std::strlen(f.prefix)) == 0) file = f.file;

        char label[96];
        std::snprintf(label, sizeof(label), "%s_Name", actor);
        uintptr_t got = *reinterpret_cast<const uintptr_t*>(Base() + offsets::Got_SafeStringVtable);
        SafeString f{got + 0x10, file}, l{got + 0x10, label};
        MessageString m{nullptr, 0};
        if (GetMessageHook::Orig(&f, &l, &m) != 0 || !m.text || m.length == 0) return false;

        /* UTF-16 to UTF-8, skipping control tags (0x0e group type size params...). */
        size_t n = 0;
        for (u32 i = 0; i < m.length && m.text[i]; i++) {
            char16_t c = m.text[i];
            if (c == 0x0e) {
                if (i + 3 < m.length) i += 3 + m.text[i + 3] / 2;
                continue;
            }
            if (c < 0x80) {
                if (n + 1 >= cap) break;
                out[n++] = static_cast<char>(c);
            } else if (c < 0x800) {
                if (n + 2 >= cap) break;
                out[n++] = static_cast<char>(0xc0 | (c >> 6));
                out[n++] = static_cast<char>(0x80 | (c & 0x3f));
            } else {
                if (n + 3 >= cap) break;
                out[n++] = static_cast<char>(0xe0 | (c >> 12));
                out[n++] = static_cast<char>(0x80 | ((c >> 6) & 0x3f));
                out[n++] = static_cast<char>(0x80 | (c & 0x3f));
            }
        }
        out[n] = '\0';
        return n > 0;
    }
}
