/**
 * @file TestArena.hpp
 * @brief Resilience Test Arena harness for validating HFT failure scenarios.
 */

#pragma once

#include "hft/protocol/SequenceGapDetector.hpp"
#include "hft/protocol/SequenceReorderBuffer.hpp"
#include "hft/common/ClockSynchronizer.hpp"
#include "hft/networking/FeedArbitrator.hpp"
#include "hft/protocol/OrderBookRecoveryManager.hpp"
#include "hft/protocol/PreTradeRiskManager.hpp"
#include "hft/common/CpuSchedulerBenchmark.hpp"
#include "hft/protocol/FixParser.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <chrono>

namespace hft::arena
{
    /**
     * @struct TestScenarioResult
     * @brief Detailed results and assertions for a resilience test scenario.
     */
    struct TestScenarioResult
    {
        std::string scenario_name;
        bool passed{false};
        size_t total_tests_run{0};
        size_t assertions_passed{0};
        size_t assertions_failed{0};
        double execution_time_ms{0.0};
        std::vector<std::string> log_details;
    };

    /**
     * @class TestArena
     * @brief Automated test harness for verifying HFT failure prevention mechanisms.
     */
    class TestArena
    {
    public:
        TestArena() = default;
        ~TestArena() = default;

        /**
         * @brief Runs Scenario #1: Packet Loss & Sequence Gap Detection.
         * @return `TestScenarioResult` containing pass/fail metrics and generated ResendRequest verification.
         */
        TestScenarioResult run_packet_loss_scenario();

        /**
         * @brief Runs Scenario #2: Out-of-Order Messages & Sequence Reordering Buffer.
         * @return `TestScenarioResult` verifying strict in-order message reconstruction.
         */
        TestScenarioResult run_out_of_order_scenario();

        /**
         * @brief Runs Scenario #3: Clock Synchronization & Nanosecond Timestamping.
         * @return `TestScenarioResult` verifying clock calibration, jump prevention, and UTC formatting.
         */
        TestScenarioResult run_clock_sync_scenario();

        /**
         * @brief Runs Scenario #4: Duplicate Feeds & Line A / Line B Arbitration.
         * @return `TestScenarioResult` verifying 100% de-duplication and sub-nanosecond feed arbitration.
         */
        TestScenarioResult run_duplicate_feeds_scenario();

        /**
         * @brief Runs Scenario #5: Order Book Recovery & Snapshot Sync.
         * @return `TestScenarioResult` verifying Snapshot (35=W) and Incremental (35=X) synchronization.
         */
        TestScenarioResult run_order_book_recovery_scenario();

        /**
         * @brief Runs Scenario #6: Pre-Trade Risk Validation & Fat-Finger Protection.
         * @return `TestScenarioResult` verifying inline risk check rules (Qty, Price Collar, Notional, Kill Switch).
         */
        TestScenarioResult run_risk_validation_scenario();

        /**
         * @brief Runs Scenario #7: CPU Scheduling & Deterministic Core Isolation Benchmark.
         * @return `TestScenarioResult` verifying thread pinning, SCHED_FIFO real-time priority, and CPU jitter determinism.
         */
        TestScenarioResult run_cpu_scheduling_scenario();

        /**
         * @brief Runs all implemented resilience scenarios sequentially.
         */
        void run_all_scenarios();
    };
} // namespace hft::arena
