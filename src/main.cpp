/**
 * @file main.cpp
 * @brief Application entry point orchestrating the FIX Engine processing pipeline.
 */

#include "hft/common/EngineLauncher.hpp"
#include "hft/worker/FixWorker.hpp"

int main(int argc, char *argv[])
{
    return hft::common::EngineLauncher::run(
        "HFT FIX ENGINE & ZERO-COPY RX RING BYPASS", argc, argv, [](const hft::common::EngineContext &ctx) {
            return std::make_unique<hft::worker::FixWorker>(ctx.queue, ctx.telemetry, ctx.log_filename,
                                                            ctx.total_messages, ctx.worker_cpu);
        });
}
