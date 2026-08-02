/**
 * @file RxRingConsumer.cpp
 * @brief Implementation of the zero-copy PACKET_MMAP receiver.
 */

#include "hft/networking/RxRingConsumer.hpp"
#include "hft/common/ConsoleLogger.hpp"
#include "hft/common/SystemOptimizations.hpp"

#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <poll.h>

namespace hft::networking
{
    using namespace std;

    PacketMmapRxConsumer::PacketMmapRxConsumer(string interface_name, uint16_t filter_port,
                                               hft::common::SPSCQueue<hft::common::FixMessagePacket, 8192> &queue,
                                               hft::monitoring::TelemetryCounters &telemetry, int cpu_pin)
        : m_interface_name(std::move(interface_name)), m_filter_port(filter_port), m_queue(queue),
          m_telemetry(telemetry), m_cpu_pin(cpu_pin)
    {
    }

    PacketMmapRxConsumer::~PacketMmapRxConsumer()
    {
        cleanup();
    }

    void PacketMmapRxConsumer::cleanup() noexcept
    {
        if (m_mapped_buffer != nullptr && m_mapped_buffer != MAP_FAILED)
        {
            munmap(m_mapped_buffer, m_total_ring_size);
            m_mapped_buffer = nullptr;
        }
        if (m_sockfd >= 0)
        {
            close(m_sockfd);
            m_sockfd = -1;
        }
    }

