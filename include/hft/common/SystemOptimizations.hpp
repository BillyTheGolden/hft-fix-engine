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
     *          2. **NUMA Node Placement** (`numa_run_on_node` / `numa_set_preferred`) when `HFT_ENABLE_NUMA` is
     * defined.
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
    inline bool pin_thread_to_cpu(std::thread &thread, int cpu_id) noexcept
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
        struct sched_param param{};
        param.sched_priority = priority;

        if (sched_setscheduler(0, SCHED_FIFO, &param) != 0)
        {
            std::cerr << "[SystemOptimizations] Warning: SCHED_FIFO setting failed (requires root/CAP_SYS_NICE).\n";
            return false;
        }
        return true;
    }

    /**
     * @brief Applies the full suite of real-time thread settings for an HFT hot-path thread.
     *
     * @details This function is the **primary entry point** for activating low-latency thread configuration.
     *          It must be called at the very start of every hot-path thread's execution (e.g., the worker,
     *          consumer, and producer threads) — ideally before the first iteration of their main loop.
     *
     *          The three operations performed are deliberately ordered:
     *
     *          ### 1. CPU Core Pinning (`pthread_setaffinity_np`)
     *          Locks the calling thread to a single logical core. This prevents the Linux CFS scheduler
     *          from migrating the thread across cores, which would:
     *          - Invalidate the thread's private L1/L2 cache (cold-start penalty of ~100–200 ns per line)
     *          - Disrupt hardware branch predictor state (retrained from scratch on every migration)
     *          - Introduce TLB shootdowns as the new core must flush stale page-table entries
     *          Without pinning, the OS can migrate threads every scheduler tick (~4 ms on a 250 Hz kernel).
     *
     *          ### 2. SCHED_FIFO Real-Time Priority (`sched_setscheduler`)
     *          Elevates the thread from the default CFS (Completely Fair Scheduler) time-sharing class to
     *          the SCHED_FIFO real-time class. The critical difference:
     *          - **CFS**: Each thread receives a time quantum (~1–4 ms). When the quantum expires the kernel
     *            PREEMPTS the thread — even if it is in the middle of the matching hot loop — and context-
     *            switches to another task. This adds up to 4 ms of worst-case latency on every preemption.
     *          - **SCHED_FIFO**: The thread is NEVER preempted by a lower-priority task. It runs until it
     *            voluntarily yields (e.g., `sleep_for`, blocking on I/O, or calling `sched_yield()`). This
     *            eliminates scheduler-induced latency jitter entirely on the hot path.
     *
     *          **Note:** The HFT engine already requires `root` or `CAP_SYS_NICE` for `AF_PACKET`
     *          (PACKET_MMAP zero-copy sockets), so the privilege requirement is already satisfied.
     *
     *          Priority 80 is chosen as a safe default: above general-purpose realtime (50), but below
     *          kernel watchdog threads (99), preventing a runaway loop from hanging the machine.
     *
     *          ### 3. Thread Name (`pthread_setname_np`)
     *          Sets the thread's kernel name (visible in `htop`, `perf`, `/proc/<pid>/task/*/comm`).
     *          This is essential for operator diagnosis during production incidents — an unlabeled thread
     *          makes it impossible to correlate `perf` flame graphs, `strace` output, or `top` CPU usage
     *          with specific engine components.
     *
     * @param cpu_id      Logical CPU core to pin to. Pass -1 to skip pinning (non-real-time mode).
     * @param rt_priority SCHED_FIFO real-time priority. Pass 0 to skip real-time elevation.
     * @param thread_name Name to assign via `pthread_setname_np` (max 15 chars on Linux). Pass nullptr to skip.
     */
    inline void apply_realtime_thread_settings(int cpu_id, int rt_priority = 80,
                                               const char* thread_name = nullptr) noexcept
    {
        // ── Step 1: CPU Core Pinning ─────────────────────────────────────────────────────────────
        // Lock execution to a single physical core. Without this, CFS can migrate this thread to any
        // other core at will, destroying cache locality and branch predictor state.
        if (cpu_id >= 0)
        {
            if (pin_thread_to_cpu(cpu_id))
            {
                std::cerr << "[SystemOptimizations] Thread pinned to CPU core " << cpu_id << ".\n";
            }
        }

        // ── Step 2: SCHED_FIFO Real-Time Scheduling ──────────────────────────────────────────────
        // Promote this thread from the CFS time-sharing class to SCHED_FIFO real-time class.
        // This is the single most impactful change for worst-case latency: it eliminates the OS
        // scheduler from preempting this thread mid-loop (which normally occurs every 1–4 ms under CFS).
        //
        // Priority 80 is used: safe for production (below kernel watchdog at 99), yet high enough to
        // supersede all normal-priority threads and other realtime tasks at priority ≤ 79.
        //
        // Requires: root or CAP_SYS_NICE capability (already needed for AF_PACKET sockets).
        if (rt_priority > 0)
        {
            if (set_realtime_priority(rt_priority))
            {
                std::cerr << "[SystemOptimizations] SCHED_FIFO priority " << rt_priority << " activated.\n";
            }
            // If SCHED_FIFO fails (e.g., no CAP_SYS_NICE), we continue — the engine will still work
            // under CFS, just with higher worst-case latency. A warning is printed by set_realtime_priority().
        }

        // ── Step 3: Thread Naming ────────────────────────────────────────────────────────────────
        // Assign a human-readable kernel-visible name to this thread. Names are capped at 15 chars
        // on Linux (TASK_COMM_LEN - 1). This name appears in:
        //   - `htop` and `top` process trees
        //   - `perf record` flame graphs
        //   - `/proc/<pid>/task/<tid>/comm`
        //   - `strace -f` output
        // Without named threads, production debugging requires matching TIDs to roles manually.
        if (thread_name != nullptr)
        {
            // pthread_setname_np silently truncates to 15 chars; no error handling needed.
            pthread_setname_np(pthread_self(), thread_name);
        }
    }

    /**
     * @brief Locks all current and future memory pages of the calling process into physical RAM.
     *
     * @details Calls `mlockall(MCL_CURRENT | MCL_FUTURE)` to instruct the kernel to never swap any
     *          page belonging to this process out to disk. This is **critical** for HFT workloads
     *          because:
     *
     *          ### Why Page Faults Kill Latency
     *          Without `mlockall`, the Linux kernel may opportunistically swap out cold memory pages
     *          (e.g., `.text` sections not recently executed, `BSS` globals, or heap arenas) to the
     *          swap partition when memory pressure builds. When the engine subsequently accesses a
     *          swapped page:
     *          1. A **major page fault** is triggered (hardware exception).
     *          2. The kernel must perform a synchronous disk I/O to load the page back.
     *          3. The faulting thread is **blocked** for the duration — typically **1–10 ms**.
     *
     *          For context: the entire target latency of the engine is 500–2000 ns. A single major
     *          page fault on a hot-path address is a **1000× latency spike**.
     *
     *          ### What `MCL_CURRENT | MCL_FUTURE` Locks
     *          - `MCL_CURRENT`: All pages already mapped at call time (`.text`, `.data`, `.bss`,
     *            heap, stack, shared libraries).
     *          - `MCL_FUTURE`: All pages that will be mapped after this call (e.g., new `mmap`
     *            regions, stack growth, heap expansions). This is essential to pin the huge-page
     *            queue and UMEM ring buffers that are created after `main()` starts.
     *
     *          ### Privilege Requirement
     *          `mlockall` requires `CAP_IPC_LOCK` or `root`. The HFT engine already runs as root
     *          (required for `AF_PACKET` raw sockets), so this incurs no additional privilege cost.
     *
     *          ### Recommended Call Site
     *          Call this function **as early as possible in `main()`**, before allocating the SPSC
     *          queue, UMEM ring, or launching any threads, so that all subsequent allocations are
     *          automatically locked.
     *
     * @return `true` if all pages were successfully locked, `false` otherwise (warning is printed).
     */
    inline bool lock_process_memory() noexcept
    {
        // MCL_CURRENT pins all pages that already exist in the process address space at this point.
        // MCL_FUTURE ensures that every page mapped after this call (heap, mmap, stack growth) is
        // also immediately pinned — this covers the huge-page SPSC queue and UMEM ring buffers
        // that are created later in main().
        if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        {
            std::cerr << "[SystemOptimizations] Warning: mlockall(MCL_CURRENT|MCL_FUTURE) failed. "
                      << "Memory pages may be paged out under pressure, causing major page-fault "
                      << "latency spikes. Run as root or grant CAP_IPC_LOCK to the binary.\n";
            return false;
        }

        std::cerr << "[SystemOptimizations] mlockall: all process memory pages pinned to RAM.\n";
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
    template <typename T, typename... Args> [[nodiscard]] inline T *allocate_on_huge_pages(Args &&...args) noexcept
    {
        size_t size = sizeof(T);
        size_t huge_page_size = 2 * 1024 * 1024;
        size_t rounded_size = ((size + huge_page_size - 1) / huge_page_size) * huge_page_size;

        void *ptr =
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
    template <typename T> inline void deallocate_huge_pages(T *ptr) noexcept
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
