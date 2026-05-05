#!/bin/bash
# test_ir64_model.sh - SIFT10M Test ir=64 Trained Model

set -e

BASE_DIR="/home/ray/code/SPTAG"
RESULT_DIR="$BASE_DIR/results/adaptive_budget/sift10m_ir64_retrain"
BIN="$BASE_DIR/Release/ssdserving"

mkdir -p $RESULT_DIR
cd $BASE_DIR

echo "=========================================="
echo "SIFT10M Testing ir=64 Trained Model"
echo "Config: SearchThreadNum=8, NumberOfThreads=16, InternalResultNum=64"
echo "=========================================="
echo "Start time: $(date)"
echo ""

# Baseline
echo "[Baseline] Clearing cache..."
cat > "$RESULT_DIR/baseline_test.ini" << 'EOF'
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
SearchResult=/home/ray/code/SPTAG/results/adaptive_budget/sift10m_ir64_retrain/search_results_baseline.bin
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
DetailedIOStatsOutput=/home/ray/code/SPTAG/results/adaptive_budget/sift10m_ir64_retrain/baseline_test.csv
DetailedIOStatsSampleRate=1.0
EnablePageCache=false
EnableInFlightCoalescing=true
EOF

sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
$BIN "$RESULT_DIR/baseline_test.ini" 2>&1 | tee "$RESULT_DIR/baseline_test.log" | grep -E "actuallQPS|Recall@10"
sleep 3

# Test thresholds
THRESHOLDS="0.90 0.95 0.97"

for threshold in $THRESHOLDS; do
    echo ""
    echo "[Learned threshold=$threshold] Clearing cache..."

    cat > "$RESULT_DIR/learned_test_t${threshold}.ini" << EOF
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
SearchResult=/home/ray/code/SPTAG/results/adaptive_budget/sift10m_ir64_retrain/search_results_learned_t${threshold}.bin
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
DetailedIOStatsOutput=/home/ray/code/SPTAG/results/adaptive_budget/sift10m_ir64_retrain/learned_test_t${threshold}.csv
DetailedIOStatsSampleRate=1.0
EnablePageCache=false
EnableInFlightCoalescing=true
EnableLearnedBudget=true
LearnedBudgetModelPath=/home/ray/code/SPTAG/results/adaptive_budget/sift10m_ir64_retrain
LearnedBudgetThreshold=${threshold}
LearnedBudgetDefault=64
LearnedBudgetMin=32
EOF

    sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
    $BIN "$RESULT_DIR/learned_test_t${threshold}.ini" 2>&1 | tee "$RESULT_DIR/learned_test_t${threshold}.log" | grep -E "actuallQPS|Recall@10"
    sleep 3
done

echo ""
echo "=========================================="
echo "All tests completed!"
echo "End time: $(date)"
echo "=========================================="
