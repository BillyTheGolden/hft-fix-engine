/**
 * @file SystemOptimizations.hpp
 * @brief OS-level and hardware-level performance tuning helpers for Linux systems.
 */

#pragma once

#include <cstring>
#include <iostream>
#include <new>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <utility>

#if defined(HFT_ENABLE_NUMA)
#include <numa.h>
#endif

namespace hft::common
{

    /**
     * @brief Pins a native thread (via POSIX `pthread_t` handle) to a specific CPU core.
     * @details Depending on build-time conditional compilation (`HFT_ENABLE_NUMA`), this function uses:
     *          1. **POSIX Thread Affinity** (`pthread_setaffinity_np`) to lock execution to the target core.
     *          2. **NUMA Node Placement** (`numa_run_on_node` / `numa_set_preferred`) when `HFT_ENABLE_NUMA` is defined.
     * @param native_handle POSIX thread identifier (`pthread_t`).
     * @param cpu_id Logical core identifier (from 0 up to `sysconf(_SC_NPROCESSORS_ONLN) - 1`).
     * @return `true` if affinity was set successfully, `false` otherwise.
     */
    inline bool pin_native_thread_to_cpu(pthread_t native_handle, int cpu_id) noexcept
    {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_id, &cpuset);

        int result = pthread_setaffinity_np(native_handle, sizeof(cpu_set_t), &cpuset);
        if (result != 0)
        {
            std::cerr << "[SystemOptimizations] Warning: Failed to pin thread to CPU " << cpu_id
                      << " (Error: " << result << ")\n";
        }

#if defined(HFT_ENABLE_NUMA)
        // Configure NUMA node execution affinity and memory allocation policies if enabled and available
        if (numa_available() >= 0)
        {
            int node = numa_node_of_cpu(cpu_id);
            if (node >= 0)
            {
                if (numa_run_on_node(node) != 0)
                {
                    std::cerr << "[SystemOptimizations] Warning: Failed to set NUMA execution node " << node << "\n";
                }
                numa_set_preferred(node);
            }
        }
#endif

        return result == 0;
    }

    /**
     * @brief Pins the currently executing thread to a specific CPU core.
     * @details Calls `pin_native_thread_to_cpu` passing `pthread_self()`.
     * @param cpu_id Logical core identifier.
     * @return `true` if affinity was set successfully, `false` otherwise.
     */
    inline bool pin_thread_to_cpu(int cpu_id) noexcept
    {
        return pin_native_thread_to_cpu(pthread_self(), cpu_id);
    }

    /**
     * @brief Pins a C++20 `std::thread` instance to a specific CPU core using its native handle (`native_handle()`).
     * @details Leverages C++20 standard thread API interoperability by extracting `thread.native_handle()`
     *          (which maps to `pthread_t` on Linux POSIX platforms) and delegating to `pin_native_thread_to_cpu`.
     * @param thread Reference to the C++20 `std::thread` object.
     * @param cpu_id Logical core identifier.
     * @return `true` if affinity was set successfully, `false` otherwise.
     */
    inline bool pin_thread_to_cpu(std::thread& thread, int cpu_id) noexcept
    {
        return pin_native_thread_to_cpu(thread.native_handle(), cpu_id);
    }

    /**
     * @brief Requests real-time SCHED_FIFO scheduling priority for the calling process/thread.
     * @details Requires `root` or `CAP_SYS_NICE` privileges. Prevents time-sharing OS preemption of critical trading
     * loops.
     * @param priority Real-time priority level (typically between 1 and 99, where 99 is highest).
     * @return `true` if successful, `false` otherwise.
     */
    inline bool set_realtime_priority(int priority = 50) noexcept
    {
        struct sched_param param {};
        param.sched_priority = priority;

        if (sched_setscheduler(0, SCHED_FIFO, &param) != 0)
        {
            std::cerr << "[SystemOptimizations] Warning: SCHED_FIFO setting failed (requires root/CAP_SYS_NICE).\n";
            return false;
        }
        return true;
    }

    /**
     * @brief Returns the total number of online logical CPU cores available on the system.
     * @return Core count.
     */
    [[nodiscard]] inline int get_cpu_count() noexcept
    {
        return static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
    }

    /**
     * @brief Allocates an object of type T on a 2MB huge page using anonymous mmap.
     * @details Attempts to map virtual memory pages using MAP_HUGETLB to minimize TLB misses.
     *          If huge pages are not pre-allocated on the system, it automatically falls back
     *          to allocating standard 4KB virtual memory pages, logging a warning.
     *          Constructs the object in-place (placement new) using standard constructor forwarding.
     * @tparam T Type of the object to allocate.
     * @tparam Args Constructor argument types.
     * @param args Constructor arguments.
     * @return Pointer to the allocated object, or nullptr if allocation fails.
     */
    template <typename T, typename... Args>
    [[nodiscard]] inline T* allocate_on_huge_pages(Args &&...args) noexcept
    {
        size_t size = sizeof(T);
        size_t huge_page_size = 2 * 1024 * 1024;
        size_t rounded_size = ((size + huge_page_size - 1) / huge_page_size) * huge_page_size;

        void* ptr =
            mmap(nullptr, rounded_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);

        if (ptr == MAP_FAILED)
        {
            // Fallback to standard anonymous mmap if huge pages are not configured/available on the system
            ptr = mmap(nullptr, rounded_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (ptr == MAP_FAILED)
            {
                return nullptr;
            }
            std::cerr << "[SystemOptimizations] Warning: Failed to allocate huge pages. Fell back to standard pages.\n";
        }

        // Construct object in-place (placement new)
        return ::new (ptr) T(std::forward<Args>(args)...);
    }

    /**
     * @brief Deallocates an object of type T previously allocated on huge pages.
     * @details Safely runs the destructor of the object (~T) and unmaps the corresponding
     *          huge page segments via munmap.
     * @tparam T Type of the object.
     * @param ptr Pointer to the object.
     */
    template <typename T>
    inline void deallocate_huge_pages(T* ptr) noexcept
    {
        if (ptr == nullptr)
        {
            return;
        }
        ptr->~T();
        size_t size = sizeof(T);
        size_t huge_page_size = 2 * 1024 * 1024;
        size_t rounded_size = ((size + huge_page_size - 1) / huge_page_size) * huge_page_size;
        munmap(ptr, rounded_size);
    }

} // namespace hft::common
