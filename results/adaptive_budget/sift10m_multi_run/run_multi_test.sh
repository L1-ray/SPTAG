#!/bin/bash
# Multi-run test script for SIFT10M
# Interleaved baseline and learned runs to minimize system bias

set -e

BASE_DIR="/home/ray/code/SPTAG"
RESULT_DIR="$BASE_DIR/results/adaptive_budget/sift10m_multi_run"
BIN="$BASE_DIR/Release/ssdserving"

cd $BASE_DIR

echo "=========================================="
echo "SIFT10M Multi-run Test (3x baseline, 3x learned, interleaved)"
echo "=========================================="
echo "Start time: $(date)"
echo ""

# Function to update CSV path in config
update_csv_path() {
    local config=$1
    local csv_path=$2
    sed -i "s|DetailedIOStatsOutput=.*|DetailedIOStatsOutput=$csv_path|g" $config
}

# Run 1: Baseline
echo "[Run 1/6] Baseline..."
update_csv_path "$RESULT_DIR/baseline.ini" "$RESULT_DIR/baseline_run1.csv"
$BIN "$RESULT_DIR/baseline.ini" 2>&1 | tee "$RESULT_DIR/baseline_run1.log" | grep -E "QPS|Recall@10|P99|99tiles"

# Run 2: Learned
echo ""
echo "[Run 2/6] Learned..."
update_csv_path "$RESULT_DIR/learned.ini" "$RESULT_DIR/learned_run1.csv"
$BIN "$RESULT_DIR/learned.ini" 2>&1 | tee "$RESULT_DIR/learned_run1.log" | grep -E "QPS|Recall@10|P99|99tiles"

# Run 3: Baseline
echo ""
echo "[Run 3/6] Baseline..."
update_csv_path "$RESULT_DIR/baseline.ini" "$RESULT_DIR/baseline_run2.csv"
$BIN "$RESULT_DIR/baseline.ini" 2>&1 | tee "$RESULT_DIR/baseline_run2.log" | grep -E "QPS|Recall@10|P99|99tiles"

# Run 4: Learned
echo ""
echo "[Run 4/6] Learned..."
update_csv_path "$RESULT_DIR/learned.ini" "$RESULT_DIR/learned_run2.csv"
$BIN "$RESULT_DIR/learned.ini" 2>&1 | tee "$RESULT_DIR/learned_run2.log" | grep -E "QPS|Recall@10|P99|99tiles"

# Run 5: Baseline
echo ""
echo "[Run 5/6] Baseline..."
update_csv_path "$RESULT_DIR/baseline.ini" "$RESULT_DIR/baseline_run3.csv"
$BIN "$RESULT_DIR/baseline.ini" 2>&1 | tee "$RESULT_DIR/baseline_run3.log" | grep -E "QPS|Recall@10|P99|99tiles"

# Run 6: Learned
echo ""
echo "[Run 6/6] Learned..."
update_csv_path "$RESULT_DIR/learned.ini" "$RESULT_DIR/learned_run3.csv"
$BIN "$RESULT_DIR/learned.ini" 2>&1 | tee "$RESULT_DIR/learned_run3.log" | grep -E "QPS|Recall@10|P99|99tiles"

echo ""
echo "=========================================="
echo "All runs completed!"
echo "End time: $(date)"
echo "=========================================="

# Extract results
echo ""
echo "=== Results Summary ==="
echo ""
echo "Baseline QPS:"
for i in 1 2 3; do
    qps=$(grep "actuallQPS is" "$RESULT_DIR/baseline_run$i.log" | grep -oP '\d+\.\d+' | head -1)
    echo "  Run $i: $qps"
done

echo ""
echo "Learned QPS:"
for i in 1 2 3; do
    qps=$(grep "actuallQPS is" "$RESULT_DIR/learned_run$i.log" | grep -oP '\d+\.\d+' | head -1)
    echo "  Run $i: $qps"
done

echo ""
echo "Baseline Recall@10:"
for i in 1 2 3; do
    recall=$(grep "Recall@10:" "$RESULT_DIR/baseline_run$i.log" | grep -oP '0\.\d+' | tail -1)
    echo "  Run $i: $recall"
done

echo ""
echo "Learned Recall@10:"
for i in 1 2 3; do
    recall=$(grep "Recall@10:" "$RESULT_DIR/learned_run$i.log" | grep -oP '0\.\d+' | tail -1)
    echo "  Run $i: $recall"
done
