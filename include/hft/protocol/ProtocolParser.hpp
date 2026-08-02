/**
 * @file ProtocolParser.hpp
 * @brief Unified multi-protocol zero-copy parser supporting FIX 4.2 (ASCII), Nasdaq OUCH 5.0 (Binary), and CME SBE
 * (Binary).
 */

#pragma once

#include "hft/protocol/FixParser.hpp"
#include "hft/protocol/ParsedOrder.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
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
     * @brief Parses and auto-detects the ProtocolType from a string name or input dataset file path.
     * @param name String identifier or dataset file path (e.g., "ouch", "sbe", "./ouch_messages_10m.data").
     * @return `ProtocolType::OUCH` if "ouch" is found, `ProtocolType::SBE` if "sbe" is found, otherwise
     * `ProtocolType::FIX`.
     */
    inline ProtocolType parse_protocol_type(std::string_view name) noexcept
    {
        std::string s(name);
        for (char &c : s)
        {
            c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        }

        if (s.find("ouch") != std::string::npos)
            return ProtocolType::OUCH;
        if (s.find("sbe") != std::string::npos)
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

                // Zero-Copy Direct C-Struct Pointer Cast! O(1) Parsing
                const auto *packet = reinterpret_cast<const OuchEnterOrderPacket *>(payload);

                ParsedOrder order{};
                order.msg_type = "D";
                order.msg_type_char = 'D';
                order.seq_num = packet->seq_num;
                order.cl_ord_id = trim_right(std::string_view(packet->cl_ord_id, 14));
                order.symbol = trim_right(std::string_view(packet->symbol, 6));
                order.side = (packet->side == 'B' || packet->side == '1') ? 1 : 2;
                order.quantity = packet->quantity;
                order.price = static_cast<int64_t>(packet->price_scaled);
                return order;
            }

            case ProtocolType::SBE: {
                if (len < sizeof(SbeNewOrderSinglePacket))
                {
                    return ParsedOrder{};
                }

                // Zero-Copy Direct C-Struct Pointer Cast! O(1) Parsing
                const auto *packet = reinterpret_cast<const SbeNewOrderSinglePacket *>(payload);

                ParsedOrder order{};
                order.msg_type = "D";
                order.msg_type_char = 'D';
                order.seq_num = packet->seq_num;
                order.cl_ord_id =
                    trim_right(std::string_view(reinterpret_cast<const char *>(&packet->cl_ord_id_num), 8));
                order.symbol = trim_right(std::string_view(packet->symbol, 8));
                order.side = static_cast<int>(packet->side);
                order.quantity = packet->quantity;
                order.price = static_cast<int64_t>(packet->price_scaled);
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
