/**
 * @file FixWorker.cpp
 * @brief Implementation of the FIX worker consumer and asynchronous log4cplus logger.
 */

#include "hft/worker/FixWorker.hpp"
#include "hft/common/ConsoleLogger.hpp"
#include "hft/common/SystemOptimizations.hpp"
#include "hft/protocol/FixParser.hpp"

#include <immintrin.h>
#include <linux/if_packet.h>

#include <log4cplus/fileappender.h>
#include <log4cplus/initializer.h>
#include <log4cplus/loggingmacros.h>

namespace hft::worker
{
    using namespace std;

    FixWorker::FixWorker(FixMessagePacktQueue &queue, hft::monitoring::TelemetryCounters &telemetry,
                         string log_filename, size_t expected_messages, int cpu_pin)
        : m_queue(queue), m_telemetry(telemetry), m_log_filename(std::move(log_filename)),
          m_expected_messages(expected_messages), m_cpu_pin(cpu_pin), m_logging_running(true)
    {
        m_logging_thread = thread(&FixWorker::run_logging_loop, this);
    }

    FixWorker::~FixWorker()
    {
        m_logging_running.store(false, memory_order_relaxed);
        if (m_logging_thread.joinable())
        {
            m_logging_thread.join();
        }
    }

    void FixWorker::run_logging_loop()
    {
        // Initialize log4cplus inside the logging thread
        log4cplus::Initializer initializer;

        // Setup log4cplus PatternLayout and FileAppender
        log4cplus::SharedAppenderPtr appender(new log4cplus::FileAppender(LOG4CPLUS_TEXT(m_log_filename)));
        appender->setName(LOG4CPLUS_TEXT("FileAppender"));

        auto layout = make_unique<log4cplus::PatternLayout>(LOG4CPLUS_TEXT("%d{%Y-%m-%d %H:%M:%S.%q} [%p] %m%n"));
        appender->setLayout(std::move(layout));

        log4cplus::Logger root = log4cplus::Logger::getRoot();
        root.setLogLevel(log4cplus::DEBUG_LOG_LEVEL);
        root.addAppender(appender);

        log4cplus::Logger fix_log = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("FixWorker"));

        hft::common::LogMessage log_msg;

