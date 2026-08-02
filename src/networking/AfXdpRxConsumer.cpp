/**
 * @file AfXdpRxConsumer.cpp
 * @brief Implementation of the high-performance AF_XDP Zero-Copy / SKB Mode receiver.
 */

#include "hft/networking/AfXdpRxConsumer.hpp"

#if defined(HFT_ENABLE_AF_XDP)

#include "hft/common/ConsoleLogger.hpp"
#include "hft/common/SystemOptimizations.hpp"

#include <cstring>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

namespace hft::networking
{
    using namespace std;

    AfXdpRxConsumer::AfXdpRxConsumer(string interface_name, uint16_t filter_port,
                                     hft::common::SPSCQueue<hft::common::FixMessagePacket, 8192> &queue,
                                     hft::monitoring::TelemetryCounters &telemetry, int cpu_pin)
        : m_interface_name(std::move(interface_name)), m_filter_port(filter_port), m_queue(queue),
          m_telemetry(telemetry), m_cpu_pin(cpu_pin)
    {
    }

    AfXdpRxConsumer::~AfXdpRxConsumer()
    {
        cleanup();
    }

    void AfXdpRxConsumer::cleanup() noexcept
    {
        if (m_xsk != nullptr)
        {
            xsk_socket__delete(m_xsk);
            m_xsk = nullptr;
        }
        if (m_umem != nullptr)
        {
            xsk_umem__delete(m_umem);
            m_umem = nullptr;
        }
        if (m_umem_buffer != nullptr)
        {
            munmap(m_umem_buffer, m_umem_size);
            m_umem_buffer = nullptr;
        }
        m_initialized = false;
    }

    bool AfXdpRxConsumer::init() noexcept
    {
        m_umem_size = NUM_FRAMES * FRAME_SIZE;

        // Allocate contiguous UMEM memory aligned to system page size
        m_umem_buffer =
            mmap(nullptr, m_umem_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);

        if (m_umem_buffer == MAP_FAILED)
        {
            hft::common::log_warn("[AfXdpRxConsumer] Error: Failed to allocate UMEM buffer via mmap.");
            m_umem_buffer = nullptr;
            return false;
        }

        struct xsk_umem_config umem_cfg{};
        memset(&umem_cfg, 0, sizeof(umem_cfg));
        umem_cfg.fill_size = NUM_FRAMES;
        umem_cfg.comp_size = NUM_FRAMES;
        umem_cfg.frame_size = FRAME_SIZE;
        umem_cfg.frame_headroom = XSK_UMEM__DEFAULT_FRAME_HEADROOM;
        umem_cfg.flags = 0;

        int ret = xsk_umem__create(&m_umem, m_umem_buffer, m_umem_size, &m_fq, &m_cq, &umem_cfg);
        if (ret != 0)
        {
            hft::common::log_warn("[AfXdpRxConsumer] Error: xsk_umem__create failed (code " + to_string(ret) + ").");
            cleanup();
            return false;
        }

        // Populate initial Fill Ring descriptors with UMEM frame offsets
        uint32_t fq_idx = 0;
        ret = xsk_ring_prod__reserve(&m_fq, NUM_FRAMES, &fq_idx);
        if (ret != static_cast<int>(NUM_FRAMES))
        {
            hft::common::log_warn("[AfXdpRxConsumer] Error: Failed to reserve Fill Ring frames.");
            cleanup();
            return false;
        }

        for (size_t i = 0; i < NUM_FRAMES; ++i)
        {
            *xsk_ring_prod__fill_addr(&m_fq, fq_idx + static_cast<uint32_t>(i)) = static_cast<uint64_t>(i * FRAME_SIZE);
        }
        xsk_ring_prod__submit(&m_fq, NUM_FRAMES);

        // Attempt XSK Socket creation - Try Native Zero-Copy first
        struct xsk_socket_config xsk_cfg{};
        memset(&xsk_cfg, 0, sizeof(xsk_cfg));
        xsk_cfg.rx_size = NUM_FRAMES;
        xsk_cfg.tx_size = NUM_FRAMES;
        xsk_cfg.libbpf_flags = 0;
        xsk_cfg.xdp_flags = XDP_FLAGS_DRV_MODE;
        xsk_cfg.bind_flags = XDP_ZEROCOPY;

        uint32_t queue_id = 0;
        ret = xsk_socket__create(&m_xsk, m_interface_name.c_str(), queue_id, m_umem, &m_rx, &m_tx, &xsk_cfg);

        if (ret == 0)
        {
            m_is_native = true;
            hft::common::log_info(
                "[AfXdpRxConsumer] AF_XDP initialized successfully in NATIVE HARDWARE ZERO-COPY MODE!");
        }
        else
        {
            hft::common::log_warn(
                "[AfXdpRxConsumer] Note: Native DRV Zero-Copy not supported by driver/NIC. Trying Generic SKB Mode...");
            xsk_cfg.xdp_flags = XDP_FLAGS_SKB_MODE;
            xsk_cfg.bind_flags = XDP_COPY;

            ret = xsk_socket__create(&m_xsk, m_interface_name.c_str(), queue_id, m_umem, &m_rx, &m_tx, &xsk_cfg);
            if (ret == 0)
            {
                m_is_native = false;
                hft::common::log_info("[AfXdpRxConsumer] AF_XDP initialized successfully in GENERIC SKB COPY MODE!");
            }
            else
            {
                hft::common::log_warn(
                    "[AfXdpRxConsumer] Warning: xsk_socket__create failed in Generic SKB Mode (code " + to_string(ret) +
                    "). AF_XDP unavailable.");
                cleanup();
                return false;
            }
        }

        m_initialized = true;
        return true;
    }

