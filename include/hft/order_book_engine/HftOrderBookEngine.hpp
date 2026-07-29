/**
 * @file HftOrderBookEngine.hpp
 * @brief Modern C++20 Capstone HFT Order Book Engine with BBO Telemetry & L1/L2 Depth Reporting.
 */

#pragma once

#include "hft/common/Types.hpp"
#include "hft/common/SPSCQueue.hpp"
#include "hft/common/MpscQueue.hpp"
#include "hft/common/SystemOptimizations.hpp"
#include "hft/protocol/ProtocolParser.hpp"
#include "hft/protocol/PreTradeRiskManager.hpp"
#include "hft/protocol/SequenceGapDetector.hpp"
#include "hft/networking/FeedArbitrator.hpp"
#include "hft/common/ClockSynchronizer.hpp"
#include "hft/matching/ZeroAllocHftOrderBook.hpp"
#include "hft/monitoring/Telemetry.hpp"

#include <cstdint>
#include <cstddef>
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <format>
#include <concepts>
#include <thread>
#include <chrono>

namespace hft::order_book_engine
{
    // --- C++20 Concepts ---
    template <typename T>
    concept ValidPacketType = requires(T a)
    {
        { a.payload } -> std::same_as<char*>;
        { a.payload_len } -> std::same_as<uint16_t>;
        { a.rx_timestamp_cycles } -> std::same_as<uint64_t>;
    };

    /**
     * @struct DepthLevel
     * @brief Structure representing a price-depth level in the order book.
     */
    struct DepthLevel
    {
        int64_t price{ 0 };
        uint32_t qty{ 0 };
        uint32_t order_count{ 0 };
    };

    /**
     * @struct TopOfBookSnapshot
     * @brief C++20 snapshot structure capturing Top-of-Book BBO and L1-L5 Depth.
     */
    struct TopOfBookSnapshot
    {
        int64_t best_bid_price{ 0 };
        uint32_t best_bid_qty{ 0 };
        int64_t best_ask_price{ 0 };
        uint32_t best_ask_qty{ 0 };
        int64_t spread_price{ 0 };

        std::array<DepthLevel, 5> bid_depth{};
        std::array<DepthLevel, 5> ask_depth{};
        size_t bid_levels_count{ 0 };
        size_t ask_levels_count{ 0 };
    };

    /**
     * @class HftOrderBookEngine
     * @brief Core worker thread orchestrating packet parsing, risk gates, L1/L2 matching, and BBO telemetry.
     */
    class HftOrderBookEngine
    {
    public:
        using FixMessagePacketQueue = hft::common::SPSCQueue<hft::common::FixMessagePacket, 8192>;

    private:
        FixMessagePacketQueue& m_queue;
        hft::monitoring::TelemetryCounters& m_telemetry;
        hft::protocol::ProtocolType m_protocol_type;
        int m_cpu_pin{ -1 };
        std::string m_log_filename;

        hft::protocol::PreTradeRiskManager m_risk_mgr;
        hft::protocol::SequenceGapDetector m_gap_detector;
        hft::networking::FeedArbitrator<2048> m_feed_arbitrator;
        hft::common::ClockSynchronizer m_clock_sync;
        hft::matching::ZeroAllocHftOrderBook m_order_book;

        hft::common::MpscQueue<hft::common::LogMessage> m_log_queue;
        std::thread m_logging_thread;
        std::atomic<bool> m_logging_running{ true };

    public:
        HftOrderBookEngine(FixMessagePacketQueue& queue,
            hft::monitoring::TelemetryCounters& telemetry,
            hft::protocol::ProtocolType protocol_type,
            int cpu_pin = -1,
            std::string log_filename = "hft_order_book_engine.log") noexcept
            : m_queue(queue),
            m_telemetry(telemetry),
            m_protocol_type(protocol_type),
            m_cpu_pin(cpu_pin),
            m_log_filename(std::move(log_filename)),
            m_order_book("PETR4")
        {
            m_logging_thread = std::thread(&HftOrderBookEngine::run_logging_loop, this);
        }

        ~HftOrderBookEngine()
        {
            m_logging_running.store(false, std::memory_order_relaxed);
            if (m_logging_thread.joinable())
            {
                m_logging_thread.join();
            }
        }

        void run();

        [[nodiscard]] TopOfBookSnapshot get_top_of_book_snapshot() const noexcept;
        [[nodiscard]] uint64_t total_trades() const noexcept { return m_order_book.total_trades(); }
        [[nodiscard]] uint64_t total_volume() const noexcept { return m_order_book.total_volume(); }

    private:
        void run_logging_loop();
    };
}
