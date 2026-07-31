/**
 * @file HftOrderBookEngine.cpp
 * @brief Implementation of C++20 Capstone HFT Order Book Engine with BBO Telemetry.
 */

#include "hft/order_book_engine/HftOrderBookEngine.hpp"
#include "hft/common/ConsoleLogger.hpp"
#include <log4cplus/fileappender.h>
#include <log4cplus/layout.h>
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

#include <chrono>
#include <emmintrin.h>
#include <format>
#include <immintrin.h>
#include <linux/if_packet.h>
#include <thread>

using namespace std;

namespace hft::order_book_engine
{
    void HftOrderBookEngine::run_logging_loop()
    {
        auto appender = log4cplus::SharedAppenderPtr(
            new log4cplus::FileAppender(LOG4CPLUS_TEXT(m_log_filename), std::ios_base::out | std::ios_base::trunc));
        appender->setName(LOG4CPLUS_TEXT("HftOrderBookAppender"));

        auto layout = make_unique<log4cplus::PatternLayout>(LOG4CPLUS_TEXT("%d{%Y-%m-%d %H:%M:%S.%q} [%p] %m%n"));
        appender->setLayout(std::move(layout));

        log4cplus::Logger root = log4cplus::Logger::getRoot();
        root.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
        root.addAppender(appender);

        log4cplus::Logger match_log = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("HftOrderBookEngine"));

        hft::common::LogMessage log_msg;

