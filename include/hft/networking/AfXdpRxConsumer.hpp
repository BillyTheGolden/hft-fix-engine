/**
 * @file AfXdpRxConsumer.hpp
 * @brief High-performance AF_XDP (XDP Sockets) receiver with Native & SKB Mode support.
 */

#pragma once

#include "hft/common/SPSCQueue.hpp"
#include "hft/common/Types.hpp"
#include "hft/monitoring/Telemetry.hpp"
#include "hft/networking/RxRingConsumer.hpp"

#include <cstdint>
#include <memory>
#include <string>

#if defined(HFT_ENABLE_AF_XDP)

#include <linux/if_link.h>
#include <bpf/libbpf.h>
#if defined(HFT_HAS_LIBXDP)
#include <xdp/xsk.h>
#else
#include <bpf/xsk.h>
#endif

namespace hft::networking
{

    /**
     * @class AfXdpRxConsumer
     * @brief AF_XDP Zero-Copy / Generic SKB Mode receiver for ultra-low-latency network frame capture.
     * @details Leverages Linux eBPF / AF_XDP (XSK) sockets to receive raw UDP packets directly into a
     *          lock-free UMEM ring buffer. Tries Native Hardware Zero-Copy Mode (`XDP_FLAGS_DRV_MODE`) first,
     *          and automatically falls back to Generic SKB Mode (`XDP_FLAGS_SKB_MODE`) if the underlying
     *          NIC driver (e.g. `r8169`) does not support native XDP hooks.
     */
    class AfXdpRxConsumer final : public IRxConsumer
    {
      public:
        static constexpr size_t NUM_FRAMES = 4096;
        static constexpr size_t FRAME_SIZE = 2048;
        static constexpr size_t BATCH_SIZE = 64;

        /**
         * @brief Constructs an AF_XDP RX Consumer.
         * @param interface_name Network interface to bind (e.g., "lo", "eth0", "enp3s0").
         * @param filter_port Destination UDP port number to extract FIX payloads from.
         * @param queue Shared lock-free SPSC queue for payload dispatching.
         * @param telemetry Telemetry counters for metric tracking.
         * @param cpu_pin CPU core ID for thread affinity (-1 for unpinned).
         */
        AfXdpRxConsumer(std::string interface_name, uint16_t filter_port,
                        hft::common::SPSCQueue<hft::common::FixMessagePacket, 8192> &queue,
                        hft::monitoring::TelemetryCounters &telemetry, int cpu_pin = -1);

        ~AfXdpRxConsumer() override;

        /**
         * @brief Initializes UMEM memory, XSK socket, and attaches BPF XDP hook.
         * @return `true` if AF_XDP initialization succeeded; `false` if unsupported/failed.
         */
        bool init() noexcept;

        /**
         * @brief Starts the busy-polling AF_XDP frame extraction loop.
         */
        void run() override;

        /**
         * @brief Returns whether the socket successfully initialized in Native Zero-Copy Mode.
         * @return `true` if Native Zero-Copy is active, `false` if running in SKB Copy Mode.
         */
        [[nodiscard]] bool is_native_zero_copy() const noexcept { return m_is_native; }

      private:
        std::string m_interface_name;
        uint16_t m_filter_port;
        hft::common::SPSCQueue<hft::common::FixMessagePacket, 8192> &m_queue;
        hft::monitoring::TelemetryCounters &m_telemetry;
        int m_cpu_pin;

        void *m_umem_buffer{nullptr};
        size_t m_umem_size{0};

        // Ring and socket handles
        struct xsk_ring_prod m_fq{};
        struct xsk_ring_cons m_cq{};
        struct xsk_ring_cons m_rx{};
        struct xsk_ring_prod m_tx{};

        struct xsk_umem *m_umem{nullptr};
        struct xsk_socket *m_xsk{nullptr};

        bool m_is_native{false};
        bool m_initialized{false};

        void cleanup() noexcept;
    };

} // namespace hft::networking

#endif // HFT_ENABLE_AF_XDP
