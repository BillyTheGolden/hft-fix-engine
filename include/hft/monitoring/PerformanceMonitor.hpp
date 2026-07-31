/**
 * @file PerformanceMonitor.hpp
 * @brief Dedicated background monitoring thread that samples HFT metrics and outputs time-series CSV data.
 */

#pragma once

#include "hft/common/SPSCQueue.hpp"
#include "hft/common/Types.hpp"
#include "hft/monitoring/Telemetry.hpp"

#include <string>

namespace hft::monitoring
{

    /**
     * @class IPerformanceMonitor
     * @brief Abstract interface representing a telemetry and performance sampling engine.
     */
    class IPerformanceMonitor
    {
      public:
        virtual ~IPerformanceMonitor() = default;

        /**
         * @brief Starts the periodic sampling and CSV generation loop in the calling thread.
         */
        virtual void run() = 0;
    };

    /**
     * @class CsvPerformanceMonitor
     * @brief Concrete monitoring thread that periodically samples atomic counters (`TelemetryCounters`)
     *        and `SPSCQueue` depth, recording high-precision time-series data to a CSV file
     * (`metrics_time_series.csv`).
     * @details Adheres to Single Responsibility Principle (SRP). Does not perform any locks or blocking I/O
     *          on the trading threads; reads are relaxed atomic loads executed at fixed millisecond intervals.
     */
    class CsvPerformanceMonitor final : public IPerformanceMonitor
    {
      public:
        /**
         * @brief Constructs a new CSV performance monitor.
         * @param telemetry Reference to shared atomic telemetry counters updated by hot path threads.
         * @param queue Reference to the SPSC queue to monitor instantaneous buffer fill depth.
         * @param csv_filename Destination CSV file path for recording time-series metrics.
         * @param sample_interval_ms Sampling frequency in milliseconds (e.g., 5 ms or 10 ms).
         * @param cpu_pin Logical CPU core ID for thread pinning (-1 for unpinned).
         */
        CsvPerformanceMonitor(const TelemetryCounters &telemetry,
                              const hft::common::SPSCQueue<hft::common::FixMessagePacket, 8192> &queue,
                              std::string csv_filename = "fix_metrics_time_series.csv",
                              uint32_t sample_interval_ms = 10, int cpu_pin = -1);

        ~CsvPerformanceMonitor() override = default;

        /**
         * @brief Executes the periodic sampling loop until `g_running` becomes false.
         */
        void run() override;

      private:
        const TelemetryCounters &m_telemetry;
        const hft::common::SPSCQueue<hft::common::FixMessagePacket, 8192> &m_queue;
        std::string m_csv_filename;
        uint32_t m_sample_interval_ms;
        int m_cpu_pin;
    };

} // namespace hft::monitoring
