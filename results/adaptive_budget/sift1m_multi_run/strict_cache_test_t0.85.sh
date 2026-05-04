#!/bin/bash
# strict_cache_test_t0.85.sh - 测试 threshold=0.85

set -e

BASE_DIR="/home/ray/code/SPTAG"
RESULT_DIR="$BASE_DIR/results/adaptive_budget/sift1m_multi_run"
BIN="$BASE_DIR/Release/ssdserving"

cd $BASE_DIR

echo "=========================================="
echo "SIFT1M Test: threshold=0.85 (strict cache)"
echo "=========================================="
echo "Start time: $(date)"
echo ""

update_csv_path() {
    local config=$1
    local csv_path=$2
    sed -i "s|DetailedIOStatsOutput=.*|DetailedIOStatsOutput=$csv_path|g" $config
}

for i in 1 2 3; do
    echo "[Run $((2*i-1))/6] Baseline - clearing cache..."
    sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null

    update_csv_path "$RESULT_DIR/baseline.ini" "$RESULT_DIR/baseline_t0.85_run${i}.csv"
    $BIN "$RESULT_DIR/baseline.ini" 2>&1 | tee "$RESULT_DIR/baseline_t0.85_run${i}.log" | grep -E "actuallQPS|Recall@10"

    echo ""
    echo "[Run $((2*i))/6] Learned (threshold=0.85) - clearing cache..."
    sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null

    update_csv_path "$RESULT_DIR/learned.ini" "$RESULT_DIR/learned_t0.85_run${i}.csv"
    $BIN "$RESULT_DIR/learned.ini" 2>&1 | tee "$RESULT_DIR/learned_t0.85_run${i}.log" | grep -E "actuallQPS|Recall@10"
    echo ""
done

echo "=========================================="
echo "All runs completed!"
echo "End time: $(date)"
echo "=========================================="
