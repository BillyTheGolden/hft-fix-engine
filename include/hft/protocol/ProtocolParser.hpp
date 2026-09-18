/**
 * @file ProtocolParser.hpp
 * @brief Unified multi-protocol zero-copy parser supporting FIX 4.2 (ASCII), Nasdaq OUCH 5.0 (Binary), and CME SBE
 * (Binary).
 */

#pragma once

#include "hft/protocol/FixParser.hpp"
#include "hft/protocol/ParsedOrder.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace hft::protocol
{
    /**
     * @enum class ProtocolType
     * @brief Supported HFT trade entry protocols.
     */
    enum class ProtocolType
    {
        FIX,  ///< ASCII Tag=Value (FIX 4.2)
        OUCH, ///< Nasdaq OUCH 5.0 Fixed-width Binary C-Struct
        SBE   ///< CME iLink 3 Simple Binary Encoding (SBE)
    };

    /**
     * @brief Zero-allocation, constexpr case-insensitive substring search.
     */
    inline constexpr bool contains_ci(std::string_view haystack, std::string_view needle) noexcept
    {
        if (needle.empty())
            return true;
        if (haystack.size() < needle.size())
            return false;
        for (size_t i = 0; i <= haystack.size() - needle.size(); ++i)
        {
            bool match = true;
            for (size_t j = 0; j < needle.size(); ++j)
            {
                char h = haystack[i + j];
                char n = needle[j];
                if (h >= 'A' && h <= 'Z')
                    h = static_cast<char>(h + ('a' - 'A'));
                if (n >= 'A' && n <= 'Z')
                    n = static_cast<char>(n + ('a' - 'A'));
                if (h != n)
                {
                    match = false;
                    break;
                }
            }
            if (match)
                return true;
        }
        return false;
    }

    /**
     * @brief Parses and auto-detects the ProtocolType from a string name or input dataset file path.
     * @param name String identifier or dataset file path (e.g., "ouch", "sbe", "./ouch_messages_10m.data").
     * @return `ProtocolType::OUCH` if "ouch" is found, `ProtocolType::SBE` if "sbe" is found, otherwise
     * `ProtocolType::FIX`.
     */
    inline ProtocolType parse_protocol_type(std::string_view name) noexcept
    {
        if (contains_ci(name, "ouch"))
            return ProtocolType::OUCH;
        if (contains_ci(name, "sbe"))
            return ProtocolType::SBE;
        return ProtocolType::FIX;
    }

    inline const char *protocol_type_to_string(ProtocolType type) noexcept
    {
        switch (type)
        {
        case ProtocolType::OUCH:
            return "OUCH (Nasdaq Binary)";
        case ProtocolType::SBE:
            return "SBE (CME iLink 3 Binary)";
        case ProtocolType::FIX:
        default:
            return "FIX 4.2 (ASCII Tag=Value)";
        }
    }

#pragma pack(push, 1)
    /**
     * @struct OuchEnterOrderPacket
     * @brief Nasdaq OUCH 5.0 Binary Enter Order packet format (46 bytes).
     */
    struct OuchEnterOrderPacket
    {
        char packet_type;      // 'O' = Enter Order
        uint64_t seq_num;      // MsgSeqNum (8 bytes)
        char cl_ord_id[14];    // ClOrdID padded string (14 bytes)
        char side;             // 'B' = Buy (1), 'S' = Sell (2)
        uint32_t quantity;     // Order Quantity (4 bytes)
        uint32_t price_scaled; // Fixed-point price scaled by 1,000,000 (4 bytes)
        char symbol[6];        // Ticker symbol padded (6 bytes)
        uint64_t timestamp_ns; // Timestamp in nanoseconds (8 bytes)
    };

    /**
     * @struct SbeHeader
     * @brief CME SBE Message Framing Header (8 bytes).
     */
    struct SbeHeader
    {
        uint16_t block_length;
        uint16_t template_id; // 514 = NewOrderSingle
        uint16_t schema_id;
        uint16_t version;
    };

    /**
     * @struct SbeNewOrderSinglePacket
     * @brief CME iLink 3 SBE New Order Single packet format (44 bytes).
     */
    struct SbeNewOrderSinglePacket
    {
        SbeHeader header;
        uint64_t seq_num;       // MsgSeqNum (8 bytes)
        uint64_t cl_ord_id_num; // Numeric ClOrdID (8 bytes)
        uint64_t price_scaled;  // Fixed-point price scaled by 1,000,000 (8 bytes)
        uint32_t quantity;      // Order Quantity (4 bytes)
        uint8_t side;           // 1 = Buy, 2 = Sell (1 byte)
        char symbol[8];         // Ticker symbol padded (8 bytes)
        uint8_t time_in_force;  // 0 = Day (1 byte)
    };
#pragma pack(pop)

    inline std::string_view trim_right(std::string_view sv) noexcept
    {
        size_t len = sv.length();
        while (len > 0 && (sv[len - 1] == ' ' || sv[len - 1] == '\0'))
        {
            --len;
        }
        return sv.substr(0, len);
    }

    /**
     * @class ProtocolParser
     * @brief High-speed zero-allocation parser for FIX, OUCH, and SBE protocols.
     */
    class ProtocolParser
    {
      public:
        /**
         * @brief Zero-allocation in-place parsing of FIX, OUCH, or SBE message payload.
         * @param type Target protocol format.
         * @param payload Raw memory buffer pointer.
         * @param len Buffer length in bytes.
         * @return ParsedOrder normalized internal representation.
         */
        [[nodiscard]] static ParsedOrder parse_in_place(ProtocolType type, const char *payload, size_t len) noexcept
        {
            if (payload == nullptr || len == 0)
            {
                return ParsedOrder{};
            }

            switch (type)
            {
            case ProtocolType::OUCH: {
                if (len < sizeof(OuchEnterOrderPacket))
                {
                    return ParsedOrder{};
                }

                // Portable, strict-aliasing compliant copy into local stack struct
                // (Compilers auto-vectorize fixed-size memcpy into zero-cost register moves)
                OuchEnterOrderPacket packet{};
                std::memcpy(&packet, payload, sizeof(OuchEnterOrderPacket));

                ParsedOrder order{};
                order.msg_type = "D";
                order.msg_type_char = 'D';
                order.seq_num = packet.seq_num;
                order.cl_ord_id = trim_right(
                    std::string_view(payload + offsetof(OuchEnterOrderPacket, cl_ord_id), sizeof(packet.cl_ord_id)));
                order.symbol = trim_right(
                    std::string_view(payload + offsetof(OuchEnterOrderPacket, symbol), sizeof(packet.symbol)));
                order.side = (packet.side == 'B' || packet.side == '1') ? 1 : 2;
                order.quantity = static_cast<int>(packet.quantity);
                order.price = static_cast<int64_t>(packet.price_scaled);
                return order;
            }

            case ProtocolType::SBE: {
                if (len < sizeof(SbeNewOrderSinglePacket))
                {
                    return ParsedOrder{};
                }

                // Portable, strict-aliasing compliant copy into local stack struct
                SbeNewOrderSinglePacket packet{};
                std::memcpy(&packet, payload, sizeof(SbeNewOrderSinglePacket));

                ParsedOrder order{};
                order.msg_type = "D";
                order.msg_type_char = 'D';
                order.seq_num = packet.seq_num;

                // Fast zero-allocation format of numeric cl_ord_id_num into inline buffer
                auto [ptr, ec] = std::to_chars(
                    order.cl_ord_id_buf, order.cl_ord_id_buf + sizeof(order.cl_ord_id_buf) - 1, packet.cl_ord_id_num);
                if (ec == std::errc{})
                {
                    *ptr = '\0';
                    order.cl_ord_id =
                        std::string_view(order.cl_ord_id_buf, static_cast<size_t>(ptr - order.cl_ord_id_buf));
                }

                order.symbol = trim_right(
                    std::string_view(payload + offsetof(SbeNewOrderSinglePacket, symbol), sizeof(packet.symbol)));
                order.side = static_cast<int>(packet.side);
                order.quantity = static_cast<int>(packet.quantity);
                order.price = static_cast<int64_t>(packet.price_scaled);
                return order;
            }

            case ProtocolType::FIX:
            default: {
                return FixParser::parse_in_place(payload, static_cast<uint32_t>(len));
            }
            }
        }
    };
} // namespace hft::protocol
