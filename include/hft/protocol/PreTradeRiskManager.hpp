/**
 * @file PreTradeRiskManager.hpp
 * @brief High-speed zero-allocation inline Pre-Trade Risk Manager & Fat-Finger Protection Gate.
 */

#pragma once

#include "hft/protocol/ParsedOrder.hpp"
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <atomic>

namespace hft::protocol
{
    /**
     * @enum class RiskCheckStatus
     * @brief Pre-trade risk evaluation status code.
     */
    enum class RiskCheckStatus : uint8_t
    {
        APPROVED = 0,
        REJECTED_MAX_QTY,
        REJECTED_PRICE_COLLAR,
        REJECTED_MAX_NOTIONAL,
        REJECTED_KILL_SWITCH,
        REJECTED_TRADING_HALT
    };

    /**
     * @struct RiskCheckResult
     * @brief Pre-allocated result structure from risk evaluation.
     */
    struct RiskCheckResult
    {
        RiskCheckStatus status{RiskCheckStatus::APPROVED};
        char rejection_reason[128]{'\0'};
    };

    /**
     * @struct RiskLimits
     * @brief Configurable pre-trade risk thresholds.
     */
    struct RiskLimits
    {
        uint32_t max_order_qty{50000};                           ///< Max allowed order quantity (shares/contracts).
        int64_t max_price_deviation_scaled{5000000};             ///< Max price deviation ($5.00 in micro-dollars).
        uint64_t max_notional_value_usd{2000000ULL};             ///< Max order notional value in USD ($2,000,000).
        bool kill_switch_active{false};                          ///< Emergency kill switch status.
    };

    /**
     * @class PreTradeRiskManager
     * @brief Zero-allocation inline risk validator executed in the critical path before order execution.
     */
    class PreTradeRiskManager
    {
    public:
        explicit PreTradeRiskManager(RiskLimits limits = {}) noexcept
            : m_limits(limits) {}

        /**
         * @brief Evaluates an incoming parsed order against pre-trade risk limits in sub-10 nanoseconds.
         * @param order Parsed FIX order details.
         * @param reference_price_scaled Current market reference price scaled by 1,000,000.
         * @return `RiskCheckResult` indicating approval or risk rejection reason.
         */
        [[nodiscard]] RiskCheckResult evaluate_order(const ParsedOrder& order, int64_t reference_price_scaled) noexcept
        {
            RiskCheckResult result{};

            // 1. Emergency Kill Switch Check
            if (m_limits.kill_switch_active)
            {
                result.status = RiskCheckStatus::REJECTED_KILL_SWITCH;
                std::snprintf(result.rejection_reason, sizeof(result.rejection_reason),
                              "REJECTED: Global Emergency Kill Switch is ACTIVE");
                m_rejected_count++;
                return result;
            }

            // 2. Max Order Quantity Check (Fat-Finger Quantity Guard)
            if (order.quantity > 0 && static_cast<uint32_t>(order.quantity) > m_limits.max_order_qty)
            {
                result.status = RiskCheckStatus::REJECTED_MAX_QTY;
                std::snprintf(result.rejection_reason, sizeof(result.rejection_reason),
                              "REJECTED: Qty %d exceeds max allowed limit %u",
                              order.quantity, m_limits.max_order_qty);
                m_rejected_count++;
                return result;
            }

            // 3. Price Collar Check (Fat-Finger Price Deviation Guard)
            if (reference_price_scaled > 0)
            {
                int64_t dev = (order.price > reference_price_scaled) 
                    ? (order.price - reference_price_scaled) 
                    : (reference_price_scaled - order.price);

                if (dev > m_limits.max_price_deviation_scaled)
                {
                    result.status = RiskCheckStatus::REJECTED_PRICE_COLLAR;
                    std::snprintf(result.rejection_reason, sizeof(result.rejection_reason),
                                  "REJECTED: Price $%.2f deviates beyond collar limit $%.2f from ref $%.2f",
                                  static_cast<double>(order.price) / 1000000.0,
                                  static_cast<double>(m_limits.max_price_deviation_scaled) / 1000000.0,
                                  static_cast<double>(reference_price_scaled) / 1000000.0);
                    m_rejected_count++;
                    return result;
                }
            }

            // 4. Max Notional Value Check (Quantity * Price)
            uint64_t notional_usd = (static_cast<uint64_t>(order.quantity) * static_cast<uint64_t>(order.price)) / 1000000ULL;
            if (notional_usd > m_limits.max_notional_value_usd)
            {
                result.status = RiskCheckStatus::REJECTED_MAX_NOTIONAL;
                std::snprintf(result.rejection_reason, sizeof(result.rejection_reason),
                              "REJECTED: Order notional value $%llu exceeds max cap $%llu",
                              static_cast<unsigned long long>(notional_usd),
                              static_cast<unsigned long long>(m_limits.max_notional_value_usd));
                m_rejected_count++;
                return result;
            }

            // Order Approved!
            result.status = RiskCheckStatus::APPROVED;
            m_approved_count++;
            return result;
        }

        /** @brief Activates or deactivates emergency kill switch. */
        void set_kill_switch(bool active) noexcept { m_limits.kill_switch_active = active; }
        
        /** @brief Updates risk limits dynamically. */
        void set_limits(const RiskLimits& limits) noexcept { m_limits = limits; }

        [[nodiscard]] uint64_t approved_count() const noexcept { return m_approved_count; }
        [[nodiscard]] uint64_t rejected_count() const noexcept { return m_rejected_count; }
        [[nodiscard]] const RiskLimits& limits() const noexcept { return m_limits; }

    private:
        RiskLimits m_limits{};
        uint64_t m_approved_count{0};
        uint64_t m_rejected_count{0};
    };
} // namespace hft::protocol
