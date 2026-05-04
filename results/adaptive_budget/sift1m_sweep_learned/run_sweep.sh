#!/bin/bash
# sweep_learned_policy.sh - 在不同参数配置下测试 Learned Policy

set -e

BASE_DIR="/home/ray/code/SPTAG"
RESULT_DIR="$BASE_DIR/results/adaptive_budget/sift1m_sweep_learned"
BIN="$BASE_DIR/Release/ssdserving"

mkdir -p $RESULT_DIR

cd $BASE_DIR

echo "=========================================="
echo "SIFT1M Learned Policy Sweep Test"
echo "Threshold: 0.90 (optimal from threshold sweep)"
echo "=========================================="
echo "Start time: $(date)"
echo ""

# 测试配置列表 (基于 SIFT1M_Official_Alignment_Summary.md 8.6 sweep 全量结果表)
# 格式: "name st nt ir pl"
CONFIGS=(
    "st2_nt40_ir32_pl4 2 40 32 4"
    "st2_nt40_ir64_pl4 2 40 64 4"
    "st2_nt40_ir96_pl4 2 40 96 4"
    "st4_nt40_ir64_pl4 4 40 64 4"
    "st8_nt40_ir64_pl4 8 40 64 4"
    "st8_nt16_ir64_pl4 8 16 64 4"
)

# 创建配置文件模板函数
create_config() {
    local name=$1
    local st=$2
    local nt=$3
    local ir=$4
    local pl=$5
    local is_baseline=$6

    local config_path="$RESULT_DIR/${name}_$([[ "$is_baseline" == "true" ]] && echo "baseline" || echo "learned").ini"

    cat > $config_path << EOF
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
SearchResult=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_sweep_learned/search_results_${name}.bin
HeadIndexFolder=head_index

[SelectHead]
isExecute=false

[BuildHead]
isExecute=false

[SearchSSDIndex]
isExecute=true
BuildSsdIndex=false
InternalResultNum=${ir}
PostingBudget=$([[ "$is_baseline" == "true" ]] && echo "64" || echo "0")
NumberOfThreads=${nt}
SearchThreadNum=${st}
ResultNum=10
MaxDistRatio=1000000
SearchPostingPageLimit=${pl}
EnableDetailedIOStats=true
DetailedIOStatsOutput=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_sweep_learned/${name}_$([[ "$is_baseline" == "true" ]] && echo "baseline" || echo "learned").csv
DetailedIOStatsSampleRate=1.0
EnablePageCache=false
EnableInFlightCoalescing=true
EOF

    # 如果是 learned，添加 Learned Policy 配置
    if [[ "$is_baseline" == "false" ]]; then
        cat >> $config_path << EOF
EnableLearnedBudget=true
LearnedBudgetModelPath=/home/ray/code/SPTAG/results/adaptive_budget/phase4_learned
LearnedBudgetThreshold=0.90
LearnedBudgetDefault=64
LearnedBudgetMin=32
EOF
    fi

    echo $config_path
}

echo "=========================================="
echo "Creating configuration files..."
echo "=========================================="

for config in "${CONFIGS[@]}"; do
    read name st nt ir pl <<< "$config"

    # 创建 baseline 配置
    create_config "$name" "$st" "$nt" "$ir" "$pl" "true"
    # 创建 learned 配置
    create_config "$name" "$st" "$nt" "$ir" "$pl" "false"
done

echo ""
echo "=========================================="
echo "Running tests (clearing cache before each)..."
echo "=========================================="
echo ""

# 输出表头
printf "%-25s %10s %10s %12s %12s %12s\n" "Config" "Baseline" "Learned" "QPS Delta" "Baseline R" "Learned R"
echo "--------------------------------------------------------------------------------------------------------"

for config in "${CONFIGS[@]}"; do
    read name st nt ir pl <<< "$config"

    baseline_config="$RESULT_DIR/${name}_baseline.ini"
    learned_config="$RESULT_DIR/${name}_learned.ini"

    # Baseline test
    sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
    baseline_output=$($BIN "$baseline_config" 2>&1 | grep -E "actuallQPS|Recall@10")
    baseline_qps=$(echo "$baseline_output" | grep "actuallQPS" | sed 's/.*actuallQPS is \([0-9.]*\).*/\1/')
    baseline_recall=$(echo "$baseline_output" | grep "Recall@10" | sed 's/.*Recall@10: \([0-9.]*\).*/\1/')

    # Learned test
    sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
    learned_output=$($BIN "$learned_config" 2>&1 | grep -E "actuallQPS|Recall@10")
    learned_qps=$(echo "$learned_output" | grep "actuallQPS" | sed 's/.*actuallQPS is \([0-9.]*\).*/\1/')
    learned_recall=$(echo "$learned_output" | grep "Recall@10" | sed 's/.*Recall@10: \([0-9.]*\).*/\1/')

    # Calculate delta
    if [[ -n "$baseline_qps" && -n "$learned_qps" ]]; then
        delta=$(python3 -c "print(f'{100*($learned_qps - $baseline_qps)/$baseline_qps:.2f}')")
    else
        delta="N/A"
    fi

    printf "%-25s %10s %10s %11s%% %12s %12s\n" "$name" "$baseline_qps" "$learned_qps" "$delta" "$baseline_recall" "$learned_recall"
done

echo ""
echo "=========================================="
echo "All tests completed!"
echo "End time: $(date)"
echo "=========================================="
