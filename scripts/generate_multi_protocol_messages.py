#!/usr/bin/env python3
"""
Multi-Protocol Synthetic Message Generator for HFT Benchmark Testing.
Supports FIX 4.2 (ASCII), Nasdaq OUCH 5.0 (Binary), and CME SBE (Binary).
Generates messages with randomized risk rejections (<= 20% of total messages).
"""

import sys
import os
import struct
import random
import argparse
from datetime import datetime, timezone

# --- BINARY STRUCT PACKING DEFINITIONS ---
# OUCH 5.0 (46 bytes): 'O' (char), seq (uint64), cl_ord_id (14s), side (char), qty (uint32), price (uint32), symbol (6s), ts (uint64)
OUCH_STRUCT_FORMAT = "<c Q 14s c I I 6s Q"

# CME SBE Header (8 bytes): block_len (uint16), template_id (uint16), schema_id (uint16), version (uint16)
# CME SBE Payload (36 bytes): seq (uint64), cl_ord_id (uint64), price (uint64), qty (uint32), side (uint8), symbol (8s), tif (uint8)
SBE_HEADER_FORMAT = "<H H H H"
SBE_PAYLOAD_FORMAT = "<Q Q Q I B 8s B"


def generate_messages(protocol: str, output_filepath: str, count: int, risk_pct: float = 0.15):
    print(f"[MultiProtocolGenerator] Protocol: {protocol.upper()} | Target Count: {count:,} | Out: {output_filepath} ...")
    
    symbols = ["PETR4", "VALE3", "ITUB4", "BBDC4", "ABEV3", "WEGE3"]
    # Base prices centered around the engine's $35.00 reference price to prevent unintended price collar rejections
    base_prices = {"PETR4": 35.00, "VALE3": 35.20, "ITUB4": 34.80, "BBDC4": 35.10, "ABEV3": 34.90, "WEGE3": 35.30}
    
    start_time = datetime.now()
    total_written = 0
    risk_count = 0
    seq_counter = 1

    mode = "wb" if protocol.upper() in ["OUCH", "SBE"] else "w"
    encoding = None if mode == "wb" else "utf-8"

    with open(output_filepath, mode, encoding=encoding, buffering=16 * 1024 * 1024) as f:
        batch = []
        
        while seq_counter <= count:
            i = seq_counter
            timestamp_ns = int(datetime.now(timezone.utc).timestamp() * 1e9)
            
            # Determine if this message is a risk rejection test case (probability <= risk_pct)
            is_risk_case = (random.random() < risk_pct)
            
            sym = random.choice(symbols)
            base_p = base_prices[sym]
            
            if is_risk_case:
                risk_count += 1
                # Explicit Risk Violation: Fat-finger quantity > 50,000 OR Price collar deviation > $5.00 from $35.00
                if random.choice([True, False]):
                    qty = 100000 # Fat-finger quantity (> 50,000 limit)
                    price = base_p
                else:
                    qty = 500
                    price = base_p + 10.00 # Price collar deviation ($45.00 vs $35.00 ref > $5.00 limit)
                side_val = 1 # Buy
            else:
                # Valid Normal Order Pair (Buy / Sell) within risk limits ($35.00 ref +- $0.50, Qty <= 5,000)
                price_offset = round(random.choice([-0.40, -0.20, 0.00, 0.20, 0.40]), 2)
                price = base_p + price_offset
                qty = random.randint(1, 50) * 100 # Qty between 100 and 5,000 (well under 50,000 limit)
                side_val = 1 if (seq_counter % 2 == 1) else 2

            price_scaled = int(round(price * 1000000))

            if protocol.upper() == "OUCH":
                cl_id_str = f"ORD_{i}".ljust(14).encode("ascii")
                sym_str = sym.ljust(6).encode("ascii")
                side_char = b'B' if side_val == 1 else b'S'
                
                pkt = struct.pack(
                    OUCH_STRUCT_FORMAT,
                    b'O', i, cl_id_str, side_char, qty, price_scaled, sym_str, timestamp_ns
                )
                batch.append(pkt)

            elif protocol.upper() == "SBE":
                header = struct.pack(SBE_HEADER_FORMAT, 36, 514, 1, 1)
                sym_str = sym.ljust(8).encode("ascii")
                payload = struct.pack(
                    SBE_PAYLOAD_FORMAT,
                    i, i, price_scaled, qty, side_val, sym_str, 0
                )
                batch.append(header + payload)

            else: # FIX 4.2 ASCII Tag=Value
                ts_str = datetime.now(timezone.utc).strftime("%Y%m%d-%H:%M:%S.%f")[:-3]
                side_str = "1" if side_val == 1 else "2"
                fix_line = (
                    f"8=FIX.4.2\x019=120\x0135=D\x0149=PRODUCER\x0156=ENGINE\x01"
                    f"34={i}\x0152={ts_str}\x0111=ORD_{i}\x01"
                    f"55={sym}\x0154={side_str}\x0138={qty}\x0140=2\x0144={price:.2f}\x0110=128\x01\n"
                )
                batch.append(fix_line)

            seq_counter += 1

            if len(batch) >= 10000:
                if mode == "wb":
                    f.write(b"".join(batch))
                else:
                    f.writelines(batch)
                total_written += len(batch)
                batch.clear()

        if batch:
            if mode == "wb":
                f.write(b"".join(batch))
            else:
                f.writelines(batch)
            total_written += len(batch)

    elapsed = (datetime.now() - start_time).total_seconds()
    file_size_mb = os.path.getsize(output_filepath) / (1024 * 1024)
    print(f"[Success] Generated {total_written:,} {protocol.upper()} messages ({risk_count:,} risk test cases = {risk_count/total_written:.1%})!")
    print(f"File Path : {os.path.abspath(output_filepath)} | Size: {file_size_mb:.2f} MB | Time: {elapsed:.2f}s")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Generate synthetic HFT trade messages (FIX, OUCH, SBE).")
    parser.add_argument("--protocol", choices=["FIX", "OUCH", "SBE", "fix", "ouch", "sbe"], default="FIX", help="Target protocol (default: FIX).")
    parser.add_argument("--out", default="messages.data", help="Output file path.")
    parser.add_argument("--count", type=int, default=100000, help="Number of messages to generate (default: 100,000).")
    parser.add_argument("--risk-pct", type=float, default=0.15, help="Percentage of risk validation cases (default: 0.15).")
    args = parser.parse_args()

    generate_messages(args.protocol, args.out, args.count, args.risk_pct)
