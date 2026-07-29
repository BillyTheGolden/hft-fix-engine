/**
 * @file FeedArbitrator.hpp
 * @brief High-speed zero-allocation Active-Active Line A / Line B dual feed arbitrator.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <array>
#include <string>
#include <atomic>

namespace hft::networking
{
    /**
     * @enum class FeedLine
     * @brief Source network line identifier.
     */
    enum class FeedLine : uint8_t
    {
        LINE_A = 0,
        LINE_B = 1
    };

    /**
     * @struct ArbitrationResult
     * @brief Outcome of feed arbitration for an incoming packet.
     */
    struct ArbitrationResult
    {
        bool is_duplicate{false};  ///< True if packet was already processed from the opposing line.
        FeedLine winner_line{FeedLine::LINE_A}; ///< Line that won the race for this sequence number.
        uint64_t seq_num{0};
    };

    /**
     * @class FeedArbitrator
     * @brief Performs $O(1)$ sub-nanosecond de-duplication across redundant Line A and Line B market feeds.
     * @tparam WindowSize Size of sliding window sequence tracking array (default 65536).
     */
    template <size_t WindowSize = 65536>
    class FeedArbitrator
    {
        static_assert((WindowSize & (WindowSize - 1)) == 0, "WindowSize must be a power of two");

    public:
        FeedArbitrator() noexcept
        {
            m_seen_sequences.fill(0);
        }

        /**
         * @brief Evaluates an incoming packet from Line A or Line B.
         * @param line Source feed line (LINE_A or LINE_B).
         * @param seq_num Sequence number (tag 34) of incoming packet.
         * @return `ArbitrationResult` indicating whether packet is accepted or dropped as duplicate.
         */
        [[nodiscard]] ArbitrationResult process_packet(FeedLine line, uint64_t seq_num) noexcept
        {
            ArbitrationResult result{};
            result.seq_num = seq_num;

            size_t idx = seq_num & (WindowSize - 1);
            uint64_t seen_seq = m_seen_sequences[idx];

            if (seen_seq == seq_num)
            {
                // Duplicate! The opposing line already delivered this exact sequence number first.
                result.is_duplicate = true;
                result.winner_line = m_winner_lines[idx];
                m_duplicates_suppressed++;
            }
            else
            {
                // First arrival wins the race!
                result.is_duplicate = false;
                result.winner_line = line;

                m_seen_sequences[idx] = seq_num;
                m_winner_lines[idx] = line;

                if (line == FeedLine::LINE_A)
                {
                    m_line_a_wins++;
                }
                else
                {
                    m_line_b_wins++;
                }
                m_unique_processed++;
            }

            return result;
        }

        /** @brief Resets arbitrator state and counters. */
        void reset() noexcept
        {
            m_seen_sequences.fill(0);
            m_winner_lines.fill(FeedLine::LINE_A);
            m_unique_processed = 0;
            m_duplicates_suppressed = 0;
            m_line_a_wins = 0;
            m_line_b_wins = 0;
        }

        [[nodiscard]] uint64_t unique_processed() const noexcept { return m_unique_processed; }
        [[nodiscard]] uint64_t duplicates_suppressed() const noexcept { return m_duplicates_suppressed; }
        [[nodiscard]] uint64_t line_a_wins() const noexcept { return m_line_a_wins; }
        [[nodiscard]] uint64_t line_b_wins() const noexcept { return m_line_b_wins; }

    private:
        std::array<uint64_t, WindowSize> m_seen_sequences{};
        std::array<FeedLine, WindowSize> m_winner_lines{};
        uint64_t m_unique_processed{0};
        uint64_t m_duplicates_suppressed{0};
        uint64_t m_line_a_wins{0};
        uint64_t m_line_b_wins{0};
    };
} // namespace hft::networking
