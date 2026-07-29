/**
 * @file Telemetry.hpp
 * @brief Zero-overhead atomic telemetry counters shared across hot trading threads and the monitor thread.
 */

#pragma once

#include <atomic>
#include <cstdint>

namespace hft::monitoring
{

    /**
     * @struct TelemetryCounters
     * @brief Cache-line aligned telemetry storage updated via relaxed atomic operations by hot path threads.
     * @details Separated into distinct 64-byte cache lines to guarantee zero false sharing between
     *          the network consumer thread (writer of RX metrics) and the FIX worker thread (writer of parse metrics).
     */
    struct TelemetryCounters
    {
        /**
         * @struct RxMetrics
         * @brief Metrics recorded exclusively by the RX Ring Consumer thread.
         */
        struct alignas(64) RxMetrics
        {
            /** @brief Total number of valid UDP frames extracted from the kernel ring buffer. */
            std::atomic<uint64_t> frames_captured{0};
        } rx;

        /**
         * @struct WorkerMetrics
         * @brief Metrics recorded exclusively by the FIX Worker parser thread.
         */
        struct alignas(64) WorkerMetrics
        {
            /** @brief Total number of FIX messages fully processed. */
            std::atomic<uint64_t> messages_processed{0};

            /** @brief Total number of orders approved by pre-trade risk validation. */
            std::atomic<uint64_t> risk_approved{0};

            /** @brief Total number of orders rejected by pre-trade risk validation. */
            std::atomic<uint64_t> risk_rejected{0};

            /** @brief Cumulative nanosecond latency across all processed messages. */
            std::atomic<uint64_t> total_latency_ns{0};

            /** @brief Cumulative nanosecond latency for risk-approved orders. */
            std::atomic<uint64_t> approved_total_latency_ns{0};

            /** @brief Cumulative nanosecond latency for risk-rejected orders. */
            std::atomic<uint64_t> rejected_total_latency_ns{0};

            /** @brief Minimum observed nanosecond latency in the current sample window. */
            std::atomic<uint64_t> min_latency_ns{UINT64_MAX};

            /** @brief Maximum observed nanosecond latency in the current sample window. */
            std::atomic<uint64_t> max_latency_ns{0};
        } worker;
    };

} // namespace hft::monitoring
