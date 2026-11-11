#include "config.hpp"

#include <cstdlib>
#include <cstring>

#include <lib.hpp>
#include <nn/fs.hpp>

#include "coop_config.hpp"

namespace nn::fs {
    Result MountSdCard(const char* mount_name);
}

namespace nn::account {
    struct Uid { u64 id[2]; };
    struct Nickname { char name[33]; };
    Result Initialize();
    Result ListAllUsers(int* out_count, Uid* out, int count);
    Result GetNickname(Nickname* out, const Uid& uid);
}

namespace coop {

    namespace {

        void Copy(char* dst, size_t cap, const char* src, size_t len) {
            if (len >= cap) len = cap - 1;
            std::memcpy(dst, src, len);
            dst[len] = '\0';
        }

        const char* Trim(const char* s, const char* end, size_t* len) {
            while (s < end && (*s == ' ' || *s == '\t')) s++;
            while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
            *len = end - s;
            return s;
        }

        bool IsTrue(const char* v, size_t n) {
            return n > 0 && (v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y' || v[0] == 'Y' ||
                             (n > 1 && (v[0] == 'o' || v[0] == 'O') && (v[1] == 'n' || v[1] == 'N')));
        }

        void Apply(Settings& out, const char* key, size_t klen, const char* val, size_t vlen) {
            auto is = [&](const char* k) { return std::strlen(k) == klen && std::memcmp(k, key, klen) == 0; };
            if (is("name")) Copy(out.name, sizeof(out.name), val, vlen);
            else if (is("server")) Copy(out.host, sizeof(out.host), val, vlen);
            else if (is("password")) Copy(out.password, sizeof(out.password), val, vlen);
            else if (is("ghost_actor")) Copy(out.ghost_actor, sizeof(out.ghost_actor), val, vlen);
            else if (is("echo")) out.echo = IsTrue(val, vlen);
            else if (is("autostart")) out.autostart = IsTrue(val, vlen);
            else if (is("test_spawn")) Copy(out.test_spawn, sizeof(out.test_spawn), val, vlen);
            else if (is("test_flag")) Copy(out.test_flag, sizeof(out.test_flag), val, vlen);
            else if (is("test_move")) out.test_move = IsTrue(val, vlen);
            else if (is("test_menu")) out.test_menu = IsTrue(val, vlen);
            else if (is("test_revive")) out.test_revive = IsTrue(val, vlen);
            else if (is("test_bind")) Copy(out.test_bind, sizeof(out.test_bind), val, vlen);
            else if (is("test_keys")) Copy(out.test_keys, sizeof(out.test_keys), val, vlen);
            else if (is("test_drop")) Copy(out.test_drop, sizeof(out.test_drop), val, vlen);
            else if (is("beasts_anywhere")) out.beasts_anywhere = IsTrue(val, vlen);
            else if (is("log_file")) out.log_file = IsTrue(val, vlen);
            else if (is("test_gate")) Copy(out.test_gate, sizeof(out.test_gate), val, vlen);
            else if (is("test_heal")) {
                char tmp[8];
                Copy(tmp, sizeof(tmp), val, vlen);
                out.test_heal = std::atoi(tmp);
            }
            else if (is("test_follow")) {
                char tmp[8];
                Copy(tmp, sizeof(tmp), val, vlen);
                out.test_follow = std::atoi(tmp);
            }
            else if (is("test_research")) {
                char tmp[8];
                Copy(tmp, sizeof(tmp), val, vlen);
                out.test_research = std::atoi(tmp);
            }
            else if (is("test_life")) {
                char tmp[8];
                Copy(tmp, sizeof(tmp), val, vlen);
                out.test_life = std::atoi(tmp);
            }
            else if (is("port")) {
                char tmp[8];
                Copy(tmp, sizeof(tmp), val, vlen);
                int p = std::atoi(tmp);
                if (p > 0 && p < 65536) out.port = static_cast<u16>(p);
            }
        }

