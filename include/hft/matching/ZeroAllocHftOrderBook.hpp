/**
 * @file ZeroAllocHftOrderBook.hpp
 * @brief Zero-Allocation High-Frequency Price-Time Priority Limit Order Book.
 */

#pragma once

#include "hft/protocol/ParsedOrder.hpp"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <array>
#include <algorithm>
#include <string_view>

namespace hft::matching
{
    constexpr size_t MAX_CL_ORD_ID_LEN = 32;
    constexpr size_t MAX_BOOK_LEVELS = 64;
    constexpr size_t MAX_ORDERS_PER_LEVEL = 32;
    constexpr size_t MAX_BOOK_ORDERS = 4096;

    /**
     * @struct ZeroAllocTrade
     * @brief Zero-allocation trade execution report structure.
     */
    struct ZeroAllocTrade
    {
        uint64_t trade_id{ 0 };
        char symbol[16]{ '\0' };
        char buy_cl_ord_id[MAX_CL_ORD_ID_LEN]{ '\0' };
        char sell_cl_ord_id[MAX_CL_ORD_ID_LEN]{ '\0' };
        int64_t match_price{ 0 };
        uint32_t match_qty{ 0 };
        uint64_t timestamp_ns{ 0 };
    };

    /**
     * @struct BookOrder
     * @brief Flat, cache-aligned (64 bytes) order node stored in the limit book.
     */
    struct alignas(64) BookOrder
    {
        uint64_t order_id{ 0 };
        char cl_ord_id[MAX_CL_ORD_ID_LEN]{ '\0' };
        int side{ 1 }; // 1 = Buy, 2 = Sell
        int64_t price{ 0 };
        uint32_t remaining_qty{ 0 };
        uint64_t timestamp_ns{ 0 };
        bool active{ false };
    };

    inline void copy_sv_to_char(char* dest, std::string_view sv, size_t max_len) noexcept
    {
        size_t len = std::min(sv.length(), max_len);
        std::memcpy(dest, sv.data(), len);
        dest[len] = '\0';
    }

    inline void copy_char_to_char(char* dest, const char* src, size_t max_len) noexcept
    {
        size_t len = 0;
        while (len < max_len && src[len] != '\0')
        {
            dest[len] = src[len];
            ++len;
        }
        dest[len] = '\0';
    }

    /**
     * @class ZeroAllocHftOrderBook
     * @brief High-performance, zero-allocation Limit Order Book for a single instrument.
     */
    class ZeroAllocHftOrderBook
    {
    private:
        char m_symbol[16]{ '\0' };

        struct PriceLevel
        {
            int64_t price{ 0 };
            std::array<BookOrder, MAX_ORDERS_PER_LEVEL> orders{};
            size_t order_count{ 0 };

            [[nodiscard]] uint32_t total_qty() const noexcept
            {
                uint32_t qty = 0;
                for (size_t i = 0; i < order_count; ++i)
                {
                    qty += orders[i].remaining_qty;
                }
                return qty;
            }
        };

        std::array<PriceLevel, MAX_BOOK_LEVELS> m_bids{}; // Sorted descending
        size_t m_bid_levels{ 0 };

        std::array<PriceLevel, MAX_BOOK_LEVELS> m_asks{}; // Sorted ascending
        size_t m_ask_levels{ 0 };

        uint64_t m_total_trades{ 0 };
        uint64_t m_total_volume{ 0 };

    public:
        explicit ZeroAllocHftOrderBook(const char* symbol = "PETR4") noexcept
        {
            copy_char_to_char(m_symbol, symbol, sizeof(m_symbol) - 1);
        }

