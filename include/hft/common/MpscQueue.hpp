/**
 * @file MpscQueue.hpp
 * @brief Thread-safe Multi-Producer Single-Consumer queue using mutex and condition variable.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <utility>

namespace hft::common
{
    /**
     * @class MpscQueue
     * @brief A simple thread-safe queue designed for multi-producer, single-consumer communication.
     * @tparam T The element type stored in the queue.
     */
    template <typename T> class MpscQueue
    {
      public:
        MpscQueue() = default;
        ~MpscQueue() = default;

        // Delete copy constructor and assignment operator
        MpscQueue(const MpscQueue &) = delete;
        MpscQueue &operator=(const MpscQueue &) = delete;

        /**
         * @brief Pushes a new item into the queue (blocking/guaranteed write).
         * @param item Const reference to the item.
         */
        void push(const T &item)
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_queue.push(item);
            }
            m_cv.notify_one();
        }

        /**
         * @brief Pushes a new item using move semantics.
         * @param item Rvalue reference to the item.
         */
        void push(T &&item)
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_queue.push(std::move(item));
            }
            m_cv.notify_one();
        }

        /**
         * @brief Non-blocking push for use in latency-sensitive paths.
         * @param item Const reference to the item.
         * @return `true` if the push succeeded (lock was acquired), `false` otherwise.
         */
        bool try_push(const T &item)
        {
            std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
            if (!lock.owns_lock())
            {
                return false;
            }
            m_queue.push(item);
            m_cv.notify_one();
            return true;
        }

        /**
         * @brief Non-blocking push using move semantics for latency-sensitive paths.
         * @param item Rvalue reference to the item.
         * @return `true` if succeeded, `false` otherwise.
         */
        bool try_push(T &&item)
        {
            std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
            if (!lock.owns_lock())
            {
                return false;
            }
            m_queue.push(std::move(item));
            m_cv.notify_one();
            return true;
        }

        /**
         * @brief Blocks until an item is available or the running flag is set to false.
         * @param item Output reference where the popped item is moved.
         * @param running Atomic flag indicating if the consumer thread should keep running.
         * @return `true` if an item was popped; `false` if the queue is empty and running is false.
         */
        bool pop(T &item, std::atomic<bool> &running)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this, &running] { return !m_queue.empty() || !running.load(std::memory_order_relaxed); });

            if (m_queue.empty())
            {
                return false;
            }

            item = std::move(m_queue.front());
            m_queue.pop();
            return true;
        }

        /**
         * @brief Blocks until an item is available, the running flag is set to false, or the timeout expires.
         * @param item Output reference.
         * @param running Atomic running flag.
         * @param timeout_ms Timeout duration in milliseconds.
         * @return `true` if an item was popped; `false` if timeout expired or queue is empty and running is false.
         */
        bool pop_with_timeout(T &item, std::atomic<bool> &running, int timeout_ms)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                          [this, &running] { return !m_queue.empty() || !running.load(std::memory_order_relaxed); });

            if (m_queue.empty())
            {
                return false;
            }

            item = std::move(m_queue.front());
            m_queue.pop();
            return true;
        }

        /**
         * @brief Non-blocking pop.
         * @param item Reference where the popped item is copied.
         * @return `true` if an item was popped; `false` if the queue is empty.
         */
        bool try_pop(T &item)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_queue.empty())
            {
                return false;
            }
            item = std::move(m_queue.front());
            m_queue.pop();
            return true;
        }

        /**
         * @brief Checks if the queue is empty.

         * @return `true` if empty, `false` otherwise.
         */
        bool empty() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_queue.empty();
        }

        /**
         * @brief Gets the current size of the queue.
         * @return Number of elements in the queue.
         */
        size_t size() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_queue.size();
        }

        /**
         * @brief Notifies all waiting threads on the condition variable.
         */
        void notify_all()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_cv.notify_all();
        }

      private:
        std::queue<T> m_queue;
        mutable std::mutex m_mutex;
        std::condition_variable m_cv;
    };
} // namespace hft::common
