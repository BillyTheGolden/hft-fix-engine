/**
 * @file ConsoleLogger.hpp
 * @brief Thread-safe asynchronous console logger with fixed bottom progress bar.
 */

#pragma once

#include "hft/common/MpscQueue.hpp"
#include "hft/common/Types.hpp"
#include "hft/monitoring/Telemetry.hpp"
#include <atomic>
#include <string>
#include <thread>

namespace hft::common
{
    /**
     * @class ConsoleLogger
     * @brief Singleton class that manages all console output asynchronously, keeping a fixed progress bar at the
     * bottom.
     */
    class ConsoleLogger
    {
      public:
        /**
         * @brief Retrieves the singleton instance of ConsoleLogger.
         * @return Reference to the global ConsoleLogger instance.
         */
        static ConsoleLogger &getInstance() noexcept;

        // Delete copy and assignment operations
        ConsoleLogger(const ConsoleLogger &) = delete;
        ConsoleLogger &operator=(const ConsoleLogger &) = delete;

        /**
         * @brief Initializes the console logger.
         * @param telemetry Reference to the shared atomic telemetry counters.
         * @param expected_messages The total number of messages expected to process.
         */
        void initialize(hft::monitoring::TelemetryCounters &telemetry, size_t expected_messages) noexcept;

        /**
         * @brief Queues a message for printing (blocking).
         * @param category The severity category (INFO_MSG, WARN_MSG, ERROR_MSG).
         * @param msg The message string to log.
         */
        void log(ConsoleCategory category, const std::string &msg) noexcept;

        /**
         * @brief Queues a message for printing (non-blocking).
         * @param category The severity category.
         * @param msg The message string to log.
         * @return `true` if queued successfully; `false` if queue was busy.
         */
        bool try_log(ConsoleCategory category, const std::string &msg) noexcept;

        /**
         * @brief Shuts down the console logging thread and restores terminal settings.
         */
        void shutdown() noexcept;

      private:
        ConsoleLogger() noexcept = default;
        ~ConsoleLogger() noexcept;

        void run_loop() noexcept;
        void draw_progress_bar(size_t processed) noexcept;
        void update_terminal_size() noexcept;
        void restore_terminal() noexcept;

        MpscQueue<ConsoleMessage> m_queue;
        hft::monitoring::TelemetryCounters *m_telemetry{nullptr};
        size_t m_expected_messages{0};
        std::atomic<bool> m_running{false};
        std::thread m_thread;

        int m_terminal_rows{24};
        int m_terminal_cols{80};
        bool m_initialized{false};
        bool m_terminal_configured{false};
    };

    // Global logging helpers
    void log_info(const std::string &msg) noexcept;
    void log_warn(const std::string &msg) noexcept;
    void log_error(const std::string &msg) noexcept;

    // Hot-path non-blocking helpers
    bool try_log_info(const std::string &msg) noexcept;
    bool try_log_warn(const std::string &msg) noexcept;
    bool try_log_error(const std::string &msg) noexcept;

} // namespace hft::common
