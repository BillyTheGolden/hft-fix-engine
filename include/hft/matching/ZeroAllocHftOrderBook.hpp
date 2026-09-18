/**
 * @file ZeroAllocHftOrderBook.hpp
 * @brief Zero-Allocation High-Frequency Price-Time Priority Limit Order Book.
 * @details Implements O(1) order pops via circular FIFO ring indexing within price levels,
 *          and pointer-indirected price level arrays backed by pre-allocated memory pools
 *          to eliminate hot-path memory shift cascades.
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
     * @brief Flat order node stored in the limit book.
     */
    struct BookOrder
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
         * @details Uses a circular FIFO order ring buffer (head_order_idx) to ensure O(1) order pops
         *          without shifting subsequent BookOrder array elements on execution.
         */
        struct PriceLevel
        {
            int64_t price{0};
            std::array<BookOrder, MAX_ORDERS_PER_LEVEL> orders{};
            size_t head_order_idx{0};
            size_t order_count{0};
            uint32_t cached_total_qty{0};

            [[nodiscard]] uint32_t total_qty() const noexcept
            {
                return cached_total_qty;
            }

            void reset() noexcept
            {
                price = 0;
                head_order_idx = 0;
                order_count = 0;
                cached_total_qty = 0;
            }

            [[nodiscard]] BookOrder &front_order() noexcept
            {
                return orders[head_order_idx];
            }

            [[nodiscard]] const BookOrder &front_order() const noexcept
            {
                return orders[head_order_idx];
            }

            void pop_front_order() noexcept
            {
                if (order_count > 0)
                {
                    head_order_idx = (head_order_idx + 1) % MAX_ORDERS_PER_LEVEL;
                    --order_count;
                    if (order_count == 0)
                    {
                        head_order_idx = 0;
                    }
                }
            }

            void push_back_order(uint64_t id, std::string_view cl_ord_id, int side, int64_t p, uint32_t qty,
                                 uint64_t ts) noexcept
            {
                if (order_count < MAX_ORDERS_PER_LEVEL)
                {
                    size_t idx = (head_order_idx + order_count) % MAX_ORDERS_PER_LEVEL;
                    auto &node = orders[idx];
                    node.order_id = id;
                    copy_sv_to_char(node.cl_ord_id, cl_ord_id, MAX_CL_ORD_ID_LEN - 1);
                    node.side = side;
                    node.price = p;
                    node.remaining_qty = qty;
                    node.timestamp_ns = ts;
                    node.active = true;
                    cached_total_qty += qty;
                    ++order_count;
                }
            }
        };

        // Pre-allocated pools and pointer indirection arrays for price levels
        std::array<PriceLevel, MAX_BOOK_LEVELS> m_bid_pool{};
        std::array<PriceLevel *, MAX_BOOK_LEVELS> m_bid_free_list{};
        size_t m_bid_free_count{MAX_BOOK_LEVELS};
        std::array<PriceLevel *, MAX_BOOK_LEVELS> m_bids{}; // 8-byte pointers sorted descending
        size_t m_bid_levels{0};

        std::array<PriceLevel, MAX_BOOK_LEVELS> m_ask_pool{};
        std::array<PriceLevel *, MAX_BOOK_LEVELS> m_ask_free_list{};
        size_t m_ask_free_count{MAX_BOOK_LEVELS};
        std::array<PriceLevel *, MAX_BOOK_LEVELS> m_asks{}; // 8-byte pointers sorted ascending
        size_t m_ask_levels{0};

        uint64_t m_total_trades{0};
        uint64_t m_total_volume{0};

        PriceLevel *acquire_bid_level() noexcept
        {
            if (m_bid_free_count == 0)
                return nullptr;
            PriceLevel *lvl = m_bid_free_list[--m_bid_free_count];
            lvl->reset();
            return lvl;
        }

        void release_bid_level(PriceLevel *lvl) noexcept
        {
            if (m_bid_free_count < MAX_BOOK_LEVELS && lvl != nullptr)
            {
                m_bid_free_list[m_bid_free_count++] = lvl;
            }
        }

        PriceLevel *acquire_ask_level() noexcept
        {
            if (m_ask_free_count == 0)
                return nullptr;
            PriceLevel *lvl = m_ask_free_list[--m_ask_free_count];
            lvl->reset();
            return lvl;
        }

        void release_ask_level(PriceLevel *lvl) noexcept
        {
            if (m_ask_free_count < MAX_BOOK_LEVELS && lvl != nullptr)
            {
                m_ask_free_list[m_ask_free_count++] = lvl;
            }
        }

      public:
        explicit ZeroAllocHftOrderBook(std::string_view symbol = "PETR4") noexcept
        {
            copy_sv_to_char(m_symbol, symbol, sizeof(m_symbol) - 1);
            for (size_t i = 0; i < MAX_BOOK_LEVELS; ++i)
            {
                m_bid_free_list[i] = &m_bid_pool[i];
                m_ask_free_list[i] = &m_ask_pool[i];
            }
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
                    PriceLevel *best_level = m_asks[0];
                    if (order.price < best_level->price)
                        break; // Price limit reached

                    while (best_level->order_count > 0 && remaining_qty > 0)
                    {
                        auto &passive_order = best_level->front_order();
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
                        best_level->cached_total_qty -= match_qty;

                        if (passive_order.remaining_qty == 0)
                        {
                            best_level->pop_front_order(); // O(1) circular buffer advance!
                        }
                    }

                    if (best_level->order_count == 0)
                    {
                        release_ask_level(best_level);
                        // Shift remaining ask level POINTERS left (8 bytes each, NOT 2,048 bytes!)
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
                    PriceLevel *best_level = m_bids[0];
                    if (order.price > best_level->price)
                        break; // Price limit reached

                    while (best_level->order_count > 0 && remaining_qty > 0)
                    {
                        auto &passive_order = best_level->front_order();
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
                        best_level->cached_total_qty -= match_qty;

                        if (passive_order.remaining_qty == 0)
                        {
                            best_level->pop_front_order(); // O(1) circular buffer advance!
                        }
                    }

                    if (best_level->order_count == 0)
                    {
                        release_bid_level(best_level);
                        // Shift remaining bid level POINTERS left (8 bytes each, NOT 2,048 bytes!)
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
            return m_bid_levels > 0 ? m_bids[0]->price : 0;
        }
        [[nodiscard]] uint32_t best_bid_qty() const noexcept
        {
            return m_bid_levels > 0 ? m_bids[0]->total_qty() : 0;
        }

        [[nodiscard]] int64_t best_ask_price() const noexcept
        {
            return m_ask_levels > 0 ? m_asks[0]->price : 0;
        }
        [[nodiscard]] uint32_t best_ask_qty() const noexcept
        {
            return m_ask_levels > 0 ? m_asks[0]->total_qty() : 0;
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
            return index < m_bid_levels ? m_bids[index]->price : 0;
        }
        [[nodiscard]] uint32_t get_bid_level_qty(size_t index) const noexcept
        {
            return index < m_bid_levels ? m_bids[index]->total_qty() : 0;
        }
        [[nodiscard]] uint32_t get_bid_level_order_count(size_t index) const noexcept
        {
            return index < m_bid_levels ? static_cast<uint32_t>(m_bids[index]->order_count) : 0;
        }

        [[nodiscard]] int64_t get_ask_level_price(size_t index) const noexcept
        {
            return index < m_ask_levels ? m_asks[index]->price : 0;
        }
        [[nodiscard]] uint32_t get_ask_level_qty(size_t index) const noexcept
        {
            return index < m_ask_levels ? m_asks[index]->total_qty() : 0;
        }
        [[nodiscard]] uint32_t get_ask_level_order_count(size_t index) const noexcept
        {
            return index < m_ask_levels ? static_cast<uint32_t>(m_asks[index]->order_count) : 0;
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
         * @details Locates existing level via binary search or allocates a new level node from the pool,
         *          shifting only 8-byte level pointers (avoiding 2 KB PriceLevel struct copies).
         */
        void add_to_book(std::array<PriceLevel *, MAX_BOOK_LEVELS> &levels, size_t &level_count, bool is_bid,
                         const hft::protocol::ParsedOrder &order, uint32_t qty, uint64_t timestamp_ns) noexcept
        {
            size_t low = 0;
            size_t high = level_count;

            while (low < high)
            {
                size_t mid = low + (high - low) / 2;
                if (levels[mid]->price == order.price)
                {
                    levels[mid]->push_back_order(order.seq_num, order.cl_ord_id, order.side, order.price, qty,
                                                 timestamp_ns);
                    return;
                }

                bool is_better = is_bid ? (order.price > levels[mid]->price) : (order.price < levels[mid]->price);
                if (is_better)
                {
                    high = mid;
                }
                else
                {
                    low = mid + 1;
                }
            }

            // Create new price level from pool at insertion position
            size_t insert_pos = low;
            if (level_count < MAX_BOOK_LEVELS)
            {
                PriceLevel *new_level = is_bid ? acquire_bid_level() : acquire_ask_level();
                if (new_level == nullptr)
                    return;

                // Shift pointer array right (8 bytes each instead of 2 KB PriceLevel structs!)
                for (size_t j = level_count; j > insert_pos; --j)
                {
                    levels[j] = levels[j - 1];
                }

                levels[insert_pos] = new_level;
                new_level->price = order.price;
                new_level->push_back_order(order.seq_num, order.cl_ord_id, order.side, order.price, qty, timestamp_ns);
                level_count++;
            }
        }
    };
} // namespace hft::matching
