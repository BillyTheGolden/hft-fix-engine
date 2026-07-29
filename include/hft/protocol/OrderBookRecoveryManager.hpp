/**
 * @file OrderBookRecoveryManager.hpp
 * @brief Zero-allocation contiguous Flat Array Order Book & Snapshot/Incremental state recovery.
 * @details Replaces std::unordered_map with a contiguous L1-cache friendly FlatOrderBook array,
 *          eliminating heap node allocations, pointer chasing, and cache invalidation stalls.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <array>
#include <algorithm>

namespace hft::protocol
{
    /**
     * @enum class RecoveryState
     * @brief Lifecycle state of order book synchronization.
     */
    enum class RecoveryState
    {
        RECOVERING,   ///< Waiting for Snapshot (35=W); buffering incoming Incrementals (35=X).
        SYNCHRONIZED  ///< Snapshot applied and incrementals replayed; operating in real-time.
    };

    /**
     * @struct PriceLevel
     * @brief Simple order book price level entry.
     */
    struct PriceLevel
    {
        int64_t price_scaled{0};
        uint32_t quantity{0};
    };

    /**
     * @struct IncrementalUpdate
     * @brief Represents a FIX 35=X incremental book update payload.
     */
    struct IncrementalUpdate
    {
        uint64_t seq_num{0};
        std::string symbol;
        int64_t price_scaled{0};
        uint32_t quantity{0};
        char action{'0'}; // '0' = Add, '1' = Modify, '2' = Delete
    };

    /**
     * @struct BookSnapshot
     * @brief Represents a FIX 35=W full book snapshot payload.
     */
    struct BookSnapshot
    {
        uint64_t snapshot_last_seq{0};
        std::string symbol;
        std::vector<PriceLevel> bids;
        std::vector<PriceLevel> asks;
    };

    /**
     * @class FlatOrderBook
     * @brief Contiguous, zero-heap fixed-size array Limit Order Book side (Bids or Asks).
     * @tparam MaxLevels Maximum number of price levels supported (default 1024).
     * @tparam IsBid `true` for Bids (sorted descending), `false` for Asks (sorted ascending).
     */
    template <size_t MaxLevels = 1024, bool IsBid = true>
    class FlatOrderBook
    {
    public:
        FlatOrderBook() noexcept : m_count(0) {}

        /** @brief Clears all price levels without heap deallocation. */
        void clear() noexcept { m_count = 0; }

        /** @brief Returns current price level depth count. */
        [[nodiscard]] size_t size() const noexcept { return m_count; }

        /**
         * @brief Inserts a new price level or updates an existing one in sorted order.
         * @param price_scaled Price scaled by 1,000,000.
         * @param qty Level quantity.
         */
        void insert_or_update(int64_t price_scaled, uint32_t qty) noexcept
        {
            for (size_t i = 0; i < m_count; ++i)
            {
                if (m_levels[i].price_scaled == price_scaled)
                {
                    m_levels[i].quantity = qty;
                    return;
                }
                bool place_here = IsBid ? (price_scaled > m_levels[i].price_scaled) 
                                         : (price_scaled < m_levels[i].price_scaled);
                if (place_here)
                {
                    if (m_count < MaxLevels)
                    {
                        for (size_t j = m_count; j > i; --j)
                        {
                            m_levels[j] = m_levels[j - 1];
                        }
                        m_levels[i] = PriceLevel{price_scaled, qty};
                        ++m_count;
                    }
                    return;
                }
            }

            if (m_count < MaxLevels)
            {
                m_levels[m_count] = PriceLevel{price_scaled, qty};
                ++m_count;
            }
        }

        /**
         * @brief Erases a price level by shifting contiguous memory.
         * @param price_scaled Price scaled by 1,000,000 to remove.
         */
        void erase(int64_t price_scaled) noexcept
        {
            for (size_t i = 0; i < m_count; ++i)
            {
                if (m_levels[i].price_scaled == price_scaled)
                {
                    for (size_t j = i; j < m_count - 1; ++j)
                    {
                        m_levels[j] = m_levels[j + 1];
                    }
                    --m_count;
                    return;
                }
            }
        }

        /** @brief Returns pointer to underlying contiguous level array. */
        [[nodiscard]] const PriceLevel* data() const noexcept { return m_levels.data(); }

    private:
        std::array<PriceLevel, MaxLevels> m_levels{};
        size_t m_count{0};
    };

    /**
     * @class OrderBookRecoveryManager
     * @brief Manages Order Book state recovery by combining Snapshots (35=W) and Incremental Updates (35=X).
     */
    class OrderBookRecoveryManager
    {
    public:
        explicit OrderBookRecoveryManager(std::string symbol)
            : m_symbol(std::move(symbol)), m_state(RecoveryState::RECOVERING) {}

        /**
         * @brief Ingests an incoming FIX 35=X Incremental Update.
         * @param update Incremental update structure.
         * @return `true` if update was immediately applied, `false` if buffered for recovery.
         */
        bool process_incremental(const IncrementalUpdate& update)
        {
            if (m_state == RecoveryState::RECOVERING)
            {
                m_buffered_incrementals.push_back(update);
                m_incrementals_buffered_count++;
                return false;
            }

            // Normal synchronized state: apply directly to book
            apply_incremental(update);
            m_last_applied_seq = update.seq_num;
            return true;
        }

        /**
         * @brief Applies a FIX 35=W Full Book Snapshot and replays buffered incremental updates.
         * @param snapshot Full order book snapshot image.
         * @return Count of buffered incremental updates successfully replayed.
         */
        size_t apply_snapshot(const BookSnapshot& snapshot)
        {
            m_bids.clear();
            m_asks.clear();

            for (const auto& lvl : snapshot.bids)
            {
                m_bids.insert_or_update(lvl.price_scaled, lvl.quantity);
            }
            for (const auto& lvl : snapshot.asks)
            {
                m_asks.insert_or_update(lvl.price_scaled, lvl.quantity);
            }

            m_last_applied_seq = snapshot.snapshot_last_seq;
            m_snapshot_applied = true;

            // Replay phase: filter out incrementals already covered by snapshot, apply remaining
            size_t replayed_count = 0;
            size_t discarded_count = 0;

            for (const auto& inc : m_buffered_incrementals)
            {
                if (inc.seq_num <= snapshot.snapshot_last_seq)
                {
                    // Discard stale incremental (already reflected in snapshot)
                    discarded_count++;
                }
                else
                {
                    // Replay fresh incremental update
                    apply_incremental(inc);
                    m_last_applied_seq = inc.seq_num;
                    replayed_count++;
                }
            }

            m_incrementals_discarded_count = discarded_count;
            m_incrementals_replayed_count = replayed_count;
            m_buffered_incrementals.clear();
            m_state = RecoveryState::SYNCHRONIZED;

            return replayed_count;
        }

        /** @brief Triggers recovery mode (e.g., upon detecting packet loss). */
        void trigger_recovery() noexcept
        {
            m_state = RecoveryState::RECOVERING;
            m_buffered_incrementals.clear();
        }

        [[nodiscard]] RecoveryState state() const noexcept { return m_state; }
        [[nodiscard]] uint64_t last_applied_seq() const noexcept { return m_last_applied_seq; }
        [[nodiscard]] size_t incrementals_buffered_count() const noexcept { return m_incrementals_buffered_count; }
        [[nodiscard]] size_t incrementals_replayed_count() const noexcept { return m_incrementals_replayed_count; }
        [[nodiscard]] size_t incrementals_discarded_count() const noexcept { return m_incrementals_discarded_count; }
        [[nodiscard]] bool is_snapshot_applied() const noexcept { return m_snapshot_applied; }
        [[nodiscard]] size_t bid_depth() const noexcept { return m_bids.size(); }
        [[nodiscard]] size_t ask_depth() const noexcept { return m_asks.size(); }

    private:
        void apply_incremental(const IncrementalUpdate& update)
        {
            if (update.action == '0' || update.action == '1') // Add or Modify
            {
                m_bids.insert_or_update(update.price_scaled, update.quantity);
            }
            else if (update.action == '2') // Delete
            {
                m_bids.erase(update.price_scaled);
            }
        }

        std::string m_symbol;
        RecoveryState m_state{RecoveryState::RECOVERING};
        uint64_t m_last_applied_seq{0};
        bool m_snapshot_applied{false};

        FlatOrderBook<1024, true> m_bids;  ///< Contiguous bids array sorted descending.
        FlatOrderBook<1024, false> m_asks; ///< Contiguous asks array sorted ascending.
        std::vector<IncrementalUpdate> m_buffered_incrementals;

        size_t m_incrementals_buffered_count{0};
        size_t m_incrementals_replayed_count{0};
        size_t m_incrementals_discarded_count{0};
    };
} // namespace hft::protocol
