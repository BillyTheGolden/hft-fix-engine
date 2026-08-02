/**
 * @file RxRingConsumer.hpp
 * @brief Zero-copy PACKET_MMAP RX ring buffer bypass receiver.
 */

#pragma once

#include "hft/common/SPSCQueue.hpp"
#include "hft/common/Types.hpp"
#include "hft/monitoring/Telemetry.hpp"
#include <cstdint>
#include <string>

namespace hft::networking
{

    /**
     * @class IRxConsumer
     * @brief Abstract interface representing a network frame consumer.
     */
    class IRxConsumer
    {
      public:
        virtual ~IRxConsumer() = default;

        /**
         * @brief Starts the packet polling and zero-copy extraction loop.
         */
        virtual void run() = 0;
    };

    /**
     * @class PacketMmapRxConsumer
     * @brief Zero-copy receiver utilizing Linux `AF_PACKET` with `PACKET_RX_RING` (`tpacket_v2`).
     * @details Binds to a network interface at Layer 2, maps kernel packet buffers into user memory (`mmap`),
     *          manually strips Ethernet/IP/UDP headers without kernel network stack processing, and pushes
     *          extracted payloads into an ultra-low-latency `SPSCQueue`. Applies `SCHED_FIFO` real-time scheduling
     *          (priority 80) and CPU pinning via `apply_realtime_thread_settings()` on startup.
     */
    class PacketMmapRxConsumer final : public IRxConsumer
    {
      public:
        /**
         * @brief Constructs a zero-copy RX ring consumer.
         * @param interface_name Name of the network interface to bind (e.g., "lo" or "eth0").
         * @param filter_port Destination UDP port to filter and extract FIX payloads from.
         * @param queue Reference to the shared lock-free SPSC queue where packets will be pushed.
         * @param telemetry Reference to shared atomic telemetry counters.
         * @param cpu_pin Logical CPU core ID for thread pinning (-1 for unpinned).
         */
        PacketMmapRxConsumer(std::string interface_name, uint16_t filter_port,
                             hft::common::SPSCQueue<hft::common::FixMessagePacket, 8192> &queue,
                             hft::monitoring::TelemetryCounters &telemetry, int cpu_pin = -1);

        ~PacketMmapRxConsumer() override;

        /**
         * @brief Executes the busy-polling ring buffer consumption loop.
         * @details Applies `apply_realtime_thread_settings(m_cpu_pin, 80, "hft_rx_ring")` as its first action
         *          to lock thread affinity and elevate to `SCHED_FIFO` real-time scheduling class, eliminating
         *          OS preemption jitter at the network ingestion head.
         */
        void run() override;

      private:
        std::string m_interface_name;
        uint16_t m_filter_port;
        hft::common::SPSCQueue<hft::common::FixMessagePacket, 8192> &m_queue;
        hft::monitoring::TelemetryCounters &m_telemetry;
        int m_cpu_pin;
        int m_sockfd{-1};
        uint8_t *m_mapped_buffer{nullptr};
        size_t m_total_ring_size{0};

        void cleanup() noexcept;
    };

} // namespace hft::networking
