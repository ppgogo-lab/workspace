#include "logger/logger.h"

#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <memory>
#include <vector>
#include <filesystem>

namespace omm {

namespace {
    std::shared_ptr<spdlog::logger> g_logger;
}

/**
 * @brief Implements Init.
 * @param log_path Parameter supplied by the caller.
 * @param to_stdout Parameter supplied by the caller.
 * @return None.
 */
void OmmLogger::init(std::string_view log_path, bool to_stdout) {
    // Ensure log directory exists
    std::filesystem::path p(log_path);
    if (p.has_parent_path())
        std::filesystem::create_directories(p.parent_path());

    // Async thread pool: 8192-entry queue, 1 background thread
    // Overflow policy: drop oldest — never block the caller
    spdlog::init_thread_pool(8192, 1);

    std::vector<spdlog::sink_ptr> sinks;

    // Rotating file: 50 MB per file, keep 5 files
    sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        std::string(log_path), 50 * 1024 * 1024, 5));

    if (to_stdout)
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

    g_logger = std::make_shared<spdlog::async_logger>(
        "omm",
        sinks.begin(), sinks.end(),
        spdlog::thread_pool(),
        spdlog::async_overflow_policy::overrun_oldest);

    // Pattern: [timestamp] [level] message
    g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%f] [%^%l%$] %v");
    g_logger->set_level(spdlog::level::debug);
    g_logger->flush_on(spdlog::level::warn);

    spdlog::register_logger(g_logger);
    g_logger->info("[startup] logger initialised path={}", log_path);
}

/**
 * @brief Implements Shutdown.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void OmmLogger::shutdown() noexcept {
    if (g_logger) {
        g_logger->info("[shutdown] logger shutting down");
        g_logger->flush();
    }
    spdlog::shutdown();
    g_logger.reset();
}

/**
 * @brief Implements Get.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
spdlog::logger* OmmLogger::get() noexcept {
    return g_logger.get();
}

} // namespace omm
