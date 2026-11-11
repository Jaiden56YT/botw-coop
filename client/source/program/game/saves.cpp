#include "saves.hpp"

#include <cstdio>
#include <cstring>

#include <lib.hpp>
#include <nn/fs.hpp>

/* Paths are built into fixed buffers; anything too long is skipped by length checks. */
#pragma GCC diagnostic ignored "-Wformat-truncation"

namespace nn::account {
    struct Uid;
}

/* The nn::fs functions BotW imports that take a path (see the game's
   import list). Declared here with their exact signatures. */
namespace nn::fs {
    Result CreateFile(char const* path, s64 size);
    Result DeleteFile(char const* path);
    Result GetEntryType(DirectoryEntryType* type, char const* path);
    Result OpenDirectory(DirectoryHandle* handle, char const* path, s32 mode);
    Result ReadDirectory(s64* count, DirectoryEntry* entries, DirectoryHandle handle, s64 max);
    void CloseDirectory(DirectoryHandle handle);
    Result CreateDirectory(char const* path);
    Result DeleteDirectoryRecursively(char const* path);
    Result MountSaveData(char const* name, nn::account::Uid const& user);
    Result CommitSaveData(char const* name);
}

namespace coop::game::saves {

    namespace {
        constexpr const char* CoopDir = "coop";

        bool g_Enabled = true;
        bool g_Decided = false;
        int g_Backups = 3;      /* `backups` in the config: how many launches' backups to keep (0-3) */
        constexpr long JournalBudget = 0x100000;
        long g_Uncommitted = 0; /* bytes written since the last commit */
        char g_Mount[32];       /* the save data's mount name, e.g. "save" */
        size_t g_MountLen = 0;
        int g_Redirects = 0;

        /* "save:/0/game_data.sav" -> "save:/coop/0/game_data.sav". Returns
           `path` unchanged for anything else. */
        const char* Rewrite(const char* path, char* buf, size_t cap) {
            if (!g_Enabled || !g_MountLen || !path) return path;
            if (std::strncmp(path, g_Mount, g_MountLen) != 0 || path[g_MountLen] != ':') return path;
            const char* rest = path + g_MountLen + 1;
            while (*rest == '/') rest++;
            size_t dl = std::strlen(CoopDir);
            if (std::strncmp(rest, CoopDir, dl) == 0 && (rest[dl] == '/' || rest[dl] == '\0')) return path;
            std::snprintf(buf, cap, "%s:/%s/%s", g_Mount, CoopDir, rest);
            if (g_Redirects++ < 40) Logging.Log("[coop] saves: %s -> %s", path, buf);
            return buf;
        }

        /* Reads `separate_save` from the mod's config (the network thread
           reads the rest later; the game mounts its save before that). */
        void Decide() {
            if (g_Decided) return;
            g_Decided = true;
            nn::fs::FileHandle f;
            if (nn::fs::OpenFile(&f, "content:/botw-coop/config.ini", nn::fs::OpenMode_Read) != 0) return;
            static char buf[2048];
            long size = 0;
            if (nn::fs::GetFileSize(&size, f) == 0 && size > 0) {
                if (size >= static_cast<long>(sizeof(buf))) size = sizeof(buf) - 1;
                if (nn::fs::ReadFile(f, 0, buf, static_cast<ulong>(size)) == 0) {
                    buf[size] = '\0';
                    for (char* line = buf; line && *line;) {
                        char* next = std::strchr(line, '\n');
                        if (next) *next++ = '\0';
                        while (*line == ' ' || *line == '\t') line++;
                        if (std::strncmp(line, "separate_save", 13) == 0) {
                            char* eq = std::strchr(line, '=');
                            if (eq) {
                                eq++;
                                while (*eq == ' ') eq++;
                                g_Enabled = !(std::strncmp(eq, "false", 5) == 0 || *eq == '0' || std::strncmp(eq, "no", 2) == 0);
                            }
                        }
                        if (std::strncmp(line, "backups", 7) == 0) {
                            char* eq = std::strchr(line, '=');
                            if (eq) {
                                eq++;
                                while (*eq == ' ') eq++;
                                if (*eq >= '0' && *eq <= '3') g_Backups = *eq - '0';
                            }
                        }
                        line = next;
                    }
                }
            }
            nn::fs::CloseFile(f);
        }
    }

    /* ---- hooks: every path-taking call the game makes ---- */

