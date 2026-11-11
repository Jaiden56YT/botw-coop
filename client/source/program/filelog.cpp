#include "filelog.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>

#include <lib.hpp>
#include <nn/fs.hpp>
#include <nn/os.hpp>

namespace nn::fs {
    Result DeleteFile(char const* path);
}

namespace coop::filelog {

    namespace {
        constexpr size_t BufSize = 0x10000;
        constexpr s64 MaxFile = 4 * 1024 * 1024; /* then it stops, so a long session can't fill the card */
        constexpr const char* Mount = "cooplog";
        constexpr const char* Dir = "cooplog:/botw-coop";
        constexpr const char* Path = "cooplog:/botw-coop/log.txt";

        char g_Buf[BufSize];
        size_t g_Len = 0;
        int g_Dropped = 0;
        std::atomic_flag g_Lock = ATOMIC_FLAG_INIT;
        std::atomic<bool> g_On{false};
        int g_State = 0; /* 0 = not opened yet, 1 = ready, -1 = can't */
        s64 g_Offset = 0;
        char g_Out[BufSize];

        void Lock() {
            while (g_Lock.test_and_set(std::memory_order_acquire)) {}
        }
        void Unlock() { g_Lock.clear(std::memory_order_release); }
    }

    void Append(std::string_view line) {
        char stamp[24];
        s64 ms = nn::os::ConvertToTimeSpan(nn::os::GetSystemTick()).GetMilliSeconds();
        int sl = std::snprintf(stamp, sizeof(stamp), "%lld.%03d ", static_cast<long long>(ms / 1000), static_cast<int>(ms % 1000));
        Lock();
        if (g_Len + sl + line.size() + 1 <= BufSize) {
            std::memcpy(g_Buf + g_Len, stamp, sl);
            g_Len += sl;
            std::memcpy(g_Buf + g_Len, line.data(), line.size());
            g_Len += line.size();
            g_Buf[g_Len++] = '\n';
        } else {
            g_Dropped++;
        }
        Unlock();
    }

    void Enable(bool on) { g_On.store(on); }

    void Flush() {
        if (!g_On.load() || g_State < 0) {
            /* Not wanted (or not possible): keep only the newest lines. */
            Lock();
            if (g_Len > BufSize / 2) g_Len = 0;
            Unlock();
            return;
        }
        if (g_State == 0) {
            g_State = -1;
            Result rc = nn::fs::MountSdCardForDebug(Mount);
            if (rc != 0) {
                Logging.Log("[coop] log file: can't open the SD card (0x%x)", rc);
                return;
            }
            nn::fs::CreateDirectory(Dir); /* fine if it's there */
            nn::fs::DeleteFile(Path);     /* a new log each launch */
            rc = nn::fs::CreateFile(Path, 0);
            if (rc != 0) {
                Logging.Log("[coop] log file: can't create %s (0x%x)", Path, rc);
                return;
            }
            Logging.Log("[coop] log file: writing sd:/botw-coop/log.txt");
            g_State = 1;
        }
        Lock();
        size_t n = g_Len;
        int dropped = g_Dropped;
        std::memcpy(g_Out, g_Buf, n);
        g_Len = 0;
        g_Dropped = 0;
        Unlock();
        if (n == 0 || g_Offset >= MaxFile) return;
        nn::fs::FileHandle f;
        if (nn::fs::OpenFile(&f, Path, nn::fs::OpenMode_Write | nn::fs::OpenMode_Append) != 0) return;
        if (dropped) {
            char note[48];
            int nl = std::snprintf(note, sizeof(note), "(%d lines lost)\n", dropped);
            if (nn::fs::WriteFile(f, g_Offset, note, nl, nn::fs::WriteOption::CreateOption(0)) == 0) g_Offset += nl;
        }
        if (nn::fs::WriteFile(f, g_Offset, g_Out, n, nn::fs::WriteOption::CreateOption(nn::fs::WriteOptionFlag_Flush)) == 0)
            g_Offset += static_cast<s64>(n);
        nn::fs::CloseFile(f);
    }
}
