#!/bin/bash
# strict_cache_maxratio_test.sh - SIFT10M MaxDistRatio=7 vs 1000000 严格缓存测试

set -e

BASE_DIR="/home/ray/code/SPTAG"
RESULT_DIR="$BASE_DIR/results/adaptive_budget/sift10m_maxratio_test"
BIN="$BASE_DIR/Release/ssdserving"

mkdir -p $RESULT_DIR
cd $BASE_DIR

# 创建 baseline 配置 (MaxDistRatio=7)
cat > "$RESULT_DIR/baseline_maxratio7.ini" << 'EOF'
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
SearchResult=/home/ray/code/SPTAG/results/adaptive_budget/sift10m_maxratio_test/search_results_baseline_maxratio7.bin
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
MaxDistRatio=7
SearchPostingPageLimit=4
EnableDetailedIOStats=true
DetailedIOStatsOutput=/home/ray/code/SPTAG/results/adaptive_budget/sift10m_maxratio_test/baseline_maxratio7.csv
DetailedIOStatsSampleRate=1.0
EnablePageCache=false
EnableInFlightCoalescing=true
EOF

# 创建 baseline 配置 (MaxDistRatio=1000000)
cat > "$RESULT_DIR/baseline_maxratio_old.ini" << 'EOF'
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
SearchResult=/home/ray/code/SPTAG/results/adaptive_budget/sift10m_maxratio_test/search_results_baseline_old.bin
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
MaxDistRatio=1000000
SearchPostingPageLimit=4
EnableDetailedIOStats=true
DetailedIOStatsOutput=/home/ray/code/SPTAG/results/adaptive_budget/sift10m_maxratio_test/baseline_old.csv
DetailedIOStatsSampleRate=1.0
EnablePageCache=false
EnableInFlightCoalescing=true
EOF

# 创建 learned 配置 (MaxDistRatio=7)
cat > "$RESULT_DIR/learned_maxratio7.ini" << 'EOF'
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
SearchResult=/home/ray/code/SPTAG/results/adaptive_budget/sift10m_maxratio_test/search_results_learned_maxratio7.bin
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
MaxDistRatio=7
SearchPostingPageLimit=4
EnableDetailedIOStats=true
DetailedIOStatsOutput=/home/ray/code/SPTAG/results/adaptive_budget/sift10m_maxratio_test/learned_maxratio7.csv
DetailedIOStatsSampleRate=1.0
EnablePageCache=false
EnableInFlightCoalescing=true
EnableLearnedBudget=true
LearnedBudgetModelPath=/home/ray/code/SPTAG/results/adaptive_budget/sift10m_learned
LearnedBudgetThreshold=0.80
LearnedBudgetDefault=64
LearnedBudgetMin=32
EOF

# 创建 learned 配置 (MaxDistRatio=1000000)
cat > "$RESULT_DIR/learned_maxratio_old.ini" << 'EOF'
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
SearchResult=/home/ray/code/SPTAG/results/adaptive_budget/sift10m_maxratio_test/search_results_learned_old.bin
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
MaxDistRatio=1000000
SearchPostingPageLimit=4
EnableDetailedIOStats=true
DetailedIOStatsOutput=/home/ray/code/SPTAG/results/adaptive_budget/sift10m_maxratio_test/learned_old.csv
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
echo "SIFT10M MaxDistRatio Comparison Test"
echo "MaxDistRatio=7 vs MaxDistRatio=1000000"
echo "=========================================="
echo "Start time: $(date)"
echo ""

update_csv() {
    local config=$1
    local csv_path=$2
    sed -i "s|DetailedIOStatsOutput=.*|DetailedIOStatsOutput=$csv_path|g" $config
}

# Test 1: Baseline MaxDistRatio=1000000 (old)
echo "[1/4] Baseline MaxDistRatio=1000000 - clearing cache..."
sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
update_csv "$RESULT_DIR/baseline_maxratio_old.ini" "$RESULT_DIR/baseline_old_run1.csv"
$BIN "$RESULT_DIR/baseline_maxratio_old.ini" 2>&1 | tee "$RESULT_DIR/baseline_old_run1.log" | grep -E "actuallQPS|Recall@10|postings_touched"
sleep 5

# Test 2: Learned MaxDistRatio=1000000 (old)
echo ""
echo "[2/4] Learned MaxDistRatio=1000000 - clearing cache..."
sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
update_csv "$RESULT_DIR/learned_maxratio_old.ini" "$RESULT_DIR/learned_old_run1.csv"
$BIN "$RESULT_DIR/learned_maxratio_old.ini" 2>&1 | tee "$RESULT_DIR/learned_old_run1.log" | grep -E "actuallQPS|Recall@10|postings_touched"
sleep 5

# Test 3: Baseline MaxDistRatio=7 (paper)
echo ""
echo "[3/4] Baseline MaxDistRatio=7 - clearing cache..."
sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
update_csv "$RESULT_DIR/baseline_maxratio7.ini" "$RESULT_DIR/baseline_maxratio7_run1.csv"
$BIN "$RESULT_DIR/baseline_maxratio7.ini" 2>&1 | tee "$RESULT_DIR/baseline_maxratio7_run1.log" | grep -E "actuallQPS|Recall@10|postings_touched"
sleep 5

# Test 4: Learned MaxDistRatio=7 (paper)
echo ""
echo "[4/4] Learned MaxDistRatio=7 - clearing cache..."
sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
update_csv "$RESULT_DIR/learned_maxratio7.ini" "$RESULT_DIR/learned_maxratio7_run1.csv"
$BIN "$RESULT_DIR/learned_maxratio7.ini" 2>&1 | tee "$RESULT_DIR/learned_maxratio7_run1.log" | grep -E "actuallQPS|Recall@10|postings_touched"

echo ""
echo "=========================================="
echo "All tests completed!"
echo "End time: $(date)"
echo "=========================================="
