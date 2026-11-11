#pragma once

#include <string_view>

#include <lib/log/ilogger.hpp>

/* The mod's log, also written to the SD card: sd:/botw-coop/log.txt (in
   Ryujinx: <Ryujinx folder>/sdcard/botw-coop/log.txt). A real Switch has no
   other way to show it. Off unless `log_file = true` in config.ini; lines
   logged before the config is read are kept and written once it says so. */
namespace coop::filelog {

    /* Any thread. Never blocks on the file. */
    void Append(std::string_view line);

    void Enable(bool on);

    /* Network thread, every couple of seconds: writes what has gathered. */
    void Flush();

    struct Logger : public exl::log::ILogger {
        virtual void LogRaw(std::string_view line) final { Append(line); }
    };
}
