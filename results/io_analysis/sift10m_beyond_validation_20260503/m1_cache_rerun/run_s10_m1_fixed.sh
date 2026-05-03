#!/bin/bash
# S10-M1: Page Cache validation for SIFT10M (Fixed config format)
# Test st=8, cache=off vs cache=on (256MB)
# Each point 3 runs

BASE_DIR="/home/ray/code/SPTAG"
OUTPUT_DIR="$BASE_DIR/results/io_analysis/sift10m_beyond_validation_20260503/m1_cache_rerun"

cd $BASE_DIR

mkdir -p $OUTPUT_DIR

for cache in off on; do
    for run in 1 2 3; do
        LOG_FILE="$OUTPUT_DIR/m1_st8_cache${cache}_run${run}.log"
        STATS_FILE="$OUTPUT_DIR/query_io_stats_st8_cache${cache}_run${run}.csv"

        # Create config with CORRECT format (each parameter on its own line)
        cat > "$OUTPUT_DIR/m1_st8_cache${cache}_run${run}.ini" << EOF
[Base]
ValueType=UInt8
DistCalcMethod=L2
IndexAlgoType=BKT
Dim=128
VectorPath=/home/ray/data/sift10m/bigann10m_base.u8bin
VectorType=DEFAULT
VectorSize=10000000
VectorDelimiter=
QueryPath=/home/ray/data/sift10m/query.public.10K.u8bin
QueryType=DEFAULT
QuerySize=10000
QueryDelimiter=
WarmupPath=
WarmupType=DEFAULT
WarmupSize=10000
WarmupDelimiter=
TruthPath=/home/ray/data/sift10m/bigann-10M.bin
TruthType=DEFAULT
GenerateTruth=false
IndexDirectory=/home/ray/data/sift10m/spann_index_u8default_20260430
SearchResult=$OUTPUT_DIR/search_results_st8_cache${cache}_run${run}.bin
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
InternalResultNum=64
NumberOfThreads=16
SearchThreadNum=8
ResultNum=10
MaxDistRatio=1000000
SearchPostingPageLimit=4
EOF

        if [ "$cache" = "off" ]; then
            echo "EnablePageCache=false" >> "$OUTPUT_DIR/m1_st8_cache${cache}_run${run}.ini"
        else
            echo "EnablePageCache=true" >> "$OUTPUT_DIR/m1_st8_cache${cache}_run${run}.ini"
            echo "PageCacheMaxBytes=268435456" >> "$OUTPUT_DIR/m1_st8_cache${cache}_run${run}.ini"
        fi

        echo "EnableDetailedIOStats=true" >> "$OUTPUT_DIR/m1_st8_cache${cache}_run${run}.ini"
        echo "DetailedIOStatsOutput=$STATS_FILE" >> "$OUTPUT_DIR/m1_st8_cache${cache}_run${run}.ini"

        echo "Running M1: st=8, cache=$cache, run=$run"
        ./Release/ssdserving "$OUTPUT_DIR/m1_st8_cache${cache}_run${run}.ini" > "$LOG_FILE" 2>&1

        # Extract key metrics
        QPS=$(grep "Finish sending" "$LOG_FILE" | grep -oP 'actuallQPS is \K[0-9.]+')
        RECALL=$(grep "Recall10@10:" "$LOG_FILE" | grep -oP 'Recall10@10: \K[0-9.]+')
        echo "  QPS: $QPS, Recall@10: $RECALL"

        # Extract cache stats if available
        CACHE_STATS=$(grep "Sharded page cache stats" "$LOG_FILE")
        if [ -n "$CACHE_STATS" ]; then
            echo "  Cache stats: $CACHE_STATS"
        fi
    done
done

echo ""
echo "S10-M1 complete. Summary:"
echo ""

echo "Cache OFF:"
QPS1=$(grep "actuallQPS is" "$OUTPUT_DIR/m1_st8_cacheoff_run1.log" 2>/dev/null | grep -oP 'actuallQPS is \K[0-9.]+')
QPS2=$(grep "actuallQPS is" "$OUTPUT_DIR/m1_st8_cacheoff_run2.log" 2>/dev/null | grep -oP 'actuallQPS is \K[0-9.]+')
QPS3=$(grep "actuallQPS is" "$OUTPUT_DIR/m1_st8_cacheoff_run3.log" 2>/dev/null | grep -oP 'actuallQPS is \K[0-9.]+')
OFF_AVG=$(echo "$QPS1 $QPS2 $QPS3" | awk '{printf "%.2f", ($1+$2+$3)/3}')
R=$(grep "Recall10@10:" "$OUTPUT_DIR/m1_st8_cacheoff_run1.log" 2>/dev/null | grep -oP 'Recall10@10: \K[0-9.]+')
echo "st=8, QPS_avg=$OFF_AVG, Recall=$R"

echo ""
echo "Cache ON (256MB):"
QPS1=$(grep "actuallQPS is" "$OUTPUT_DIR/m1_st8_cacheon_run1.log" 2>/dev/null | grep -oP 'actuallQPS is \K[0-9.]+')
QPS2=$(grep "actuallQPS is" "$OUTPUT_DIR/m1_st8_cacheon_run2.log" 2>/dev/null | grep -oP 'actuallQPS is \K[0-9.]+')
QPS3=$(grep "actuallQPS is" "$OUTPUT_DIR/m1_st8_cacheon_run3.log" 2>/dev/null | grep -oP 'actuallQPS is \K[0-9.]+')
ON_AVG=$(echo "$QPS1 $QPS2 $QPS3" | awk '{printf "%.2f", ($1+$2+$3)/3}')
R=$(grep "Recall10@10:" "$OUTPUT_DIR/m1_st8_cacheon_run1.log" 2>/dev/null | grep -oP 'Recall10@10: \K[0-9.]+')
CHANGE=$(echo "$ON_AVG $OFF_AVG" | awk '{printf "%.1f%%", ($1-$2)/$2*100}')
echo "st=8, QPS_avg=$ON_AVG, Recall=$R, Change=$CHANGE"

echo ""
echo "Cache stats from run1:"
grep "Sharded page cache stats" "$OUTPUT_DIR/m1_st8_cacheon_run1.log" 2>/dev/null || echo "No cache stats found"
