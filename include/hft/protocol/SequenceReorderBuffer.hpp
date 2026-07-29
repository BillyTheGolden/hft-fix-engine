/**
 * @file SequenceReorderBuffer.hpp
 * @brief High-speed zero-allocation sequence reordering buffer for out-of-order UDP packets.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include <vector>
#include <string>
#include <algorithm>

namespace hft::protocol
{
    /**
     * @struct ReorderSlot
     * @brief Storage slot for out-of-order messages.
     */
    template <size_t MaxPayloadLen = 512>
    struct ReorderSlot
    {
        uint64_t seq_num{ 0 };
        uint32_t payload_len{ 0 };
        char payload[MaxPayloadLen]{ '\0' };
        bool occupied{ false };
    };

    /**
     * @class SequenceReorderBuffer
     * @brief Bounded zero-allocation circular reordering ring buffer.
     * @details Holds out-of-order packets arriving ahead of sequence and emits them in strict sequential order.
     * @tparam Capacity Maximum lookahead window for reordering (must be power of two, default 256).
     */
    template <size_t Capacity = 256>
    class SequenceReorderBuffer
    {
        static_assert((Capacity& (Capacity - 1)) == 0, "Capacity must be a power of two");

    public:
        explicit SequenceReorderBuffer(uint64_t initial_expected = 1) noexcept
            : m_expected_seq(initial_expected)
        {
            for (auto& slot : m_ring)
            {
                slot.occupied = false;
                slot.seq_num = 0;
            }
        }

        /**
         * @brief Processes an incoming packet with a sequence number.
         * @param seq_num Sequence number (tag 34) of incoming packet.
         * @param payload Pointer to raw packet payload bytes.
         * @param len Length of packet payload.
         * @param out_emitted Vector into which in-order payloads (current + buffered drained) are appended.
         * @return Count of in-order messages emitted.
         */
        size_t push(uint64_t seq_num, const char* payload, uint32_t len, std::vector<std::string>& out_emitted)
        {
            size_t emitted_count = 0;

            if (seq_num == m_expected_seq)
            {
                // In-order packet! Emit immediately
                if (payload && len > 0)
                {
                    out_emitted.emplace_back(payload, len);
                    emitted_count++;
                }
                m_expected_seq++;

                // Drain subsequent consecutive packets buffered in the ring
                emitted_count += drain_buffered(out_emitted);
            }
            else if (seq_num > m_expected_seq)
            {
                // Out-of-order packet arriving ahead of sequence!
                if (seq_num < m_expected_seq + Capacity)
                {
                    size_t index = seq_num & (Capacity - 1);
                    auto& slot = m_ring[index];

                    slot.seq_num = seq_num;
                    slot.payload_len = std::min(len, static_cast<uint32_t>(512));
                    if (payload && slot.payload_len > 0)
                    {
                        std::memcpy(slot.payload, payload, slot.payload_len);
                        slot.payload[slot.payload_len] = '\0';
                    }
                    slot.occupied = true;
                    m_out_of_order_buffered_count++;
                }
                else
                {
                    // Out-of-bounds lookahead window overflow
                    m_overflow_count++;
                }
            }
            else
            {
                // Late/Duplicate packet (seq_num < m_expected_seq)
                m_duplicates_count++;
            }

            return emitted_count;
        }

        /** @brief Resets state. */
        void reset(uint64_t initial_expected = 1) noexcept
        {
            m_expected_seq = initial_expected;
            m_out_of_order_buffered_count = 0;
            m_duplicates_count = 0;
            m_overflow_count = 0;
            for (auto& slot : m_ring)
            {
                slot.occupied = false;
                slot.seq_num = 0;
            }
        }

        [[nodiscard]] uint64_t expected_seq() const noexcept { return m_expected_seq; }
        [[nodiscard]] uint64_t out_of_order_buffered_count() const noexcept { return m_out_of_order_buffered_count; }
        [[nodiscard]] uint64_t duplicates_count() const noexcept { return m_duplicates_count; }
        [[nodiscard]] uint64_t overflow_count() const noexcept { return m_overflow_count; }

    private:
        size_t drain_buffered(std::vector<std::string>& out_emitted)
        {
            size_t drained = 0;
            while (true)
            {
                size_t index = m_expected_seq & (Capacity - 1);
                auto& slot = m_ring[index];
                if (slot.occupied && slot.seq_num == m_expected_seq)
                {
                    out_emitted.emplace_back(slot.payload, slot.payload_len);
                    slot.occupied = false;
                    m_expected_seq++;
                    drained++;

                    continue;
                }

                break;
            }

            return drained;
        }

        uint64_t m_expected_seq{ 1 };
        uint64_t m_out_of_order_buffered_count{ 0 };
        uint64_t m_duplicates_count{ 0 };
        uint64_t m_overflow_count{ 0 };
        std::array<ReorderSlot<512>, Capacity> m_ring{};
    };
} // namespace hft::protocol
