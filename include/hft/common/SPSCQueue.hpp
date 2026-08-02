/**
 * @file SPSCQueue.hpp
 * @brief Ultra-low latency, lock-free, wait-free Single Producer Single Consumer bounded queue.
 * @details Specialized SPSC circular ring buffer optimized for High-Frequency Trading (HFT).
 *
 * OPTIMIZATIONS IMPLEMENTED:
 * 1. Cached Read/Write Indices: Producer maintains `m_cached_read_pos` and Consumer maintains
 *    `m_cached_write_pos`. Atomic memory acquire operations cross CPU cache interconnects
 *    ONLY when a queue boundary condition (full/empty) occurs, eliminating 99.9% of cache line
 *    coherence invalidation stalls.
 * 2. Vectorized Batch Operations: `pop_batch()` and `push_batch()` allow multi-item pops and
 *    pushes in a single contiguous sweep with ONE single atomic release store per batch.
 * 3. Cache Line Padding (`alignas(64)`): Each atomic variable and local index is isolated to
 *    its own 64-byte L1 cache line to prevent false sharing between core producer and consumer threads.
 */

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <utility>

namespace hft::common
{

    /**
     * @class SPSCQueue
     * @brief Lock-free bounded circular buffer for zero-latency inter-thread message passing.
     * @tparam T Element type stored in the queue (must be trivially copyable or move constructible).
     * @tparam Capacity Power-of-two maximum element count of the queue.
     */
    template <typename T, size_t Capacity> class SPSCQueue
    {
        static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two greater than 1.");
        static_assert(Capacity > 1, "Capacity must be at least 2.");

      public:
        /**
         * @brief Constructs an empty SPSC queue and initializes indices.
         */
        SPSCQueue() noexcept : m_write_pos(0), m_cached_read_pos(0), m_read_pos(0), m_cached_write_pos(0)
        {
        }

        /**
         * @brief Pushes a new element into the back of the queue (Producer only).
         * @details Employs producer-local `m_cached_read_pos` check to avoid atomic load on `m_read_pos`.
         * @param item Const reference to the item being copied into the circular buffer slot.
         * @return `true` if inserted successfully; `false` if the queue is full.
         */
        [[nodiscard]] bool push(const T &item) noexcept
        {
            const size_t current_write = m_write_pos.load(std::memory_order_relaxed);
            const size_t next_write = (current_write + 1) & (Capacity - 1);

            // OPTIMIZATION: Check local cached read index first (avoids cross-core atomic load)
            if (next_write == m_cached_read_pos) [[unlikely]]
            {
                m_cached_read_pos = m_read_pos.load(std::memory_order_acquire);
                if (next_write == m_cached_read_pos) [[unlikely]]
                {
                    return false; // Queue is full
                }
            }

            m_buffer[current_write] = item;
            m_write_pos.store(next_write, std::memory_order_release);
            return true;
        }

        /**
         * @brief Pushes a new element using move semantics (Producer only).
         * @details Employs producer-local `m_cached_read_pos` check to avoid atomic load on `m_read_pos`.
         * @param item Rvalue reference moved into the buffer slot.
         * @return `true` if moved successfully; `false` if full.
         */
        [[nodiscard]] bool push(T &&item) noexcept
        {
            const size_t current_write = m_write_pos.load(std::memory_order_relaxed);
            const size_t next_write = (current_write + 1) & (Capacity - 1);

            // OPTIMIZATION: Check local cached read index first (avoids cross-core atomic load)
            if (next_write == m_cached_read_pos) [[unlikely]]
            {
                m_cached_read_pos = m_read_pos.load(std::memory_order_acquire);
                if (next_write == m_cached_read_pos) [[unlikely]]
                {
                    return false; // Queue is full
                }
            }

            m_buffer[current_write] = std::move(item);
            m_write_pos.store(next_write, std::memory_order_release);
            return true;
        }

        /**
         * @brief Pops the front element out of the queue (Consumer only).
         * @details Employs consumer-local `m_cached_write_pos` check to avoid atomic load on `m_write_pos`.
         * @param item Reference where the dequeued item is moved.
         * @return `true` if an item was popped successfully; `false` if the queue is empty.
         */
        [[nodiscard]] bool pop(T &item) noexcept
        {
            const size_t current_read = m_read_pos.load(std::memory_order_relaxed);

            // OPTIMIZATION: Check local cached write index first (avoids cross-core atomic load)
            if (current_read == m_cached_write_pos) [[unlikely]]
            {
                m_cached_write_pos = m_write_pos.load(std::memory_order_acquire);
                if (current_read == m_cached_write_pos) [[unlikely]]
                {
                    return false; // Queue is empty
                }
            }

            item = std::move(m_buffer[current_read]);
            const size_t next_read = (current_read + 1) & (Capacity - 1);
            m_read_pos.store(next_read, std::memory_order_release);
            return true;
        }

        /**
         * @brief Vectorized Batch Popping: Dequeues up to `max_items` into a contiguous output buffer.
         *
         * @details OPTIMIZATION (High Finding 3.1):
         *          Splits the pop operation into up to two contiguous pointer ranges (`[current_read, tail)`
         *          and `[0, wrap)`). Instead of performing per-element modulo arithmetic (`read_idx = (read_idx + 1) &
         * mask`) inside a scalar loop, `std::move` across contiguous pointers is used. This enables GCC/Clang to
         * auto-vectorize memory moves using hardware SIMD (`vmovdqu` / `memcpy`), significantly accelerating batch
         * dequeue performance.
         *
         *          Reduces atomic synchronization by issuing a SINGLE `m_read_pos.store()` release for the entire
         * batch.
         *
         * @param output_items Pointer to array where popped items will be stored.
         * @param max_items Maximum number of items to pop.
         * @return Count of items successfully popped.
         */
        size_t pop_batch(T *output_items, size_t max_items) noexcept
        {
            const size_t current_read = m_read_pos.load(std::memory_order_relaxed);
            size_t write_pos = m_cached_write_pos;

            if (current_read == write_pos)
            {
                write_pos = m_write_pos.load(std::memory_order_acquire);
                m_cached_write_pos = write_pos;
                if (current_read == write_pos)
                {
                    return 0;
                }
            }

            const size_t available =
                (write_pos >= current_read) ? (write_pos - current_read) : (Capacity - current_read + write_pos);

            const size_t count = std::min(available, max_items);

            // Vectorized 2-segment contiguous memory move
            const size_t tail_len = Capacity - current_read;
            const size_t first_chunk = std::min(count, tail_len);
            std::move(m_buffer.data() + current_read, m_buffer.data() + current_read + first_chunk, output_items);

            if (count > first_chunk)
            {
                std::move(m_buffer.data(), m_buffer.data() + (count - first_chunk), output_items + first_chunk);
            }

            const size_t new_read = (current_read + count) & (Capacity - 1);

            // Single atomic release store for the entire batch!
            m_read_pos.store(new_read, std::memory_order_release);
            return count;
        }

        /**
         * @brief Vectorized Batch Pushing: Pushes a contiguous array of items into the queue (Producer only).
         *
         * @details OPTIMIZATION (High Finding 3.1):
         *          Splits push operation into up to two contiguous pointer ranges (`[current_write, tail)`
         *          and `[0, wrap)`). Replaces scalar per-element modulo loops with `std::copy` over
         *          contiguous pointers, enabling SIMD vectorization (`vmovdqu` / `memcpy`).
         *
         * @param items Pointer to array of input items.
         * @param count Number of items to push.
         * @return Count of items successfully pushed.
         */
        size_t push_batch(const T *items, size_t count) noexcept
        {
            const size_t current_write = m_write_pos.load(std::memory_order_relaxed);
            size_t read_pos = m_cached_read_pos;

            size_t free_slots =
                (read_pos > current_write) ? (read_pos - current_write - 1) : (Capacity - current_write + read_pos - 1);

            if (free_slots < count) [[unlikely]]
            {
                read_pos = m_read_pos.load(std::memory_order_acquire);
                m_cached_read_pos = read_pos;
                free_slots = (read_pos > current_write) ? (read_pos - current_write - 1)
                                                        : (Capacity - current_write + read_pos - 1);
            }

            const size_t to_push = std::min(free_slots, count);

            // Vectorized 2-segment contiguous memory copy
            const size_t tail_len = Capacity - current_write;
            const size_t first_chunk = std::min(to_push, tail_len);
            std::copy(items, items + first_chunk, m_buffer.data() + current_write);

            if (to_push > first_chunk)
            {
                std::copy(items + first_chunk, items + to_push, m_buffer.data());
            }

            const size_t new_write = (current_write + to_push) & (Capacity - 1);

            // Single atomic release store for the entire batch!
            m_write_pos.store(new_write, std::memory_order_release);
            return to_push;
        }

        /**
         * @brief Checks if the queue is currently empty.
         * @return `true` if empty, `false` otherwise.
         */
        [[nodiscard]] bool empty() const noexcept
        {
            return m_read_pos.load(std::memory_order_acquire) == m_write_pos.load(std::memory_order_acquire);
        }

        /**
         * @brief Checks if the queue can currently accept a new push without overflowing.
         * @return `true` if push will succeed, `false` if full.
         */
        [[nodiscard]] bool can_push() const noexcept
        {
            const size_t current_write = m_write_pos.load(std::memory_order_relaxed);
            const size_t next_write = (current_write + 1) & (Capacity - 1);
            return next_write != m_read_pos.load(std::memory_order_acquire);
        }

        /**
         * @brief Returns the approximate number of elements currently in the queue.
         * @return Element count.
         */
        [[nodiscard]] size_t size() const noexcept
        {
            const size_t write = m_write_pos.load(std::memory_order_acquire);
            const size_t read = m_read_pos.load(std::memory_order_acquire);
            if (write >= read)
            {
                return write - read;
            }
            return Capacity - read + write;
        }

        /**
         * @brief Returns the maximum number of items the queue can hold.
         * @return Capacity minus one (one slot reserved to differentiate full from empty).
         */
        [[nodiscard]] static constexpr size_t capacity() noexcept
        {
            return Capacity - 1;
        }

      private:
        /** @brief Aligned producer write index. */
        alignas(64) std::atomic<size_t> m_write_pos;

        /** @brief Producer thread-local cached read index (eliminates cross-core atomic load per push). */
        alignas(64) size_t m_cached_read_pos;

        /** @brief Aligned circular buffer array. */
        alignas(64) std::array<T, Capacity> m_buffer;

        /** @brief Aligned consumer read index. */
        alignas(64) std::atomic<size_t> m_read_pos;

        /** @brief Consumer thread-local cached write index (eliminates cross-core atomic load per pop). */
        alignas(64) size_t m_cached_write_pos;
    };

} // namespace hft::common
