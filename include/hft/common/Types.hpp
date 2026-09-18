/**
 * @file Types.hpp
 * @brief Common types, constants, and high-resolution timing primitives for the HFT FIX Engine.
 * @details Contains memory-aligned structures for zero-allocation packet passing and monotonic timestamps.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ctime>
#ifdef __x86_64__
#include <x86intrin.h>
#endif

namespace hft::common
{

    /**
     * @brief Size of each kernel allocation block for the PACKET_MMAP ring buffer (4 MB).
     */
    inline constexpr size_t BLOCK_SIZE = 4 * 1024 * 1024;

    /**
     * @brief Size of an individual frame slot within the ring buffer (2 KB, accommodates standard MTU).
     */
    inline constexpr size_t FRAME_SIZE = 2048;

    /**
     * @brief Total number of memory blocks allocated in kernel space for the ring buffer (128 MB total).
     */
    inline constexpr size_t BLOCK_NR = 32;

    /**
     * @brief Total number of frame slots across all blocks in the ring buffer (65,536 frames).
     */
    inline constexpr size_t FRAME_NR = (BLOCK_SIZE * BLOCK_NR) / FRAME_SIZE;

    /**
     * @brief Maximum byte length of a message protocol payload length.
     */
    inline constexpr size_t MAX_PAYLOAD_LEN = 512;

    /**
     * @struct AlignedAtomicFlag
     * @brief 64-byte cache-line aligned wrapper around std::atomic<bool>.
     *
     * @details OPTIMIZATION (High Finding 4.2):
     *          Prevents false sharing between global execution flags (`g_running`, `g_producer_done`,
     *          `g_consumer_done`). Previously, declared as adjacent `extern std::atomic<bool>` in BSS,
     *          they shared a single 64-byte cache line. When the producer or consumer thread wrote to
     *          its done flag, the L1/L2 cache line was invalidated across all reader CPU cores.
     *          `alignas(64)` guarantees each flag occupies its own dedicated cache line.
     */
    struct alignas(64) AlignedAtomicFlag
    {
        std::atomic<bool> flag{false};

        constexpr AlignedAtomicFlag() noexcept = default;

        explicit AlignedAtomicFlag(bool initial) noexcept : flag(initial)
        {
        }

        [[nodiscard]] bool load(std::memory_order order = std::memory_order_seq_cst) const noexcept
        {
            return flag.load(order);
        }

        void store(bool desired, std::memory_order order = std::memory_order_seq_cst) noexcept
        {
            flag.store(desired, order);
        }
    };

    /**
     * @brief Global execution flag to coordinate graceful shutdown across all active threads.
     * @details Aligned to 64-byte boundary to prevent false sharing invalidations on the hot path.
     */
    extern AlignedAtomicFlag g_running;

    /**
     * @brief Global flag to signal that the producer thread has completed injecting all packets.
     * @details Aligned to 64-byte boundary to prevent false sharing invalidations on the hot path.
     */
    extern AlignedAtomicFlag g_producer_done;

    /**
     * @brief Global flag to signal that the consumer thread has completed reading all packets and exited.
     * @details Aligned to 64-byte boundary to prevent false sharing invalidations on the hot path.
     */
    extern AlignedAtomicFlag g_consumer_done;

    /**
     * @brief Global flag to signal that the process was interrupted/stopped by user signal (SIGINT / SIGTERM).
     * @details Aligned to 64-byte boundary to prevent false sharing invalidations on the hot path.
     */
    extern AlignedAtomicFlag g_user_stopped;

    /**
     * @struct FixMessagePacket
     * @brief Compact 32-byte packet container transferred via the lock-free SPSC queue.
     * @details Packs naturally into 32 bytes (2 packets per 64-byte L1 cache line) to minimize
     *          L1/L2 cache footprint and avoid artificial padding bloat.
     */
    struct FixMessagePacket
    {
        /** @brief CPU cycle count recorded exactly when the frame was read from the kernel ring. */
        uint64_t rx_timestamp_cycles{0};

        /** @brief Number of valid bytes in the payload. */
        uint16_t payload_len{0};

        /** @brief Pointer to the raw payload buffer. */
        char *payload{nullptr};

        /** @brief Pointer to kernel packet ring header for ring buffer release. */
        void *ring_hdr{nullptr};
    };

    /**
     * @brief Retrieves the current monotonic timestamp in nanoseconds.
     * @details Utilizes `clock_gettime(CLOCK_MONOTONIC)` to ensure steady progression regardless of wall-clock updates.
     * @return Monotonic time in nanoseconds since epoch.
     */
    [[nodiscard]] inline uint64_t get_timestamp_ns() noexcept
    {
        struct timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<uint64_t>(ts.tv_nsec);
    }

    /**
     * @brief Reads the Time-Stamp Counter (RDTSC) with hardware instruction serialization.
     * @details Uses `__rdtscp` on x86_64 to prevent speculative out-of-order instruction reordering
     *          around the measurement boundary, providing accurate nanosecond cycle counts.
     * @return CPU cycle count from the `__rdtscp()` intrinsic or fallback.
     */
    [[nodiscard]] inline uint64_t rdtsc() noexcept
    {
#if defined(__x86_64__) || defined(_M_X64)
        unsigned int aux = 0;
        return __rdtscp(&aux);
#else
        return get_timestamp_ns();
#endif
    }

    /**
     * @brief Global variable storing calibrated CPU cycles per nanosecond.
     */
    extern double g_cycles_per_ns;

    /**
     * @brief Calibrates the RDTSC counter against CLOCK_MONOTONIC.
     * @return Calibrated cycles per nanosecond.
     */
    inline double calibrate_rdtsc() noexcept
    {
        // Warmup loop
        uint64_t start_time = get_timestamp_ns();
        while (get_timestamp_ns() - start_time < 10'000'000ULL)
        {
            __asm__ __volatile__("pause");
        }

        // Measurement loop (100ms)
        uint64_t start_cycles = rdtsc();
        start_time = get_timestamp_ns();
        while (get_timestamp_ns() - start_time < 100'000'000ULL)
        {
            __asm__ __volatile__("pause");
        }
        uint64_t end_cycles = rdtsc();
        uint64_t end_time = get_timestamp_ns();

        uint64_t elapsed_ns = end_time - start_time;
        uint64_t elapsed_cycles = end_cycles - start_cycles;

        if (elapsed_ns == 0) [[unlikely]]
        {
            return 1.0;
        }

        return static_cast<double>(elapsed_cycles) / static_cast<double>(elapsed_ns);
    }

    /**
     * @brief Severity categories for structured log messages.
     */
    enum class LogCategory
    {
        DEBUG_LEVEL,
        INFO_LEVEL,
        WARNING_LEVEL,
        ERROR_LEVEL,
        CRITICAL_LEVEL,
        FATAL_LEVEL
    };

    /**
     * @struct LogMessage
     * @brief Container for log events transferred asynchronously to the logging thread.
     */
    struct alignas(64) LogMessage
    {
        uint64_t timestamp_ns{0};
        LogCategory category{LogCategory::INFO_LEVEL};
        char message[256]{};

        constexpr LogMessage() noexcept = default;
    };

    /**
     * @brief Console message levels for UI coloring.
     */
    enum class ConsoleCategory
    {
        INFO_MSG,
        WARN_MSG,
        ERROR_MSG
    };

    /**
     * @struct ConsoleMessage
     * @brief Container for log events transferred asynchronously to the console logging thread.
     */
    struct alignas(64) ConsoleMessage
    {
        ConsoleCategory category{ConsoleCategory::INFO_MSG};
        char message[256]{};

        constexpr ConsoleMessage() noexcept = default;
    };

} // namespace hft::common
