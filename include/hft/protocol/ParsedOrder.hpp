/**
 * @file ParsedOrder.hpp
 * @brief Domain model structure representing a decoded financial order from a FIX string.
 */

#pragma once

#include <cstdint>
#include <string_view>

namespace hft::protocol
{

    /**
     * @struct ParsedOrder
     * @brief Represents the structured fields of a financial order parsed from raw FIX protocol tags.
     * @details Uses non-owning string views when applicable during zero-copy parsing inside the worker thread.
     */
    struct ParsedOrder
    {
        /** @brief FIX Tag 35 (MsgType, e.g., "D" for New Order Single). */
        std::string_view msg_type;

        /**
         * @brief Single-character MsgType representation ('D' for New Order Single).
         * @details OPTIMIZATION (High Finding 2.5): Enables 1-cycle integer comparison on the hot path.
         */
        char msg_type_char{'?'};

        /** @brief FIX Tag 11 (ClOrdID, unique client identifier). */
        std::string_view cl_ord_id;

        /** @brief FIX Tag 55 (Symbol / Ticker, e.g., "PETR4"). */
        std::string_view symbol;

        /** @brief FIX Tag 54 (Side: 1 = Buy, 2 = Sell). */
        int side{0};

        /** @brief FIX Tag 38 (OrderQty, quantity of shares/contracts). */
        int quantity{0};

        /** @brief FIX Tag 44 (Price of the limit order scaled by 1,000,000). */
        int64_t price{0};

        /** @brief FIX Tag 34 (MsgSeqNum, message sequence number). */
        uint64_t seq_num{0};

        /** @brief Elapsed nanoseconds measured from RX Ring kernel departure to parser completion. */
        uint64_t latency_ns{0};
    };

} // namespace hft::protocol
