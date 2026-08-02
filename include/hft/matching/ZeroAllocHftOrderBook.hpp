/**
 * @file ZeroAllocHftOrderBook.hpp
 * @brief Zero-Allocation High-Frequency Price-Time Priority Limit Order Book.
 */

#pragma once

#include "hft/protocol/ParsedOrder.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
        uint64_t trade_id{0};
        char symbol[16]{'\0'};
        char buy_cl_ord_id[MAX_CL_ORD_ID_LEN]{'\0'};
        char sell_cl_ord_id[MAX_CL_ORD_ID_LEN]{'\0'};
        int64_t match_price{0};
        uint32_t match_qty{0};
        uint64_t timestamp_ns{0};
    };

    /**
     * @struct BookOrder
     * @brief Flat, cache-aligned (64 bytes) order node stored in the limit book.
     */
    struct alignas(64) BookOrder
    {
        uint64_t order_id{0};
        char cl_ord_id[MAX_CL_ORD_ID_LEN]{'\0'};
        int side{1}; // 1 = Buy, 2 = Sell
        int64_t price{0};
        uint32_t remaining_qty{0};
        uint64_t timestamp_ns{0};
        bool active{false};
    };

    inline void copy_sv_to_char(char *dest, std::string_view sv, size_t max_len) noexcept
    {
        size_t len = std::min(sv.length(), max_len);
        std::memcpy(dest, sv.data(), len);
        dest[len] = '\0';
    }

    /**
     * @brief High-performance vectorised string copying wrapper.
     *
     * @details OPTIMIZATION (High Finding 1.6):
     *          Replaces the scalar character-by-character while loop (`while (len < max_len && src[len] != '\0')`)
     *          with `strnlen()` followed by `std::memcpy()`.
     *          Eliminates branch mispredictions inside the inner copy loop and enables compiler SIMD auto-vectorisation
     *          (`vmovdqu` / `memcpy`), significantly accelerating string copies during trade generation.
     */
    inline void copy_char_to_char(char *dest, const char *src, size_t max_len) noexcept
    {
        size_t len = strnlen(src, max_len);
        std::memcpy(dest, src, len);
        dest[len] = '\0';
    }

    /**
     * @class ZeroAllocHftOrderBook
     * @brief High-performance, zero-allocation Limit Order Book for a single instrument.
     */
    class ZeroAllocHftOrderBook
    {
      private:
        char m_symbol[16]{'\0'};

        /**
         * @struct PriceLevel
         * @brief Container representing an aggregated price level in the order book.
         *
         * @details OPTIMIZATION (High Finding 1.5):
         *          Maintains `cached_total_qty` incrementally on every order addition, match, and removal.
         *          `total_qty()` returns `cached_total_qty` directly in 1 CPU cycle (O(1)) instead of
         *          iterating and summing up to 32 `BookOrder` entries on every BBO query.
         */
        struct PriceLevel
        {
            int64_t price{0};
            std::array<BookOrder, MAX_ORDERS_PER_LEVEL> orders{};
            size_t order_count{0};
            uint32_t cached_total_qty{0};

            [[nodiscard]] uint32_t total_qty() const noexcept
            {
                return cached_total_qty;
            }
        };

        std::array<PriceLevel, MAX_BOOK_LEVELS> m_bids{}; // Sorted descending
        size_t m_bid_levels{0};

        std::array<PriceLevel, MAX_BOOK_LEVELS> m_asks{}; // Sorted ascending
        size_t m_ask_levels{0};

        uint64_t m_total_trades{0};
        uint64_t m_total_volume{0};

      public:
        explicit ZeroAllocHftOrderBook(std::string_view symbol = "PETR4") noexcept
        {
            copy_sv_to_char(m_symbol, symbol, sizeof(m_symbol) - 1);
        }

        /**
         * @brief Submits a new limit order and executes price-time priority matching.
         * @param order Parsed incoming FIX/OUCH/SBE order.
         * @param out_trades Pointer to output array for generated trade execution reports.
         * @param max_trades Maximum number of trades `out_trades` can hold.
         * @param trade_counter Reference to global trade execution ID counter.
         * @param timestamp_ns High-resolution execution timestamp.
         * @return Count of generated trade executions.
         */
        size_t submit_order(const hft::protocol::ParsedOrder &order, ZeroAllocTrade *out_trades, size_t max_trades,
                            uint64_t &trade_counter, uint64_t timestamp_ns) noexcept
        {
            size_t trades_generated = 0;
            uint32_t remaining_qty = static_cast<uint32_t>(order.quantity);

            if (order.side == 1) // BUY ORDER
            {
                // Match against best Asks (Asks sorted ascending)
                while (m_ask_levels > 0 && remaining_qty > 0)
                {
                    auto &best_level = m_asks[0];
                    if (order.price < best_level.price)
                        break; // Price limit reached

                    while (best_level.order_count > 0 && remaining_qty > 0)
                    {
                        auto &passive_order = best_level.orders[0];
                        uint32_t match_qty = std::min(remaining_qty, passive_order.remaining_qty);

                        if (trades_generated < max_trades && out_trades != nullptr)
                        {
                            auto &trade = out_trades[trades_generated++];
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
                        best_level.cached_total_qty -= match_qty;

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
                    auto &best_level = m_bids[0];
                    if (order.price > best_level.price)
                        break; // Price limit reached

                    while (best_level.order_count > 0 && remaining_qty > 0)
                    {
                        auto &passive_order = best_level.orders[0];
                        uint32_t match_qty = std::min(remaining_qty, passive_order.remaining_qty);

                        if (trades_generated < max_trades && out_trades != nullptr)
                        {
                            auto &trade = out_trades[trades_generated++];
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
                        best_level.cached_total_qty -= match_qty;

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

        [[nodiscard]] int64_t best_bid_price() const noexcept
        {
            return m_bid_levels > 0 ? m_bids[0].price : 0;
        }
        [[nodiscard]] uint32_t best_bid_qty() const noexcept
        {
            return m_bid_levels > 0 ? m_bids[0].total_qty() : 0;
        }

        [[nodiscard]] int64_t best_ask_price() const noexcept
        {
            return m_ask_levels > 0 ? m_asks[0].price : 0;
        }
        [[nodiscard]] uint32_t best_ask_qty() const noexcept
        {
            return m_ask_levels > 0 ? m_asks[0].total_qty() : 0;
        }

        [[nodiscard]] size_t bid_levels_count() const noexcept
        {
            return m_bid_levels;
        }
        [[nodiscard]] size_t ask_levels_count() const noexcept
        {
            return m_ask_levels;
        }

        [[nodiscard]] int64_t get_bid_level_price(size_t index) const noexcept
        {
            return index < m_bid_levels ? m_bids[index].price : 0;
        }
        [[nodiscard]] uint32_t get_bid_level_qty(size_t index) const noexcept
        {
            return index < m_bid_levels ? m_bids[index].total_qty() : 0;
        }
        [[nodiscard]] uint32_t get_bid_level_order_count(size_t index) const noexcept
        {
            return index < m_bid_levels ? static_cast<uint32_t>(m_bids[index].order_count) : 0;
        }

        [[nodiscard]] int64_t get_ask_level_price(size_t index) const noexcept
        {
            return index < m_ask_levels ? m_asks[index].price : 0;
        }
        [[nodiscard]] uint32_t get_ask_level_qty(size_t index) const noexcept
        {
            return index < m_ask_levels ? m_asks[index].total_qty() : 0;
        }
        [[nodiscard]] uint32_t get_ask_level_order_count(size_t index) const noexcept
        {
            return index < m_ask_levels ? static_cast<uint32_t>(m_asks[index].order_count) : 0;
        }

        [[nodiscard]] uint64_t total_trades() const noexcept
        {
            return m_total_trades;
        }
        [[nodiscard]] uint64_t total_volume() const noexcept
        {
            return m_total_volume;
        }

      private:
        /**
         * @brief Inserts an un-matched order into the order book price levels.
         *
         * @details OPTIMIZATION (High Finding 1.4):
         *          Uses O(log N) binary search (`low`/`high` bisection) to locate existing price levels or the
         *          insertion position. Requires at most 6 comparisons for 64 levels instead of up to 64 linear
         * comparisons.
         */
        void add_to_book(std::array<PriceLevel, MAX_BOOK_LEVELS> &levels, size_t &level_count, bool is_bid,
                         const hft::protocol::ParsedOrder &order, uint32_t qty, uint64_t timestamp_ns) noexcept
        {
            // Binary search for matching price level or insertion position
            size_t low = 0;
            size_t high = level_count;

            while (low < high)
            {
                size_t mid = low + (high - low) / 2;
                if (levels[mid].price == order.price)
                {
                    if (levels[mid].order_count < MAX_ORDERS_PER_LEVEL)
                    {
                        auto &node = levels[mid].orders[levels[mid].order_count++];
                        node.order_id = order.seq_num;
                        copy_sv_to_char(node.cl_ord_id, order.cl_ord_id, MAX_CL_ORD_ID_LEN - 1);
                        node.side = order.side;
                        node.price = order.price;
                        node.remaining_qty = qty;
                        node.timestamp_ns = timestamp_ns;
                        node.active = true;
                        levels[mid].cached_total_qty += qty;
                    }
                    return;
                }

                bool is_better = is_bid ? (order.price > levels[mid].price) : (order.price < levels[mid].price);
                if (is_better)
                {
                    high = mid;
                }
                else
                {
                    low = mid + 1;
                }
            }

            // Create new price level at insertion position
            size_t insert_pos = low;
            if (level_count < MAX_BOOK_LEVELS)
            {
                for (size_t j = level_count; j > insert_pos; --j)
                {
                    levels[j] = levels[j - 1];
                }

                auto &new_level = levels[insert_pos];
                new_level.price = order.price;
                new_level.order_count = 1;
                new_level.cached_total_qty = qty;

                auto &node = new_level.orders[0];
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
} // namespace hft::matching
