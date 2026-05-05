#!/bin/bash
# retrain_ir64.sh - 用 ir=64 重新训练 Learned Policy
# 配置: SearchThreadNum=8, NumberOfThreads=16, InternalResultNum=64

set -e

BASE_DIR="/home/ray/code/SPTAG"
RESULT_DIR="$BASE_DIR/results/adaptive_budget/sift1m_ir64_retrain"
BIN="$BASE_DIR/Release/ssdserving"

mkdir -p $RESULT_DIR
cd $BASE_DIR

echo "=========================================="
echo "SIFT1M Learned Policy Retrain (ir=64)"
echo "Config: SearchThreadNum=8, NumberOfThreads=16, InternalResultNum=64"
echo "=========================================="
echo "Start time: $(date)"
echo ""

# Step 1: Budget Sweep for ir=64
echo "[Step 1] Running Budget Sweep with ir=64..."
BUDGETS="16 32 40 48 64 80 96 128"

for b in $BUDGETS; do
    echo "  Budget=$b"
    cat > "$RESULT_DIR/budget_${b}.ini" << EOF
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
SearchResult=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_ir64_retrain/search_results_b${b}.bin
HeadIndexFolder=head_index

[SelectHead]
isExecute=false

[BuildHead]
isExecute=false

[SearchSSDIndex]
isExecute=true
BuildSsdIndex=false
InternalResultNum=64
PostingBudget=${b}
NumberOfThreads=16
SearchThreadNum=8
ResultNum=10
MaxDistRatio=1000000
SearchPostingPageLimit=4
EnableDetailedIOStats=true
DetailedIOStatsOutput=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_ir64_retrain/budget_${b}.csv
DetailedIOStatsSampleRate=1.0
EnablePageCache=false
EnableInFlightCoalescing=true
EOF

    sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
    $BIN "$RESULT_DIR/budget_${b}.ini" 2>&1 | tee "$RESULT_DIR/budget_${b}.log" | grep -E "actuallQPS|Recall@10"
    sleep 3
done

echo ""
echo "[Step 1] Budget Sweep completed"
echo "End time: $(date)"
echo ""
echo "下一步: 提取特征并训练模型 (运行 python 脚本)"
