#!/bin/bash
# sift1m_ir_comparison.sh - SIFT1M ir=64 vs ir=128 对比测试
# 配置: SearchThreadNum=8, NumberOfThreads=16, threshold=0.90

set -e

BASE_DIR="/home/ray/code/SPTAG"
RESULT_DIR="$BASE_DIR/results/adaptive_budget/sift1m_ir_comparison"
BIN="$BASE_DIR/Release/ssdserving"

mkdir -p $RESULT_DIR
cd $BASE_DIR

# 创建 baseline ir=64 配置
cat > "$RESULT_DIR/baseline_ir64.ini" << 'EOF'
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
SearchResult=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_ir_comparison/search_results_baseline_ir64.bin
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
DetailedIOStatsOutput=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_ir_comparison/baseline_ir64.csv
DetailedIOStatsSampleRate=1.0
EnablePageCache=false
EnableInFlightCoalescing=true
EOF

# 创建 baseline ir=128 配置
cat > "$RESULT_DIR/baseline_ir128.ini" << 'EOF'
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
SearchResult=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_ir_comparison/search_results_baseline_ir128.bin
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
NumberOfThreads=16
SearchThreadNum=8
ResultNum=10
MaxDistRatio=1000000
SearchPostingPageLimit=4
EnableDetailedIOStats=true
DetailedIOStatsOutput=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_ir_comparison/baseline_ir128.csv
DetailedIOStatsSampleRate=1.0
EnablePageCache=false
EnableInFlightCoalescing=true
EOF

# 创建 learned ir=64 配置
cat > "$RESULT_DIR/learned_ir64.ini" << 'EOF'
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
SearchResult=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_ir_comparison/search_results_learned_ir64.bin
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
DetailedIOStatsOutput=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_ir_comparison/learned_ir64.csv
DetailedIOStatsSampleRate=1.0
EnablePageCache=false
EnableInFlightCoalescing=true
EnableLearnedBudget=true
LearnedBudgetModelPath=/home/ray/code/SPTAG/results/adaptive_budget/phase4_learned
LearnedBudgetThreshold=0.90
LearnedBudgetDefault=64
LearnedBudgetMin=32
EOF

# 创建 learned ir=128 配置
cat > "$RESULT_DIR/learned_ir128.ini" << 'EOF'
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
SearchResult=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_ir_comparison/search_results_learned_ir128.bin
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
NumberOfThreads=16
SearchThreadNum=8
ResultNum=10
MaxDistRatio=1000000
SearchPostingPageLimit=4
EnableDetailedIOStats=true
DetailedIOStatsOutput=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_ir_comparison/learned_ir128.csv
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
echo "SIFT1M ir=64 vs ir=128 Comparison"
echo "Config: SearchThreadNum=8, NumberOfThreads=16, threshold=0.90"
echo "=========================================="
echo "Start time: $(date)"
echo ""

# Test 1: Baseline ir=64
echo "[1/4] Baseline ir=64 - Clearing cache..."
sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
$BIN "$RESULT_DIR/baseline_ir64.ini" 2>&1 | tee "$RESULT_DIR/baseline_ir64.log" | grep -E "actuallQPS|Recall@10"
sleep 5

# Test 2: Learned ir=64
echo ""
echo "[2/4] Learned ir=64 - Clearing cache..."
sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
$BIN "$RESULT_DIR/learned_ir64.ini" 2>&1 | tee "$RESULT_DIR/learned_ir64.log" | grep -E "actuallQPS|Recall@10"
sleep 5

# Test 3: Baseline ir=128
echo ""
echo "[3/4] Baseline ir=128 - Clearing cache..."
sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
$BIN "$RESULT_DIR/baseline_ir128.ini" 2>&1 | tee "$RESULT_DIR/baseline_ir128.log" | grep -E "actuallQPS|Recall@10"
sleep 5

# Test 4: Learned ir=128
echo ""
echo "[4/4] Learned ir=128 - Clearing cache..."
sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
$BIN "$RESULT_DIR/learned_ir128.ini" 2>&1 | tee "$RESULT_DIR/learned_ir128.log" | grep -E "actuallQPS|Recall@10"

echo ""
echo "=========================================="
echo "All tests completed!"
echo "End time: $(date)"
echo "=========================================="
