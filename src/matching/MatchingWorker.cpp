/**
 * @file MatchingWorker.cpp
 * @brief Implementation of MatchingWorker consumer thread and log4cplus logger.
 */

#include "hft/matching/MatchingWorker.hpp"
#include "hft/common/ConsoleLogger.hpp"
#include "hft/common/SystemOptimizations.hpp"
#include "hft/protocol/FixParser.hpp"

#include <chrono>
#include <format>
#include <immintrin.h>
#include <linux/if_packet.h>

#include <log4cplus/fileappender.h>
#include <log4cplus/initializer.h>
#include <log4cplus/loggingmacros.h>

namespace hft::matching
{
    using namespace std;

    MatchingWorker::MatchingWorker(FixMessagePacktQueue &queue, hft::monitoring::TelemetryCounters &telemetry,
                                   string log_filename, size_t expected_messages,
                                   hft::protocol::ProtocolType protocol_type, int cpu_pin)
        : m_queue(queue), m_telemetry(telemetry), m_log_filename(std::move(log_filename)),
          m_expected_messages(expected_messages), m_protocol_type(protocol_type), m_cpu_pin(cpu_pin),
          m_recovery_mgr("PETR4"), m_logging_running(true)
    {
        // Setup default risk limits
        hft::protocol::RiskLimits limits;
        limits.max_order_qty = 50000;
        limits.max_price_deviation_scaled = 5000000; // $5.00 deviation collar
        limits.max_notional_value_usd = 2000000ULL;  // $2,000,000 USD cap
        limits.kill_switch_active = false;
        m_risk_mgr.set_limits(limits);

        m_logging_thread = thread(&MatchingWorker::run_logging_loop, this);
    }

    MatchingWorker::~MatchingWorker()
    {
        m_logging_running.store(false, memory_order_relaxed);
        if (m_logging_thread.joinable())
        {
            m_logging_thread.join();
        }
    }