    HOOK_DEFINE_TRAMPOLINE(OpenFileHook) {
        static Result Callback(nn::fs::FileHandle * h, char const* path, int mode) {
            char buf[0x310];
            return Orig(h, Rewrite(path, buf, sizeof(buf)), mode);
        }
    };
    HOOK_DEFINE_TRAMPOLINE(CreateFileHook) {
        static Result Callback(char const* path, s64 size) {
            char buf[0x310];
            return Orig(Rewrite(path, buf, sizeof(buf)), size);
        }
    };
    HOOK_DEFINE_TRAMPOLINE(DeleteFileHook) {
        static Result Callback(char const* path) {
            char buf[0x310];
            return Orig(Rewrite(path, buf, sizeof(buf)));
        }
    };
    HOOK_DEFINE_TRAMPOLINE(GetEntryTypeHook) {
        static Result Callback(nn::fs::DirectoryEntryType * type, char const* path) {
            char buf[0x310];
            return Orig(type, Rewrite(path, buf, sizeof(buf)));
        }
    };
    HOOK_DEFINE_TRAMPOLINE(OpenDirectoryHook) {
        static Result Callback(nn::fs::DirectoryHandle * h, char const* path, s32 mode) {
            char buf[0x310];
            return Orig(h, Rewrite(path, buf, sizeof(buf)), mode);
        }
    };
    HOOK_DEFINE_TRAMPOLINE(CreateDirectoryHook) {
        static Result Callback(char const* path) {
            char buf[0x310];
            return Orig(Rewrite(path, buf, sizeof(buf)));
        }
    };
    HOOK_DEFINE_TRAMPOLINE(DeleteDirectoryRecursivelyHook) {
        static Result Callback(char const* path) {
            char buf[0x310];
            return Orig(Rewrite(path, buf, sizeof(buf)));
        }
    };

    namespace {
        /* Copying the normal save into coop/ the first time (with the real,
           un-redirected calls). */
        alignas(16) u8 g_CopyBuf[0x40000];
        nn::fs::DirectoryEntry g_Entries[16];
        int g_Copied = 0;

        bool CopyFile(const char* from, const char* to) {
            nn::fs::FileHandle in, out;
            Result orc = OpenFileHook::Orig(&in, from, nn::fs::OpenMode_Read);
            if (orc != 0) {
                Logging.Log("[coop] saves: can't open %s (0x%x)", from, orc);
                return false;
            }
            long size = 0;
            nn::fs::GetFileSize(&size, in);
            /* A real Switch only holds 1.5 MB of BotW's save writes between
               two commits (the save data's journal); more than that and the
               write fails. So commit before the next file would go past 1 MB.
               (A save file is just over 1 MB, the biggest there is.) */
            if (g_Uncommitted > 0 && g_Uncommitted + size > JournalBudget) {
                nn::fs::CommitSaveData(g_Mount);
                g_Uncommitted = 0;
            }
            g_Uncommitted += size + 0x8000; /* plus the file's own bookkeeping */
            bool ok = CreateFileHook::Orig(to, size) == 0 && OpenFileHook::Orig(&out, to, nn::fs::OpenMode_Write) == 0;
            if (ok) {
                for (long pos = 0; pos < size && ok;) {
                    ulong n = size - pos > static_cast<long>(sizeof(g_CopyBuf)) ? sizeof(g_CopyBuf) : static_cast<ulong>(size - pos);
                    ok = nn::fs::ReadFile(in, pos, g_CopyBuf, n) == 0 &&
                         nn::fs::WriteFile(out, pos, g_CopyBuf, n, nn::fs::WriteOption::CreateOption(0)) == 0;
                    pos += n;
                }
                nn::fs::FlushFile(out);
                nn::fs::CloseFile(out);
            }
            nn::fs::CloseFile(in);
            if (ok) g_Copied++;
            return ok;
        }

