#!/bin/bash
# strict_cache_old_params.sh - 旧参数 MaxDistRatio=1000000 严格缓存测试（重新验证）

set -e

BASE_DIR="/home/ray/code/SPTAG"
RESULT_DIR="$BASE_DIR/results/adaptive_budget/sift1m_paper_params"
BIN="$BASE_DIR/Release/ssdserving"

cd $BASE_DIR

# 创建旧参数 baseline 配置
cat > "$RESULT_DIR/baseline_old.ini" << 'EOF'
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
SearchResult=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_paper_params/search_results_baseline_old.bin
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
DetailedIOStatsOutput=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_paper_params/baseline_old.csv
DetailedIOStatsSampleRate=1.0
EnablePageCache=false
EnableInFlightCoalescing=true
EOF

# 创建旧参数 learned 配置
cat > "$RESULT_DIR/learned_old.ini" << 'EOF'
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
SearchResult=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_paper_params/search_results_learned_old.bin
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
DetailedIOStatsOutput=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_paper_params/learned_old.csv
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
echo "SIFT1M Old Params Strict Cache Test"
echo "MaxDistRatio=1000000, threshold=0.90"
echo "=========================================="
echo "Start time: $(date)"
echo ""

update_csv() {
    local config=$1
    local csv_path=$2
    sed -i "s|DetailedIOStatsOutput=.*|DetailedIOStatsOutput=$csv_path|g" $config
}

for i in 1 2 3; do
    echo "[Run $((2*i-1))/6] Baseline (MaxDistRatio=1000000) - clearing cache..."
    sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null

    update_csv "$RESULT_DIR/baseline_old.ini" "$RESULT_DIR/baseline_old_strict${i}.csv"
    $BIN "$RESULT_DIR/baseline_old.ini" 2>&1 | tee "$RESULT_DIR/baseline_old_strict${i}.log" | grep -E "actuallQPS|Recall@10"

    echo ""
    echo "[Run $((2*i))/6] Learned (MaxDistRatio=1000000, threshold=0.90) - clearing cache..."
    sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null

    update_csv "$RESULT_DIR/learned_old.ini" "$RESULT_DIR/learned_old_strict${i}.csv"
    $BIN "$RESULT_DIR/learned_old.ini" 2>&1 | tee "$RESULT_DIR/learned_old_strict${i}.log" | grep -E "actuallQPS|Recall@10"
    echo ""
done

echo "=========================================="
echo "All runs completed!"
echo "End time: $(date)"
echo "=========================================="