    void MatchingWorker::run_logging_loop()
    {
        log4cplus::Initializer initializer;
        log4cplus::SharedAppenderPtr appender(new log4cplus::FileAppender(LOG4CPLUS_TEXT(m_log_filename)));
        appender->setName(LOG4CPLUS_TEXT("FileAppender"));

        auto layout = make_unique<log4cplus::PatternLayout>(LOG4CPLUS_TEXT("%d{%Y-%m-%d %H:%M:%S.%q} [%p] %m%n"));
        appender->setLayout(std::move(layout));

        log4cplus::Logger root = log4cplus::Logger::getRoot();
        root.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
        root.addAppender(appender);

        log4cplus::Logger match_log = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("MatchingWorker"));

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
                LOG4CPLUS_FATAL_FMT(match_log, "TS=%lu | %s", log_msg.timestamp_ns, log_msg.message);
                break;
            case hft::common::LogCategory::FATAL_LEVEL:
                LOG4CPLUS_FATAL_FMT(match_log, "TS=%lu | %s", log_msg.timestamp_ns, log_msg.message);
                break;
            }
        }
    }

    void MatchingWorker::run()
    {
        // Apply the full real-time thread configuration as the very first action of this thread.
        //
        // The previous code only called pin_thread_to_cpu(), which locked the thread to a single
        // CPU core but left it under the CFS (Completely Fair Scheduler) time-sharing class.
        // CFS assigns each thread a time quantum (~1–4 ms on a typical 250 Hz kernel). When the
        // quantum expires the kernel PREEMPTS the thread — even mid-loop — adding up to 4 ms of
        // scheduler-induced latency jitter to every order processed after a preemption event.
        //
        // apply_realtime_thread_settings() activates all three layers of protection:
        //   1. pin_thread_to_cpu(m_cpu_pin)         — lock to dedicated core, preserve L1/L2 cache
        //   2. set_realtime_priority(80)             — SCHED_FIFO: no CFS preemption on the hot path
        //   3. pthread_setname_np("hft_matcher")     — visible in perf, htop, /proc/<pid>/task/*/comm
        //
        // SCHED_FIFO requires root/CAP_SYS_NICE; this engine already runs as root for AF_PACKET.
        // On non-root dev builds the call degrades gracefully (warning printed, CFS retained).
        hft::common::apply_realtime_thread_settings(m_cpu_pin, 80, "hft_matcher");

        hft::common::log_info("[MatchingWorker] Order Matching Engine initialized. Logging to: " + m_log_filename);

        std::array<hft::common::FixMessagePacket, 32> pkt_batch;
        // Pre-allocated trade execution vector (reused across all packet iterations).
        //
        // OPTIMIZATION (Critical Finding 2.1):
        // Previously, `std::vector<TradeExecution> trades` was constructed on the stack inside
        // the per-packet `for (size_t b = 0; b < batch_count; ++b)` loop. Constructing a vector
        // inside the hot path causes a dynamic heap allocation (`malloc`) on every packet, even
        // if no trades are matched. For 1M messages, this resulted in 1M heap allocations.
        //
        // Pre-allocating `trades` outside the loop with `reserve(16)` and invoking `trades.clear()`
        // inside the loop reuses the existing heap capacity without deallocating memory. `clear()`
        // simply resets the vector size counter to 0 in a single CPU cycle, achieving 0-allocation
        // matching execution on the hot path.
        std::vector<TradeExecution> trades;
        trades.reserve(16);

        auto processed = 0u;
        auto total_latency_ns = 0ull;
        auto min_latency_ns = UINT64_MAX;
        auto max_latency_ns = 0ull;

        int64_t reference_price = 35000000; // $35.00 reference price for risk collars
        auto start_time = chrono::steady_clock::now();

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
                        "[MatchingWorker] Queue is empty and consumer finished. Exiting matching loop.");
                    break;
                }
                this_thread::sleep_for(chrono::microseconds(5));
                continue;
            }

            for (size_t b = 0; b < batch_count; ++b)
            {
                const auto &pkt = pkt_batch[b];

                // Pre-fetch lookahead packet payload into L1 CPU cache
                if (b + 1 < batch_count && pkt_batch[b + 1].payload != nullptr)
                {
                    _mm_prefetch(pkt_batch[b + 1].payload, _MM_HINT_T0);
                }

                // 1. Zero-allocation in-place multi-protocol parsing (FIX, OUCH, or SBE)
                auto order =
                    hft::protocol::ProtocolParser::parse_in_place(m_protocol_type, pkt.payload, pkt.payload_len);

                auto parse_cycles = hft::common::rdtsc();
                auto elapsed_cycles =
                    (parse_cycles > pkt.rx_timestamp_cycles) ? (parse_cycles - pkt.rx_timestamp_cycles) : 0;
                auto latency =
                    static_cast<uint64_t>(static_cast<double>(elapsed_cycles) / hft::common::g_cycles_per_ns);
                order.latency_ns = latency;

                uint64_t current_ts = hft::common::get_timestamp_ns();

                // 2. Scenario #3: Clock Synchronization & Anomaly Detection
                uint64_t proc_ts = current_ts;
                uint64_t rx_ts = pkt.rx_timestamp_cycles / static_cast<uint64_t>(hft::common::g_cycles_per_ns);
                (void)m_clock_sync.evaluate_latency(rx_ts, proc_ts);

                // 3. Scenario #4: Active-Active Feed Arbitration & Duplicate Suppression
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

                        // Release ring frame memory and skip processing duplicate
                        if (pkt.ring_hdr != nullptr)
                        {
                            auto *hdr = static_cast<struct tpacket2_hdr *>(pkt.ring_hdr);
                            hdr->tp_status = TP_STATUS_KERNEL;
                        }
                        continue;
                    }
                }

                // 4. Scenario #1: Packet Loss & Sequence Gap Detection
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
                                                    "Generated FIX ResendRequest (35=2): {}",
                                                    gap_res.gap.begin_seq, gap_res.gap.end_seq,
                                                    gap_res.gap.missing_count, gap_res.resend_request_msg);
                        *res.out = '\0';
                        (void)m_log_queue.push(log_msg);
                    }
                }

                // 5. Scenario #6: Pre-Trade Risk Validation & Fat-Finger Protection Gate
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

                // 6. Order Matching Engine Execution (Only for approved orders)
                if (risk_approved && order.msg_type == "D") // New Order Single
                {
                    trades.clear();
                    bool matched = m_matching_engine.process_order(order, trades, current_ts);

                    if (matched && !trades.empty())
                    {
                        for (const auto &tr : trades)
                        {
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

                // Update zero-contention atomic telemetry
                m_telemetry.worker.messages_processed.store(processed, memory_order_relaxed);
                m_telemetry.worker.risk_approved.store(approved_count, memory_order_relaxed);
                m_telemetry.worker.risk_rejected.store(rejected_count, memory_order_relaxed);
                m_telemetry.worker.total_latency_ns.store(total_latency_ns, memory_order_relaxed);
                m_telemetry.worker.approved_total_latency_ns.store(approved_total_lat, memory_order_relaxed);
                m_telemetry.worker.rejected_total_latency_ns.store(rejected_total_lat, memory_order_relaxed);
                m_telemetry.worker.min_latency_ns.store(min_latency_ns, memory_order_relaxed);
                m_telemetry.worker.max_latency_ns.store(max_latency_ns, memory_order_relaxed);

                // Release ring frame memory back to kernel Ring DMA
                if (pkt.ring_hdr != nullptr)
                {
                    auto *hdr = static_cast<struct tpacket2_hdr *>(pkt.ring_hdr);
                    hdr->tp_status = TP_STATUS_KERNEL;
                }

                if (processed >= m_expected_messages)
                {
                    break;
                }
            }

            if (processed >= m_expected_messages)
            {
                break;
            }
        }

        auto end_time = chrono::steady_clock::now();
        auto duration_sec = chrono::duration<double>(end_time - start_time).count();
        auto avg_latency_ns =
            (processed > 0) ? static_cast<double>(total_latency_ns) / static_cast<double>(processed) : 0.0;

        hft::common::log_info("");
        hft::common::log_info("====================================================");
        hft::common::log_info("        ORDER MATCHING ENGINE PERFORMANCE REPORT   ");
        hft::common::log_info("====================================================");
        hft::common::log_info(std::format("Total Messages Ingested  : {}", processed));
        hft::common::log_info(std::format("Risk Approved Orders     : {}", processed - m_risk_mgr.rejected_count()));
        hft::common::log_info(std::format("Risk Rejected Orders     : {}", m_risk_mgr.rejected_count()));
        hft::common::log_info(std::format("Total Matched Trades     : {}", m_matching_engine.total_trades()));
        hft::common::log_info(std::format("Total Matched Volume     : {} shares", m_matching_engine.total_volume()));
        hft::common::log_info(std::format("Total Execution Time     : {:.4f} seconds", duration_sec));

        size_t throughput =
            (duration_sec > 0.0) ? static_cast<size_t>(static_cast<double>(processed) / duration_sec) : 0;
        hft::common::log_info(std::format("Throughput               : {} msgs/sec", throughput));
        hft::common::log_info(
            std::format("Minimum Latency          : {} ns", min_latency_ns == UINT64_MAX ? 0 : min_latency_ns));
        hft::common::log_info(
            std::format("Average Latency          : {:.2f} ns ({:.2f} us)", avg_latency_ns, avg_latency_ns / 1000.0));
        hft::common::log_info(std::format("Maximum Latency          : {} ns", max_latency_ns));
        hft::common::log_info("====================================================");
        hft::common::log_info("");

        hft::common::g_running.store(false, memory_order_release);
    }
} // namespace hft::matching