        /* Copies directory `from` into `to` (which exists), skipping coop/.
           Files are copied as the directory is read (the picture book alone
           holds hundreds); subdirectories are collected and copied after. */
        bool CopyTree(const char* from, const char* to, int depth) {
            if (depth > 4) return false;
            nn::fs::DirectoryHandle dir;
            Result orc = OpenDirectoryHook::Orig(&dir, from, nn::fs::OpenDirectoryMode_All);
            if (orc != 0) {
                Logging.Log("[coop] saves: can't open directory %s (0x%x)", from, orc);
                return false;
            }
            constexpr int MaxDirs = 32;
            char dirs[MaxDirs][64];
            int ndirs = 0;
            bool ok = true;
            const char* sep = from[std::strlen(from) - 1] == '/' ? "" : "/";
            for (;;) {
                s64 got = 0;
                Result rrc = nn::fs::ReadDirectory(&got, g_Entries, dir, 16);
                if (rrc != 0) {
                    Logging.Log("[coop] saves: reading directory %s failed (0x%x)", from, rrc);
                    ok = false;
                }
                if (rrc != 0 || got <= 0) break;
                for (s64 i = 0; i < got; i++) {
                    const char* name = g_Entries[i].m_Name;
                    if (std::strlen(name) >= 64) {
                        Logging.Log("[coop] saves: name too long, not copied: %.60s...", name);
                        ok = false;
                        continue;
                    }
                    /* The entry type (0 = directory, 1 = file) is the byte at
                       +0x304 (the header's m_Type at +0x301 is padding). */
                    bool is_dir = (g_Entries[i].field_304 & 0xff) == 0;
                    if (is_dir) {
                        if (depth == 0 && std::strncmp(name, CoopDir, std::strlen(CoopDir)) == 0) continue; /* coop/, coop_backup/ */
                        if (ndirs < MaxDirs) std::snprintf(dirs[ndirs++], 64, "%s", name);
                        else ok = false;
                        continue;
                    }
                    char src[0x200], dst[0x200];
                    std::snprintf(src, sizeof(src), "%s%s%s", from, sep, name);
                    std::snprintf(dst, sizeof(dst), "%s/%s", to, name);
                    ok &= CopyFile(src, dst);
                }
            }
            nn::fs::CloseDirectory(dir);
            for (int i = 0; i < ndirs; i++) {
                char src[0x200], dst[0x200];
                std::snprintf(src, sizeof(src), "%s%s%s", from, sep, dirs[i]);
                std::snprintf(dst, sizeof(dst), "%s/%s", to, dirs[i]);
                CreateDirectoryHook::Orig(dst);
                ok &= CopyTree(src, dst, depth + 1);
            }
            return ok;
        }

        /* Backups of the co-op saves, as they were when the game started: the
           last three launches, in save:/coop_backups/1, 2 and 3. Each launch
           replaces the oldest; `next` (one digit) says which that is. */
        void Backup() {
            const int BackupCount = g_Backups;
            char coop[48], dir[48], next_path[64], backup[64];
            std::snprintf(coop, sizeof(coop), "%s:/%s", g_Mount, CoopDir);
            std::snprintf(dir, sizeof(dir), "%s:/%s_backups", g_Mount, CoopDir);
            std::snprintf(next_path, sizeof(next_path), "%s/next", dir);
            nn::fs::DirectoryEntryType type;
            /* Fewer backups wanted than there are (a real Switch's save data
               has room for one): the extra ones go. */
            for (int i = BackupCount + 1; i <= 3; i++) {
                std::snprintf(backup, sizeof(backup), "%s/%d", dir, i);
                if (GetEntryTypeHook::Orig(&type, backup) == 0) {
                    DeleteDirectoryRecursivelyHook::Orig(backup);
                    nn::fs::CommitSaveData(g_Mount);
                }
            }
            if (BackupCount <= 0) return;
            if (GetEntryTypeHook::Orig(&type, dir) != 0) CreateDirectoryHook::Orig(dir);

            int slot = 1;
            nn::fs::FileHandle f;
            if (OpenFileHook::Orig(&f, next_path, nn::fs::OpenMode_Read) == 0) {
                char c = '1';
                if (nn::fs::ReadFile(f, 0, &c, 1) == 0 && c >= '1' && c < '1' + BackupCount) slot = c - '0';
                nn::fs::CloseFile(f);
            }
            std::snprintf(backup, sizeof(backup), "%s/%d", dir, slot);
            if (GetEntryTypeHook::Orig(&type, backup) == 0) {
                DeleteDirectoryRecursivelyHook::Orig(backup);
                nn::fs::CommitSaveData(g_Mount); /* its room is free for the new one */
            }
            int before = g_Copied;
            g_Uncommitted = 0;
            bool ok = CreateDirectoryHook::Orig(backup) == 0 && CopyTree(coop, backup, 1);
            /* Half a backup (no room left, say) is worse than none. */
            if (!ok) DeleteDirectoryRecursivelyHook::Orig(backup);

            /* Only a complete backup moves the pointer on; a failed one is tried again in the same slot. */
            if (ok) {
                char c = static_cast<char>('0' + slot % BackupCount + 1);
                if (GetEntryTypeHook::Orig(&type, next_path) != 0) CreateFileHook::Orig(next_path, 1);
                if (OpenFileHook::Orig(&f, next_path, nn::fs::OpenMode_Write) == 0) {
                    nn::fs::WriteFile(f, 0, &c, 1, nn::fs::WriteOption::CreateOption(nn::fs::WriteOptionFlag_Flush));
                    nn::fs::CloseFile(f);
                }
            }
            /* The single backup folder of earlier versions. */
            char old[48];
            std::snprintf(old, sizeof(old), "%s:/%s_backup", g_Mount, CoopDir);
            if (ok && GetEntryTypeHook::Orig(&type, old) == 0) DeleteDirectoryRecursivelyHook::Orig(old);

            Result crc = nn::fs::CommitSaveData(g_Mount);
            g_Uncommitted = 0;
            Logging.Log("[coop] saves: %s %s (%d files, commit 0x%x)", ok ? "backed up the co-op saves to" : "couldn't back up to",
                        backup, g_Copied - before, crc);
        }

