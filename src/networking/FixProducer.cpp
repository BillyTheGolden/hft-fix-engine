/**
 * @file FixProducer.cpp
 * @brief Implementation of the UDP FIX packet generator and injector.
 */

#include "hft/networking/FixProducer.hpp"
#include "hft/common/ConsoleLogger.hpp"
#include "hft/common/SystemOptimizations.hpp"

#include <arpa/inet.h>
#include <format>
#include <fstream>
#include <net/if.h>
#include <sys/ioctl.h>

namespace hft::networking
{
    using namespace std;

    UdpFixProducer::UdpFixProducer(string interface_name, string target_ip, uint16_t target_port, size_t total_messages,
                                   string fix_file_path, int cpu_pin, FixMessagePacktQueue *direct_queue)
        : m_interface_name(std::move(interface_name)), m_target_ip(std::move(target_ip)), m_target_port(target_port),
          m_total_messages(total_messages), m_fix_file_path(std::move(fix_file_path)), m_direct_queue(direct_queue),
          m_cpu_pin(cpu_pin)
    {
        if (!m_fix_file_path.empty())
        {
            ifstream file(m_fix_file_path, ios::binary);
            if (file.is_open())
            {
                char header[8] = {0};
                file.read(header, sizeof(header));
                string_view head_view(header, static_cast<size_t>(file.gcount()));

                bool is_fix_ascii = head_view.starts_with("8=FIX") || head_view.starts_with("8=");
                bool is_binary = !is_fix_ascii && (string_view(m_fix_file_path).ends_with(".data") ||
                                                   (head_view.length() > 0 && head_view[0] == 'O'));

                file.seekg(0, ios::beg);
                if (is_binary)
                {
                    file.seekg(0, ios::end);
                    size_t file_size = static_cast<size_t>(file.tellg());
                    file.seekg(0, ios::beg);

                    // OPTIMIZATION (High Finding 9.1): Read entire dataset into 1 contiguous buffer
                    m_raw_file_data.resize(file_size);
                    file.read(m_raw_file_data.data(), static_cast<streamsize>(file_size));

                    // Auto-detect packet size (46 bytes OUCH or 44 bytes SBE)
                    size_t pkt_size = (file_size % 46 == 0) ? 46 : 44;
                    size_t count = file_size / pkt_size;
                    m_loaded_messages.reserve(count);

                    for (size_t offset = 0; offset + pkt_size <= file_size; offset += pkt_size)
                    {
                        m_loaded_messages.emplace_back(m_raw_file_data.data() + offset, pkt_size);
                    }
                }
                else
                {
                    file.seekg(0, ios::end);
                    size_t file_size = static_cast<size_t>(file.tellg());
                    file.seekg(0, ios::beg);

                    m_raw_file_data.resize(file_size);
                    file.read(m_raw_file_data.data(), static_cast<streamsize>(file_size));

                    string_view file_view(m_raw_file_data);
                    size_t start = 0;
                    while (start < file_view.length())
                    {
                        size_t end = file_view.find_first_of("\r\n", start);
                        if (end == string_view::npos)
                            end = file_view.length();
                        if (end > start)
                        {
                            m_loaded_messages.emplace_back(m_raw_file_data.data() + start, end - start);
                        }
                        start = file_view.find_first_not_of("\r\n", end);
                    }
                }
                hft::common::log_info("[FixProducer] Loaded " + to_string(m_loaded_messages.size()) +
                                      " pre-generated trade messages into memory queue from '" + m_fix_file_path +
                                      "'.");
            }
            else
            {
                hft::common::log_error("[FixProducer] Error: Could not open trade message file '" + m_fix_file_path +
                                       "'");
            }
        }
    }

    void UdpFixProducer::run()
    {
        if (m_cpu_pin >= 0)
        {
            hft::common::pin_thread_to_cpu(m_cpu_pin);
            hft::common::log_info("[FixProducer] Thread pinned to CPU core " + std::to_string(m_cpu_pin));
        }

        if (m_direct_queue != nullptr)
        {
            hft::common::log_info("[Producer] Direct In-Memory Queue Mode active (0% packet loss guaranteed). Pushing "
                                  "packets directly to SPSC queue...");
            size_t injected = 0;
            for (const auto &msg : m_loaded_messages)
            {
                if (!hft::common::g_running.load(memory_order_relaxed))
                    break;

                hft::common::FixMessagePacket pkt{};
                pkt.payload_len = static_cast<uint16_t>(std::min(msg.size(), hft::common::MAX_PAYLOAD_LEN));
                pkt.payload = const_cast<char *>(msg.data());
                pkt.rx_timestamp_cycles = hft::common::rdtsc();
                pkt.ring_hdr = nullptr;

                while (!m_direct_queue->push(pkt) && hft::common::g_running.load(memory_order_relaxed))
                {
                    std::this_thread::yield();
                }
                ++injected;
            }

            hft::common::log_info("[Producer] Queue injection complete (" + to_string(injected) +
                                  " injected). Producer thread terminating.");
            hft::common::g_producer_done.store(true, memory_order_release);
            return;
        }

        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0)
        {
            hft::common::log_error("[FixProducer] Fatal Error: socket(AF_INET, SOCK_DGRAM) failed.");
            hft::common::g_producer_done.store(true, memory_order_release);
            return;
        }