    void PacketMmapRxConsumer::run()
    {
        const uint16_t filter_port_nbo = htons(m_filter_port);
        const uint16_t eth_p_ip_nbo = htons(ETH_P_IP);

        // Apply the full real-time thread configuration as the very first action of this thread.
        //
        // The consumer thread sits at the head of the packet ingestion pipeline: every nanosecond
        // of scheduling jitter here directly delays all downstream message processing. Under CFS
        // the kernel can preempt this thread for up to 4 ms — causing the SPSC queue to drain
        // completely and stalling the worker thread for the duration of the preemption.
        //
        // apply_realtime_thread_settings() applies in order:
        //   1. pin_thread_to_cpu(m_cpu_pin)    — dedicate a core; no TLB shootdown or cache eviction
        //   2. set_realtime_priority(80)        — SCHED_FIFO: OS will not interrupt this thread
        //   3. pthread_setname_np("hft_rx_ring") — kernel-visible thread name for perf/strace/htop
        hft::common::apply_realtime_thread_settings(m_cpu_pin, 80, "hft_rx_ring");

        bool is_udp_fallback = false;
        m_sockfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
        if (m_sockfd < 0)
        {
            hft::common::log_warn("[RxRingConsumer] Note: socket(AF_PACKET) requires ROOT. Falling back to standard "
                                  "AF_INET UDP socket...");
            m_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
            if (m_sockfd < 0)
            {
                hft::common::log_error("[RxRingConsumer] Fatal Error: socket(AF_INET) failed.");
                hft::common::g_running.store(false, memory_order_release);
                return;
            }

            int reuse = 1;
            setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

            struct sockaddr_in sin{};
            sin.sin_family = AF_INET;
            sin.sin_port = filter_port_nbo;
            sin.sin_addr.s_addr = INADDR_ANY;

            int rcvbuf = 33554432; // 32 MB socket receive buffer
            setsockopt(m_sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

            if (bind(m_sockfd, reinterpret_cast<struct sockaddr *>(&sin), sizeof(sin)) < 0)
            {
                hft::common::log_error("[RxRingConsumer] Fatal Error: UDP socket bind() failed.");
                cleanup();
                hft::common::g_running.store(false, memory_order_release);
                return;
            }

            is_udp_fallback = true;
            hft::common::log_info("[RxRingConsumer] Standard UDP Consumer bound to port " +
                                  std::to_string(m_filter_port));
        }

        if (!is_udp_fallback)
        {
            auto if_index = if_nametoindex(m_interface_name.c_str());
            if (if_index == 0)
            {
                hft::common::log_error("[RxRingConsumer] Fatal Error: Network interface '" + m_interface_name +
                                       "' not found.");
                cleanup();
                hft::common::g_running.store(false, memory_order_release);
                return;
            }

            struct sockaddr_ll sll{};
            memset(&sll, 0, sizeof(sll));
            sll.sll_family = AF_PACKET;
            sll.sll_protocol = htons(ETH_P_ALL);
            sll.sll_ifindex = static_cast<int>(if_index);

            if (bind(m_sockfd, reinterpret_cast<struct sockaddr *>(&sll), sizeof(sll)) < 0)
            {
                hft::common::log_error("[RxRingConsumer] Fatal Error: bind() to interface failed.");
                cleanup();
                hft::common::g_running.store(false, memory_order_release);
                return;
            }

            struct packet_mreq mr{};
            memset(&mr, 0, sizeof(mr));
            mr.mr_ifindex = static_cast<int>(if_index);
            mr.mr_type = PACKET_MR_ALLMULTI;
            if (setsockopt(m_sockfd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof(mr)) < 0)
            {
                hft::common::log_warn("[RxRingConsumer] Warning: Failed to set PACKET_MR_ALLMULTI.");
            }

            int busy_poll_us = 50;
            if (setsockopt(m_sockfd, SOL_SOCKET, SO_BUSY_POLL, &busy_poll_us, sizeof(busy_poll_us)) < 0)
            {
                hft::common::log_warn("[RxRingConsumer] Note: SO_BUSY_POLL not enabled.");
            }

            int version = TPACKET_V2;
            if (setsockopt(m_sockfd, SOL_PACKET, PACKET_VERSION, &version, sizeof(version)) < 0)
            {
                hft::common::log_error("[RxRingConsumer] Fatal Error: setsockopt(PACKET_VERSION) failed.");
                cleanup();
                hft::common::g_running.store(false, memory_order_release);
                return;
            }

            struct tpacket_req req{};
            memset(&req, 0, sizeof(req));
            req.tp_block_size = static_cast<unsigned int>(hft::common::BLOCK_SIZE);
            req.tp_block_nr = static_cast<unsigned int>(hft::common::BLOCK_NR);
            req.tp_frame_size = static_cast<unsigned int>(hft::common::FRAME_SIZE);
            req.tp_frame_nr = static_cast<unsigned int>(hft::common::FRAME_NR);

            if (setsockopt(m_sockfd, SOL_PACKET, PACKET_RX_RING, &req, sizeof(req)) < 0)
            {
                hft::common::log_error("[RxRingConsumer] Fatal Error: setsockopt(PACKET_RX_RING) failed.");
                cleanup();
                hft::common::g_running.store(false, memory_order_release);
                return;
            }

            m_total_ring_size = static_cast<size_t>(req.tp_block_size) * req.tp_block_nr;
            m_mapped_buffer = static_cast<uint8_t *>(
                mmap(nullptr, m_total_ring_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_LOCKED, m_sockfd, 0));
            if (m_mapped_buffer == MAP_FAILED)
            {
                hft::common::log_error("[RxRingConsumer] Fatal Error: mmap() of kernel ring failed.");
                cleanup();
                hft::common::g_running.store(false, memory_order_release);
                return;
            }

            hft::common::log_info(
                "[RxRingConsumer] Successfully mapped " + std::to_string(m_total_ring_size / (1024 * 1024)) +
                " MB kernel ring buffer via DMA Zero-Copy! Monitoring UDP port " + std::to_string(m_filter_port));
        }

        struct pollfd pfd{.fd = m_sockfd, .events = POLLIN, .revents = 0};

        uint32_t frame_idx = 0;
        size_t rx_count = 0;
        uint32_t spin_count = 0;
        uint32_t empty_checks = 0;
        constexpr uint32_t SPIN_LIMIT = 2'000'000;

        while (hft::common::g_running.load(memory_order_relaxed))
        {
            if (is_udp_fallback || m_mapped_buffer == nullptr)
            {
                static char udp_ring[8192][512];
                static uint32_t udp_slot = 0;
                char *current_buf = udp_ring[udp_slot];
                ssize_t n = recv(m_sockfd, current_buf, 512, MSG_DONTWAIT);
                if (n > 0)
                {
                    uint64_t rx_ts = hft::common::rdtsc();
                    hft::common::FixMessagePacket pkt;
                    pkt.rx_timestamp_cycles = rx_ts;
                    pkt.payload_len = static_cast<uint16_t>(n);
                    pkt.payload = current_buf;
                    pkt.ring_hdr = nullptr;

                    while (!m_queue.push(pkt) && hft::common::g_running.load(memory_order_relaxed))
                    {
                        __asm__ __volatile__("pause");
                    }
                    udp_slot = (udp_slot + 1) % 8192;
                    ++rx_count;
                    m_telemetry.rx.frames_captured.store(rx_count, memory_order_relaxed);
                }
                else if (hft::common::g_producer_done.load(memory_order_relaxed))
                {
                    this_thread::sleep_for(chrono::milliseconds(1));
                    ++empty_checks;
                    if (empty_checks >= 2000)
                        break;
                }
                continue;
            }

            auto *hdr =
                reinterpret_cast<struct tpacket2_hdr *>(m_mapped_buffer + (frame_idx * hft::common::FRAME_SIZE));

            if ((hdr->tp_status & TP_STATUS_USER) == 0)
            {
                if (hft::common::g_producer_done.load(memory_order_relaxed))
                {
                    this_thread::sleep_for(chrono::milliseconds(1));
                    ++empty_checks;
                    if (empty_checks >= 100)
                    {
                        hft::common::log_info(
                            "[RxRingConsumer] Inactivity timeout after producer completed. Terminating consumer.");
                        break;
                    }
                }
                else
                {
                    if (spin_count < SPIN_LIMIT)
                    {
                        __asm__ __volatile__("pause");
                        ++spin_count;
                    }
                    else if (spin_count < SPIN_LIMIT + 1000)
                    {
                        this_thread::yield();
                        ++spin_count;
                    }
                    else
                    {
                        poll(&pfd, 1, 1);
                    }
                }
                continue;
            }

            spin_count = 0; // Reset backoff counter when a packet is ready
            empty_checks = 0;
            uint64_t rx_ts = hft::common::rdtsc();
            uint8_t *raw_frame = reinterpret_cast<uint8_t *>(hdr) + hdr->tp_mac;
            bool pushed = false;

            // Manual Layer-2/3/4 header stripping
            auto *eth = reinterpret_cast<struct ethhdr *>(raw_frame);
            if (eth->h_proto == eth_p_ip_nbo)
            {
                auto *ip = reinterpret_cast<struct iphdr *>(raw_frame + sizeof(struct ethhdr));
                if (ip->protocol == IPPROTO_UDP)
                {
                    uint32_t ip_hdr_len = ip->ihl * 4;
                    auto *udp = reinterpret_cast<struct udphdr *>(raw_frame + sizeof(struct ethhdr) + ip_hdr_len);

                    if (udp->dest == filter_port_nbo)
                    {
                        uint8_t *payload = reinterpret_cast<uint8_t *>(udp) + sizeof(struct udphdr);
                        uint16_t payload_len = static_cast<uint16_t>(ntohs(udp->len) - sizeof(struct udphdr));

                        if (payload_len > 0 && payload_len <= hft::common::MAX_PAYLOAD_LEN)
                        {
                            hft::common::FixMessagePacket pkt;
                            pkt.rx_timestamp_cycles = rx_ts;
                            pkt.payload_len = payload_len;
                            pkt.payload = reinterpret_cast<char *>(payload);
                            pkt.ring_hdr = hdr;

                            // Push zero-allocation packet to SPSC queue
                            while (!m_queue.push(pkt) && hft::common::g_running.load(memory_order_relaxed))
                            {
                                __asm__ __volatile__("pause");
                            }
                            ++rx_count;
                            m_telemetry.rx.frames_captured.store(rx_count, memory_order_relaxed);
                            pushed = true;
                        }
                    }
                }
            }

            // Return frame memory ownership back to kernel Ring DMA if not delegated to the worker
            if (!pushed)
            {
                hdr->tp_status = TP_STATUS_KERNEL;
            }
            // OPTIMIZATION (High Finding 8.2): Bitwise AND for power-of-two ring index wrapping (1 cycle vs 20-40
            // cycles fmod)
            frame_idx = (frame_idx + 1) & (hft::common::FRAME_NR - 1);
        }

        hft::common::log_info("[RxRingConsumer] Loop finished. Total frames captured and queued: " +
                              std::to_string(rx_count));
        cleanup();
        hft::common::g_consumer_done.store(true, memory_order_release);
    }

} // namespace hft::networking
