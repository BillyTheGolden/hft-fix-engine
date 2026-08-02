/**
 * @file main.cpp
 * @brief Application entry point orchestrating the 3 isolated low-latency HFT threads.
 */

#include "hft/common/ConsoleLogger.hpp"
#include "hft/common/SystemOptimizations.hpp"
#include "hft/monitoring/PerformanceMonitor.hpp"
#include "hft/networking/FixProducer.hpp"
#include "hft/networking/RxConsumerFactory.hpp"
#include "hft/networking/RxRingConsumer.hpp"
#include "hft/worker/FixWorker.hpp"

#include <csignal>
#include <filesystem>
#include <fstream>

using namespace std;

/**
 * @brief Global signal handler for graceful shutdown on SIGINT (Ctrl+C).
 * @param signum Captured POSIX signal number.
 */
static void handle_signal(int signum) noexcept
{
    if (signum == SIGINT || signum == SIGTERM)
    {
        hft::common::g_running.store(false, memory_order_release);
    }
}

/**
 * @brief Main function parsing CLI parameters and spawning isolated engine threads.
 * @param argc Argument count.
 * @param argv Argument string vector (`<interface> <udp_port> <num_messages_or_fix_file> [pin_cores]`).
 * @return `EXIT_SUCCESS` or `EXIT_FAILURE`.
 */
