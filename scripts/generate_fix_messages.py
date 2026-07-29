#!/usr/bin/env python3
"""
FIX 4.2 Synthetic Message & Resilience Scenario Generator
Generates randomized FIX 4.2 orders (New Order Single 35=D, Snapshot 35=W, Incremental 35=X)
with matching Buy/Sell prices for trade execution and periodic embedded scenario test cases
(packet drops, sequence gaps, duplicates, fat-finger risk orders).
"""

import sys
import os
import random
import argparse
from datetime import datetime, timezone


def generate_fix_messages(output_filepath: str, count: int, batch_size: int = 10000):
    print(f"[Generator] Generating {count:,} FIX 4.2 messages with Order Matching & Scenario Cases to: {output_filepath} ...")
    
    symbols = ["PETR4", "VALE3", "ITUB4", "BBDC4", "ABEV3", "WEGE3"]
    
    # Base price reference per symbol
    base_prices = {
        "PETR4": 35.00,
        "VALE3": 68.00,
        "ITUB4": 32.00,
        "BBDC4": 15.00,
        "ABEV3": 14.00,
        "WEGE3": 40.00
    }
    
    start_time = datetime.now()
    total_written = 0
    seq_counter = 1
    
    with open(output_filepath, "w", encoding="utf-8", buffering=16 * 1024 * 1024) as f:
        batch = []
        
        while seq_counter <= count:
            i = seq_counter
            timestamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H:%M:%S.%f")[:-3]
            
            # --- INJECT SCENARIO TEST CASES PERIODICALLY ---
            
            # Scenario A: Fat-Finger Quantity Risk Rejection (Every 5,000th order)
            if i % 5000 == 150:
                sym = "PETR4"
                qty = 100000 # Exceeds max 50,000 limit!
                price = 35.50
                fix_line = (
                    f"8=FIX.4.2\x019=120\x0135=D\x0149=PRODUCER\x0156=ENGINE\x01"
                    f"34={i}\x0152={timestamp}\x0111=ORD_RISK_QTY_{i}\x01"
                    f"55={sym}\x0154=1\x0138={qty}\x0140=2\x0144={price:.2f}\x0110=128\x01\n"
                )
                batch.append(fix_line)
                seq_counter += 1
                continue
                
            # Scenario B: Price Collar Deviation Risk Rejection (Every 5,000th order)
            if i % 5000 == 350:
                sym = "PETR4"
                qty = 500
                price = 45.00 # Deviates $10.00 from ref $35.00 (collar limit $5.00)!
                fix_line = (
                    f"8=FIX.4.2\x019=120\x0135=D\x0149=PRODUCER\x0156=ENGINE\x01"
                    f"34={i}\x0152={timestamp}\x0111=ORD_RISK_PRICE_{i}\x01"
                    f"55={sym}\x0154=1\x0138={qty}\x0140=2\x0144={price:.2f}\x0110=128\x01\n"
                )
                batch.append(fix_line)
                seq_counter += 1
                continue

            # Scenario C: Sequence Gap Drop (Simulates dropped sequence numbers)
            if i == 200:
                # Jump sequence counter by 5 to create sequence gap [200 .. 204]
                seq_counter += 5
                i = seq_counter

            # --- STANDARD MATCHING ORDERS (BUY / SELL PAIRS) ---
            sym = random.choice(symbols)
            base_p = base_prices[sym]
            
            # Generate matching price offset (e.g. $35.00, $35.10, $35.20)
            price_offset = round(random.choice([-0.20, -0.10, 0.00, 0.10, 0.20]), 2)
            price = base_p + price_offset
            qty = random.randint(1, 10) * 100
            
            # Generate Buy order (Side=1)
            fix_line_buy = (
                f"8=FIX.4.2\x019=120\x0135=D\x0149=PRODUCER\x0156=ENGINE\x01"
                f"34={i}\x0152={timestamp}\x0111=BUY_{i}_{random.randint(1000, 9999)}\x01"
                f"55={sym}\x0154=1\x0138={qty}\x0140=2\x0144={price:.2f}\x0110=128\x01\n"
            )
            batch.append(fix_line_buy)
            seq_counter += 1
            
            if seq_counter > count:
                break
                
            # Generate matching Sell order (Side=2) with overlapping price so trade matches!
            i_sell = seq_counter
            fix_line_sell = (
                f"8=FIX.4.2\x019=120\x0135=D\x0149=PRODUCER\x0156=ENGINE\x01"
                f"34={i_sell}\x0152={timestamp}\x0111=SELL_{i_sell}_{random.randint(1000, 9999)}\x01"
                f"55={sym}\x0154=2\x0138={qty}\x0140=2\x0144={price:.2f}\x0110=128\x01\n"
            )
            batch.append(fix_line_sell)
            seq_counter += 1

            if len(batch) >= batch_size:
                f.writelines(batch)
                total_written += len(batch)
                batch.clear()
                if total_written % 200000 == 0:
                    print(f"  -> Generated {total_written:,} / {count:,} messages ...")

        if batch:
            f.writelines(batch)
            total_written += len(batch)

    elapsed = (datetime.now() - start_time).total_seconds()
    file_size_mb = os.path.getsize(output_filepath) / (1024 * 1024)
    print(f"\n[Success] Successfully generated {total_written:,} random FIX messages with Order Matching & Scenario Cases!")
    print(f"File Path    : {os.path.abspath(output_filepath)}")
    print(f"File Size    : {file_size_mb:.2f} MB")
    print(f"Time Taken   : {elapsed:.2f} seconds ({total_written / max(elapsed, 0.001):,.0f} msgs/sec)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Generate random FIX 4.2 messages for Order Matching Engine testing.")
    parser.add_argument("--out", default="fix_messages_1m.txt", help="Output file path.")
    parser.add_argument("--count", type=int, default=1000000, help="Number of messages to generate (default: 1,000,000).")
    args = parser.parse_args()

    generate_fix_messages(args.out, args.count)
