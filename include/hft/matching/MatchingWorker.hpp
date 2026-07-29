/**
 * @file MatchingWorker.hpp
 * @brief Consumer matching worker thread integrating resilience scenario detection and order matching.
 */

#pragma once

#include "hft/common/SPSCQueue.hpp"
#include "hft/common/MpscQueue.hpp"
#include "hft/common/Types.hpp"
#include "hft/monitoring/Telemetry.hpp"
#include "hft/protocol/ProtocolParser.hpp"
#include "hft/protocol/SequenceGapDetector.hpp"
#include "hft/protocol/SequenceReorderBuffer.hpp"
#include "hft/common/ClockSynchronizer.hpp"
#include "hft/networking/FeedArbitrator.hpp"
#include "hft/protocol/OrderBookRecoveryManager.hpp"
#include "hft/protocol/PreTradeRiskManager.hpp"
#include "hft/matching/MatchingEngine.hpp"

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
     */
    class MatchingWorker
    {
    public:
        MatchingWorker(FixMessagePacktQueue& queue,
                       hft::monitoring::TelemetryCounters& telemetry,
                       std::string log_filename,
                       size_t expected_messages,
                       hft::protocol::ProtocolType protocol_type = hft::protocol::ProtocolType::FIX,
                       int cpu_pin = -1);

        ~MatchingWorker();

        MatchingWorker(const MatchingWorker&) = delete;
        MatchingWorker& operator=(const MatchingWorker&) = delete;

        void run();

        [[nodiscard]] uint64_t total_trades() const noexcept { return m_matching_engine.total_trades(); }
        [[nodiscard]] uint64_t total_volume() const noexcept { return m_matching_engine.total_volume(); }
        [[nodiscard]] uint64_t risk_rejected_count() const noexcept { return m_risk_mgr.rejected_count(); }

    private:
        void run_logging_loop();

        FixMessagePacktQueue& m_queue;
        hft::monitoring::TelemetryCounters& m_telemetry;
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
        hft::common::MpscQueue<hft::common::LogMessage> m_log_queue;
        std::atomic<bool> m_logging_running{true};
        std::thread m_logging_thread;
    };
} // namespace hft::matching