static void print_usage(const char *prog_name)
{
    cerr << "====================================================\n"
         << "  HFT FIX ENGINE & ZERO-COPY RX RING BYPASS\n"
         << "====================================================\n"
         << "Usage:\n"
         << "  " << prog_name << " <rx_interface> [tx_interface] <port> <source_file_or_count> [options]\n\n"
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
         << "  --pin-cores             Enable real-time thread core pinning and affinity\n\n"
         << "Examples:\n"
         << "  " << prog_name << " veth1 veth0 8888 ./fix_messages_1m.data --pin-cores\n"
         << "  " << prog_name << " enp4s0 8888 ./fix_messages_1m.data --pin-cores --direct-queue\n"
         << "  " << prog_name << " lo 8888 50000 --pin-cores\n"
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

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (geteuid() != 0)
    {
        cerr << "Error: Root privileges required for AF_PACKET / PACKET_MMAP zero-copy socket.\n";
        cerr << "Please run with sudo.\n";
        return 1;
    }

    hft::common::lock_process_memory();
    hft::common::g_cycles_per_ns = hft::common::calibrate_rdtsc();

    string rx_interface;
    string tx_interface;
    uint16_t target_port = 0;
    string source_param;
    bool pin_cores = false;
    bool direct_queue_mode = false;
    uint32_t queue_id = 0;

    vector<string> positional_args;
    for (int i = 1; i < argc; ++i)
    {
        string arg = argv[i];
        if (arg == "--pin-cores" || arg == "--pin")
        {
            pin_cores = true;
        }
        else if (arg == "--direct-queue" || arg == "--in-memory")
        {
            direct_queue_mode = true;
        }
        else if (arg.starts_with("--tx-iface=") || arg.starts_with("--tx-nic=") || arg.starts_with("--producer-iface="))
        {
            tx_interface = arg.substr(arg.find('=') + 1);
        }
        else if (arg.starts_with("--rx-iface=") || arg.starts_with("--rx-nic=") || arg.starts_with("--consumer-iface="))
        {
            rx_interface = arg.substr(arg.find('=') + 1);
        }
        else if (arg.starts_with("--queue-id=") || arg.starts_with("--queue="))
        {
            queue_id = static_cast<uint32_t>(stoul(arg.substr(arg.find('=') + 1)));
        }
        else if (!arg.starts_with("-"))
        {
            positional_args.push_back(arg);
        }
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
            print_usage(argv[0]);
            return 1;
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
            cerr << "[Error] Invalid positional arguments.\n\n";
            print_usage(argv[0]);
            return 1;
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
            cerr << "[Main] Error: argument '" << source_param << "' must be an existing file or integer count.\n";
            return 1;
        }
    }

    const string target_ip = (tx_interface == "lo") ? "127.0.0.1" : "239.255.0.1";
    const string log_filename = "fix_engine.log";
    const string csv_filename = "fix_metrics_time_series.csv";

    // Optional CPU core pinning assignments
    int cpu_count = hft::common::get_cpu_count();
    int worker_cpu = (pin_cores && cpu_count >= 2) ? 1 : -1;
    int consumer_cpu = (pin_cores && cpu_count >= 3) ? 2 : -1;
    int producer_cpu = (pin_cores && cpu_count >= 4) ? 3 : -1;
    int monitor_cpu = (pin_cores && cpu_count >= 5) ? 4 : -1;

    // Shared zero-contention data structures allocated on 2MB huge pages
    auto *shared_queue =
        hft::common::allocate_on_huge_pages<hft::common::SPSCQueue<hft::common::FixMessagePacket, 8192>>();
    auto *shared_telemetry = hft::common::allocate_on_huge_pages<hft::monitoring::TelemetryCounters>();

    if (shared_queue == nullptr || shared_telemetry == nullptr)
    {
        cerr << "[Main] Fatal Error: Memory allocation for queues/telemetry on huge pages failed.\n";
        return 1;
    }

    // Initialize async console logger
    hft::common::ConsoleLogger::getInstance().initialize(*shared_telemetry, total_messages);

    hft::common::log_info("[Main] Calibrated RDTSC CPU frequency: " + to_string(hft::common::g_cycles_per_ns) +
                          " cycles/ns");

    if (!fix_file_path.empty())
    {
        hft::common::log_info("[Main] Detected pre-generated FIX messages file '" + fix_file_path + "' containing " +
                              to_string(total_messages) + " messages.");
    }

    hft::common::log_info("====================================================");
    hft::common::log_info("HFT C++20 FIX ENGINE & ZERO-COPY RX RING BYPASS");
    hft::common::log_info("Rx Interface: " + rx_interface + " | Tx Interface: " + tx_interface +
                          " | Port: " + to_string(target_port));
    hft::common::log_info("Target Msgs : " + to_string(total_messages) +
                          " | Core Pinning: " + (pin_cores ? "ENABLED" : "DISABLED"));
    if (!fix_file_path.empty())
    {
        hft::common::log_info("Source File : " + fix_file_path);
    }
    hft::common::log_info("Execution Mode: " + string(direct_queue_mode
                                                          ? "Direct In-Memory Queue (0% Loss Guaranteed)"
                                                          : "Network Socket Ring (UDP / PACKET_MMAP / AF_XDP)"));
    hft::common::log_info("Telemetry CSV: " + csv_filename + " | Sample Rate: 10 ms");
    hft::common::log_info("====================================================");

    // Instantiate modular components
    hft::worker::FixWorker worker(*shared_queue, *shared_telemetry, log_filename, total_messages, worker_cpu);
    auto consumer = hft::networking::create_rx_consumer(rx_interface, target_port, *shared_queue, *shared_telemetry,
                                                        consumer_cpu, queue_id);
    hft::networking::UdpFixProducer producer(tx_interface, target_ip, target_port, total_messages, fix_file_path,
                                             producer_cpu, direct_queue_mode ? shared_queue : nullptr);
    hft::monitoring::CsvPerformanceMonitor monitor(*shared_telemetry, *shared_queue, csv_filename, 10, monitor_cpu);

    // Launch isolated threads (4 threads total)
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

    // Shut down console logger cleanly before printing the final message to restore the terminal
    hft::common::ConsoleLogger::getInstance().shutdown();

    // Deallocate shared data structures from huge pages
    hft::common::deallocate_huge_pages(shared_queue);
    hft::common::deallocate_huge_pages(shared_telemetry);

    cout << "[main] Engine and Telemetry Monitor cleanly shut down. Goodbye.\n";
    return EXIT_SUCCESS;
}
