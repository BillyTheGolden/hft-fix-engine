/**
 * @file FixParser.hpp
 * @brief High-speed, zero-allocation and zero-mutation FIX protocol message parser.
 */

#pragma once

#include "hft/protocol/ParsedOrder.hpp"
#include <charconv>
#include <string_view>

namespace hft::protocol
{

    /**
     * @class FixParser
     * @brief Stateless, zero-heap-allocation, zero-mutation FIX tag extractor.
     * @details Adheres to Single Responsibility Principle (SRP). Decodes tags directly using std::from_chars
     *          without modifying the underlying buffer.
     */
    class FixParser
    {
      public:
        /**
         * @brief Parses a price string to a scaled 64-bit integer (scaled by 1,000,000) without float overhead.
         * @details Efficiently parses fractional price representations (e.g. "35.85") directly into
         *          micro-dollar integer representation (e.g. 35850000) using integer math. Consumes
         *          up to 6 decimal places.
         * @param start Pointer to the start of the price characters.
         * @param end Pointer to the end of the price characters.
         * @return Scaled 64-bit price value.
         */
        [[nodiscard]] static int64_t parse_fixed_point_price(const char *start, const char *end) noexcept
        {
            int64_t value = 0;
            const char *ptr = start;
            bool negative = false;
            if (ptr < end && *ptr == '-')
            {
                negative = true;
                ++ptr;
            }
            // Parse integer part
            int64_t integer_part = 0;
            while (ptr < end && *ptr >= '0' && *ptr <= '9')
            {
                integer_part = integer_part * 10 + (*ptr - '0');
                ++ptr;
            }
            value = integer_part * 1'000'000LL;

            if (ptr < end && *ptr == '.')
            {
                ++ptr; // Skip '.'
                int64_t multiplier = 100'000LL;
                while (ptr < end && *ptr >= '0' && *ptr <= '9' && multiplier > 0)
                {
                    value += (*ptr - '0') * multiplier;
                    multiplier /= 10;
                    ++ptr;
                }
                // Skip extra decimal digits beyond micro-dollar precision
                while (ptr < end && *ptr >= '0' && *ptr <= '9')
                {
                    ++ptr;
                }
            }
            return negative ? -value : value;
        }

        /**
         * @brief Parses a raw FIX message buffer in a read-only manner into a structured domain object.
         * @details Does not mutate the buffer (no null-terminator injection). Utilizes locale-independent
         * std::from_chars.
         * @param buffer Pointer to the raw UDP payload characters.
         * @param len Total number of valid bytes inside the @p buffer.
         * @return Fully populated `ParsedOrder` structure.
         */
        [[nodiscard]] static ParsedOrder parse_in_place(const char *buffer, uint32_t len) noexcept
        {
            ParsedOrder order{};
            if (buffer == nullptr || len == 0) [[unlikely]]
            {
                return order;
            }

            const char *ptr = buffer;
            const char *const end = buffer + len;

            while (ptr < end)
            {
                const char *tag_start = ptr;
                while (ptr < end && *ptr != '=')
                {
                    ++ptr;
                }
                if (ptr >= end)
                    break;

                int tag = 0;
                std::from_chars(tag_start, ptr, tag);
                ++ptr; // Skip '='

                const char *val_start = ptr;
                while (ptr < end && *ptr != '\x01' && *ptr != '|')
                {
                    ++ptr;
                }
                const char *val_end = ptr;

                // Map standard FIX tags to domain fields
                switch (tag)
                {
                case 35:
                    order.msg_type = std::string_view(val_start, static_cast<size_t>(val_end - val_start));
                    if (val_start < val_end)
                    {
                        order.msg_type_char = *val_start;
                    }
                    break;
                case 11:
                    order.cl_ord_id = std::string_view(val_start, static_cast<size_t>(val_end - val_start));
                    break;
                case 55:
                    order.symbol = std::string_view(val_start, static_cast<size_t>(val_end - val_start));
                    break;
                case 54:
                    std::from_chars(val_start, val_end, order.side);
                    break;
                case 38:
                    std::from_chars(val_start, val_end, order.quantity);
                    break;
                case 44:
                    order.price = parse_fixed_point_price(val_start, val_end);
                    break;
                case 34:
                    std::from_chars(val_start, val_end, order.seq_num);
                    break;
                default:
                    break;
                }

                if (ptr < end)
                {
                    ++ptr; // Skip delimiter '\x01' or '|'
                }
            }

            return order;
        }
    };

} // namespace hft::protocol