        while (m_logging_running.load(memory_order_relaxed) || !m_log_queue.empty())
        {
            if (!m_log_queue.pop(log_msg))
            {
                this_thread::sleep_for(chrono::microseconds(5));
                continue;
            }

            // Map internal category to log4cplus macro level and log asynchronously
            switch (log_msg.category)
            {
            case hft::common::LogCategory::DEBUG_LEVEL:
                LOG4CPLUS_DEBUG_FMT(fix_log, "TS=%lu | %s", log_msg.timestamp_ns, log_msg.message);
                break;
            case hft::common::LogCategory::INFO_LEVEL:
                LOG4CPLUS_INFO_FMT(fix_log, "TS=%lu | %s", log_msg.timestamp_ns, log_msg.message);
                break;
            case hft::common::LogCategory::WARNING_LEVEL:
                LOG4CPLUS_WARN_FMT(fix_log, "TS=%lu | %s", log_msg.timestamp_ns, log_msg.message);
                break;
            case hft::common::LogCategory::ERROR_LEVEL:
                LOG4CPLUS_ERROR_FMT(fix_log, "TS=%lu | %s", log_msg.timestamp_ns, log_msg.message);
                break;
            case hft::common::LogCategory::CRITICAL_LEVEL:
                LOG4CPLUS_FATAL_FMT(fix_log, "TS=%lu | %s", log_msg.timestamp_ns, log_msg.message);
                break;
            case hft::common::LogCategory::FATAL_LEVEL:
                LOG4CPLUS_FATAL_FMT(fix_log, "TS=%lu | %s", log_msg.timestamp_ns, log_msg.message);
                break;
            }
        }
    }

    void FixWorker::run()
    {
        if (m_cpu_pin >= 0)
        {
            hft::common::pin_thread_to_cpu(m_cpu_pin);
            hft::common::log_info("[FixWorker] Thread pinned to CPU core " + std::to_string(m_cpu_pin));
        }

        hft::common::log_info("[FixWorker] Parser engine initialized and waiting for queue items. Log: " +
                              m_log_filename);

        std::array<hft::common::FixMessagePacket, 32> pkt_batch;
        auto processed = 0u;
        auto total_latency_ns = 0ull;
        auto min_latency_ns = UINT64_MAX;
        auto max_latency_ns = 0ull;

        auto start_time = chrono::steady_clock::now();

        while (hft::common::g_running.load(memory_order_relaxed) || !m_queue.empty())
        {
            // Vectorized Batch Popping: Dequeues up to 32 packets with a SINGLE atomic release store
            size_t batch_count = m_queue.pop_batch(pkt_batch.data(), pkt_batch.size());

            if (batch_count == 0)
            {
                if (hft::common::g_consumer_done.load(memory_order_relaxed) && m_queue.empty())
                {
                    hft::common::log_info("[FixWorker] Queue is empty and consumer finished. Exiting worker loop.");
                    break;
                }
                this_thread::sleep_for(chrono::microseconds(5));
                continue;
            }

            for (size_t b = 0; b < batch_count; ++b)
            {
                const auto &pkt = pkt_batch[b];

                // OPTIMIZATION: Lookahead L1 CPU Cache Pre-fetching for next packet payload
                if (b + 1 < batch_count && pkt_batch[b + 1].payload != nullptr)
                {
                    _mm_prefetch(pkt_batch[b + 1].payload, _MM_HINT_T0);
                }

                // Zero-allocation in-place parsing
                auto order = hft::protocol::FixParser::parse_in_place(pkt.payload, pkt.payload_len);

                auto parse_cycles = hft::common::rdtsc();
                auto elapsed_cycles =
                    (parse_cycles > pkt.rx_timestamp_cycles) ? (parse_cycles - pkt.rx_timestamp_cycles) : 0;
                auto latency =
                    static_cast<uint64_t>(static_cast<double>(elapsed_cycles) / hft::common::g_cycles_per_ns);
                order.latency_ns = latency;

                total_latency_ns += latency;
                if (latency < min_latency_ns)
                    min_latency_ns = latency;
                if (latency > max_latency_ns)
                    max_latency_ns = latency;
                ++processed;

                // Update zero-contention atomic telemetry
                m_telemetry.worker.messages_processed.store(processed, memory_order_relaxed);
                m_telemetry.worker.total_latency_ns.store(total_latency_ns, memory_order_relaxed);
                m_telemetry.worker.min_latency_ns.store(min_latency_ns, memory_order_relaxed);
                m_telemetry.worker.max_latency_ns.store(max_latency_ns, memory_order_relaxed);

                // Release ring frame memory back to kernel Ring DMA immediately so PACKET_RX_RING never starves
                if (pkt.ring_hdr != nullptr)
                {
                    auto *hdr = static_cast<struct tpacket2_hdr *>(pkt.ring_hdr);
                    hdr->tp_status = TP_STATUS_KERNEL;
                }

                // Queue log entry asynchronously without blocking the hot path or running format when queue is full
                if (m_log_queue.can_push())
                {
                    hft::common::LogMessage log_msg;
                    log_msg.timestamp_ns = hft::common::get_timestamp_ns();
                    log_msg.category = hft::common::LogCategory::INFO_LEVEL;
                    auto res = std::format_to_n(log_msg.message, sizeof(log_msg.message) - 1,
                                                "ClOrdID={} | Sym={} | Side={} | Qty={} | Price={:.4f} | Latency={}ns",
                                                order.cl_ord_id, order.symbol, order.side, order.quantity,
                                                static_cast<double>(order.price) / 1'000'000.0, latency);
                    *res.out = '\0';
                    (void)m_log_queue.push(log_msg);
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
        hft::common::log_info("               PERFORMANCE REPORT                   ");
        hft::common::log_info("====================================================");
        hft::common::log_info(std::format("Total Messages Processed : {}", processed));
        hft::common::log_info(std::format("Total Execution Time     : {:.4f} seconds", duration_sec));

        size_t throughput = static_cast<size_t>(static_cast<double>(processed) / duration_sec);
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

} // namespace hft::worker
