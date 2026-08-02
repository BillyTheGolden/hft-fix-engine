/**
 * @file MatchingWorker.hpp
 * @brief Consumer matching worker thread integrating resilience scenario detection and order matching.
 */

#pragma once

#include "hft/common/ClockSynchronizer.hpp"
#include "hft/common/MpscQueue.hpp"
#include "hft/common/SPSCQueue.hpp"
#include "hft/common/Types.hpp"
#include "hft/matching/MatchingEngine.hpp"
#include "hft/monitoring/Telemetry.hpp"
#include "hft/networking/FeedArbitrator.hpp"
#include "hft/protocol/OrderBookRecoveryManager.hpp"
#include "hft/protocol/PreTradeRiskManager.hpp"
#include "hft/protocol/ProtocolParser.hpp"
#include "hft/protocol/SequenceGapDetector.hpp"
#include "hft/protocol/SequenceReorderBuffer.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace hft::matching
{
    using FixMessagePacktQueue = hft::common::SPSCQueue<hft::common::FixMessagePacket, 8192>;

    /**
     * @class MatchingWorker
     * @brief Dedicated worker thread executing real-time FIX/OUCH/SBE parsing, resilience scenario detection,
     *        pre-trade risk validation, and order book trade matching.
     * @details Activates `SCHED_FIFO` real-time scheduling (priority 80), CPU core affinity pinning, and kernel
     *          thread naming via `apply_realtime_thread_settings()`. Operates on a pre-allocated trade execution
     *          vector (`trades.reserve(16)`) and `trades.clear()` to eliminate dynamic heap allocations (`malloc`)
     *          from the packet processing hot path.
     */
    class MatchingWorker
    {
      public:
        MatchingWorker(FixMessagePacktQueue &queue, hft::monitoring::TelemetryCounters &telemetry,
                       std::string log_filename, size_t expected_messages,
                       hft::protocol::ProtocolType protocol_type = hft::protocol::ProtocolType::FIX, int cpu_pin = -1);

        ~MatchingWorker();

        MatchingWorker(const MatchingWorker &) = delete;
        MatchingWorker &operator=(const MatchingWorker &) = delete;

        /**
         * @brief Executes the main multi-protocol order parsing, pre-trade risk evaluation, and order matching loop.
         * @details Applies `apply_realtime_thread_settings(m_cpu_pin, 80, "hft_matcher")` as its first action to
         *          lock thread affinity and prevent OS scheduler preemption jitter under Linux CFS. Reuses a
         * pre-allocated `std::vector<TradeExecution>` buffer (`trades.clear()`) per packet iteration to guarantee zero
         * heap allocations during trade matching execution.
         */
        void run();

        [[nodiscard]] uint64_t total_trades() const noexcept
        {
            return m_matching_engine.total_trades();
        }
        [[nodiscard]] uint64_t total_volume() const noexcept
        {
            return m_matching_engine.total_volume();
        }
        [[nodiscard]] uint64_t risk_rejected_count() const noexcept
        {
            return m_risk_mgr.rejected_count();
        }

      private:
        void run_logging_loop();

        FixMessagePacktQueue &m_queue;
        hft::monitoring::TelemetryCounters &m_telemetry;
        std::string m_log_filename;
        size_t m_expected_messages;
        hft::protocol::ProtocolType m_protocol_type;
        int m_cpu_pin;

        // Resilience Scenarios & Detection Components
        hft::protocol::SequenceGapDetector m_gap_detector;
        hft::protocol::SequenceReorderBuffer<1024> m_reorder_buffer;
        hft::common::ClockSynchronizer m_clock_sync;
        hft::networking::FeedArbitrator<2048> m_feed_arbitrator;
        hft::protocol::OrderBookRecoveryManager m_recovery_mgr;
        hft::protocol::PreTradeRiskManager m_risk_mgr;
        MatchingEngine m_matching_engine;

        // Asynchronous Logging Subsystem
        using LogMessageQueue = hft::common::SPSCQueue<hft::common::LogMessage, 16384>;
        LogMessageQueue m_log_queue;
        std::atomic<bool> m_logging_running{true};
        std::thread m_logging_thread;
    };
} // namespace hft::matching
