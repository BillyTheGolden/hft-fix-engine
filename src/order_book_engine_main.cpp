/**
 * @file order_book_engine_main.cpp
 * @brief Capstone C++20 HFT Order Book Engine with BBO Telemetry & L1-L5 Depth.
 */

#include "hft/common/EngineLauncher.hpp"
#include "hft/order_book_engine/HftOrderBookEngine.hpp"

#include <format>
#include <iostream>

using namespace std;

int main(int argc, char *argv[])
{
    return hft::common::EngineLauncher::run(
        "CAPSTONE C++20 HFT ORDER BOOK & BBO TELEMETRY", argc, argv,
        [](const hft::common::EngineContext &ctx) {
            return std::make_unique<hft::order_book_engine::HftOrderBookEngine>(ctx.queue, ctx.telemetry, ctx.protocol,
                                                                                ctx.worker_cpu, ctx.log_filename);
        },
        [](const hft::order_book_engine::HftOrderBookEngine &engine) {
            auto bbo = engine.get_top_of_book_snapshot();

            cout << "====================================================\n";
            cout << "    CAPSTONE L1/L2 ORDER BOOK BBO DEPTH SNAPSHOT   \n";
            cout << "====================================================\n";
            cout << std::format("  Symbol               : PETR4\n");
            cout << std::format("  Total Trades Matched : {}\n", engine.total_trades());
            cout << std::format("  Total Volume Matched : {} shares\n", engine.total_volume());
            cout << "----------------------------------------------------\n";
            cout << std::format("  Best Bid (Top of Book): ${:.2f} (Qty: {})\n",
                                static_cast<double>(bbo.best_bid_price) / 1e6, bbo.best_bid_qty);
            cout << std::format("  Best Ask (Top of Book): ${:.2f} (Qty: {})\n",
                                static_cast<double>(bbo.best_ask_price) / 1e6, bbo.best_ask_qty);
            cout << std::format("  Top-of-Book Spread    : ${:.2f}\n", static_cast<double>(bbo.spread_price) / 1e6);
            cout << "----------------------------------------------------\n";
            cout << "             L1 - L5 PRICE DEPTH DECK               \n";
            cout << "----------------------------------------------------\n";
            cout << "  [ASKS]\n";
            for (int i = static_cast<int>(bbo.ask_levels_count) - 1; i >= 0; --i)
            {
                const auto idx = static_cast<size_t>(i);
                cout << std::format("    Ask L{} : ${:.2f} | Qty: {} | Orders: {}\n", i + 1,
                                    static_cast<double>(bbo.ask_depth[idx].price) / 1e6, bbo.ask_depth[idx].qty,
                                    bbo.ask_depth[idx].order_count);
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
        });
}
