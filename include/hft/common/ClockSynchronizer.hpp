/**
 * @file ClockSynchronizer.hpp
 * @brief Nanosecond clock synchronization, RDTSC cycle calibration, and clock-drift protection.
 */

#pragma once

#include "hft/common/Types.hpp"
#include <cstdint>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <string>
#include <thread>
#include <atomic>

namespace hft::common
{
    /**
     * @struct ClockStatus
     * @brief Result of timestamp normalization and drift evaluation.
     */
    struct ClockStatus
    {
        uint64_t rdtsc_cycles{ 0 };
        uint64_t timestamp_ns{ 0 };
        int64_t clock_drift_ns{ 0 };
        bool clock_jump_detected{ false };
        bool negative_latency_prevented{ false };
    };

    /**
     * @class ClockSynchronizer
     * @brief Manages sub-nanosecond RDTSC cycle calibration and guards against NTP/PTP clock jumps.
     */
    class ClockSynchronizer
    {
    public:
        ClockSynchronizer() noexcept
        {
            calibrate();
        }

        /**
         * @brief Calibrates RDTSC cycle frequency against CLOCK_MONOTONIC_RAW.
         */
        void calibrate() noexcept
        {
            uint64_t start_cycles = rdtsc();
            auto start_ts = std::chrono::steady_clock::now();

            std::this_thread::sleep_for(std::chrono::milliseconds(5));

            uint64_t end_cycles = rdtsc();
            auto end_ts = std::chrono::steady_clock::now();

            double elapsed_ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end_ts - start_ts).count());
            double elapsed_cycles = static_cast<double>(end_cycles - start_cycles);

            if (elapsed_ns > 0.0 && elapsed_cycles > 0.0)
            {
                m_cycles_per_ns.store(elapsed_cycles / elapsed_ns, std::memory_order_relaxed);
            }
            m_last_valid_ts_ns = get_realtime_ns();
        }

        /**
         * @brief Converts raw RDTSC cycles to nanoseconds based on calibrated frequency.
         * @param cycles Cycle delta.
         * @return Nanosecond duration.
         */
        [[nodiscard]] uint64_t cycles_to_ns(uint64_t cycles) const noexcept
        {
            double factor = m_cycles_per_ns.load(std::memory_order_relaxed);
            return (factor > 0.0) ? static_cast<uint64_t>(static_cast<double>(cycles) / factor) : cycles;
        }

        /**
         * @brief Evaluates an incoming RX timestamp against processing timestamp to prevent negative latency anomalies.
         * @param rx_ts_ns Timestamp recorded at packet ingress.
         * @param proc_ts_ns Timestamp recorded at parser execution.
         * @return Normalized latency in nanoseconds (clamped to 0 if clock jump caused negative delta).
         */
        [[nodiscard]] ClockStatus evaluate_latency(uint64_t rx_ts_ns, uint64_t proc_ts_ns) noexcept
        {
            ClockStatus status{};
            status.rdtsc_cycles = rdtsc();

            if (proc_ts_ns < rx_ts_ns)
            {
                // Backward time jump detected! Prevent negative latency
                status.clock_jump_detected = true;
                status.negative_latency_prevented = true;
                status.clock_drift_ns = static_cast<int64_t>(proc_ts_ns) - static_cast<int64_t>(rx_ts_ns);
                status.timestamp_ns = 0; // Clamped to zero
                m_clock_jumps_prevented++;
            }
            else
            {
                status.timestamp_ns = proc_ts_ns - rx_ts_ns;
                status.clock_drift_ns = 0;
            }

            return status;
        }

        /**
         * @brief Returns current wall-clock UTC time in nanoseconds since Epoch.
         */
        [[nodiscard]] static uint64_t get_realtime_ns() noexcept
        {
            struct timespec ts {};
            clock_gettime(CLOCK_REALTIME, &ts);
            return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<uint64_t>(ts.tv_nsec);
        }

        /**
         * @brief Formats a nanosecond timestamp into a standard FIX UTC timestamp string (`YYYYMMDD-HH:MM:SS.ffffff`).
         */
        static void format_utc_timestamp(uint64_t ns_since_epoch, char* out_buf, size_t buf_size) noexcept
        {
            if (!out_buf || buf_size < 32) return;

            time_t sec = static_cast<time_t>(ns_since_epoch / 1'000'000'000ULL);
            uint64_t sub_ms = (ns_since_epoch % 1'000'000'000ULL) / 1'000'000ULL;

            struct tm tm_info {};
            gmtime_r(&sec, &tm_info);

            std::snprintf(out_buf, buf_size, "%04d%02d%02d-%02d:%02d:%02d.%03u",
                tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
                static_cast<unsigned int>(sub_ms));
        }

        [[nodiscard]] double cycles_per_ns() const noexcept { return m_cycles_per_ns.load(std::memory_order_relaxed); }
        [[nodiscard]] uint64_t clock_jumps_prevented() const noexcept { return m_clock_jumps_prevented; }

    private:
        std::atomic<double> m_cycles_per_ns{ 3.0 }; // Default 3.0 GHz baseline
        uint64_t m_last_valid_ts_ns{ 0 };
        uint64_t m_clock_jumps_prevented{ 0 };
    };
} // namespace hft::common
