#!/bin/bash
# paper_params_test.sh - 使用论文参数 MaxDistRatio=7 测试

set -e

BASE_DIR="/home/ray/code/SPTAG"
RESULT_DIR="$BASE_DIR/results/adaptive_budget/sift1m_paper_params"
BIN="$BASE_DIR/Release/ssdserving"

mkdir -p $RESULT_DIR

cd $BASE_DIR

# 创建 baseline 配置 (MaxDistRatio=7)
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
SearchResult=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_paper_params/search_results_baseline.bin
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
MaxDistRatio=7
SearchPostingPageLimit=4
EnableDetailedIOStats=true
DetailedIOStatsOutput=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_paper_params/baseline.csv
DetailedIOStatsSampleRate=1.0
EnablePageCache=false
EnableInFlightCoalescing=true
EOF

# 创建 learned 配置 (MaxDistRatio=7, Learned Policy enabled)
cat > "$RESULT_DIR/learned.ini" << 'EOF'
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
SearchResult=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_paper_params/search_results_learned.bin
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
MaxDistRatio=7
SearchPostingPageLimit=4
EnableDetailedIOStats=true
DetailedIOStatsOutput=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_paper_params/learned.csv
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
echo "SIFT1M Paper Params Test (MaxDistRatio=7)"
echo "=========================================="
echo "Start time: $(date)"
echo ""

update_csv() {
    local config=$1
    local csv_path=$2
    sed -i "s|DetailedIOStatsOutput=.*|DetailedIOStatsOutput=$csv_path|g" $config
}

# 对比测试：旧参数 (MaxDistRatio=1000000) vs 新参数 (MaxDistRatio=7)
echo "=========================================="
echo "Test 1: Baseline with MaxDistRatio=1000000 (old)"
echo "=========================================="
sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
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
EnableDetailedIOStats=false
EnablePageCache=false
EnableInFlightCoalescing=true
EOF
$BIN "$RESULT_DIR/baseline_old.ini" 2>&1 | tee "$RESULT_DIR/baseline_old.log" | grep -E "actuallQPS|Recall@10"

echo ""
echo "=========================================="
echo "Test 2: Baseline with MaxDistRatio=7 (paper)"
echo "=========================================="
sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
$BIN "$RESULT_DIR/baseline.ini" 2>&1 | tee "$RESULT_DIR/baseline.log" | grep -E "actuallQPS|Recall@10"

echo ""
echo "=========================================="
echo "Test 3: Learned Policy with MaxDistRatio=7 (paper)"
echo "=========================================="
sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
$BIN "$RESULT_DIR/learned.ini" 2>&1 | tee "$RESULT_DIR/learned.log" | grep -E "actuallQPS|Recall@10"

echo ""
echo "=========================================="
echo "All tests completed!"
echo "End time: $(date)"
echo "=========================================="
