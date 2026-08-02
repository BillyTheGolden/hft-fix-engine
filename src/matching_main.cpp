/**
 * @file matching_main.cpp
 * @brief Entry point for the High-Frequency Trading Order Matching Engine application.
 */

#include "hft/common/ConsoleLogger.hpp"
#include "hft/common/SPSCQueue.hpp"
#include "hft/common/SystemOptimizations.hpp"
#include "hft/common/Types.hpp"
#include "hft/matching/MatchingWorker.hpp"
#include "hft/monitoring/PerformanceMonitor.hpp"
#include "hft/monitoring/Telemetry.hpp"
#include "hft/networking/FixProducer.hpp"
#include "hft/networking/RxConsumerFactory.hpp"
#include "hft/networking/RxRingConsumer.hpp"

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

static void print_usage(const char *prog_name)
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

static bool is_binary_dataset_file(const string &file_path)
{
    if (file_path.empty())
        return false;
    ifstream file(file_path, ios::binary);
    if (!file.is_open())
        return false;

    char header[8] = {0};
    file.read(header, sizeof(header));
    string_view head_view(header, static_cast<size_t>(file.gcount()));

    if (head_view.starts_with("8=FIX") || head_view.starts_with("8="))
    {
        return false; // ASCII FIX Protocol message file
    }
    return file_path.ends_with(".data") || (head_view.length() > 0 && head_view[0] == 'O');
}