    void AfXdpRxConsumer::run()
    {
        if (m_cpu_pin >= 0)
        {
            hft::common::pin_thread_to_cpu(m_cpu_pin);
            hft::common::log_info("[AfXdpRxConsumer] Thread pinned to CPU core " + to_string(m_cpu_pin));
        }

        if (!m_initialized && !init())
        {
            hft::common::log_error("[AfXdpRxConsumer] Fatal Error: AF_XDP initialization failed.");
            hft::common::g_running.store(false, memory_order_release);
            return;
        }

        const uint16_t filter_port_nbo = htons(m_filter_port);
        const uint16_t eth_p_ip_nbo = htons(ETH_P_IP);

        uint32_t rx_idx = 0;
        uint32_t fq_idx = 0;
        size_t rx_count = 0;
        uint32_t spin_count = 0;
        uint32_t empty_checks = 0;
        constexpr uint32_t SPIN_LIMIT = 2'000'000;

        int xsk_fd = xsk_socket__fd(m_xsk);
        struct pollfd pfd{.fd = xsk_fd, .events = POLLIN, .revents = 0};

        hft::common::log_info("[AfXdpRxConsumer] Starting busy-polling AF_XDP extraction loop on port " +
                              to_string(m_filter_port) + "...");

        while (hft::common::g_running.load(memory_order_relaxed))
        {
            uint32_t rcvd = xsk_ring_cons__peek(&m_rx, BATCH_SIZE, &rx_idx);
            if (rcvd == 0)
            {
                if (hft::common::g_producer_done.load(memory_order_relaxed))
                {
                    this_thread::sleep_for(chrono::milliseconds(1));
                    ++empty_checks;
                    if (empty_checks >= 200)
                    {
                        hft::common::log_info(
                            "[AfXdpRxConsumer] Inactivity timeout after producer completed. Terminating.");
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

            spin_count = 0;
            empty_checks = 0;
            uint64_t rx_ts = hft::common::rdtsc();

            // Reserve slots in Fill Ring to recycle consumed UMEM frame buffers
            uint32_t reserved = 0;
            while (reserved < rcvd && hft::common::g_running.load(memory_order_relaxed))
            {
                reserved += xsk_ring_prod__reserve(&m_fq, rcvd - reserved, &fq_idx);
            }

            for (size_t i = 0; i < rcvd; ++i)
            {
                const struct xdp_desc *desc = xsk_ring_cons__rx_desc(&m_rx, rx_idx + static_cast<uint32_t>(i));
                uint64_t addr = desc->addr;
                uint32_t len = desc->len;

                auto *raw_frame = static_cast<uint8_t *>(m_umem_buffer) + addr;

                // Layer 2/3/4 Zero-Copy Header Stripping
                if (len >= sizeof(struct ethhdr) + sizeof(struct iphdr) + sizeof(struct udphdr))
                {
                    auto *eth = reinterpret_cast<struct ethhdr *>(raw_frame);
                    if (eth->h_proto == eth_p_ip_nbo)
                    {
                        auto *ip = reinterpret_cast<struct iphdr *>(raw_frame + sizeof(struct ethhdr));
                        if (ip->protocol == IPPROTO_UDP)
                        {
                            uint32_t ip_hdr_len = ip->ihl * 4;
                            auto *udp =
                                reinterpret_cast<struct udphdr *>(raw_frame + sizeof(struct ethhdr) + ip_hdr_len);

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
                                    pkt.ring_hdr = nullptr;

                                    while (!m_queue.push(pkt) && hft::common::g_running.load(memory_order_relaxed))
                                    {
                                        __asm__ __volatile__("pause");
                                    }
                                    ++rx_count;
                                    m_telemetry.rx.frames_captured.store(rx_count, memory_order_relaxed);
                                }
                            }
                        }
                    }
                }

                // Recycle UMEM frame offset back into Fill Ring
                if (reserved >= rcvd)
                {
                    *xsk_ring_prod__fill_addr(&m_fq, fq_idx + static_cast<uint32_t>(i)) = addr;
                }
            }

            xsk_ring_cons__release(&m_rx, rcvd);
            if (reserved >= rcvd)
            {
                xsk_ring_prod__submit(&m_fq, rcvd);
            }
        }

        hft::common::log_info("[AfXdpRxConsumer] Loop finished. Total frames captured: " + to_string(rx_count));
        cleanup();
        hft::common::g_consumer_done.store(true, memory_order_release);
    }

} // namespace hft::networking

#endif // HFT_ENABLE_AF_XDP
