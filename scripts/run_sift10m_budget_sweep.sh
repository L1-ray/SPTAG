#!/bin/bash
# Fixed Budget Sweep for SIFT10M
# B ∈ {16, 32, 40, 48, 64, 80, 96, 128}

cd /home/ray/code/SPTAG

BUDGETS=(16 32 40 48 64 80 96 128)
INTERNAL_RESULT_NUM=128

for B in "${BUDGETS[@]}"; do
    echo "=== Running SIFT10M PostingBudget=$B ==="

    # Create config file
    cat > results/adaptive_budget/sift10m_budget_sweep/test_b${B}.ini << EOF
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
SearchResult=/home/ray/code/SPTAG/results/adaptive_budget/sift10m_budget_sweep/search_results_b${B}.bin
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
InternalResultNum=${INTERNAL_RESULT_NUM}
PostingBudget=${B}
NumberOfThreads=40
SearchThreadNum=8
ResultNum=10
MaxDistRatio=1000000
SearchPostingPageLimit=4
EnableDetailedIOStats=true
DetailedIOStatsOutput=/home/ray/code/SPTAG/results/adaptive_budget/sift10m_budget_sweep/query_io_stats_b${B}.csv
DetailedIOStatsSampleRate=1.0
EnablePageCache=false
EnableInFlightCoalescing=true
EOF

    # Run test
    ./Release/ssdserving results/adaptive_budget/sift10m_budget_sweep/test_b${B}.ini > results/adaptive_budget/sift10m_budget_sweep/run_b${B}.log 2>&1

    # Extract key metrics
    RECALL=$(grep "^.*Recall@10:" results/adaptive_budget/sift10m_budget_sweep/run_b${B}.log | tail -1 | sed 's/.*Recall@10: //' | awk '{print $1}')
    QPS=$(grep "actuallQPS is" results/adaptive_budget/sift10m_budget_sweep/run_b${B}.log | sed 's/.*actuallQPS is //' | awk '{print $1}')
    PAGES=$(grep -A2 "Pages Read Per Query:" results/adaptive_budget/sift10m_budget_sweep/run_b${B}.log | tail -1 | awk '{print $2}')

    echo "  Recall@10: $RECALL, QPS: $QPS, Pages: $PAGES"
done

echo "=== SIFT10M Budget Sweep Complete ==="
