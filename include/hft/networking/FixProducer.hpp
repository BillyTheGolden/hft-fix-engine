/**
 * @file FixProducer.hpp
 * @brief UDP-based FIX market data and order injector thread.
 */

#pragma once

#include "hft/common/SPSCQueue.hpp"
#include "hft/common/Types.hpp"

#include <cstddef>
#include <cstdint>
#include <queue>
#include <string>
#include <string_view>
#include <vector>

namespace hft::networking
{
    using FixMessagePacktQueue = hft::common::SPSCQueue<hft::common::FixMessagePacket, 8192>;

    /**
     * @class IFixProducer
     * @brief Abstract interface representing a producer that injects financial messages into the network.
     * @details Adheres to the Dependency Inversion Principle (DIP) and Interface Segregation Principle (ISP).
     */
    class IFixProducer
    {
      public:
        virtual ~IFixProducer() = default;

        /**
         * @brief Starts the packet injection loop in the calling thread.
         */
        virtual void run() = 0;
    };

    /**
     * @class UdpFixProducer
     * @brief Concrete implementation that constructs or loads FIX 4.2 messages and sends them over UDP.
     */
    class UdpFixProducer final : public IFixProducer
    {
      public:
        UdpFixProducer(std::string interface_name, std::string target_ip, uint16_t target_port, size_t total_messages,
                       std::string fix_file_path = "", int cpu_pin = -1, FixMessagePacktQueue* direct_queue = nullptr);

        ~UdpFixProducer() override = default;

        /**
         * @brief Executes the generation/loading and UDP `sendto` loop until queue is empty.
         */
        void run() override;

      private:
        std::string m_interface_name;
        std::string m_target_ip;
        uint16_t m_target_port;
        size_t m_total_messages;
        std::string m_fix_file_path;
        FixMessagePacktQueue* m_direct_queue;
        int m_cpu_pin;
        std::vector<std::string> m_loaded_messages;
    };
} // namespace hft::networking
