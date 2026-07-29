/**
 * @file matching_main.cpp
 * @brief Entry point for the High-Frequency Trading Order Matching Engine application.
 */

#include "hft/common/ConsoleLogger.hpp"
#include "hft/common/SPSCQueue.hpp"
#include "hft/common/SystemOptimizations.hpp"
#include "hft/common/Types.hpp"
#include "hft/monitoring/PerformanceMonitor.hpp"
#include "hft/monitoring/Telemetry.hpp"
#include "hft/networking/FixProducer.hpp"
#include "hft/networking/RxRingConsumer.hpp"
#include "hft/matching/MatchingWorker.hpp"

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace std;

static void handle_signal(int signum) noexcept
{
    if (signum == SIGINT || signum == SIGTERM)
    {
        hft::common::g_running.store(false, memory_order_release);
    }
}

static void print_usage(const char* prog_name)
{
    cerr << "====================================================\n"
         << "  HFT MULTI-PROTOCOL ORDER MATCHING ENGINE\n"
         << "====================================================\n"
         << "Usage:\n"
         << "  " << prog_name << " <interface> <port> <source_file_or_count> [options]\n\n"
         << "Positional Arguments:\n"
         << "  <interface>             Network interface to bind (e.g., 'lo', 'eth0')\n"
         << "  <port>                  UDP port number (e.g., 8888)\n"
         << "  <source_file_or_count>  Input message file path (.data or .txt) or message count\n\n"
         << "Options:\n"
         << "  --help, -h              Display this usage help message and exit\n"
         << "  --direct-queue          Enable direct in-memory queue mode (0% packet loss)\n"
         << "  --protocol=<type>       Explicitly set protocol: FIX | OUCH | SBE\n"
         << "  --pin-cores             Enable real-time thread core pinning and affinity\n\n"
         << "Examples:\n"
         << "  " << prog_name << " lo 8888 sbe_messages_500k.data --direct-queue\n"
         << "  " << prog_name << " lo 8888 ouch_messages_50k.data --pin-cores\n"
         << "  " << prog_name << " --help\n"
         << "====================================================\n";
}

