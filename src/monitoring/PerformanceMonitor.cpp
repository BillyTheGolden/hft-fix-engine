/**
 * @file PerformanceMonitor.cpp
 * @brief Implementation of the dedicated performance and metrics sampling thread.
 */

#include "hft/monitoring/PerformanceMonitor.hpp"
#include "hft/common/ConsoleLogger.hpp"
#include "hft/common/SystemOptimizations.hpp"

#include <fstream>
#include <iomanip>

namespace hft::monitoring
{
    using namespace std;

    CsvPerformanceMonitor::CsvPerformanceMonitor(
        const TelemetryCounters &telemetry, const hft::common::SPSCQueue<hft::common::FixMessagePacket, 8192> &queue,
        string csv_filename, uint32_t sample_interval_ms, int cpu_pin)
        : m_telemetry(telemetry), m_queue(queue), m_csv_filename(std::move(csv_filename)),
          m_sample_interval_ms(sample_interval_ms), m_cpu_pin(cpu_pin)
    {
    }

    void CsvPerformanceMonitor::run()
    {
        if (m_cpu_pin >= 0)
        {
            hft::common::pin_thread_to_cpu(m_cpu_pin);
            hft::common::log_info("[PerformanceMonitor] Thread pinned to CPU core " + std::to_string(m_cpu_pin));
        }

        ofstream csv_file(m_csv_filename, ios::out | ios::trunc);
        if (!csv_file.is_open())
        {
            hft::common::log_error("[PerformanceMonitor] Error: Could not open destination CSV file '" +
                                   m_csv_filename + "'");
            return;
        }

        // Write CSV header exactly formatted for Python pandas / matplotlib processing
        csv_file << "elapsed_sec,throughput_msg_sec,avg_latency_ns,min_latency_ns,max_latency_ns,spsc_queue_depth,"
                    "rx_frames_captured,total_processed,risk_approved,risk_rejected,approved_avg_latency_ns,rejected_avg_latency_ns\n";
        csv_file.flush();

        hft::common::log_info("[PerformanceMonitor] Sampling active every " + std::to_string(m_sample_interval_ms) +
                              " ms. Recording time-series to: " + m_csv_filename);

        auto start_time = chrono::steady_clock::now();
        auto last_processed = uint64_t(0);
        auto last_sample_time = start_time;

        while (hft::common::g_running.load(memory_order_relaxed))
        {
            this_thread::sleep_for(chrono::milliseconds(m_sample_interval_ms));

            auto now = chrono::steady_clock::now();
            auto elapsed_total_sec = chrono::duration<double>(now - start_time).count();
            auto delta_sec = chrono::duration<double>(now - last_sample_time).count();

            auto current_processed = m_telemetry.worker.messages_processed.load(memory_order_relaxed);
            auto current_rx = m_telemetry.rx.frames_captured.load(memory_order_relaxed);
            auto current_queue_len = m_queue.size();

            auto current_approved = m_telemetry.worker.risk_approved.load(memory_order_relaxed);
            auto current_rejected = m_telemetry.worker.risk_rejected.load(memory_order_relaxed);
            auto approved_total_lat = m_telemetry.worker.approved_total_latency_ns.load(memory_order_relaxed);
            auto rejected_total_lat = m_telemetry.worker.rejected_total_latency_ns.load(memory_order_relaxed);

            auto total_lat = m_telemetry.worker.total_latency_ns.load(memory_order_relaxed);
            auto min_lat = m_telemetry.worker.min_latency_ns.load(memory_order_relaxed);
            auto max_lat = m_telemetry.worker.max_latency_ns.load(memory_order_relaxed);

            auto delta_processed = (current_processed >= last_processed) ? (current_processed - last_processed) : 0;
            auto instantaneous_throughput =
                (delta_sec > 0.0) ? (static_cast<double>(delta_processed) / delta_sec) : 0.0;
            auto overall_avg_latency = (current_processed > 0)
                                           ? (static_cast<double>(total_lat) / static_cast<double>(current_processed))
                                           : 0.0;

            auto approved_avg_latency = (current_approved > 0)
                                            ? (static_cast<double>(approved_total_lat) / static_cast<double>(current_approved))
                                            : 0.0;
            auto rejected_avg_latency = (current_rejected > 0)
                                            ? (static_cast<double>(rejected_total_lat) / static_cast<double>(current_rejected))
                                            : 0.0;

            if (min_lat == UINT64_MAX)
                min_lat = 0;

            // Write row to CSV
            csv_file << fixed << setprecision(4) << elapsed_total_sec << "," << fixed << setprecision(2)
                     << instantaneous_throughput << "," << fixed << setprecision(2) << overall_avg_latency << ","
                     << min_lat << "," << max_lat << "," << current_queue_len << "," << current_rx << ","
                     << current_processed << "," << current_approved << "," << current_rejected << ","
                     << fixed << setprecision(2) << approved_avg_latency << "," << rejected_avg_latency << "\n";

            last_processed = current_processed;
            last_sample_time = now;
        }

        // Final sample snapshot on shutdown
        auto final_now = chrono::steady_clock::now();
        auto final_elapsed = chrono::duration<double>(final_now - start_time).count();
        auto final_processed = m_telemetry.worker.messages_processed.load(memory_order_relaxed);
        auto final_rx = m_telemetry.rx.frames_captured.load(memory_order_relaxed);
        auto final_approved = m_telemetry.worker.risk_approved.load(memory_order_relaxed);
        auto final_rejected = m_telemetry.worker.risk_rejected.load(memory_order_relaxed);
        auto final_approved_tot = m_telemetry.worker.approved_total_latency_ns.load(memory_order_relaxed);
        auto final_rejected_tot = m_telemetry.worker.rejected_total_latency_ns.load(memory_order_relaxed);

        auto final_total_lat = m_telemetry.worker.total_latency_ns.load(memory_order_relaxed);
        auto final_avg_lat =
            (final_processed > 0) ? (static_cast<double>(final_total_lat) / static_cast<double>(final_processed)) : 0.0;
        auto final_approved_avg =
            (final_approved > 0) ? (static_cast<double>(final_approved_tot) / static_cast<double>(final_approved)) : 0.0;
        auto final_rejected_avg =
            (final_rejected > 0) ? (static_cast<double>(final_rejected_tot) / static_cast<double>(final_rejected)) : 0.0;

        csv_file << fixed << setprecision(4) << final_elapsed << ","
                 << "0.0," << final_avg_lat << ",0,0," << m_queue.size() << "," << final_rx << "," << final_processed << ","
                 << final_approved << "," << final_rejected << ","
                 << fixed << setprecision(2) << final_approved_avg << "," << final_rejected_avg << "\n";

        csv_file.flush();
        csv_file.close();
        hft::common::log_info("[PerformanceMonitor] Time-series recording complete (" + m_csv_filename + ").");
    }

} // namespace hft::monitoring
