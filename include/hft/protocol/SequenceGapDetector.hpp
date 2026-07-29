/**
 * @file SequenceGapDetector.hpp
 * @brief High-speed zero-allocation sequence gap detector and FIX ResendRequest (35=2) generator.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <chrono>

namespace hft::protocol
{
    /**
     * @struct SequenceGap
     * @brief Represents a detected missing sequence gap range.
     */
    struct SequenceGap
    {
        uint64_t begin_seq{0};
        uint64_t end_seq{0};
        uint64_t missing_count{0};
    };

    /**
     * @enum class GapStatus
     * @brief Result status of checking an incoming sequence number.
     */
    enum class GapStatus
    {
        IN_SEQUENCE,  ///< Message arrived in exact sequential order.
        GAP_DETECTED, ///< One or more packets were dropped/lost.
        DUPLICATE     ///< Message sequence number is older than expected (duplicate or retransmitted).
    };

    /**
     * @struct GapCheckResult
     * @brief Outcome of a sequence number evaluation.
     */
    struct GapCheckResult
    {
        GapStatus status{GapStatus::IN_SEQUENCE};
        SequenceGap gap{};
        char resend_request_msg[256]{'\0'};
        size_t resend_request_len{0};
    };

    /**
     * @class SequenceGapDetector
     * @brief Tracks incoming FIX MsgSeqNum (tag 34) to detect packet loss and construct FIX 35=2 ResendRequests.
     */
    class SequenceGapDetector
    {
    public:
        /**
         * @brief Constructs a gap detector starting at expected sequence number 1 (or custom value).
         * @param initial_expected Starting sequence number expected from the sender.
         */
        explicit SequenceGapDetector(uint64_t initial_expected = 1) noexcept
            : m_expected_seq(initial_expected) {}

        /**
         * @brief Evaluates an incoming sequence number for packet loss.
         * @param incoming_seq The `MsgSeqNum` (tag 34) parsed from the incoming FIX message.
         * @return `GapCheckResult` containing status, gap details, and pre-formatted FIX ResendRequest if a gap was found.
         */
        [[nodiscard]] GapCheckResult process_sequence(uint64_t incoming_seq) noexcept
        {
            GapCheckResult result{};

            if (incoming_seq == m_expected_seq)
            {
                result.status = GapStatus::IN_SEQUENCE;
                ++m_expected_seq;
            }
            else if (incoming_seq > m_expected_seq)
            {
                // Packet loss detected! Gap range is [m_expected_seq, incoming_seq - 1]
                result.status = GapStatus::GAP_DETECTED;
                result.gap.begin_seq = m_expected_seq;
                result.gap.end_seq = incoming_seq - 1;
                result.gap.missing_count = incoming_seq - m_expected_seq;

                m_gaps_detected++;
                m_packets_lost += result.gap.missing_count;

                // Advance expected sequence to incoming_seq + 1
                m_expected_seq = incoming_seq + 1;

                // Format FIX 4.2 ResendRequest (35=2) payload:
                // 8=FIX.4.2\x019=78\x0135=2\x0149=HFT_ENGINE\x0156=EXCHANGE\x017=begin_seq\x0116=end_seq\x0110=128\x01
                int len = std::snprintf(
                    result.resend_request_msg, sizeof(result.resend_request_msg),
                    "8=FIX.4.2\x01" "9=78\x01" "35=2\x01" "49=HFT_ENGINE\x01" "56=EXCHANGE\x01"
                    "7=%llu\x01" "16=%llu\x01" "10=128\x01",
                    static_cast<unsigned long long>(result.gap.begin_seq),
                    static_cast<unsigned long long>(result.gap.end_seq)
                );

                if (len > 0)
                {
                    result.resend_request_len = static_cast<size_t>(len);
                }
            }
            else
            {
                // Duplicate or late message
                result.status = GapStatus::DUPLICATE;
                m_duplicates_count++;
            }

            return result;
        }

        /** @brief Resets expected sequence number and counters. */
        void reset(uint64_t next_expected = 1) noexcept
        {
            m_expected_seq = next_expected;
            m_gaps_detected = 0;
            m_packets_lost = 0;
            m_duplicates_count = 0;
        }

        [[nodiscard]] uint64_t expected_seq() const noexcept { return m_expected_seq; }
        [[nodiscard]] uint64_t gaps_detected() const noexcept { return m_gaps_detected; }
        [[nodiscard]] uint64_t packets_lost() const noexcept { return m_packets_lost; }
        [[nodiscard]] uint64_t duplicates_count() const noexcept { return m_duplicates_count; }

    private:
        uint64_t m_expected_seq{1};
        uint64_t m_gaps_detected{0};
        uint64_t m_packets_lost{0};
        uint64_t m_duplicates_count{0};
    };
} // namespace hft::protocol
