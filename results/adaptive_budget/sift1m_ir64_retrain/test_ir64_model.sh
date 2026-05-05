#!/bin/bash
# test_ir64_model.sh - 测试 ir=64 训练的 Learned Policy
# 配置: SearchThreadNum=8, NumberOfThreads=16, InternalResultNum=64

set -e

BASE_DIR="/home/ray/code/SPTAG"
RESULT_DIR="$BASE_DIR/results/adaptive_budget/sift1m_ir64_retrain"
BIN="$BASE_DIR/Release/ssdserving"

mkdir -p $RESULT_DIR
cd $BASE_DIR

# 创建 baseline 配置
cat > "$RESULT_DIR/baseline_test.ini" << 'EOF'
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
SearchResult=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_ir64_retrain/search_results_baseline.bin
HeadIndexFolder=head_index

[SelectHead]
isExecute=false

[BuildHead]
isExecute=false

[SearchSSDIndex]
isExecute=true
BuildSsdIndex=false
InternalResultNum=64
PostingBudget=64
NumberOfThreads=16
SearchThreadNum=8
ResultNum=10
MaxDistRatio=1000000
SearchPostingPageLimit=4
EnableDetailedIOStats=true
DetailedIOStatsOutput=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_ir64_retrain/baseline_test.csv
DetailedIOStatsSampleRate=1.0
EnablePageCache=false
EnableInFlightCoalescing=true
EOF

echo "=========================================="
echo "Testing ir=64 Trained Model"
echo "Config: SearchThreadNum=8, NumberOfThreads=16, InternalResultNum=64"
echo "=========================================="
echo "Start time: $(date)"
echo ""

# 测试不同 threshold
THRESHOLDS="0.90 0.95 0.97"

# Baseline
echo "[Baseline] Clearing cache..."
sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
$BIN "$RESULT_DIR/baseline_test.ini" 2>&1 | tee "$RESULT_DIR/baseline_test.log" | grep -E "actuallQPS|Recall@10"
sleep 5

for threshold in $THRESHOLDS; do
    echo ""
    echo "[Learned threshold=$threshold] Clearing cache..."

    # 创建 learned 配置
    cat > "$RESULT_DIR/learned_test_t${threshold}.ini" << EOF
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
SearchResult=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_ir64_retrain/search_results_learned_t${threshold}.bin
HeadIndexFolder=head_index

[SelectHead]
isExecute=false

[BuildHead]
isExecute=false

[SearchSSDIndex]
isExecute=true
BuildSsdIndex=false
InternalResultNum=64
PostingBudget=0
NumberOfThreads=16
SearchThreadNum=8
ResultNum=10
MaxDistRatio=1000000
SearchPostingPageLimit=4
EnableDetailedIOStats=true
DetailedIOStatsOutput=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_ir64_retrain/learned_test_t${threshold}.csv
DetailedIOStatsSampleRate=1.0
EnablePageCache=false
EnableInFlightCoalescing=true
EnableLearnedBudget=true
LearnedBudgetModelPath=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_ir64_retrain
LearnedBudgetThreshold=${threshold}
LearnedBudgetDefault=64
LearnedBudgetMin=32
EOF

    sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
    $BIN "$RESULT_DIR/learned_test_t${threshold}.ini" 2>&1 | tee "$RESULT_DIR/learned_test_t${threshold}.log" | grep -E "actuallQPS|Recall@10"
    sleep 5
done

echo ""
echo "=========================================="
echo "All tests completed!"
echo "End time: $(date)"
echo "=========================================="
