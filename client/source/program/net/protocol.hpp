#pragma once

/* Wire protocol shared with server/protocol.go. Keep the two in sync.

   Every message: u32 length (= 4 + payload size), u16 type, u16 flags, payload.
   Everything is little-endian. Strings are u16 length + UTF-8 bytes. */

#include <common.hpp>
#include <cstring>
#include <string_view>

namespace coop::net {

    /* 2: Heal, Downed and EnemyPos messages; going down replaced the instant revive. */
    constexpr u16 ProtocolVersion = 2;
    constexpr u32 MaxMessageSize = 64 * 1024;
    constexpr u32 HeaderSize = 4; /* type + flags */

    enum class MsgType : u16 {
        Hello        = 1,
        Welcome      = 2,
        Reject       = 3,
        Ping         = 4,
        Pong         = 5,
        Log          = 6,
        PlayerJoined = 10,
        PlayerLeft   = 11,
        HostChanged  = 12,
        PlayerState  = 20,
        /* Game messages: the server forwards these to everyone else in the
           room, prefixed with the sender's id. */
        WorldState   = 30, /* host -> all: f32 WM_Time, s32 WM_NumberOfDays, u8 weather, u8 climate */
        Flag         = 31, /* any -> all: u8 kind (1 earned now, 2 catch-up), string name */
        EnemyLife    = 32, /* any -> all: u32 area, u32 enemy HashId, s32 life */
        Revived      = 33, /* any -> all: (no payload) I went down and a partner's presence revived me */
        Gear         = 34, /* any -> all: 3 strings: worn head, upper, lower armor actor names ("" = none) */
        Gift         = 36, /* any -> all: u8 recipient id, string weapon actor name[, u32 bonus flags, s32 bonus value, s32 durability] */
        Stamps       = 40, /* any -> all: u8 count, then per stamp: u8 icon, f32 pos[3] (the sender's own map stamps) */
        Downed       = 39, /* any -> all: u8 state (1 down, 0 back up, 2 out of time), f32 pos[3], u8 seconds left */
        Heal         = 38, /* any -> all: u16 quarter hearts: I just ate this much healing near you */
        EnemyPos     = 37, /* any -> all: u32 area, u8 count, then per enemy: u32 HashId, f32 pos[3], f32 rot_y */
        Weapons      = 35, /* any -> all: u8 count, then per weapon: string name, string bone, f32[12] offset from the bone */
    };

    /* Per-frame state of a player's own Link. Must stay 44 bytes. */
    struct PlayerState {
        float pos[3];
        float rot_y;
        float vel[3];
        u32 anim_hash;
        u16 health;
        u16 max_hp;
        u32 map_area;
        u32 flags; /* bit 0 gliding, 1 climbing, 2 swimming, 3 mounted */
    };
    static_assert(sizeof(PlayerState) == 44);

    /* Builds one message into a fixed buffer. */
    class Writer {
        u8* m_Buf;
        size_t m_Cap;
        size_t m_Len = 8;
        bool m_Overflow = false;

    public:
        Writer(u8* buf, size_t cap, MsgType type) : m_Buf(buf), m_Cap(cap) {
            u16 t = static_cast<u16>(type);
            std::memcpy(m_Buf + 4, &t, 2);
            std::memset(m_Buf + 6, 0, 2);
        }

        void Bytes(const void* data, size_t n) {
            if (m_Len + n > m_Cap) { m_Overflow = true; return; }
            std::memcpy(m_Buf + m_Len, data, n);
            m_Len += n;
        }
        void U8(u8 v) { Bytes(&v, 1); }
        void U16(u16 v) { Bytes(&v, 2); }
        void U32(u32 v) { Bytes(&v, 4); }
        void U64(u64 v) { Bytes(&v, 8); }
        void Str(std::string_view s) {
            U16(static_cast<u16>(s.size()));
            Bytes(s.data(), s.size());
        }

        /* Finalises the length prefix. Returns the total size, or 0 on overflow. */
        size_t Finish() {
            if (m_Overflow) return 0;
            u32 len = static_cast<u32>(m_Len - 4);
            std::memcpy(m_Buf, &len, 4);
            return m_Len;
        }
    };

    /* Reads fields out of a received payload. */
    class Reader {
        const u8* m_Ptr;
        size_t m_Left;
        bool m_Ok = true;

    public:
        Reader(const u8* p, size_t n) : m_Ptr(p), m_Left(n) {}

        bool Ok() const { return m_Ok; }
        size_t Left() const { return m_Left; }
        const u8* Take(size_t n) {
            if (!m_Ok || m_Left < n) { m_Ok = false; return nullptr; }
            const u8* p = m_Ptr;
            m_Ptr += n;
            m_Left -= n;
            return p;
        }
        template<typename T> T Get() {
            T v{};
            if (const u8* p = Take(sizeof(T))) std::memcpy(&v, p, sizeof(T));
            return v;
        }
        std::string_view Str() {
            u16 n = Get<u16>();
            const u8* p = Take(n);
            return p ? std::string_view(reinterpret_cast<const char*>(p), n) : std::string_view();
        }
    };
}
