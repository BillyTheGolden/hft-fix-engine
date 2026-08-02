/**
 * @file RxConsumerFactory.hpp
 * @brief Factory function for creating low-latency network receivers with automatic fallback chain.
 */

#pragma once

#include "hft/common/ConsoleLogger.hpp"
#include "hft/networking/RxRingConsumer.hpp"

#if defined(HFT_ENABLE_AF_XDP)
#include "hft/networking/AfXdpRxConsumer.hpp"
#endif

#include <memory>
#include <string>

namespace hft::networking
{

    /**
     * @brief Factory function creating an optimal network receiver with automatic fallback.
     * @details Primary Choice: `AfXdpRxConsumer` (Native Zero-Copy or Generic SKB Mode).
     *          Fallback 1: `PacketMmapRxConsumer` (`AF_PACKET` + `tpacket_v2`).
     *          Fallback 2: Standard UDP Socket (`AF_INET`).
     * @param interface_name Network interface to bind (e.g., "lo", "eth0", "enp3s0").
     * @param filter_port Destination UDP port number to extract FIX payloads from.
     * @param queue Shared lock-free SPSC queue for payload dispatching.
     * @param telemetry Telemetry counters for metric tracking.
     * @param cpu_pin CPU core ID for thread affinity (-1 for unpinned).
     * @return `std::unique_ptr<IRxConsumer>` initialized receiver instance.
     */
    inline std::unique_ptr<IRxConsumer> create_rx_consumer(
        const std::string &interface_name, uint16_t filter_port,
        hft::common::SPSCQueue<hft::common::FixMessagePacket, 8192> &queue,
        hft::monitoring::TelemetryCounters &telemetry, int cpu_pin = -1, uint32_t queue_id = 0)
    {
#if defined(HFT_ENABLE_AF_XDP)
        hft::common::log_info("[RxConsumerFactory] Attempting Primary Receiver: AF_XDP (XDP Sockets)...");
        auto af_xdp =
            std::make_unique<AfXdpRxConsumer>(interface_name, filter_port, queue, telemetry, cpu_pin, queue_id);
        if (af_xdp->init())
        {
            if (af_xdp->is_native_zero_copy())
            {
                hft::common::log_info(
                    "[RxConsumerFactory] Primary Receiver AF_XDP initialized in NATIVE HARDWARE ZERO-COPY MODE.");
            }
            else
            {
                hft::common::log_info(
                    "[RxConsumerFactory] Primary Receiver AF_XDP initialized in GENERIC SKB COPY MODE.");
            }
            return af_xdp;
        }
        else
        {
            hft::common::log_warn(
                "[RxConsumerFactory] Primary AF_XDP initialization failed. Triggering Fallback: PACKET_MMAP...");
        }
#else
        hft::common::log_info("[RxConsumerFactory] HFT_ENABLE_AF_XDP disabled at compile time. Using PACKET_MMAP...");
#endif

        return std::make_unique<PacketMmapRxConsumer>(interface_name, filter_port, queue, telemetry, cpu_pin);
    }

} // namespace hft::networking
