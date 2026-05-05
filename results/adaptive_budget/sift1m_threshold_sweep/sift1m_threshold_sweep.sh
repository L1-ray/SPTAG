#!/bin/bash
# sift1m_threshold_sweep.sh - SIFT1M Threshold Sweep 严格缓存测试

set -e

BASE_DIR="/home/ray/code/SPTAG"
RESULT_DIR="$BASE_DIR/results/adaptive_budget/sift1m_threshold_sweep"
BIN="$BASE_DIR/Release/ssdserving"

mkdir -p $RESULT_DIR
cd $BASE_DIR

# 创建 baseline 配置
cat > "$RESULT_DIR/baseline.ini" << 'EOF'
[Base]
ValueType=UInt8
DistCalcMethod=L2
IndexAlgoType=BKT
Dim=128
VectorPath=/home/ray/data/sift1m/bigann1m_base.u8bin
VectorType=DEFAULT
VectorSize=1000000
QueryPath=/home/ray/data/sift1m/query.public.10K.u8bin
QueryType=DEFAULT
QuerySize=10000
TruthPath=/home/ray/data/sift1m/bigann-1M.bin
TruthType=DEFAULT
GenerateTruth=false
IndexDirectory=/home/ray/data/sift1m/spann_index_official_u8default_20260430
SearchResult=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_threshold_sweep/search_results_baseline.bin
HeadIndexFolder=head_index

[SelectHead]
isExecute=false

[BuildHead]
isExecute=false

[SearchSSDIndex]
isExecute=true
BuildSsdIndex=false
InternalResultNum=128
PostingBudget=64
NumberOfThreads=40
SearchThreadNum=8
ResultNum=10
MaxDistRatio=1000000
SearchPostingPageLimit=4
EnableDetailedIOStats=true
DetailedIOStatsOutput=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_threshold_sweep/baseline.csv
DetailedIOStatsSampleRate=1.0
EnablePageCache=false
EnableInFlightCoalescing=true
EOF

# 创建 learned 配置模板
cat > "$RESULT_DIR/learned_template.ini" << 'EOF'
[Base]
ValueType=UInt8
DistCalcMethod=L2
IndexAlgoType=BKT
Dim=128
VectorPath=/home/ray/data/sift1m/bigann1m_base.u8bin
VectorType=DEFAULT
VectorSize=1000000
QueryPath=/home/ray/data/sift1m/query.public.10K.u8bin
QueryType=DEFAULT
QuerySize=10000
TruthPath=/home/ray/data/sift1m/bigann-1M.bin
TruthType=DEFAULT
GenerateTruth=false
IndexDirectory=/home/ray/data/sift1m/spann_index_official_u8default_20260430
SearchResult=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_threshold_sweep/search_results_learned.bin
HeadIndexFolder=head_index

[SelectHead]
isExecute=false

[BuildHead]
isExecute=false

[SearchSSDIndex]
isExecute=true
BuildSsdIndex=false
InternalResultNum=128
PostingBudget=0
NumberOfThreads=40
SearchThreadNum=8
ResultNum=10
MaxDistRatio=1000000
SearchPostingPageLimit=4
EnableDetailedIOStats=true
DetailedIOStatsOutput=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_threshold_sweep/learned.csv
DetailedIOStatsSampleRate=1.0
EnablePageCache=false
EnableInFlightCoalescing=true
EnableLearnedBudget=true
LearnedBudgetModelPath=/home/ray/code/SPTAG/results/adaptive_budget/phase4_learned
LearnedBudgetThreshold=0.90
LearnedBudgetDefault=64
LearnedBudgetMin=32
EOF

echo "=========================================="
echo "SIFT1M Threshold Sweep (Strict Cache)"
echo "Testing thresholds: 0.80, 0.85, 0.90, 0.95, 0.97"
echo "=========================================="
echo "Start time: $(date)"
echo ""

THRESHOLDS="0.80 0.85 0.90 0.95 0.97"

update_threshold() {
    local threshold=$1
    sed -i "s/LearnedBudgetThreshold=.*/LearnedBudgetThreshold=$threshold/g" "$RESULT_DIR/learned.ini"
}

update_csv() {
    local config=$1
    local csv_path=$2
    sed -i "s|DetailedIOStatsOutput=.*|DetailedIOStatsOutput=$csv_path|g" $config
}

# First run baseline
echo "[Baseline] Clearing cache..."
sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
cp "$RESULT_DIR/baseline.ini" "$RESULT_DIR/baseline_run.ini"
update_csv "$RESULT_DIR/baseline_run.ini" "$RESULT_DIR/baseline.csv"
$BIN "$RESULT_DIR/baseline_run.ini" 2>&1 | tee "$RESULT_DIR/baseline.log" | grep -E "actuallQPS|Recall@10"
sleep 5

# Then test each threshold
for threshold in $THRESHOLDS; do
    echo ""
    echo "[Threshold=$threshold] Clearing cache..."
    sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null

    cp "$RESULT_DIR/learned_template.ini" "$RESULT_DIR/learned.ini"
    update_threshold $threshold
    update_csv "$RESULT_DIR/learned.ini" "$RESULT_DIR/learned_t${threshold}.csv"

    $BIN "$RESULT_DIR/learned.ini" 2>&1 | tee "$RESULT_DIR/learned_t${threshold}.log" | grep -E "actuallQPS|Recall@10"
    sleep 5
done

echo ""
echo "=========================================="
echo "All tests completed!"
echo "End time: $(date)"
echo "=========================================="