int main(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i)
    {
        string arg = argv[i];
        if (arg == "--help" || arg == "-h")
        {
            print_usage(argv[0]);
            return 0;
        }
    }

    if (argc < 4)
    {
        cerr << "[Error] Missing required command line arguments.\n\n";
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    hft::common::g_running.store(true, memory_order_relaxed);
    hft::common::g_producer_done.store(false, memory_order_relaxed);
    hft::common::g_consumer_done.store(false, memory_order_relaxed);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (geteuid() != 0)
    {
        hft::common::log_warn("[Main] Note: Running without root privileges. PACKET_MMAP will fall back to high-speed AF_INET UDP socket.");
    }

    const string interface_name = argv[1];
    const uint16_t target_port = static_cast<uint16_t>(stoi(argv[2]));
    const string source_param = argv[3];

    bool pin_cores = false;
    bool protocol_explicit = false;
    bool direct_queue_mode = false;
    hft::protocol::ProtocolType protocol_type = hft::protocol::ProtocolType::FIX;

    for (int a = 4; a < argc; ++a)
    {
        string arg = argv[a];
        if (arg == "--pin-cores")
        {
            pin_cores = true;
        }
        else if (arg == "--direct-queue" || arg == "--in-memory")
        {
            direct_queue_mode = true;
        }
        else if (arg.rfind("--protocol=", 0) == 0)
        {
            protocol_type = hft::protocol::parse_protocol_type(arg.substr(11));
            protocol_explicit = true;
        }
    }

    size_t total_messages = 0;
    string fix_file_path = "";
    if (filesystem::exists(source_param))
    {
        fix_file_path = source_param;
        bool is_binary = (fix_file_path.size() >= 5 && fix_file_path.substr(fix_file_path.size() - 5) == ".data");
        if (is_binary)
        {
            size_t file_size = filesystem::file_size(fix_file_path);
            total_messages = (file_size % 46 == 0) ? (file_size / 46) : (file_size / 44);
        }
        else
        {
            ifstream f(fix_file_path);
            string line;
            while (getline(f, line))
            {
                if (!line.empty()) ++total_messages;
            }
        }

        if (!protocol_explicit)
        {
            string lower_path = fix_file_path;
            for (char &c : lower_path) c = static_cast<char>(tolower(c));
            if (lower_path.find("sbe") != string::npos)
            {
                protocol_type = hft::protocol::ProtocolType::SBE;
            }
            else if (lower_path.find("ouch") != string::npos)
            {
                protocol_type = hft::protocol::ProtocolType::OUCH;
            }
        }
    }
    else
    {
        try
        {
            total_messages = static_cast<size_t>(stoul(source_param));
        }
        catch (...)
        {
            cerr << "[Main] Error: third argument '" << source_param << "' must be an existing file or integer count.\n";
            return EXIT_FAILURE;
        }
    }

    string proto_prefix = "fix";
    if (protocol_type == hft::protocol::ProtocolType::OUCH) proto_prefix = "ouch";
    else if (protocol_type == hft::protocol::ProtocolType::SBE) proto_prefix = "sbe";

    const string target_ip = (interface_name == "lo") ? "127.0.0.1" : "239.255.0.1";
    const string log_filename = proto_prefix + "_hft_matching_engine.log";
    const string csv_filename = proto_prefix + "_matching_metrics_time_series.csv";

    int cpu_count = hft::common::get_cpu_count();
    int worker_cpu = (pin_cores && cpu_count >= 2) ? 1 : -1;
    int consumer_cpu = (pin_cores && cpu_count >= 3) ? 2 : -1;
    int producer_cpu = (pin_cores && cpu_count >= 4) ? 3 : -1;
    int monitor_cpu = (pin_cores && cpu_count >= 5) ? 4 : -1;

    // Calibrate RDTSC CPU clock frequency
    hft::common::g_cycles_per_ns = hft::common::calibrate_rdtsc();

    // Allocate shared queue and telemetry structures on 2MB Huge Pages
    auto* shared_queue = hft::common::allocate_on_huge_pages<hft::common::SPSCQueue<hft::common::FixMessagePacket, 8192>>();
    auto* shared_telemetry = hft::common::allocate_on_huge_pages<hft::monitoring::TelemetryCounters>();

    if (shared_queue == nullptr || shared_telemetry == nullptr)
    {
        cerr << "[Main] Fatal Error: Memory allocation on 2MB huge pages failed.\n";
        return EXIT_FAILURE;
    }

    // Initialize async console logger
    hft::common::ConsoleLogger::getInstance().initialize(*shared_telemetry, total_messages);

    hft::common::log_info("====================================================");
    hft::common::log_info("  HFT C++20 ORDER MATCHING ENGINE & RESILIENCE GATE ");
    hft::common::log_info("Interface : " + interface_name + " | Port: " + to_string(target_port));
    hft::common::log_info("Protocol  : " + string(hft::protocol::protocol_type_to_string(protocol_type)));
    hft::common::log_info("Target Msgs: " + to_string(total_messages) + " | Core Pinning: " + (pin_cores ? "ENABLED" : "DISABLED"));
    if (!fix_file_path.empty())
    {
        hft::common::log_info("Source File : " + fix_file_path);
    }
    hft::common::log_info("Execution Mode: " + string(direct_queue_mode ? "Direct In-Memory Queue (0% Loss Guaranteed)" : "Network Socket Ring (UDP / PACKET_MMAP)"));
    hft::common::log_info("Telemetry CSV: " + csv_filename + " | Sample Rate: 10 ms");
    hft::common::log_info("Engine Log  : " + log_filename);
    hft::common::log_info("====================================================");

    // Instantiate modular components
    hft::matching::MatchingWorker worker(*shared_queue, *shared_telemetry, log_filename, total_messages, protocol_type, worker_cpu);
    hft::networking::PacketMmapRxConsumer consumer(interface_name, target_port, *shared_queue, *shared_telemetry, consumer_cpu);
    hft::networking::UdpFixProducer producer(interface_name, target_ip, target_port, total_messages, fix_file_path, producer_cpu, direct_queue_mode ? shared_queue : nullptr);
    hft::monitoring::CsvPerformanceMonitor monitor(*shared_telemetry, *shared_queue, csv_filename, 10, monitor_cpu);

    // Launch 4 isolated engine threads
    thread t_monitor([&monitor]() { monitor.run(); });
    thread t_worker([&worker]() { worker.run(); });
    thread t_consumer([&consumer]() { consumer.run(); });
    thread t_producer([&producer]() { producer.run(); });

    // Join threads on completion or termination
    if (t_producer.joinable()) t_producer.join();
    if (t_consumer.joinable()) t_consumer.join();
    if (t_worker.joinable()) t_worker.join();
    if (t_monitor.joinable()) t_monitor.join();

    hft::common::ConsoleLogger::getInstance().shutdown();

    // Deallocate shared huge pages
    hft::common::deallocate_huge_pages(shared_queue);
    hft::common::deallocate_huge_pages(shared_telemetry);

    cout << "[main] HFT Order Matching Engine cleanly shut down. Log output written to: " << log_filename << "\n";
    return EXIT_SUCCESS;
}
