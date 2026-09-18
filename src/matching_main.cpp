/**
 * @file matching_main.cpp
 * @brief Entry point for the High-Frequency Trading Order Matching Engine application.
 */

#include "hft/common/EngineLauncher.hpp"
#include "hft/matching/MatchingWorker.hpp"

int main(int argc, char *argv[])
{
    return hft::common::EngineLauncher::run(
        "HFT MULTI-PROTOCOL ORDER MATCHING ENGINE", argc, argv,
        [](const hft::common::EngineContext &ctx) {
            return std::make_unique<hft::matching::MatchingWorker>(ctx.queue, ctx.telemetry, ctx.log_filename,
                                                                   ctx.total_messages, ctx.protocol, ctx.worker_cpu);
        },
        [](const hft::matching::MatchingWorker &worker) {
            std::cout << std::format("  Total Trades Matched   : {}\n", worker.total_trades());
        });
}