        /**
         * @brief Submits a new limit order and performs Price-Time Priority matching.
         * @return Number of trade executions generated.
         */
        size_t submit_order(const hft::protocol::ParsedOrder& order,
            ZeroAllocTrade* out_trades,
            size_t max_trades,
            uint64_t& trade_counter,
            uint64_t timestamp_ns) noexcept
        {
            size_t trades_generated = 0;
            uint32_t remaining_qty = static_cast<uint32_t>(order.quantity);

            if (order.side == 1) // BUY ORDER
            {
                // Match against best Asks (Asks sorted ascending)
                while (m_ask_levels > 0 && remaining_qty > 0)
                {
                    auto& best_level = m_asks[0];
                    if (order.price < best_level.price) break; // Price limit reached

                    while (best_level.order_count > 0 && remaining_qty > 0)
                    {
                        auto& passive_order = best_level.orders[0];
                        uint32_t match_qty = std::min(remaining_qty, passive_order.remaining_qty);

                        if (trades_generated < max_trades && out_trades != nullptr)
                        {
                            auto& trade = out_trades[trades_generated++];
                            trade.trade_id = ++trade_counter;
                            copy_char_to_char(trade.symbol, m_symbol, sizeof(trade.symbol) - 1);
                            copy_sv_to_char(trade.buy_cl_ord_id, order.cl_ord_id, MAX_CL_ORD_ID_LEN - 1);
                            copy_char_to_char(trade.sell_cl_ord_id, passive_order.cl_ord_id, MAX_CL_ORD_ID_LEN - 1);
                            trade.match_price = passive_order.price; // Passive order pricing
                            trade.match_qty = match_qty;
                            trade.timestamp_ns = timestamp_ns;
                        }

                        m_total_trades++;
                        m_total_volume += match_qty;
                        remaining_qty -= match_qty;
                        passive_order.remaining_qty -= match_qty;

                        if (passive_order.remaining_qty == 0)
                        {
                            // Shift remaining orders in level left
                            for (size_t k = 0; k < best_level.order_count - 1; ++k)
                            {
                                best_level.orders[k] = best_level.orders[k + 1];
                            }
                            best_level.order_count--;
                        }
                    }

                    if (best_level.order_count == 0)
                    {
                        // Shift remaining ask levels left
                        for (size_t l = 0; l < m_ask_levels - 1; ++l)
                        {
                            m_asks[l] = m_asks[l + 1];
                        }
                        m_ask_levels--;
                    }
                }

                // Place un-matched residual quantity on Bids book
                if (remaining_qty > 0 && m_bid_levels < MAX_BOOK_LEVELS)
                {
                    add_to_book(m_bids, m_bid_levels, true, order, remaining_qty, timestamp_ns);
                }
            }
            else if (order.side == 2) // SELL ORDER
            {
                // Match against best Bids (Bids sorted descending)
                while (m_bid_levels > 0 && remaining_qty > 0)
                {
                    auto& best_level = m_bids[0];
                    if (order.price > best_level.price) break; // Price limit reached

                    while (best_level.order_count > 0 && remaining_qty > 0)
                    {
                        auto& passive_order = best_level.orders[0];
                        uint32_t match_qty = std::min(remaining_qty, passive_order.remaining_qty);

                        if (trades_generated < max_trades && out_trades != nullptr)
                        {
                            auto& trade = out_trades[trades_generated++];
                            trade.trade_id = ++trade_counter;
                            copy_char_to_char(trade.symbol, m_symbol, sizeof(trade.symbol) - 1);
                            copy_char_to_char(trade.buy_cl_ord_id, passive_order.cl_ord_id, MAX_CL_ORD_ID_LEN - 1);
                            copy_sv_to_char(trade.sell_cl_ord_id, order.cl_ord_id, MAX_CL_ORD_ID_LEN - 1);
                            trade.match_price = passive_order.price; // Passive order pricing
                            trade.match_qty = match_qty;
                            trade.timestamp_ns = timestamp_ns;
                        }

                        m_total_trades++;
                        m_total_volume += match_qty;
                        remaining_qty -= match_qty;
                        passive_order.remaining_qty -= match_qty;

                        if (passive_order.remaining_qty == 0)
                        {
                            // Shift remaining orders in level left
                            for (size_t k = 0; k < best_level.order_count - 1; ++k)
                            {
                                best_level.orders[k] = best_level.orders[k + 1];
                            }
                            best_level.order_count--;
                        }
                    }

                    if (best_level.order_count == 0)
                    {
                        // Shift remaining bid levels left
                        for (size_t l = 0; l < m_bid_levels - 1; ++l)
                        {
                            m_bids[l] = m_bids[l + 1];
                        }
                        m_bid_levels--;
                    }
                }

                // Place un-matched residual quantity on Asks book
                if (remaining_qty > 0 && m_ask_levels < MAX_BOOK_LEVELS)
                {
                    add_to_book(m_asks, m_ask_levels, false, order, remaining_qty, timestamp_ns);
                }
            }

            return trades_generated;
        }