        void Parse(Settings& out, const char* text, size_t size) {
            const char* p = text;
            const char* end = text + size;
            while (p < end) {
                const char* eol = static_cast<const char*>(std::memchr(p, '\n', end - p));
                if (!eol) eol = end;
                const char* hash = static_cast<const char*>(std::memchr(p, '#', eol - p));
                const char* line_end = hash ? hash : eol;
                const char* eq = static_cast<const char*>(std::memchr(p, '=', line_end - p));
                if (eq) {
                    size_t klen, vlen;
                    const char* k = Trim(p, eq, &klen);
                    const char* v = Trim(eq + 1, line_end, &vlen);
                    if (klen) Apply(out, k, klen, v, vlen);
                }
                p = eol + 1;
            }
        }

        /* Opens the first config.ini that exists. The mod's own RomFS folder is
           layered into the game's "content:" mount by the emulator, so it needs
           no extra permissions; the SD card is a fallback for real consoles. */
        bool OpenConfig(nn::fs::FileHandle* file, const char** where) {
            static const char RomPath[] = "content:/botw-coop/config.ini";
            if (nn::fs::OpenFile(file, RomPath, nn::fs::OpenMode_Read) == 0) { *where = RomPath; return true; }

            Result rc = nn::fs::MountSdCard("coopsd");
            if (rc != 0) {
                Logging.Log("[coop] no %s and can't mount the SD card (0x%x)", RomPath, rc);
                return false;
            }
            static const char SdPath[] = "coopsd:/botw-coop/config.ini";
            rc = nn::fs::OpenFile(file, SdPath, nn::fs::OpenMode_Read);
            if (rc != 0) {
                Logging.Log("[coop] no config.ini in the mod's romfs or on the SD card (0x%x)", rc);
                return false;
            }
            *where = SdPath;
            return true;
        }

        bool ReadConfigFile(Settings& out) {
            nn::fs::FileHandle file;
            const char* where = nullptr;
            if (!OpenConfig(&file, &where)) return false;

            static char buf[2048];
            long size = 0;
            bool ok = nn::fs::GetFileSize(&size, file) == 0 && size > 0;
            if (ok) {
                if (size > static_cast<long>(sizeof(buf))) size = sizeof(buf);
                ok = nn::fs::ReadFile(file, 0, buf, static_cast<ulong>(size)) == 0;
            }
            nn::fs::CloseFile(file);
            if (ok) {
                Parse(out, buf, static_cast<size_t>(size));
                Logging.Log("[coop] read %s", where);
            }
            return ok;
        }

        void UseProfileName(Settings& out) {
            nn::account::Initialize();
            nn::account::Uid users[8];
            int count = 0;
            if (nn::account::ListAllUsers(&count, users, 8) != 0 || count <= 0) return;
            nn::account::Nickname nick{};
            if (nn::account::GetNickname(&nick, users[0]) != 0) return;
            Copy(out.name, sizeof(out.name), nick.name, std::strlen(nick.name));
        }
    }

    void LoadSettings(Settings& out) {
        Copy(out.host, sizeof(out.host), COOP_SERVER_HOST, std::strlen(COOP_SERVER_HOST));
        out.port = COOP_SERVER_PORT;
        out.name[0] = '\0';
        Copy(out.password, sizeof(out.password), COOP_PASSWORD, std::strlen(COOP_PASSWORD));
        static const char DefaultGhost[] = "CoopPlayer";
        Copy(out.ghost_actor, sizeof(out.ghost_actor), DefaultGhost, sizeof(DefaultGhost) - 1);
        out.echo = false;
        out.autostart = false;
        out.test_spawn[0] = '\0';
        out.test_flag[0] = '\0';
        out.test_move = false;
        out.test_menu = false;
        out.test_revive = false;
        out.test_bind[0] = '\0';
        out.test_keys[0] = '\0';
        out.test_drop[0] = '\0';
        out.test_life = 0;
        out.test_research = 0;
        out.test_heal = 0;
        out.test_follow = 0;
        out.beasts_anywhere = false;
        out.log_file = false;
        out.test_gate[0] = '\0';

        bool from_file = ReadConfigFile(out);
        if (out.name[0] == '\0') UseProfileName(out);
        if (out.name[0] == '\0') Copy(out.name, sizeof(out.name), COOP_PLAYER_NAME, std::strlen(COOP_PLAYER_NAME));

        Logging.Log("[coop] settings %s: name=%s server=%s:%d", from_file ? "from config.ini" : "(defaults)",
                    out.name, out.host, out.port);
    }
}