        // Set 32MB send buffer to prevent UDP socket drop
        int sndbuf = 32 * 1024 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

        // 1. Enable Multicast TTL and Loopback
        int ttl = 1;
        if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0)
        {
            hft::common::log_warn("[FixProducer] Warning: Failed to set IP_MULTICAST_TTL option.");
        }

        // 2. Bind socket specifically to target interface via SO_BINDTODEVICE & IP_MULTICAST_IF
        std::string bind_iface = m_interface_name;
        if (!bind_iface.empty())
        {
            if (setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, bind_iface.c_str(),
                           static_cast<socklen_t>(bind_iface.length())) < 0)
            {
                hft::common::log_warn("[FixProducer] Warning: Failed to bind UDP producer to device '" + bind_iface +
                                      "'. Ensure root/sudo privileges.");
            }

            struct ifreq ifr{};
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, bind_iface.c_str(), IFNAMSIZ - 1);
            if (ioctl(sock, SIOCGIFADDR, &ifr) == 0)
            {
                struct sockaddr_in *sa = reinterpret_cast<struct sockaddr_in *>(&ifr.ifr_addr);
                setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &sa->sin_addr, sizeof(sa->sin_addr));
            }
        }

        struct sockaddr_in target_addr{};
        memset(&target_addr, 0, sizeof(target_addr));
        target_addr.sin_family = AF_INET;
        target_addr.sin_port = htons(m_target_port);
        inet_pton(AF_INET, m_target_ip.c_str(), &target_addr.sin_addr);

        // Warm up wait allowing kernel ring to bind
        this_thread::sleep_for(chrono::seconds(1));

        if (!m_loaded_messages.empty())
        {
            hft::common::log_info("[FixProducer] Starting injection of " + to_string(m_loaded_messages.size()) +
                                  " loaded trade messages from queue to " + m_target_ip + ":" +
                                  to_string(m_target_port) + "...");

            size_t injected = 0;
            for (const auto &fix_msg : m_loaded_messages)
            {
                if (!hft::common::g_running.load(memory_order_relaxed))
                    break;

                ssize_t sent = sendto(sock, fix_msg.data(), fix_msg.size(), 0,
                                      reinterpret_cast<struct sockaddr *>(&target_addr), sizeof(target_addr));
                if (sent < 0)
                {
                    hft::common::log_warn("[FixProducer] Warning: sendto() failed for loaded queue item #" +
                                          to_string(injected + 1));
                }
                ++injected;

                // Adaptive micro-pacing every 25 packets (10us) to prevent OS socket buffer overflow
                if (injected % 25 == 0)
                {
                    this_thread::sleep_for(chrono::microseconds(10));
                }
            }
            hft::common::log_info("[FixProducer] Queue injection complete (" + to_string(injected) +
                                  " injected). Producer thread terminating.");
        }
        else
        {
            hft::common::log_info("[FixProducer] Starting injection of " + to_string(m_total_messages) +
                                  " synthetic FIX messages to " + m_target_ip + ":" + to_string(m_target_port) + "...");

            for (size_t i = 1; i <= m_total_messages && hft::common::g_running.load(memory_order_relaxed); ++i)
            {
                char fix_msg[512];
                auto res = format_to_n(fix_msg, sizeof(fix_msg) - 1,
                                       "8=FIX.4.2\x01"
                                       "9=95\x01"
                                       "35=D\x01"
                                       "49=PRODUCER\x01"
                                       "56=ENGINE\x01"
                                       "34={}\x01"
                                       "52=20260717-18:00:00.000\x01"
                                       "11=ORD_{}\x01"
                                       "55=PETR4\x01"
                                       "54=1\x01"
                                       "38=1000\x01"
                                       "40=2\x01"
                                       "44=35.85\x01"
                                       "10=128\x01",
                                       i, i);
                size_t formatted_size = static_cast<size_t>(res.size);
                size_t len = (formatted_size < sizeof(fix_msg)) ? formatted_size : sizeof(fix_msg) - 1;
                fix_msg[len] = '\0';

                ssize_t sent = sendto(sock, fix_msg, static_cast<size_t>(len), 0,
                                      reinterpret_cast<struct sockaddr *>(&target_addr), sizeof(target_addr));
                if (sent < 0)
                {
                    hft::common::log_warn("[FixProducer] Warning: sendto() failed for sequence " + to_string(i));
                }

                // Simulate realistic burst pacing every 100 orders
                if (i % 100 == 0)
                {
                    this_thread::sleep_for(chrono::microseconds(50));
                }
            }
            hft::common::log_info("[FixProducer] Synthetic injection completed successfully.");
        }

        close(sock);
        hft::common::g_producer_done.store(true, memory_order_release);
    }

} // namespace hft::networking