int main(int argc, char *argv[])
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
        hft::common::log_warn("[Main] Note: Running without root privileges. PACKET_MMAP will fall back to high-speed "
                              "AF_INET UDP socket.");
    }

    // Pin all current and future memory pages of this process into physical RAM.
    //
    // Without mlockall(), the kernel may swap out any page (hot-path .text, SPSC queue
    // pages, UMEM ring buffer) under memory pressure. Accessing a swapped page causes a
    // major page fault — a synchronous disk I/O that blocks the thread for 1–10 ms, which
    // is a catastrophic latency spike for an engine targeting sub-microsecond processing.
    //
    // MCL_CURRENT: pins all pages that already exist at this call site.
    // MCL_FUTURE:  automatically pins every new mmap region created after this call,
    //              including the huge-page SPSC queue and AF_XDP UMEM buffer below.
    //
    // Requires CAP_IPC_LOCK. When running as root (needed for AF_PACKET), this is granted.
    // On non-root dev builds, lock_process_memory() prints a warning and returns false —
    // the engine continues running, just without the page-locking guarantee.
    hft::common::lock_process_memory();

    string rx_interface;
    string tx_interface;
    uint16_t target_port = 0;
    string source_param;
    bool pin_cores = false;
    bool protocol_explicit = false;
    bool direct_queue_mode = false;
    uint32_t queue_id = 0;
    hft::protocol::ProtocolType protocol_type = hft::protocol::ProtocolType::FIX;

    vector<string> positional_args;
    for (int i = 1; i < argc; ++i)
    {
        string arg = argv[i];
        if (arg == "--direct-queue" || arg == "--in-memory")
            direct_queue_mode = true;
        else if (arg == "--pin-cores" || arg == "--pin")
            pin_cores = true;
        else if (arg.starts_with("--protocol="))
        {
            protocol_type = hft::protocol::parse_protocol_type(arg.substr(11));
            protocol_explicit = true;
        }
        else if (arg.starts_with("--tx-iface=") || arg.starts_with("--tx-nic=") || arg.starts_with("--producer-iface="))
            tx_interface = arg.substr(arg.find('=') + 1);
        else if (arg.starts_with("--rx-iface=") || arg.starts_with("--rx-nic=") || arg.starts_with("--consumer-iface="))
            rx_interface = arg.substr(arg.find('=') + 1);
        else if (arg.starts_with("--queue-id=") || arg.starts_with("--queue="))
            queue_id = static_cast<uint32_t>(stoul(arg.substr(arg.find('=') + 1)));
        else if (!arg.starts_with("-"))
            positional_args.push_back(arg);
    }

    if (!rx_interface.empty())
    {
        if (positional_args.size() >= 2)
        {
            target_port = static_cast<uint16_t>(stoi(positional_args[0]));
            source_param = positional_args[1];
        }
        else
        {
            cerr << "[Error] Missing port or source_param argument.\n\n";
            return EXIT_FAILURE;
        }
    }
    else
    {
        if (positional_args.size() >= 4)
        {
            rx_interface = positional_args[0];
            if (tx_interface.empty())
                tx_interface = positional_args[1];
            target_port = static_cast<uint16_t>(stoi(positional_args[2]));
            source_param = positional_args[3];
        }
        else if (positional_args.size() == 3)
        {
            rx_interface = positional_args[0];
            target_port = static_cast<uint16_t>(stoi(positional_args[1]));
            source_param = positional_args[2];
        }
        else
        {
            cerr << "[Error] Missing required command line arguments.\n\n";
            return EXIT_FAILURE;
        }
    }

    if (tx_interface.empty())
    {
        tx_interface = rx_interface;
    }

    if (!direct_queue_mode && rx_interface == tx_interface && rx_interface != "lo" && !rx_interface.starts_with("veth"))
    {
        cerr << "\n===================================================================================================="
                "\n"
             << "[Main] NOTICE: Single physical NIC ('" << rx_interface
             << "') specified for both RX and TX without '--direct-queue'.\n"
             << "       Physical NIC drivers do not loop back local egress TX packets to local AF_XDP RX queues.\n"
             << "       -> SUGGESTION: Pass '--direct-queue' ('--in-memory') for single-machine lock-free testing,\n"
             << "          or pass 2 distinct NICs: " << argv[0] << " <rx_iface> <tx_iface> <port> <source> [options]\n"
             << "===================================================================================================="
                "\n\n";
    }

    size_t total_messages = 0;
    string fix_file_path = "";
    if (filesystem::exists(source_param))
    {
        fix_file_path = source_param;
        bool is_binary = is_binary_dataset_file(fix_file_path);
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
                if (!line.empty())
                    ++total_messages;
            }
        }

        if (!protocol_explicit)
        {
            string lower_path = fix_file_path;
            for (char &c : lower_path)
                c = static_cast<char>(tolower(c));
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
            cerr << "[Main] Error: third argument '" << source_param
                 << "' must be an existing file or integer count.\n";
            return EXIT_FAILURE;
        }
    }

    string proto_prefix = "fix";
    if (protocol_type == hft::protocol::ProtocolType::OUCH)
        proto_prefix = "ouch";
    else if (protocol_type == hft::protocol::ProtocolType::SBE)
        proto_prefix = "sbe";

    const string target_ip = (tx_interface == "lo") ? "127.0.0.1" : "239.255.0.1";
    const string log_filename = proto_prefix + "_hft_matching_engine.log";
    const string csv_filename = proto_prefix + "_metrics_time_series.csv";

    int cpu_count = hft::common::get_cpu_count();
    int worker_cpu = (pin_cores && cpu_count >= 2) ? 1 : -1;
    int consumer_cpu = (pin_cores && cpu_count >= 3) ? 2 : -1;
    int producer_cpu = (pin_cores && cpu_count >= 4) ? 3 : -1;
    int monitor_cpu = (pin_cores && cpu_count >= 5) ? 4 : -1;

    // Calibrate RDTSC CPU clock frequency
    hft::common::g_cycles_per_ns = hft::common::calibrate_rdtsc();

    // Allocate shared queue and telemetry structures on 2MB Huge Pages
    auto *shared_queue =
        hft::common::allocate_on_huge_pages<hft::common::SPSCQueue<hft::common::FixMessagePacket, 8192>>();
    auto *shared_telemetry = hft::common::allocate_on_huge_pages<hft::monitoring::TelemetryCounters>();

    if (shared_queue == nullptr || shared_telemetry == nullptr)
    {
        cerr << "[Main] Fatal Error: Memory allocation on 2MB huge pages failed.\n";
        return EXIT_FAILURE;
    }

    // Initialize async console logger
    hft::common::ConsoleLogger::getInstance().initialize(*shared_telemetry, total_messages);

    hft::common::log_info("====================================================");
    hft::common::log_info("  HFT C++20 ORDER MATCHING ENGINE & RESILIENCE GATE ");
    hft::common::log_info("Rx Interface: " + rx_interface + " | Tx Interface: " + tx_interface +
                          " | Port: " + to_string(target_port));
    hft::common::log_info("Protocol  : " + string(hft::protocol::protocol_type_to_string(protocol_type)));
    hft::common::log_info("Target Msgs: " + to_string(total_messages) +
                          " | Core Pinning: " + (pin_cores ? "ENABLED" : "DISABLED"));
    if (!fix_file_path.empty())
    {
        hft::common::log_info("Source File : " + fix_file_path);
    }
    hft::common::log_info("Execution Mode: " + string(direct_queue_mode
                                                          ? "Direct In-Memory Queue (0% Loss Guaranteed)"
                                                          : "Network Socket Ring (UDP / PACKET_MMAP / AF_XDP)"));
    hft::common::log_info("Telemetry CSV: " + csv_filename + " | Sample Rate: 10 ms");
    hft::common::log_info("Engine Log  : " + log_filename);
    hft::common::log_info("====================================================");

    // Instantiate modular components
    hft::matching::MatchingWorker worker(*shared_queue, *shared_telemetry, log_filename, total_messages, protocol_type,
                                         worker_cpu);
    auto consumer = hft::networking::create_rx_consumer(rx_interface, target_port, *shared_queue, *shared_telemetry,
                                                        consumer_cpu, queue_id);
    hft::networking::UdpFixProducer producer(tx_interface, target_ip, target_port, total_messages, fix_file_path,
                                             producer_cpu, direct_queue_mode ? shared_queue : nullptr);
    hft::monitoring::CsvPerformanceMonitor monitor(*shared_telemetry, *shared_queue, csv_filename, 10, monitor_cpu);

    // Launch 4 isolated engine threads
    thread t_monitor([&monitor]() { monitor.run(); });
    thread t_worker([&worker]() { worker.run(); });
    thread t_consumer([&consumer]() { consumer->run(); });
    thread t_producer([&producer]() { producer.run(); });

    // Join threads on completion or termination
    if (t_producer.joinable())
        t_producer.join();
    if (t_consumer.joinable())
        t_consumer.join();
    if (t_worker.joinable())
        t_worker.join();
    if (t_monitor.joinable())
        t_monitor.join();

    hft::common::ConsoleLogger::getInstance().shutdown();

    // Deallocate shared huge pages
    hft::common::deallocate_huge_pages(shared_queue);
    hft::common::deallocate_huge_pages(shared_telemetry);

    cout << "[main] HFT Order Matching Engine cleanly shut down. Log output written to: " << log_filename << "\n";
    return EXIT_SUCCESS;
}
