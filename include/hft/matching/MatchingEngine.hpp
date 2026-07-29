/**
 * @file MatchingEngine.hpp
 * @brief High-frequency Price-Time Priority Limit Order Matching Engine.
 */

#pragma once

#include "hft/protocol/ParsedOrder.hpp"
#include "hft/protocol/OrderBookRecoveryManager.hpp"
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <algorithm>
#include <unordered_map>

namespace hft::matching
{
    /**
     * @struct TradeExecution
     * @brief Structure representing a matched trade execution report (FIX 35=8).
     */
    struct TradeExecution
    {
        uint64_t trade_id{0};
        std::string symbol;
        std::string buy_cl_ord_id;
        std::string sell_cl_ord_id;
        int64_t match_price{0};
        uint32_t match_qty{0};
        uint64_t execution_timestamp_ns{0};
    };

    /**
     * @struct LimitOrderEntry
     * @brief Book order entry stored in limit order queue.
     */
    struct LimitOrderEntry
    {
        std::string cl_ord_id;
        int side{1}; // 1 = Buy, 2 = Sell
        int64_t price{0};
        uint32_t remaining_qty{0};
        uint64_t seq_num{0};
    };

    /**
     * @class MatchingBook
     * @brief Single-symbol L1/L2 limit order book with price-time priority matching.
     */
    class MatchingBook
    {
    public:
        explicit MatchingBook(std::string symbol)
            : m_symbol(std::move(symbol)) {}

        /**
         * @brief Submits a new limit order and attempts price-time priority matching.
         * @param order Incoming parsed FIX order.
         * @param trades Vector where generated trade executions are appended.
         * @param trade_counter Reference to global trade execution ID counter.
         * @param timestamp_ns Current execution timestamp in nanoseconds.
         * @return `true` if any trade matching occurred, `false` if placed entirely on book.
         */
        bool submit_order(const hft::protocol::ParsedOrder& order,
                          std::vector<TradeExecution>& trades,
                          uint64_t& trade_counter,
                          uint64_t timestamp_ns)
        {
            uint32_t remaining_qty = static_cast<uint32_t>(order.quantity);
            bool matched = false;

            if (order.side == 1) // BUY ORDER
            {
                // Match against best Asks (Asks sorted ascending by price)
                while (!m_asks.empty() && remaining_qty > 0)
                {
                    auto& best_ask = m_asks.front();
                    if (order.price < best_ask.price)
                    {
                        break; // Price priority threshold reached
                    }

                    uint32_t match_qty = std::min(remaining_qty, best_ask.remaining_qty);
                    
                    TradeExecution trade{};
                    trade.trade_id = ++trade_counter;
                    trade.symbol = m_symbol;
                    trade.buy_cl_ord_id = std::string(order.cl_ord_id);
                    trade.sell_cl_ord_id = best_ask.cl_ord_id;
                    trade.match_price = best_ask.price; // Passive order price rule
                    trade.match_qty = match_qty;
                    trade.execution_timestamp_ns = timestamp_ns;

                    trades.push_back(trade);
                    m_total_trades++;
                    m_total_volume += match_qty;
                    matched = true;

                    remaining_qty -= match_qty;
                    best_ask.remaining_qty -= match_qty;

                    if (best_ask.remaining_qty == 0)
                    {
                        m_asks.erase(m_asks.begin());
                    }
                }

                // Rest of buy order placed on Bids book
                if (remaining_qty > 0)
                {
                    LimitOrderEntry entry{
                        std::string(order.cl_ord_id),
                        order.side,
                        order.price,
                        remaining_qty,
                        order.seq_num
                    };

                    // Insert into Bids sorted descending by price
                    auto it = std::upper_bound(m_bids.begin(), m_bids.end(), entry,
                        [](const LimitOrderEntry& a, const LimitOrderEntry& b) {
                            return a.price > b.price;
                        });
                    m_bids.insert(it, entry);
                }
            }
            else if (order.side == 2) // SELL ORDER
            {
                // Match against best Bids (Bids sorted descending by price)
                while (!m_bids.empty() && remaining_qty > 0)
                {
                    auto& best_bid = m_bids.front();
                    if (order.price > best_bid.price)
                    {
                        break; // Price priority threshold reached
                    }

                    uint32_t match_qty = std::min(remaining_qty, best_bid.remaining_qty);

                    TradeExecution trade{};
                    trade.trade_id = ++trade_counter;
                    trade.symbol = m_symbol;
                    trade.buy_cl_ord_id = best_bid.cl_ord_id;
                    trade.sell_cl_ord_id = std::string(order.cl_ord_id);
                    trade.match_price = best_bid.price; // Passive order price rule
                    trade.match_qty = match_qty;
                    trade.execution_timestamp_ns = timestamp_ns;

                    trades.push_back(trade);
                    m_total_trades++;
                    m_total_volume += match_qty;
                    matched = true;

                    remaining_qty -= match_qty;
                    best_bid.remaining_qty -= match_qty;

                    if (best_bid.remaining_qty == 0)
                    {
                        m_bids.erase(m_bids.begin());
                    }
                }

                // Rest of sell order placed on Asks book
                if (remaining_qty > 0)
                {
                    LimitOrderEntry entry{
                        std::string(order.cl_ord_id),
                        order.side,
                        order.price,
                        remaining_qty,
                        order.seq_num
                    };

                    // Insert into Asks sorted ascending by price
                    auto it = std::upper_bound(m_asks.begin(), m_asks.end(), entry,
                        [](const LimitOrderEntry& a, const LimitOrderEntry& b) {
                            return a.price < b.price;
                        });
                    m_asks.insert(it, entry);
                }
            }

            return matched;
        }

