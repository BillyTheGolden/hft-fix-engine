/**
 * @file CpuSchedulerBenchmark.hpp
 * @brief Hardware core isolation, real-time priority verification, and CPU jitter benchmark.
 */

#pragma once

#include "hft/common/SystemOptimizations.hpp"
#include "hft/common/Types.hpp"
#include <cstdint>
#include <cstddef>
#include <vector>
#include <algorithm>
#include <immintrin.h>

namespace hft::common
{
    /**
     * @struct CpuBenchmarkResult
     * @brief Detailed metrics from CPU thread scheduling & jitter benchmark.
     */
    struct CpuBenchmarkResult
    {
        bool core_pinning_success{false};
        bool realtime_priority_requested{false};
        size_t total_iterations{0};
        uint64_t min_cycles{0};
        uint64_t max_cycles{0};
        uint64_t avg_cycles{0};
        uint64_t p99_cycles{0};
        size_t preemption_spikes_count{0}; ///< Iterations with >10x avg cycles (OS context switch indicator).
        double min_ns{0.0};
        double max_ns{0.0};
        double avg_ns{0.0};
        double p99_ns{0.0};
    };

    /**
     * @class CpuSchedulerBenchmark
     * @brief Benchmarks OS scheduling determinism, thread affinity, and pause-spin CPU pipeline warmth.
     */
    class CpuSchedulerBenchmark
    {
    public:
        /**
         * @brief Executes a CPU scheduling determinism benchmark.
         * @param target_cpu Logical CPU core to pin execution thread.
         * @param iterations Number of benchmark iterations (default 100,000).
         * @return `CpuBenchmarkResult` containing latency distribution and preemption spike metrics.
         */
        static CpuBenchmarkResult run_isolation_benchmark(int target_cpu, size_t iterations = 100000) noexcept
        {
            CpuBenchmarkResult result{};
            result.total_iterations = iterations;

            // 1. Apply Thread Affinity
            result.core_pinning_success = pin_thread_to_cpu(target_cpu);

            // 2. Request Real-Time Scheduling Priority
            result.realtime_priority_requested = set_realtime_priority(50);

            std::vector<uint64_t> samples;
            samples.reserve(iterations);

            uint64_t total_cycles = 0;
            uint64_t min_c = UINT64_MAX;
            uint64_t max_c = 0;

            // Warm up CPU instruction pipeline and L1 cache
            for (int i = 0; i < 1000; ++i)
            {
                _mm_pause();
            }

            // Benchmark hot loop
            for (size_t i = 0; i < iterations; ++i)
            {
                uint64_t start_c = rdtsc();

                // Low-latency pause-spin simulation (prevents bus contention while holding CPU core)
                _mm_pause();

                uint64_t end_c = rdtsc();
                uint64_t delta = (end_c > start_c) ? (end_c - start_c) : 1;

                samples.push_back(delta);
                total_cycles += delta;

                if (delta < min_c) min_c = delta;
                if (delta > max_c) max_c = delta;
            }

            std::sort(samples.begin(), samples.end());

            result.min_cycles = min_c;
            result.max_cycles = max_c;
            result.avg_cycles = total_cycles / iterations;
            size_t p99_idx = static_cast<size_t>(static_cast<double>(iterations) * 0.99);
            result.p99_cycles = samples[p99_idx];

            // Count preemption spikes (defined as cycles > 10x average)
            uint64_t spike_threshold = result.avg_cycles * 10;
            for (uint64_t s : samples)
            {
                if (s > spike_threshold)
                {
                    result.preemption_spikes_count++;
                }
            }

            // Convert cycles to nanoseconds using baseline factor
            double cycles_per_ns = 2.5; // Baseline 2.5 GHz
            result.min_ns = static_cast<double>(result.min_cycles) / cycles_per_ns;
            result.max_ns = static_cast<double>(result.max_cycles) / cycles_per_ns;
            result.avg_ns = static_cast<double>(result.avg_cycles) / cycles_per_ns;
            result.p99_ns = static_cast<double>(result.p99_cycles) / cycles_per_ns;

            return result;
        }
    };
} // namespace hft::common
