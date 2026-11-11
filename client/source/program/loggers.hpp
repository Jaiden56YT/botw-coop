#pragma once

#include <lib/log/svc_logger.hpp>
#include <program/filelog.hpp>

/* Specify logger implementations here. */
inline exl::log::LoggerMgr<
    exl::log::SvcLogger,
    coop::filelog::Logger
> Logging;