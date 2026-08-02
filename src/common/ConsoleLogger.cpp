/**
 * @file ConsoleLogger.cpp
 * @brief Implementation of ConsoleLogger for asynchronous colored logging and progress tracking.
 */

#include "hft/common/ConsoleLogger.hpp"

#include <cstring>
#include <format>
#include <iostream>
#include <sys/ioctl.h>
#include <unistd.h>

namespace hft::common
{
    using namespace std;

    ConsoleLogger &ConsoleLogger::getInstance() noexcept
    {
        static ConsoleLogger instance;
        return instance;
    }

    void ConsoleLogger::initialize(hft::monitoring::TelemetryCounters &telemetry, size_t expected_messages) noexcept
    {
        if (m_initialized)
        {
            return;
        }

        m_telemetry = &telemetry;
        m_expected_messages = expected_messages;
        m_running.store(true, memory_order_relaxed);

        m_thread = thread(&ConsoleLogger::run_loop, this);
        m_initialized = true;
    }

    void ConsoleLogger::log(ConsoleCategory category, string_view msg) noexcept
    {
        if (!m_running.load(memory_order_relaxed))
        {
            // If logger not running, fallback to standard stream print immediately
            if (category == ConsoleCategory::ERROR_MSG)
            {
                cerr << msg << endl;
            }
            else
            {
                cout << msg << endl;
            }
            return;
        }

        ConsoleMessage m;
        m.category = category;
        // OPTIMIZATION (High Finding 6.2): Direct memcpy instead of format_to_n (10x faster)
        size_t copy_len = min(msg.length(), sizeof(m.message) - 1);
        memcpy(m.message, msg.data(), copy_len);
        m.message[copy_len] = '\0';
        m_queue.push(m);
    }

    bool ConsoleLogger::try_log(ConsoleCategory category, string_view msg) noexcept
    {
        if (!m_running.load(memory_order_relaxed))
        {
            return false;
        }

        ConsoleMessage m;
        m.category = category;
        // OPTIMIZATION (High Finding 6.2): Direct memcpy instead of format_to_n (10x faster)
        size_t copy_len = min(msg.length(), sizeof(m.message) - 1);
        memcpy(m.message, msg.data(), copy_len);
        m.message[copy_len] = '\0';
        return m_queue.try_push(m);
    }

    void ConsoleLogger::shutdown() noexcept
    {
        if (!m_running.load(memory_order_relaxed))
        {
            return;
        }

        m_running.store(false, memory_order_relaxed);
        // Push a dummy message to wake up the pop condition variable
        ConsoleMessage dummy;
        dummy.category = ConsoleCategory::INFO_MSG;
        dummy.message[0] = '\0';
        m_queue.push(dummy);

        if (m_thread.joinable())
        {
            m_thread.join();
        }

        m_initialized = false;
    }

    ConsoleLogger::~ConsoleLogger() noexcept
    {
        shutdown();
    }

    void ConsoleLogger::update_terminal_size() noexcept
    {
        struct winsize w{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 2 && w.ws_col > 10)
        {
            m_terminal_rows = w.ws_row;
            m_terminal_cols = w.ws_col;
        }
        else
        {
            m_terminal_rows = 24;
            m_terminal_cols = 80;
        }
    }

    void ConsoleLogger::restore_terminal() noexcept
    {
        if (m_terminal_configured)
        {
            // Reset scrolling margins first (which may home the cursor),
            // then position the cursor at the bottom row.
            cout << "\033[r\033[" << m_terminal_rows << ";1H\n\033[?25h\033[0m" << flush;
            m_terminal_configured = false;
        }
    }

    void ConsoleLogger::draw_progress_bar(size_t processed) noexcept
    {
        // Save cursor position
        cout << "\033[s";

        // Draw horizontal line separator on row N-1
        cout << "\033[" << (m_terminal_rows - 1) << ";1H\033[2K\033[36m";
        for (int i = 0; i < m_terminal_cols; ++i)
        {
            cout << "-";
        }
        cout << "\033[0m";

        // Move to bottom row N
        cout << "\033[" << m_terminal_rows << ";1H\033[2K";

        double pct = 0.0;
        if (m_expected_messages > 0)
        {
            pct = static_cast<double>(processed) / static_cast<double>(m_expected_messages);
            if (pct > 1.0)
            {
                pct = 1.0;
            }
        }

        string pct_str = format(" {:5.1f}% ({}/{})", pct * 100.0, processed, m_expected_messages);
        int pct_len = static_cast<int>(pct_str.size());

        int bar_width = m_terminal_cols - 4 - pct_len; // 4 chars for "[ ]" and space
        if (bar_width < 10)
        {
            bar_width = 10;
        }

        int filled = static_cast<int>(pct * bar_width);

        cout << "\033[1;36m["; // Cyan progress bar borders
        for (int i = 0; i < bar_width; ++i)
        {
            if (i < filled)
            {
                cout << "=";
            }
            else if (i == filled && pct < 1.0)
            {
                cout << ">";
            }
            else
            {
                cout << " ";
            }
        }
        cout << "]" << pct_str << "\033[0m";

        // Restore cursor position
        cout << "\033[u" << flush;
    }

