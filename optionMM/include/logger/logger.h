#pragma once

#include "common/types.h"
#include <string_view>
#include <cstdint>

// Forward-declare spdlog to keep this header lightweight.
// Only logger.cpp pulls in the full spdlog headers.
namespace spdlog { class logger; }

namespace omm {

// ─── OmmLogger ────────────────────────────────────────────────────────────────
// Thin wrapper around spdlog async logger.
//
// Design:
//   - Single global instance (call init() once in main before engine.start()).
//   - Async: all log calls post to a background thread queue — NEVER blocks the
//     critical path.  Queue size = 8192 entries; overflow policy = drop (not block).
//   - Outputs to rotating file (logs/optionmm.log, max 50 MB × 5 files) and
//     optionally to stdout.
//   - Structured fields are formatted as key=value pairs for easy grep/parsing.
//
// Usage:
//   omm::OmmLogger::init("logs/optionmm.log", /*stdout=*/true);
//   OMM_LOG_INFO("fill", "instrument_id={} side={} qty={} price={}",
//                instr_id, side, qty, price);
//   omm::OmmLogger::shutdown();

class OmmLogger {
public:
    // Call once at startup (before engine.start()).
    // log_path: path to rotating log file (directory must exist).
    // to_stdout: also log to console.
    static void init(std::string_view log_path, bool to_stdout = true);

    // Flush and shut down async thread. Call after engine.stop().
    static void shutdown() noexcept;

    // Raw access for the macros below.
    static spdlog::logger* get() noexcept;

    OmmLogger() = delete;
};

} // namespace omm

// ─── Log macros ───────────────────────────────────────────────────────────────
// Use these instead of calling the logger directly.
// The tag argument is a short category string (e.g. "fill", "risk", "startup").
// These are no-ops if the logger has not been initialised (safe to call always).

#include <spdlog/spdlog.h>

#define OMM_LOG_TRACE(tag, ...) \
    do { if (auto* _l = omm::OmmLogger::get()) _l->trace("[" tag "] " __VA_ARGS__); } while(0)
#define OMM_LOG_DEBUG(tag, ...) \
    do { if (auto* _l = omm::OmmLogger::get()) _l->debug("[" tag "] " __VA_ARGS__); } while(0)
#define OMM_LOG_INFO(tag, ...) \
    do { if (auto* _l = omm::OmmLogger::get()) _l->info("[" tag "] " __VA_ARGS__); } while(0)
#define OMM_LOG_WARN(tag, ...) \
    do { if (auto* _l = omm::OmmLogger::get()) _l->warn("[" tag "] " __VA_ARGS__); } while(0)
#define OMM_LOG_ERROR(tag, ...) \
    do { if (auto* _l = omm::OmmLogger::get()) _l->error("[" tag "] " __VA_ARGS__); } while(0)
