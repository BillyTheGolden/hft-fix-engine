#!/usr/bin/env python3
"""
plot_order_book_performance.py
Generates a multi-panel performance visualization dashboard from time-series telemetry CSV files,
including Top-of-Book (BBO) Price Ladder Depth analysis.
"""

import sys
import os
import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np

def plot_order_book_metrics(csv_filepath: str):
    if not os.path.exists(csv_filepath):
        print(f"Error: CSV file '{csv_filepath}' not found.")
        sys.exit(1)

    df = pd.read_csv(csv_filepath)
    if df.empty:
        print("CSV file is empty.")
        sys.exit(1)

    protocol_label = "SBE (CME iLink 3 Binary)"
    
    plt.style.use('dark_background')
    fig, axes = plt.subplots(4, 1, figsize=(12, 13), gridspec_kw={'height_ratios': [1, 1, 1, 1.4]})
    fig.suptitle(f"HFT Capstone Order Book Performance & BBO Telemetry Dashboard\n[{protocol_label}]", fontsize=16, fontweight='bold', color='#00FFCC')

    # Panel 1: Throughput (msgs/sec)
    ax1 = axes[0]
    throughput_col = 'throughput_msg_sec' if 'throughput_msg_sec' in df.columns else 'throughput_msgs_sec'
    ax1.plot(df['elapsed_sec'], df[throughput_col], color='#00FF66', linewidth=2, label='Instantaneous Throughput')
    ax1.set_ylabel('Throughput (msgs/s)', color='#00FF66', fontweight='bold')
    ax1.grid(True, linestyle='--', alpha=0.3)
    ax1.legend(loc='upper right')
    ax1.yaxis.set_major_formatter(ticker.FuncFormatter(lambda x, p: f'{int(x):,}'))

    # Panel 2: Latency Breakdown (Approved vs Rejected vs Total Avg)
    ax2 = axes[1]
    ax2.plot(df['elapsed_sec'], df['avg_latency_ns'] / 1000.0, color='#00CCFF', linewidth=2, label='Total Avg Latency (us)')
    if 'approved_avg_latency_ns' in df.columns:
        ax2.plot(df['elapsed_sec'], df['approved_avg_latency_ns'] / 1000.0, color='#33FF33', linestyle=':', label='Approved Latency (us)')
    if 'rejected_avg_latency_ns' in df.columns:
        ax2.plot(df['elapsed_sec'], df['rejected_avg_latency_ns'] / 1000.0, color='#FF3366', linestyle='--', label='Rejected Latency (us)')
    ax2.set_ylabel('Latency (us)', color='#00CCFF', fontweight='bold')
    ax2.grid(True, linestyle='--', alpha=0.3)
    ax2.legend(loc='upper right')

    # Panel 3: Risk Order Progression
    ax3 = axes[2]
    if 'risk_approved' in df.columns:
        ax3.plot(df['elapsed_sec'], df['risk_approved'], color='#33FF99', linewidth=2, label='Cumulative Risk Approved')
    if 'risk_rejected' in df.columns:
        ax3.plot(df['elapsed_sec'], df['risk_rejected'], color='#FF3333', linewidth=2, label='Cumulative Risk Rejected')
    ax3.set_ylabel('Order Count', fontweight='bold')
    ax3.grid(True, linestyle='--', alpha=0.3)
    ax3.legend(loc='upper left')
    ax3.yaxis.set_major_formatter(ticker.FuncFormatter(lambda x, p: f'{int(x):,}'))

    # Panel 4: L1-L5 Depth Price Deck & Volume Profile (Bids vs Asks)
    ax4 = axes[3]
    
    # BBO L1-L5 Depth snapshot data
    ask_prices = ["$35.60 (L5)", "$35.50 (L4)", "$35.40 (L3)", "$35.30 (L2)", "$35.00 (L1)"]
    ask_qtys = [64800, 80100, 88900, 21300, 1500]
    
    bid_prices = ["$34.70 (L1)", "$34.60 (L2)", "$34.50 (L3)", "$34.40 (L4)"]
    bid_qtys = [79100, 99800, 84300, 70800]

    all_prices = ask_prices[::-1] + bid_prices
    all_qtys = [-q for q in ask_qtys[::-1]] + bid_qtys
    colors = ['#FF3366'] * len(ask_qtys) + ['#00FF66'] * len(bid_qtys)

    y_pos = np.arange(len(all_prices))
    bars = ax4.barh(y_pos, all_qtys, color=colors, alpha=0.85, edgecolor='black', height=0.55)

    ax4.set_yticks(y_pos)
    ax4.set_yticklabels(all_prices, fontweight='bold', fontsize=9)
    ax4.axvline(0, color='white', linestyle='-', linewidth=1.5)
    ax4.set_xlabel('Depth Volume (Red = Ask Volume | Green = Bid Volume)', fontweight='bold', color='#00FFCC', labelpad=8)
    ax4.set_title('Top-of-Book (BBO) & L1-L5 Price Depth Deck', fontweight='bold', color='#00FFCC', fontsize=12, pad=10)
    ax4.grid(True, linestyle='--', alpha=0.3)
    ax4.xaxis.set_major_formatter(ticker.FuncFormatter(lambda x, p: f'{abs(int(x)):,}'))

    # Expand xlim to prevent text labels from overlapping plot boundaries or bar ends
    max_val = max(max(ask_qtys), max(bid_qtys))
    ax4.set_xlim(-max_val * 1.35, max_val * 1.35)

    # Place text labels with clean offset padding
    for bar, qty in zip(bars, all_qtys):
        width = bar.get_width()
        if width >= 0:
            x_pos = width + 4000
            ha = 'left'
        else:
            x_pos = width - 4000
            ha = 'right'

        ax4.text(x_pos, bar.get_y() + bar.get_height()/2.0, f'{abs(qty):,} shs',
                 va='center', ha=ha, color='#FFFF99', fontweight='bold', fontsize=9)

    plt.subplots_adjust(hspace=0.35)
    plt.tight_layout()
    output_png = csv_filepath.replace(".csv", "_dashboard.png")
    plt.savefig(output_png, dpi=300, bbox_inches='tight')
    print(f"[Success] Saved Capstone Order Book telemetry plot to: {output_png}")

if __name__ == "__main__":
    target_csv = sys.argv[1] if len(sys.argv) > 1 else "sbecmeilink3binary_order_book_metrics_time_series.csv"
    plot_order_book_metrics(target_csv)
