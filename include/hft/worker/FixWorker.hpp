/**
 * @file FixWorker.hpp
 * @brief Consumer worker thread responsible for parsing FIX messages and asynchronous logging.
 */

#pragma once

#include "hft/common/SPSCQueue.hpp"
#include "hft/common/Types.hpp"
#include "hft/monitoring/Telemetry.hpp"
#include <atomic>
#include <cstddef>
#include <string>
#include <thread>

namespace hft::worker
{

    /**
     * @class IFixWorker
     * @brief Interface representing a downstream processing worker for financial messages.
     */
    class IFixWorker
    {
      public:
        virtual ~IFixWorker() = default;

        /**
         * @brief Starts the processing and logging loop.
         */
        virtual void run() = 0;
    };

    /**
     * @class FixWorker
     * @brief Concrete worker that pops raw packets from the `SPSCQueue`, delegates to `FixParser`,
     *        records latency statistics, and writes structured logs asynchronously to disk.
     * @details Activates `SCHED_FIFO` real-time scheduling (priority 80), CPU core pinning, and kernel
     *          thread naming ("hft_fix_wkr") via `apply_realtime_thread_settings()`.
     */
    class FixWorker final : public IFixWorker
    {
      public:
        using FixMessagePacktQueue = hft::common::SPSCQueue<hft::common::FixMessagePacket, 8192>;
        using LogMessageQueue = hft::common::SPSCQueue<hft::common::LogMessage, 16384>;

        /**
         * @brief Constructs a new FIX processing worker.
         * @param queue Reference to the shared SPSC queue fed by the network consumer.
         * @param telemetry Reference to shared atomic telemetry counters.
         * @param log_filename File path for writing persistent execution logs.
         * @param expected_messages Total number of messages after which to report summary statistics and exit.
         * @param cpu_pin Logical CPU core ID for thread pinning (-1 for unpinned).
         */
        FixWorker(FixMessagePacktQueue &queue, hft::monitoring::TelemetryCounters &telemetry, std::string log_filename,
                  size_t expected_messages, int cpu_pin = -1);

        ~FixWorker() override;

        /**
         * @brief Executes the popping, parsing, and logging loop.
         * @details Applies `apply_realtime_thread_settings(m_cpu_pin, 80, "hft_fix_wkr")` as its initial action
         *          to lock thread affinity and elevate to `SCHED_FIFO` real-time scheduling class.
         */
        void run() override;

      private:
        void run_logging_loop();

        FixMessagePacktQueue &m_queue;
        hft::monitoring::TelemetryCounters &m_telemetry;
        std::string m_log_filename;
        size_t m_expected_messages;
        int m_cpu_pin;

        std::atomic<bool> m_logging_running{true};
        LogMessageQueue m_log_queue;
        std::thread m_logging_thread;
    };

} // namespace hft::worker