        while (m_logging_running.load(memory_order_relaxed) || !m_log_queue.empty())
        {
            if (!m_log_queue.pop(log_msg))
            {
                this_thread::sleep_for(chrono::microseconds(5));
                continue;
            }

            switch (log_msg.category)
            {
            case hft::common::LogCategory::DEBUG_LEVEL:
                LOG4CPLUS_DEBUG_FMT(match_log, "TS=%lu | %s", log_msg.timestamp_ns, log_msg.message);
                break;
            case hft::common::LogCategory::INFO_LEVEL:
                LOG4CPLUS_INFO_FMT(match_log, "TS=%lu | %s", log_msg.timestamp_ns, log_msg.message);
                break;
            case hft::common::LogCategory::WARNING_LEVEL:
                LOG4CPLUS_WARN_FMT(match_log, "TS=%lu | %s", log_msg.timestamp_ns, log_msg.message);
                break;
            case hft::common::LogCategory::ERROR_LEVEL:
                LOG4CPLUS_ERROR_FMT(match_log, "TS=%lu | %s", log_msg.timestamp_ns, log_msg.message);
                break;
            case hft::common::LogCategory::CRITICAL_LEVEL:
            case hft::common::LogCategory::FATAL_LEVEL:
                LOG4CPLUS_FATAL_FMT(match_log, "TS=%lu | %s", log_msg.timestamp_ns, log_msg.message);
                break;
            }
        }
    }

    void HftOrderBookEngine::run()
    {
        if (m_cpu_pin >= 0)
        {
            hft::common::pin_thread_to_cpu(m_cpu_pin);
            hft::common::log_info("[HftOrderBookEngine] Thread pinned to CPU core " + to_string(m_cpu_pin));
        }

        hft::common::log_info("[HftOrderBookEngine] Modern C++20 Order Book Engine initialized. Logging to: " +
                              m_log_filename);

        std::array<hft::common::FixMessagePacket, 32> pkt_batch;
        std::array<hft::matching::ZeroAllocTrade, 16> trade_batch;

        auto processed = 0u;
        auto total_latency_ns = 0ull;
        auto min_latency_ns = UINT64_MAX;
        auto max_latency_ns = 0ull;

        int64_t reference_price = 35000000; // $35.00 reference price for risk collars
        uint64_t trade_counter = 0;

        auto approved_count = 0ull;
        auto rejected_count = 0ull;
        auto approved_total_lat = 0ull;
        auto rejected_total_lat = 0ull;

        while (hft::common::g_running.load(memory_order_relaxed) || !m_queue.empty())
        {
            size_t batch_count = m_queue.pop_batch(pkt_batch.data(), pkt_batch.size());

            if (batch_count == 0)
            {
                if (hft::common::g_consumer_done.load(memory_order_relaxed) && m_queue.empty())
                {
                    hft::common::log_info(
                        "[HftOrderBookEngine] Queue is empty and consumer finished. Exiting matching loop.");
                    break;
                }
                this_thread::sleep_for(chrono::microseconds(5));
                continue;
            }

            for (size_t b = 0; b < batch_count; ++b)
            {
                const auto &pkt = pkt_batch[b];

                // Hardware L1 prefetch lookahead
                if (b + 1 < batch_count && pkt_batch[b + 1].payload != nullptr)
                {
                    _mm_prefetch(pkt_batch[b + 1].payload, _MM_HINT_T0);
                }

                // 1. Zero-allocation in-place multi-protocol parsing
                auto order =
                    hft::protocol::ProtocolParser::parse_in_place(m_protocol_type, pkt.payload, pkt.payload_len);

                auto parse_cycles = hft::common::rdtsc();
                auto elapsed_cycles =
                    (parse_cycles > pkt.rx_timestamp_cycles) ? (parse_cycles - pkt.rx_timestamp_cycles) : 0;
                auto latency =
                    static_cast<uint64_t>(static_cast<double>(elapsed_cycles) / hft::common::g_cycles_per_ns);
                order.latency_ns = latency;

                uint64_t current_ts = hft::common::get_timestamp_ns();

                // 2. Active-Active Line A/B Feed Arbitration
                if (order.seq_num > 0)
                {
                    auto arb_res = m_feed_arbitrator.process_packet(hft::networking::FeedLine::LINE_A, order.seq_num);
                    if (arb_res.is_duplicate)
                    {
                        hft::common::LogMessage log_msg;
                        log_msg.timestamp_ns = current_ts;
                        log_msg.category = hft::common::LogCategory::WARNING_LEVEL;
                        auto res = std::format_to_n(
                            log_msg.message, sizeof(log_msg.message) - 1,
                            "[DUPLICATE FEED DETECTED] Suppressed duplicate MsgSeqNum={} from secondary feed",
                            order.seq_num);
                        *res.out = '\0';
                        (void)m_log_queue.push(log_msg);

                        if (pkt.ring_hdr != nullptr)
                        {
                            auto *hdr = static_cast<struct tpacket2_hdr *>(pkt.ring_hdr);
                            hdr->tp_status = TP_STATUS_KERNEL;
                        }
                        continue;
                    }
                }

                // 3. Packet Loss & Sequence Gap Detection
                if (order.seq_num > 0)
                {
                    auto gap_res = m_gap_detector.process_sequence(order.seq_num);
                    if (gap_res.status == hft::protocol::GapStatus::GAP_DETECTED)
                    {
                        hft::common::LogMessage log_msg;
                        log_msg.timestamp_ns = current_ts;
                        log_msg.category = hft::common::LogCategory::ERROR_LEVEL;
                        auto res = std::format_to_n(log_msg.message, sizeof(log_msg.message) - 1,
                                                    "[SEQUENCE GAP DETECTED] Missing range [{} .. {}] ({} dropped). "
                                                    "Generated FIX ResendRequest: {}",
                                                    gap_res.gap.begin_seq, gap_res.gap.end_seq,
                                                    gap_res.gap.missing_count, gap_res.resend_request_msg);
                        *res.out = '\0';
                        (void)m_log_queue.push(log_msg);
                    }
                }

                // 4. Inline Pre-Trade Risk Gate Evaluation
                auto risk_res = m_risk_mgr.evaluate_order(order, reference_price);
                bool risk_approved = (risk_res.status == hft::protocol::RiskCheckStatus::APPROVED);
                if (!risk_approved)
                {
                    hft::common::LogMessage log_msg;
                    log_msg.timestamp_ns = current_ts;
                    log_msg.category = hft::common::LogCategory::ERROR_LEVEL;
                    auto res = std::format_to_n(log_msg.message, sizeof(log_msg.message) - 1,
                                                "[PRE-TRADE RISK REJECTION] ClOrdID={} | Reason: {}", order.cl_ord_id,
                                                risk_res.rejection_reason);
                    *res.out = '\0';
                    (void)m_log_queue.push(log_msg);
                }

                // 5. Zero-Allocation Limit Order Book Submission & Price-Time Matching
                if (risk_approved && order.msg_type == "D")
                {
                    size_t trades_cnt = m_order_book.submit_order(order, trade_batch.data(), trade_batch.size(),
                                                                  trade_counter, current_ts);

                    for (size_t t = 0; t < trades_cnt; ++t)
                    {
                        const auto &tr = trade_batch[t];
                        hft::common::LogMessage log_msg;
                        log_msg.timestamp_ns = current_ts;
                        log_msg.category = hft::common::LogCategory::INFO_LEVEL;
                        auto res = std::format_to_n(log_msg.message, sizeof(log_msg.message) - 1,
                                                    "[MATCHED TRADE EXECUTED] TradeID=#{} | Sym={} | BuyID={} | "
                                                    "SellID={} | Qty={} @ Price={:.2f}",
                                                    tr.trade_id, tr.symbol, tr.buy_cl_ord_id, tr.sell_cl_ord_id,
                                                    tr.match_qty, static_cast<double>(tr.match_price) / 1000000.0);
                        *res.out = '\0';
                        (void)m_log_queue.push(log_msg);
                    }
                }

                if (risk_approved)
                {
                    ++approved_count;
                    approved_total_lat += latency;
                }
                else
                {
                    ++rejected_count;
                    rejected_total_lat += latency;
                }

                total_latency_ns += latency;
                if (latency < min_latency_ns)
                    min_latency_ns = latency;
                if (latency > max_latency_ns)
                    max_latency_ns = latency;
                ++processed;

                // Update zero-contention atomic telemetry counters
                m_telemetry.worker.messages_processed.store(processed, memory_order_relaxed);
                m_telemetry.worker.risk_approved.store(approved_count, memory_order_relaxed);
                m_telemetry.worker.risk_rejected.store(rejected_count, memory_order_relaxed);
                m_telemetry.worker.total_latency_ns.store(total_latency_ns, memory_order_relaxed);
                m_telemetry.worker.approved_total_latency_ns.store(approved_total_lat, memory_order_relaxed);
                m_telemetry.worker.rejected_total_latency_ns.store(rejected_total_lat, memory_order_relaxed);
                m_telemetry.worker.min_latency_ns.store(min_latency_ns, memory_order_relaxed);
                m_telemetry.worker.max_latency_ns.store(max_latency_ns, memory_order_relaxed);

                // Release Layer-2 PACKET_MMAP kernel DMA ring frame
                if (pkt.ring_hdr != nullptr)
                {
                    auto *hdr = static_cast<struct tpacket2_hdr *>(pkt.ring_hdr);
                    hdr->tp_status = TP_STATUS_KERNEL;
                }
            }
        }
    }

    TopOfBookSnapshot HftOrderBookEngine::get_top_of_book_snapshot() const noexcept
    {
        TopOfBookSnapshot snapshot;
        snapshot.best_bid_price = m_order_book.best_bid_price();
        snapshot.best_bid_qty = m_order_book.best_bid_qty();
        snapshot.best_ask_price = m_order_book.best_ask_price();
        snapshot.best_ask_qty = m_order_book.best_ask_qty();

        if (snapshot.best_bid_price > 0 && snapshot.best_ask_price > 0)
        {
            snapshot.spread_price = (snapshot.best_ask_price >= snapshot.best_bid_price)
                                        ? (snapshot.best_ask_price - snapshot.best_bid_price)
                                        : 0;
        }

        // Populate L1-L5 Depth levels
        snapshot.bid_levels_count = std::min(m_order_book.bid_levels_count(), size_t{5});
        for (size_t i = 0; i < snapshot.bid_levels_count; ++i)
        {
            snapshot.bid_depth[i] = DepthLevel{m_order_book.get_bid_level_price(i), m_order_book.get_bid_level_qty(i),
                                               m_order_book.get_bid_level_order_count(i)};
        }

        snapshot.ask_levels_count = std::min(m_order_book.ask_levels_count(), size_t{5});
        for (size_t i = 0; i < snapshot.ask_levels_count; ++i)
        {
            snapshot.ask_depth[i] = DepthLevel{m_order_book.get_ask_level_price(i), m_order_book.get_ask_level_qty(i),
                                               m_order_book.get_ask_level_order_count(i)};
        }

        return snapshot;
    }
} // namespace hft::order_book_engine