        void SeedIfNeeded() {
            char root[48], coop[48], marker[64];
            std::snprintf(root, sizeof(root), "%s:/", g_Mount);
            std::snprintf(coop, sizeof(coop), "%s:/%s", g_Mount, CoopDir);
            std::snprintf(marker, sizeof(marker), "%s:/%s/seeded", g_Mount, CoopDir);
            nn::fs::DirectoryEntryType type;
            if (GetEntryTypeHook::Orig(&type, marker) == 0) {
                Logging.Log("[coop] saves: co-op saves in %s (your normal saves aren't touched)", coop);
                Backup();
                return;
            }
            /* No finished copy: start it over. */
            if (GetEntryTypeHook::Orig(&type, coop) == 0) DeleteDirectoryRecursivelyHook::Orig(coop);
            /* No room for the copy with old backups in the way (a real Switch). */
            char backups[48];
            std::snprintf(backups, sizeof(backups), "%s:/%s_backups", g_Mount, CoopDir);
            if (GetEntryTypeHook::Orig(&type, backups) == 0) DeleteDirectoryRecursivelyHook::Orig(backups);
            nn::fs::CommitSaveData(g_Mount);
            g_Uncommitted = 0;
            Result rc = CreateDirectoryHook::Orig(coop);
            bool ok = rc == 0 && CopyTree(root, coop, 0);
            /* Only a complete copy is used: anything less and the game would
               find no saves and start a new game. */
            if (ok) ok = CreateFileHook::Orig(marker, 0) == 0;
            if (!ok) {
                DeleteDirectoryRecursivelyHook::Orig(coop);
                g_Enabled = false;
            }
            Result crc = nn::fs::CommitSaveData(g_Mount);
            g_Uncommitted = 0;
            if (ok)
                Logging.Log("[coop] saves: first co-op run: copied your saves into %s (%d files, commit 0x%x)", coop, g_Copied, crc);
            else
                Logging.Log("[coop] saves: couldn't copy your saves into %s (0x%x, %d files); playing your normal saves this time",
                            coop, rc, g_Copied);
        }
    }

    HOOK_DEFINE_TRAMPOLINE(MountSaveDataHook) {
        static Result Callback(char const* name, nn::account::Uid const& user) {
            Result rc = Orig(name, user);
            if (rc != 0 || !name) return rc;
            Decide();
            std::snprintf(g_Mount, sizeof(g_Mount), "%s", name);
            g_MountLen = std::strlen(g_Mount);
            Logging.Log("[coop] saves: game mounted its save as \"%s\"; separate co-op saves %s", g_Mount,
                        g_Enabled ? "on" : "off");
            if (g_Enabled) SeedIfNeeded();
            return rc;
        }
    };

    void Install() {
        MountSaveDataHook::InstallAtFuncPtr(nn::fs::MountSaveData);
        OpenFileHook::InstallAtFuncPtr(static_cast<Result (*)(nn::fs::FileHandle*, char const*, int)>(nn::fs::OpenFile));
        CreateFileHook::InstallAtFuncPtr(nn::fs::CreateFile);
        DeleteFileHook::InstallAtFuncPtr(nn::fs::DeleteFile);
        GetEntryTypeHook::InstallAtFuncPtr(nn::fs::GetEntryType);
        OpenDirectoryHook::InstallAtFuncPtr(nn::fs::OpenDirectory);
        CreateDirectoryHook::InstallAtFuncPtr(nn::fs::CreateDirectory);
        DeleteDirectoryRecursivelyHook::InstallAtFuncPtr(nn::fs::DeleteDirectoryRecursively);
    }
}
