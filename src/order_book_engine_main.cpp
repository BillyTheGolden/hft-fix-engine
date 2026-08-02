/**
 * @file order_book_engine_main.cpp
 * @brief Capstone C++20 HFT Order Book Engine & Top-of-Book BBO Telemetry Executable.
 */

#include "hft/common/ConsoleLogger.hpp"
#include "hft/common/SPSCQueue.hpp"
#include "hft/common/SystemOptimizations.hpp"
#include "hft/common/Types.hpp"
#include "hft/monitoring/PerformanceMonitor.hpp"
#include "hft/networking/FixProducer.hpp"
#include "hft/networking/RxConsumerFactory.hpp"
#include "hft/networking/RxRingConsumer.hpp"
#include "hft/order_book_engine/HftOrderBookEngine.hpp"
#include "hft/protocol/ProtocolParser.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace std;
using namespace hft::common;
using namespace hft::networking;
using namespace hft::protocol;
using namespace hft::order_book_engine;

static void handle_signal(int signum) noexcept
{
    if (signum == SIGINT || signum == SIGTERM)
    {
        hft::common::g_user_stopped.store(true, memory_order_release);
        hft::common::g_running.store(false, memory_order_release);
    }
}

static void print_usage(const char *prog_name)
{
    cout << "====================================================\n"
         << "  CAPSTONE C++20 HFT ORDER BOOK & BBO TELEMETRY ENGINE\n"
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
         << "  " << prog_name << " lo 8888 test_book_sbe_50k.data --direct-queue --protocol=SBE\n"
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
        return 1;
    }

    hft::common::g_running.store(true, memory_order_relaxed);
    hft::common::g_producer_done.store(false, memory_order_relaxed);
    hft::common::g_consumer_done.store(false, memory_order_relaxed);
    hft::common::g_user_stopped.store(false, memory_order_relaxed);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    string rx_interface;
    string tx_interface;
    uint16_t udp_port = 0;
    string source_param;
    bool direct_queue_mode = false;
    bool enable_core_pinning = false;
    string explicit_protocol_arg;
    uint32_t queue_id = 0;

    vector<string> positional_args;
    for (int i = 1; i < argc; ++i)
    {
        string arg = argv[i];
        if (arg == "--direct-queue" || arg == "--in-memory")
            direct_queue_mode = true;
        else if (arg == "--pin-cores" || arg == "--pin")
            enable_core_pinning = true;
        else if (arg.starts_with("--protocol="))
            explicit_protocol_arg = arg.substr(11);
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
            udp_port = static_cast<uint16_t>(stoi(positional_args[0]));
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
            udp_port = static_cast<uint16_t>(stoi(positional_args[2]));
            source_param = positional_args[3];
        }
        else if (positional_args.size() == 3)
        {
            rx_interface = positional_args[0];
            udp_port = static_cast<uint16_t>(stoi(positional_args[1]));
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
    }
    else
    {
        try
        {
            total_messages = static_cast<size_t>(stoul(source_param));
        }
        catch (...)
        {
            total_messages = 50000;
        }
    }

    ProtocolType protocol_type = parse_protocol_type(source_param);
    if (!explicit_protocol_arg.empty())
    {
        protocol_type = parse_protocol_type(explicit_protocol_arg);
    }
    string protocol_name = protocol_type_to_string(protocol_type);
    string proto_prefix = "fix";
    if (protocol_type == ProtocolType::OUCH)
        proto_prefix = "ouch";
    else if (protocol_type == ProtocolType::SBE)
        proto_prefix = "sbe";

    string log_filename = std::format("{}_hft_order_book_engine.log", protocol_name);
    transform(log_filename.begin(), log_filename.end(), log_filename.begin(), ::tolower);
    log_filename.erase(remove(log_filename.begin(), log_filename.end(), ' '), log_filename.end());
    log_filename.erase(remove(log_filename.begin(), log_filename.end(), '('), log_filename.end());
    log_filename.erase(remove(log_filename.begin(), log_filename.end(), ')'), log_filename.end());

    string csv_filename = proto_prefix + "_metrics_time_series.csv";

    const string target_ip = (tx_interface == "lo") ? "127.0.0.1" : "239.255.0.1";

    hft::common::g_cycles_per_ns = hft::common::calibrate_rdtsc();
    hft::common::lock_process_memory();

    // Shared Huge Page Queue & Telemetry
    auto *shared_queue =
        hft::common::allocate_on_huge_pages<hft::common::SPSCQueue<hft::common::FixMessagePacket, 8192>>();
    auto *shared_telemetry = hft::common::allocate_on_huge_pages<hft::monitoring::TelemetryCounters>();

    if (shared_queue == nullptr || shared_telemetry == nullptr)
    {
        cerr << "[Main] Fatal Error: Failed to allocate shared memory structures.\n";
        return EXIT_FAILURE;
    }

    // Initialize async console logger with bottom progress bar
    hft::common::ConsoleLogger::getInstance().initialize(*shared_telemetry, total_messages);

    hft::common::log_info("====================================================");
    hft::common::log_info("  CAPSTONE C++20 HFT ORDER BOOK & BBO TELEMETRY     ");
    hft::common::log_info(
        std::format("Rx Interface: {} | Tx Interface: {} | Port: {}", rx_interface, tx_interface, udp_port));
    hft::common::log_info(std::format("Protocol  : {}", protocol_name));
    hft::common::log_info(std::format("Source    : {} ({} msgs)", source_param, total_messages));
    hft::common::log_info(
        std::format("Execution : {}", direct_queue_mode ? "Direct In-Memory Queue (0% Loss)" : "Network Ring Socket"));
    hft::common::log_info(std::format("CSV File  : {}", csv_filename));
    hft::common::log_info(std::format("Engine Log: {}", log_filename));
    hft::common::log_info("====================================================");

    int cpu_count = hft::common::get_cpu_count();
    int worker_cpu = (enable_core_pinning && cpu_count >= 2) ? 1 : -1;
    int consumer_cpu = (enable_core_pinning && cpu_count >= 3) ? 2 : -1;
    int producer_cpu = (enable_core_pinning && cpu_count >= 4) ? 3 : -1;
    int monitor_cpu = (enable_core_pinning && cpu_count >= 5) ? 4 : -1;

    // Instantiate modular components
    HftOrderBookEngine book_engine(*shared_queue, *shared_telemetry, protocol_type, worker_cpu, log_filename);
    auto consumer = hft::networking::create_rx_consumer(rx_interface, udp_port, *shared_queue, *shared_telemetry,
                                                        consumer_cpu, queue_id);
    UdpFixProducer producer(tx_interface, target_ip, udp_port, total_messages, fix_file_path, producer_cpu,
                            direct_queue_mode ? shared_queue : nullptr);
    hft::monitoring::CsvPerformanceMonitor monitor(*shared_telemetry, *shared_queue, csv_filename, 10, monitor_cpu);

    auto start_wall_time = chrono::high_resolution_clock::now();

    thread t_monitor([&monitor]() { monitor.run(); });
    thread t_worker([&book_engine]() { book_engine.run(); });
    thread t_consumer([&consumer]() { consumer->run(); });
    thread t_producer([&producer]() { producer.run(); });

    if (t_producer.joinable())
        t_producer.join();
    g_consumer_done.store(true, memory_order_release);
    if (t_consumer.joinable())
        t_consumer.join();
    if (t_worker.joinable())
        t_worker.join();

    g_running.store(false, memory_order_release); // Stop monitor loop
    if (t_monitor.joinable())
        t_monitor.join();

    // Shut down console logger cleanly before printing the final performance report
    hft::common::ConsoleLogger::getInstance().shutdown();

    auto end_wall_time = chrono::high_resolution_clock::now();
    double total_execution_time_sec = chrono::duration<double>(end_wall_time - start_wall_time).count();

    uint64_t total_processed = shared_telemetry->worker.messages_processed.load(memory_order_relaxed);
    uint64_t total_approved = shared_telemetry->worker.risk_approved.load(memory_order_relaxed);
    uint64_t total_rejected = shared_telemetry->worker.risk_rejected.load(memory_order_relaxed);
    uint64_t min_lat = shared_telemetry->worker.min_latency_ns.load(memory_order_relaxed);
    uint64_t max_lat = shared_telemetry->worker.max_latency_ns.load(memory_order_relaxed);
    uint64_t total_lat = shared_telemetry->worker.total_latency_ns.load(memory_order_relaxed);
    double avg_lat =
        (total_processed > 0) ? (static_cast<double>(total_lat) / static_cast<double>(total_processed)) : 0.0;
    double throughput =
        (total_execution_time_sec > 0) ? (static_cast<double>(total_processed) / total_execution_time_sec) : 0.0;

    auto bbo = book_engine.get_top_of_book_snapshot();

    cout << "\n====================================================\n";
    cout << "  CAPSTONE HFT ORDER BOOK PERFORMANCE & BBO REPORT  \n";
    cout << "====================================================\n";
    cout << std::format("Total Messages Ingested  : {}\n", total_processed);
    cout << std::format("Risk Approved Orders     : {} ({:.1f}%)\n", total_approved,
                        total_processed > 0
                            ? (static_cast<double>(total_approved) * 100.0 / static_cast<double>(total_processed))
                            : 0.0);
    cout << std::format("Risk Rejected Orders     : {} ({:.1f}%)\n", total_rejected,
                        total_processed > 0
                            ? (static_cast<double>(total_rejected) * 100.0 / static_cast<double>(total_processed))
                            : 0.0);
    cout << std::format("Matched Trade Executions : {}\n", book_engine.total_trades());
    cout << std::format("Matched Shares Volume    : {} shares\n", book_engine.total_volume());
    cout << std::format("Total Execution Time     : {:.4f} seconds\n", total_execution_time_sec);
    cout << std::format("Throughput               : {:.0f} msgs/sec\n", throughput);
    cout << std::format("Minimum Latency          : {} ns\n", min_lat == UINT64_MAX ? 0 : min_lat);
    cout << std::format("Average Latency          : {:.2f} ns ({:.2f} us)\n", avg_lat, avg_lat / 1000.0);
    cout << std::format("Maximum Latency          : {} ns\n", max_lat);
    cout << "----------------------------------------------------\n";
    cout << "        TOP-OF-BOOK (BBO) PRICE LADDER DECK         \n";
    cout << "----------------------------------------------------\n";
    cout << std::format("  Best Bid Price (L1)    : ${:.2f} (Qty: {})\n", static_cast<double>(bbo.best_bid_price) / 1e6,
                        bbo.best_bid_qty);
    cout << std::format("  Best Ask Price (L1)    : ${:.2f} (Qty: {})\n", static_cast<double>(bbo.best_ask_price) / 1e6,
                        bbo.best_ask_qty);
    cout << std::format("  Top-of-Book Bid/Ask Spread: ${:.2f}\n", static_cast<double>(bbo.spread_price) / 1e6);
    cout << "----------------------------------------------------\n";
    cout << "           L1 - L5 PRICE DEPTH DECK                 \n";
    cout << "----------------------------------------------------\n";
    cout << "  [ASKS]\n";
    for (int i = static_cast<int>(bbo.ask_levels_count) - 1; i >= 0; --i)
    {
        cout << std::format("    Ask L{} : ${:.2f} | Qty: {} | Orders: {}\n", i + 1,
                            static_cast<double>(bbo.ask_depth[i].price) / 1e6, bbo.ask_depth[i].qty,
                            bbo.ask_depth[i].order_count);
    }
    cout << "  --------------------------------------------------\n";
    cout << "  [BIDS]\n";
    for (size_t i = 0; i < bbo.bid_levels_count; ++i)
    {
        cout << std::format("    Bid L{} : ${:.2f} | Qty: {} | Orders: {}\n", i + 1,
                            static_cast<double>(bbo.bid_depth[i].price) / 1e6, bbo.bid_depth[i].qty,
                            bbo.bid_depth[i].order_count);
    }
    cout << "====================================================\n";

    hft::common::deallocate_huge_pages(shared_queue);
    hft::common::deallocate_huge_pages(shared_telemetry);

    if (hft::common::g_user_stopped.load(memory_order_acquire))
    {
        cout << "\n[main] Process was stopped by the user (SIGINT/SIGTERM received).\n";
    }
    else
    {
        cout << std::format("\n[main] Capstone HFT Order Book Engine cleanly shut down. Log output written to: {}\n",
                            log_filename);
    }
    return 0;
}
