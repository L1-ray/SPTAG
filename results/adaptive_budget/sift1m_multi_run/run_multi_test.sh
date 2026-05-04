#!/bin/bash
# Multi-run test script for SIFT1M

set -e

BASE_DIR="/home/ray/code/SPTAG"
RESULT_DIR="$BASE_DIR/results/adaptive_budget/sift1m_multi_run"
BIN="$BASE_DIR/Release/ssdserving"

cd $BASE_DIR

echo "=========================================="
echo "SIFT1M Multi-run Test (3x baseline, 3x learned, interleaved)"
echo "=========================================="
echo "Start time: $(date)"
echo ""

# Function to update CSV path
update_csv_path() {
    local config=$1
    local csv_path=$2
    sed -i "s|DetailedIOStatsOutput=.*|DetailedIOStatsOutput=$csv_path|g" $config
}

# Interleaved runs
for i in 1 2 3; do
    echo "[Run $((2*i-1))/6] Baseline..."
    update_csv_path "$RESULT_DIR/baseline.ini" "$RESULT_DIR/baseline_run${i}.csv"
    $BIN "$RESULT_DIR/baseline.ini" 2>&1 | tee "$RESULT_DIR/baseline_run${i}.log" | grep -E "actuallQPS|Recall@10"

    echo ""
    echo "[Run $((2*i))/6] Learned..."
    update_csv_path "$RESULT_DIR/learned.ini" "$RESULT_DIR/learned_run${i}.csv"
    $BIN "$RESULT_DIR/learned.ini" 2>&1 | tee "$RESULT_DIR/learned_run${i}.log" | grep -E "actuallQPS|Recall@10"
    echo ""
done

echo "=========================================="
echo "All runs completed!"
echo "End time: $(date)"
echo "=========================================="
