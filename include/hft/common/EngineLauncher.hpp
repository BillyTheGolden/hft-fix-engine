/**
 * @file EngineLauncher.hpp
 * @brief Unified engine orchestration launcher for low-latency HFT workers.
 * @details Encapsulates CLI argument parsing, network interface auto-pairing,
 *          HugePage allocation, real-time CPU affinity assignment, and thread lifecycle.
 */

#pragma once

#include "hft/common/ConsoleLogger.hpp"
#include "hft/common/SPSCQueue.hpp"
#include "hft/common/SystemOptimizations.hpp"
#include "hft/common/Types.hpp"
#include "hft/monitoring/PerformanceMonitor.hpp"
#include "hft/monitoring/Telemetry.hpp"
#include "hft/networking/FixProducer.hpp"
#include "hft/networking/RxConsumerFactory.hpp"
#include "hft/protocol/ProtocolParser.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace hft::common
{
    /**
     * @brief Context passed to worker factory callbacks during engine initialization.
     */
    struct EngineContext
    {
        SPSCQueue<FixMessagePacket, 8192> &queue;
        monitoring::TelemetryCounters &telemetry;
        protocol::ProtocolType protocol{protocol::ProtocolType::FIX};
        int worker_cpu{-1};
        std::string log_filename;
        size_t total_messages{0};
    };

    class EngineLauncher
    {
      private:
        static void handle_signal(int signum) noexcept
        {
            if (signum == SIGINT || signum == SIGTERM)
            {
                g_user_stopped.store(true, std::memory_order_release);
                g_running.store(false, std::memory_order_release);
            }
        }

        static void print_usage(const char *prog_name, const char *banner_title)
        {
            std::cerr << "====================================================\n"
                      << "  " << banner_title << "\n"
                      << "====================================================\n"
                      << "Usage:\n"
                      << "  " << prog_name
                      << " <rx_interface> [tx_interface] <port> <source_file_or_count> [options]\n\n"
                      << "Positional Arguments:\n"
                      << "  <rx_interface>          Receiver NIC to bind (e.g., 'veth1', 'lo', 'enp4s0')\n"
                      << "  [tx_interface]          Optional Transmitter NIC for producer (e.g., 'veth0')\n"
                      << "  <port>                  UDP port number (e.g., 8888)\n"
                      << "  <source_file_or_count>  Input message file path (.data or .txt) or message count\n\n"
                      << "Options:\n"
                      << "  --help, -h              Display this usage help message and exit\n"
                      << "  --rx-iface=<nic>        Explicit Receiver NIC for AF_XDP / socket consumer\n"
                      << "  --tx-iface=<nic>        Explicit Transmitter NIC for UDP producer\n"
                      << "  --direct-queue          Enable direct in-memory SPSC queue injection (0% loss)\n"
                      << "  --pin-cores             Enable real-time thread core pinning and affinity\n"
                      << "  --protocol=<type>       Explicit protocol override: FIX | OUCH | SBE\n"
                      << "  --queue-id=<id>         AF_XDP hardware queue ID (default 0)\n\n"
                      << "Examples:\n"
                      << "  " << prog_name << " veth1 veth0 8888 ./fix_messages_1m.data --pin-cores\n"
                      << "  " << prog_name << " lo 8888 sbe_messages_500k.data --direct-queue\n"
                      << "====================================================\n";
        }

        static bool is_binary_dataset(const std::string &file_path)
        {
            if (file_path.empty() || !std::filesystem::exists(file_path))
                return false;
            std::ifstream file(file_path, std::ios::binary);
            if (!file.is_open())
                return false;

            char header[8] = {0};
            file.read(header, sizeof(header));
            std::string_view head_view(header, static_cast<size_t>(file.gcount()));

            if (head_view.starts_with("8=FIX") || head_view.starts_with("8="))
                return false;
            return file_path.ends_with(".data") || (head_view.length() > 0 && head_view[0] == 'O');
        }

      public:
        template <typename WorkerFactory, typename PostRunHook = std::nullptr_t>
        static int run(const char *title, int argc, char *argv[], WorkerFactory &&factory,
                       PostRunHook &&post_hook = nullptr)
        {
            for (int i = 1; i < argc; ++i)
            {
                std::string arg = argv[i];
                if (arg == "--help" || arg == "-h")
                {
                    print_usage(argv[0], title);
                    return EXIT_SUCCESS;
                }
            }

            if (argc < 4)
            {
                std::cerr << "[Error] Missing required command line arguments.\n\n";
                print_usage(argv[0], title);
                return EXIT_FAILURE;
            }

            g_running.store(true, std::memory_order_relaxed);
            g_producer_done.store(false, std::memory_order_relaxed);
            g_consumer_done.store(false, std::memory_order_relaxed);
            g_user_stopped.store(false, std::memory_order_relaxed);

            std::signal(SIGINT, handle_signal);
            std::signal(SIGTERM, handle_signal);

            std::string rx_interface;
            std::string tx_interface;
            uint16_t udp_port = 0;
            std::string source_param;
            bool direct_queue_mode = false;
            bool enable_core_pinning = false;
            uint32_t queue_id = 0;
            std::string explicit_protocol_arg;

            std::vector<std::string> positional_args;
            for (int i = 1; i < argc; ++i)
            {
                std::string arg = argv[i];
                if (arg == "--direct-queue")
                    direct_queue_mode = true;
                else if (arg == "--pin-cores")
                    enable_core_pinning = true;
                else if (arg.starts_with("--rx-iface="))
                    rx_interface = arg.substr(11);
                else if (arg.starts_with("--tx-iface="))
                    tx_interface = arg.substr(11);
                else if (arg.starts_with("--queue-id="))
                    queue_id = static_cast<uint32_t>(std::stoul(arg.substr(11)));
                else if (arg.starts_with("--protocol="))
                    explicit_protocol_arg = arg.substr(11);
                else if (!arg.starts_with("--"))
                    positional_args.push_back(arg);
            }

            size_t pos_idx = 0;
            if (rx_interface.empty() && pos_idx < positional_args.size())
                rx_interface = positional_args[pos_idx++];

            if (tx_interface.empty())
            {
                if (positional_args.size() >= 4 && pos_idx < positional_args.size())
                {
                    try
                    {
                        (void)std::stoul(positional_args[pos_idx]);
                    }
                    catch (...)
                    {
                        tx_interface = positional_args[pos_idx++];
                    }
                }
            }

            if (pos_idx < positional_args.size())
                udp_port = static_cast<uint16_t>(std::stoul(positional_args[pos_idx++]));
            if (pos_idx < positional_args.size())
                source_param = positional_args[pos_idx++];

            // Fallback pairing for veth interfaces
            if (tx_interface.empty())
            {
                if (rx_interface == "veth0")
                    tx_interface = "veth1";
                else if (rx_interface == "veth1")
                    tx_interface = "veth0";
                else
                    tx_interface = rx_interface;
            }

            size_t total_messages = 0;
            std::string fix_file_path;
            if (std::filesystem::exists(source_param))
            {
                fix_file_path = source_param;
                if (is_binary_dataset(fix_file_path))
                {
                    size_t file_size = std::filesystem::file_size(fix_file_path);
                    total_messages = (file_size % 46 == 0) ? (file_size / 46) : (file_size / 44);
                }
                else
                {
                    std::ifstream f(fix_file_path);
                    std::string line;
                    while (std::getline(f, line))
                    {
                        if (!line.empty())
                            ++total_messages;
                    }
                }
            }
            else
            {
                try
                {
                    total_messages = static_cast<size_t>(std::stoul(source_param));
                }
                catch (...)
                {
                    total_messages = 50000;
                }
            }

            protocol::ProtocolType protocol_type = protocol::parse_protocol_type(source_param);
            if (!explicit_protocol_arg.empty())
            {
                protocol_type = protocol::parse_protocol_type(explicit_protocol_arg);
            }
            std::string protocol_name = protocol::protocol_type_to_string(protocol_type);
            std::string proto_prefix = "fix";
            if (protocol_type == protocol::ProtocolType::OUCH)
                proto_prefix = "ouch";
            else if (protocol_type == protocol::ProtocolType::SBE)
                proto_prefix = "sbe";

            std::string log_filename = std::format("{}_engine.log", proto_prefix);
            std::string csv_filename = std::format("{}_metrics_time_series.csv", proto_prefix);
            const std::string target_ip = (tx_interface == "lo") ? "127.0.0.1" : "239.255.0.1";

            g_cycles_per_ns = calibrate_rdtsc();
            lock_process_memory();

            // HugePage SPSC queue and telemetry counters
            auto *shared_queue = allocate_on_huge_pages<SPSCQueue<FixMessagePacket, 8192>>();
            auto *shared_telemetry = allocate_on_huge_pages<monitoring::TelemetryCounters>();

            if (shared_queue == nullptr || shared_telemetry == nullptr)
            {
                std::cerr << "[EngineLauncher] Fatal Error: Failed to allocate HugePage memory structures.\n";
                return EXIT_FAILURE;
            }

            ConsoleLogger::getInstance().initialize(*shared_telemetry, total_messages);

            log_info("====================================================");
            log_info(std::string("  ") + title);
            log_info(
                std::format("Rx Interface: {} | Tx Interface: {} | Port: {}", rx_interface, tx_interface, udp_port));
            log_info(std::format("Protocol  : {}", protocol_name));
            log_info(std::format("Source    : {} ({} msgs)", source_param, total_messages));
            log_info(std::format("Execution : {}",
                                 direct_queue_mode ? "Direct In-Memory Queue (0% Loss)" : "Network Ring Socket"));
            log_info(std::format("CSV File  : {}", csv_filename));
            log_info("====================================================");

            int cpu_count = get_cpu_count();
            int worker_cpu = (enable_core_pinning && cpu_count >= 2) ? 1 : -1;
            int consumer_cpu = (enable_core_pinning && cpu_count >= 3) ? 2 : -1;
            int producer_cpu = (enable_core_pinning && cpu_count >= 4) ? 3 : -1;
            int monitor_cpu = (enable_core_pinning && cpu_count >= 5) ? 4 : -1;

            EngineContext ctx{*shared_queue, *shared_telemetry, protocol_type,
                              worker_cpu,    log_filename,      total_messages};

            auto worker = factory(ctx);
            auto consumer = networking::create_rx_consumer(rx_interface, udp_port, *shared_queue, *shared_telemetry,
                                                           consumer_cpu, queue_id);
            networking::UdpFixProducer producer(tx_interface, target_ip, udp_port, total_messages, fix_file_path,
                                                producer_cpu, direct_queue_mode ? shared_queue : nullptr);
            monitoring::CsvPerformanceMonitor monitor(*shared_telemetry, *shared_queue, csv_filename, 10, monitor_cpu);

            auto start_wall_time = std::chrono::high_resolution_clock::now();

            std::thread t_monitor([&monitor]() { monitor.run(); });
            std::thread t_worker([&worker]() { worker->run(); });
            std::thread t_consumer([&consumer]() { consumer->run(); });
            std::thread t_producer([&producer]() { producer.run(); });

            if (t_producer.joinable())
                t_producer.join();
            g_consumer_done.store(true, std::memory_order_release);
            if (t_consumer.joinable())
                t_consumer.join();
            if (t_worker.joinable())
                t_worker.join();

            g_running.store(false, std::memory_order_release);
            if (t_monitor.joinable())
                t_monitor.join();

            auto end_wall_time = std::chrono::high_resolution_clock::now();
            double duration_sec =
                std::chrono::duration<double, std::chrono::seconds::period>(end_wall_time - start_wall_time).count();

            if constexpr (!std::is_null_pointer_v<std::decay_t<PostRunHook>>)
            {
                post_hook(*worker);
            }

            uint64_t final_rx = shared_telemetry->rx.frames_captured.load(std::memory_order_relaxed);
            uint64_t final_proc = shared_telemetry->worker.messages_processed.load(std::memory_order_relaxed);
            uint64_t final_lat_ns = shared_telemetry->worker.total_latency_ns.load(std::memory_order_relaxed);
            uint64_t final_min_ns = shared_telemetry->worker.min_latency_ns.load(std::memory_order_relaxed);
            uint64_t final_max_ns = shared_telemetry->worker.max_latency_ns.load(std::memory_order_relaxed);
            uint64_t final_approved = shared_telemetry->worker.risk_approved.load(std::memory_order_relaxed);
            uint64_t final_rejected = shared_telemetry->worker.risk_rejected.load(std::memory_order_relaxed);

            double avg_lat_ns =
                (final_proc > 0) ? (static_cast<double>(final_lat_ns) / static_cast<double>(final_proc)) : 0.0;
            double min_lat_ns = (final_min_ns == UINT64_MAX) ? 0.0 : static_cast<double>(final_min_ns);
            double max_lat_ns = static_cast<double>(final_max_ns);
            double throughput = (duration_sec > 0.0) ? (static_cast<double>(final_proc) / duration_sec) : 0.0;

            std::cout << "\n====================================================\n";
            std::cout << "           ENGINE RUN SUMMARY METRICS               \n";
            std::cout << "====================================================\n";
            std::cout << std::format("  Total Packets Ingested : {}\n", final_rx);
            std::cout << std::format("  Messages Processed     : {}\n", final_proc);
            std::cout << std::format("  Orders Risk Approved   : {}\n", final_approved);
            std::cout << std::format("  Orders Risk Rejected   : {}\n", final_rejected);
            std::cout << std::format("  Effective Throughput   : {:.2f} msgs/sec\n", throughput);
            std::cout << std::format("  Average Latency        : {:.2f} ns ({:.4f} us)\n", avg_lat_ns,
                                     avg_lat_ns / 1000.0);
            std::cout << std::format("  Min Latency            : {:.2f} ns\n", min_lat_ns);
            std::cout << std::format("  Max Latency (Tail)     : {:.2f} ns ({:.4f} us)\n", max_lat_ns,
                                     max_lat_ns / 1000.0);
            std::cout << std::format("  CSV Telemetry Saved    : {}\n", csv_filename);
            std::cout << "====================================================\n\n";

            deallocate_huge_pages(shared_queue);
            deallocate_huge_pages(shared_telemetry);

            return EXIT_SUCCESS;
        }
    };
} // namespace hft::common