        [[nodiscard]] size_t bid_depth() const noexcept { return m_bids.size(); }
        [[nodiscard]] size_t ask_depth() const noexcept { return m_asks.size(); }
        [[nodiscard]] uint64_t total_trades() const noexcept { return m_total_trades; }
        [[nodiscard]] uint64_t total_volume() const noexcept { return m_total_volume; }

    private:
        std::string m_symbol;
        std::vector<LimitOrderEntry> m_bids;
        std::vector<LimitOrderEntry> m_asks;
        uint64_t m_total_trades{0};
        uint64_t m_total_volume{0};
    };

    /**
     * @class MatchingEngine
     * @brief Multi-symbol Price-Time Priority Order Matching Engine router.
     */
    class MatchingEngine
    {
    public:
        MatchingEngine() = default;

        /**
         * @brief Submits order for matching to symbol-specific order book.
         * @param order Incoming parsed FIX order.
         * @param trades Vector where matched trade executions are returned.
         * @param timestamp_ns High-resolution timestamp.
         * @return `true` if a trade execution was generated.
         */
        bool process_order(const hft::protocol::ParsedOrder& order,
                           std::vector<TradeExecution>& trades,
                           uint64_t timestamp_ns)
        {
            std::string sym = std::string(order.symbol);
            if (sym.empty()) sym = "PETR4";

            auto it = m_books.find(sym);
            if (it == m_books.end())
            {
                auto [new_it, inserted] = m_books.emplace(sym, MatchingBook(sym));
                it = new_it;
            }

            return it->second.submit_order(order, trades, m_trade_counter, timestamp_ns);
        }

        [[nodiscard]] uint64_t total_trades() const noexcept
        {
            uint64_t count = 0;
            for (const auto& [sym, book] : m_books)
            {
                count += book.total_trades();
            }
            return count;
        }

        [[nodiscard]] uint64_t total_volume() const noexcept
        {
            uint64_t vol = 0;
            for (const auto& [sym, book] : m_books)
            {
                vol += book.total_volume();
            }
            return vol;
        }

    private:
        std::unordered_map<std::string, MatchingBook> m_books;
        uint64_t m_trade_counter{0};
    };
} // namespace hft::matching
