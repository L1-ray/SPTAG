#!/bin/bash
# strict_cache_test.sh - 每次测试前清空 OS 页面缓存

set -e

BASE_DIR="/home/ray/code/SPTAG"
RESULT_DIR="$BASE_DIR/results/adaptive_budget/sift1m_multi_run"
BIN="$BASE_DIR/Release/ssdserving"

cd $BASE_DIR

echo "=========================================="
echo "SIFT1M Strict Cache Test (clear cache before each run)"
echo "=========================================="
echo "Start time: $(date)"
echo ""

# Function to update CSV path
update_csv_path() {
    local config=$1
    local csv_path=$2
    sed -i "s|DetailedIOStatsOutput=.*|DetailedIOStatsOutput=$csv_path|g" $config
}

for i in 1 2 3; do
    echo "[Run $((2*i-1))/6] Baseline - clearing cache..."
    sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null

    update_csv_path "$RESULT_DIR/baseline.ini" "$RESULT_DIR/baseline_strict${i}.csv"
    $BIN "$RESULT_DIR/baseline.ini" 2>&1 | tee "$RESULT_DIR/baseline_strict${i}.log" | grep -E "actuallQPS|Recall@10"

    echo ""
    echo "[Run $((2*i))/6] Learned - clearing cache..."
    sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null

    update_csv_path "$RESULT_DIR/learned.ini" "$RESULT_DIR/learned_strict${i}.csv"
    $BIN "$RESULT_DIR/learned.ini" 2>&1 | tee "$RESULT_DIR/learned_strict${i}.log" | grep -E "actuallQPS|Recall@10"
    echo ""
done

echo "=========================================="
echo "All runs completed!"
echo "End time: $(date)"
echo "=========================================="