        [[nodiscard]] int64_t best_bid_price() const noexcept { return m_bid_levels > 0 ? m_bids[0].price : 0; }
        [[nodiscard]] uint32_t best_bid_qty() const noexcept { return m_bid_levels > 0 ? m_bids[0].total_qty() : 0; }

        [[nodiscard]] int64_t best_ask_price() const noexcept { return m_ask_levels > 0 ? m_asks[0].price : 0; }
        [[nodiscard]] uint32_t best_ask_qty() const noexcept { return m_ask_levels > 0 ? m_asks[0].total_qty() : 0; }

        [[nodiscard]] size_t bid_levels_count() const noexcept { return m_bid_levels; }
        [[nodiscard]] size_t ask_levels_count() const noexcept { return m_ask_levels; }

        [[nodiscard]] int64_t get_bid_level_price(size_t index) const noexcept { return index < m_bid_levels ? m_bids[index].price : 0; }
        [[nodiscard]] uint32_t get_bid_level_qty(size_t index) const noexcept { return index < m_bid_levels ? m_bids[index].total_qty() : 0; }
        [[nodiscard]] uint32_t get_bid_level_order_count(size_t index) const noexcept { return index < m_bid_levels ? static_cast<uint32_t>(m_bids[index].order_count) : 0; }

        [[nodiscard]] int64_t get_ask_level_price(size_t index) const noexcept { return index < m_ask_levels ? m_asks[index].price : 0; }
        [[nodiscard]] uint32_t get_ask_level_qty(size_t index) const noexcept { return index < m_ask_levels ? m_asks[index].total_qty() : 0; }
        [[nodiscard]] uint32_t get_ask_level_order_count(size_t index) const noexcept { return index < m_ask_levels ? static_cast<uint32_t>(m_asks[index].order_count) : 0; }

        [[nodiscard]] uint64_t total_trades() const noexcept { return m_total_trades; }
        [[nodiscard]] uint64_t total_volume() const noexcept { return m_total_volume; }

    private:
        void add_to_book(std::array<PriceLevel, MAX_BOOK_LEVELS>& levels,
            size_t& level_count,
            bool is_bid,
            const hft::protocol::ParsedOrder& order,
            uint32_t qty,
            uint64_t timestamp_ns) noexcept
        {
            // Find existing price level
            for (size_t i = 0; i < level_count; ++i)
            {
                if (levels[i].price == order.price)
                {
                    if (levels[i].order_count < MAX_ORDERS_PER_LEVEL)
                    {
                        auto& node = levels[i].orders[levels[i].order_count++];
                        node.order_id = order.seq_num;
                        copy_sv_to_char(node.cl_ord_id, order.cl_ord_id, MAX_CL_ORD_ID_LEN - 1);
                        node.side = order.side;
                        node.price = order.price;
                        node.remaining_qty = qty;
                        node.timestamp_ns = timestamp_ns;
                        node.active = true;
                    }
                    return;
                }
            }

            // Create new price level in sorted order
            if (level_count < MAX_BOOK_LEVELS)
            {
                size_t insert_pos = level_count;
                for (size_t i = 0; i < level_count; ++i)
                {
                    bool is_better = is_bid ? (order.price > levels[i].price) : (order.price < levels[i].price);
                    if (is_better) { insert_pos = i; break; }
                }

                for (size_t j = level_count; j > insert_pos; --j)
                {
                    levels[j] = levels[j - 1];
                }

                auto& new_level = levels[insert_pos];
                new_level.price = order.price;
                new_level.order_count = 1;
                auto& node = new_level.orders[0];
                node.order_id = order.seq_num;
                copy_sv_to_char(node.cl_ord_id, order.cl_ord_id, MAX_CL_ORD_ID_LEN - 1);
                node.side = order.side;
                node.price = order.price;
                node.remaining_qty = qty;
                node.timestamp_ns = timestamp_ns;
                node.active = true;

                level_count++;
            }
        }
    };
}
