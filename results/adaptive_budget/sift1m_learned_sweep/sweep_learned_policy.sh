#!/bin/bash
# sweep_learned_policy.sh - Test Learned Policy across different SearchThreadNum configs
# Configs: st2_nt16_ir64_pl4, st4_nt16_ir64_pl4, st8_nt16_ir64_pl4, st16_nt16_ir64_pl4

set -e

BASE_DIR="/home/ray/code/SPTAG"
RESULT_DIR="$BASE_DIR/results/adaptive_budget/sift1m_learned_sweep"
BIN="$BASE_DIR/Release/ssdserving"

mkdir -p $RESULT_DIR
cd $BASE_DIR

# Test configurations: SearchThreadNum values
STS="2 4 8 16"
NT=16
IR=64
THRESHOLD=0.95

echo "=========================================="
echo "SIFT1M Learned Policy Sweep"
echo "Config: st in {2,4,8,16}, nt=16, ir=64, threshold=0.95"
echo "=========================================="
echo "Start time: $(date)"
echo ""

# First run baseline for each config
for ST in $STS; do
    CONFIG_NAME="st${ST}_nt${NT}_ir${IR}_pl4"
    echo ""
    echo "=========================================="
    echo "[Baseline] $CONFIG_NAME"
    echo "=========================================="

    cat > "$RESULT_DIR/baseline_${CONFIG_NAME}.ini" << EOF
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
SearchResult=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_learned_sweep/search_results_baseline_${CONFIG_NAME}.bin
HeadIndexFolder=head_index

[SelectHead]
isExecute=false

[BuildHead]
isExecute=false

[SearchSSDIndex]
isExecute=true
BuildSsdIndex=false
InternalResultNum=${IR}
PostingBudget=64
NumberOfThreads=${NT}
SearchThreadNum=${ST}
ResultNum=10
MaxDistRatio=1000000
SearchPostingPageLimit=4
EnableDetailedIOStats=true
DetailedIOStatsOutput=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_learned_sweep/baseline_${CONFIG_NAME}.csv
DetailedIOStatsSampleRate=1.0
EnablePageCache=false
EnableInFlightCoalescing=true
EOF

    sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
    $BIN "$RESULT_DIR/baseline_${CONFIG_NAME}.ini" 2>&1 | tee "$RESULT_DIR/baseline_${CONFIG_NAME}.log" | grep -E "actuallQPS|Recall@10"
    sleep 3
done

# Then run learned policy for each config
for ST in $STS; do
    CONFIG_NAME="st${ST}_nt${NT}_ir${IR}_pl4"
    echo ""
    echo "=========================================="
    echo "[Learned] $CONFIG_NAME (threshold=$THRESHOLD)"
    echo "=========================================="

    cat > "$RESULT_DIR/learned_${CONFIG_NAME}.ini" << EOF
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
SearchResult=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_learned_sweep/search_results_learned_${CONFIG_NAME}.bin
HeadIndexFolder=head_index

[SelectHead]
isExecute=false

[BuildHead]
isExecute=false

[SearchSSDIndex]
isExecute=true
BuildSsdIndex=false
InternalResultNum=${IR}
PostingBudget=0
NumberOfThreads=${NT}
SearchThreadNum=${ST}
ResultNum=10
MaxDistRatio=1000000
SearchPostingPageLimit=4
EnableDetailedIOStats=true
DetailedIOStatsOutput=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_learned_sweep/learned_${CONFIG_NAME}.csv
DetailedIOStatsSampleRate=1.0
EnablePageCache=false
EnableInFlightCoalescing=true
EnableLearnedBudget=true
LearnedBudgetModelPath=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_ir64_retrain
LearnedBudgetThreshold=${THRESHOLD}
LearnedBudgetDefault=64
LearnedBudgetMin=32
EOF

    sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
    $BIN "$RESULT_DIR/learned_${CONFIG_NAME}.ini" 2>&1 | tee "$RESULT_DIR/learned_${CONFIG_NAME}.log" | grep -E "actuallQPS|Recall@10"
    sleep 3
done

echo ""
echo "=========================================="
echo "All tests completed!"
echo "End time: $(date)"
echo "=========================================="