    void ConsoleLogger::run_loop() noexcept
    {
        update_terminal_size();

        // Configure terminal: Hide cursor, Clear screen, Set scroll region
        cout << "\033[?25l\033[2J\033[H\033[1;" << (m_terminal_rows - 2) << "r" << flush;
        m_terminal_configured = true;

        ConsoleMessage m;
        int last_rows = m_terminal_rows;
        int last_cols = m_terminal_cols;

        while (m_running.load(memory_order_relaxed) || !m_queue.empty())
        {
            bool has_msg = m_queue.pop_with_timeout(m, m_running, 50);

            if (has_msg && m.message[0] != '\0')
            {
                // Handle terminal resizing dynamically
                update_terminal_size();
                if (m_terminal_rows != last_rows || m_terminal_cols != last_cols)
                {
                    cout << "\033[1;" << (m_terminal_rows - 2) << "r" << flush;
                    last_rows = m_terminal_rows;
                    last_cols = m_terminal_cols;
                }

                // Print the colored log message
                switch (m.category)
                {
                case ConsoleCategory::INFO_MSG:
                    cout << "\033[32m" << m.message << "\033[0m\n";
                    break;
                case ConsoleCategory::WARN_MSG:
                    cout << "\033[33m" << m.message << "\033[0m\n";
                    break;
                case ConsoleCategory::ERROR_MSG:
                    cout << "\033[31m" << m.message << "\033[0m\n";
                    break;
                }
            }
            else if (!m_running.load(memory_order_relaxed) && m_queue.empty())
            {
                break;
            }

            // Always update the progress bar on every tick / message pop
            if (m_telemetry != nullptr)
            {
                size_t processed = m_telemetry->worker.messages_processed.load(memory_order_relaxed);
                draw_progress_bar(processed);
            }
        }

        // Drain any remaining messages in the queue on shutdown
        while (m_queue.try_pop(m))
        {
            if (m.message[0] == '\0')
            {
                continue;
            }

            switch (m.category)
            {
            case ConsoleCategory::INFO_MSG:
                cout << "\033[32m" << m.message << "\033[0m\n";
                break;
            case ConsoleCategory::WARN_MSG:
                cout << "\033[33m" << m.message << "\033[0m\n";
                break;
            case ConsoleCategory::ERROR_MSG:
                cout << "\033[31m" << m.message << "\033[0m\n";
                break;
            }
        }

        // Final draw of 100% or final progress state
        if (m_telemetry != nullptr)
        {
            size_t processed = m_telemetry->worker.messages_processed.load(memory_order_relaxed);
            draw_progress_bar(processed);
        }

        restore_terminal();
    }

    // Helper implementations
    void log_info(string_view msg) noexcept
    {
        ConsoleLogger::getInstance().log(ConsoleCategory::INFO_MSG, msg);
    }

    void log_warn(string_view msg) noexcept
    {
        ConsoleLogger::getInstance().log(ConsoleCategory::WARN_MSG, msg);
    }

    void log_error(string_view msg) noexcept
    {
        ConsoleLogger::getInstance().log(ConsoleCategory::ERROR_MSG, msg);
    }

    bool try_log_info(string_view msg) noexcept
    {
        return ConsoleLogger::getInstance().try_log(ConsoleCategory::INFO_MSG, msg);
    }

    bool try_log_warn(string_view msg) noexcept
    {
        return ConsoleLogger::getInstance().try_log(ConsoleCategory::WARN_MSG, msg);
    }

    bool try_log_error(string_view msg) noexcept
    {
        return ConsoleLogger::getInstance().try_log(ConsoleCategory::ERROR_MSG, msg);
    }

} // namespace hft::common
