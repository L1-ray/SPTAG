#!/bin/bash
# strict_cache_maxratio_test_v2.sh - SIFT10M MaxDistRatio=0.7 vs 7 vs 1000000 严格缓存测试

set -e

BASE_DIR="/home/ray/code/SPTAG"
RESULT_DIR="$BASE_DIR/results/adaptive_budget/sift10m_maxratio_test"
BIN="$BASE_DIR/Release/ssdserving"

mkdir -p $RESULT_DIR
cd $BASE_DIR

# 创建 baseline 配置 (MaxDistRatio=0.7)
cat > "$RESULT_DIR/baseline_maxratio0.7.ini" << 'EOF'
[Base]
ValueType=UInt8
DistCalcMethod=L2
IndexAlgoType=BKT
Dim=128
VectorPath=/home/ray/data/sift10m/bigann10m_base.u8bin
VectorType=DEFAULT
VectorSize=10000000
QueryPath=/home/ray/data/sift10m/query.public.10K.u8bin
QueryType=DEFAULT
QuerySize=10000
TruthPath=/home/ray/data/sift10m/bigann-10M.bin
TruthType=DEFAULT
GenerateTruth=false
IndexDirectory=/home/ray/data/sift10m/spann_index_u8default_20260430
SearchResult=/home/ray/code/SPTAG/results/adaptive_budget/sift10m_maxratio_test/search_results_baseline_maxratio0.7.bin
HeadIndexFolder=head_index

[SelectHead]
isExecute=false

[BuildHead]
isExecute=false

[BuildSSDIndex]
isExecute=false

[SearchSSDIndex]
isExecute=true
BuildSsdIndex=false
InternalResultNum=128
PostingBudget=64
NumberOfThreads=40
SearchThreadNum=8
ResultNum=10
MaxDistRatio=0.7
SearchPostingPageLimit=4
EnableDetailedIOStats=true
DetailedIOStatsOutput=/home/ray/code/SPTAG/results/adaptive_budget/sift10m_maxratio_test/baseline_maxratio0.7.csv
DetailedIOStatsSampleRate=1.0
EnablePageCache=false
EnableInFlightCoalescing=true
EOF

# 创建 learned 配置 (MaxDistRatio=0.7)
cat > "$RESULT_DIR/learned_maxratio0.7.ini" << 'EOF'
[Base]
ValueType=UInt8
DistCalcMethod=L2
IndexAlgoType=BKT
Dim=128
VectorPath=/home/ray/data/sift10m/bigann10m_base.u8bin
VectorType=DEFAULT
VectorSize=10000000
QueryPath=/home/ray/data/sift10m/query.public.10K.u8bin
QueryType=DEFAULT
QuerySize=10000
TruthPath=/home/ray/data/sift10m/bigann-10M.bin
TruthType=DEFAULT
GenerateTruth=false
IndexDirectory=/home/ray/data/sift10m/spann_index_u8default_20260430
SearchResult=/home/ray/code/SPTAG/results/adaptive_budget/sift10m_maxratio_test/search_results_learned_maxratio0.7.bin
HeadIndexFolder=head_index

[SelectHead]
isExecute=false

[BuildHead]
isExecute=false

[BuildSSDIndex]
isExecute=false

[SearchSSDIndex]
isExecute=true
BuildSsdIndex=false
InternalResultNum=128
PostingBudget=0
NumberOfThreads=40
SearchThreadNum=8
ResultNum=10
MaxDistRatio=0.7
SearchPostingPageLimit=4
EnableDetailedIOStats=true
DetailedIOStatsOutput=/home/ray/code/SPTAG/results/adaptive_budget/sift10m_maxratio_test/learned_maxratio0.7.csv
DetailedIOStatsSampleRate=1.0
EnablePageCache=false
EnableInFlightCoalescing=true
EnableLearnedBudget=true
LearnedBudgetModelPath=/home/ray/code/SPTAG/results/adaptive_budget/sift10m_learned
LearnedBudgetThreshold=0.80
LearnedBudgetDefault=64
LearnedBudgetMin=32
EOF

echo "=========================================="
echo "SIFT10M MaxDistRatio=0.7 Test"
echo "=========================================="
echo "Start time: $(date)"
echo ""

update_csv() {
    local config=$1
    local csv_path=$2
    sed -i "s|DetailedIOStatsOutput=.*|DetailedIOStatsOutput=$csv_path|g" $config
}

# Test 1: Baseline MaxDistRatio=0.7
echo "[1/2] Baseline MaxDistRatio=0.7 - clearing cache..."
sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
update_csv "$RESULT_DIR/baseline_maxratio0.7.ini" "$RESULT_DIR/baseline_maxratio0.7_run1.csv"
$BIN "$RESULT_DIR/baseline_maxratio0.7.ini" 2>&1 | tee "$RESULT_DIR/baseline_maxratio0.7_run1.log" | grep -E "actuallQPS|Recall@10|postings_touched"
sleep 5

# Test 2: Learned MaxDistRatio=0.7
echo ""
echo "[2/2] Learned MaxDistRatio=0.7 - clearing cache..."
sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
update_csv "$RESULT_DIR/learned_maxratio0.7.ini" "$RESULT_DIR/learned_maxratio0.7_run1.csv"
$BIN "$RESULT_DIR/learned_maxratio0.7.ini" 2>&1 | tee "$RESULT_DIR/learned_maxratio0.7_run1.log" | grep -E "actuallQPS|Recall@10|postings_touched"

echo ""
echo "=========================================="
echo "All tests completed!"
echo "End time: $(date)"
echo "=========================================="
