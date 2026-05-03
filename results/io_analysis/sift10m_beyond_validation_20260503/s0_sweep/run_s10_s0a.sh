#!/bin/bash
# S10-S0a: Baseline st sweep for SIFT10M
# st=4/8/12/16, nt=16, ir=64, pl=4
# Each point 3 runs

BASE_DIR="/home/ray/code/SPTAG"
OUTPUT_DIR="$BASE_DIR/results/io_analysis/sift10m_beyond_validation_20260503/s0_sweep"
TEMPLATE="$OUTPUT_DIR/s10_s0a_template.ini"

cd $BASE_DIR

for st in 4 8 12 16; do
    for run in 1 2 3; do
        LOG_FILE="$OUTPUT_DIR/s10_s0a_st${st}_run${run}.log"
        STATS_FILE="$OUTPUT_DIR/query_io_stats_st${st}_run${run}.csv"

        # Update config for this run
        sed -e "s/SearchThreadNum=.*/SearchThreadNum=$st/" \
            -e "s|DetailedIOStatsOutput=.*|DetailedIOStatsOutput=$STATS_FILE|" \
            "$TEMPLATE" > "$OUTPUT_DIR/s10_s0a_st${st}_run${run}.ini"

        echo "Running S10-S0a: st=$st, run=$run"
        ./Release/ssdserving "$OUTPUT_DIR/s10_s0a_st${st}_run${run}.ini" > "$LOG_FILE" 2>&1

        # Extract key metrics from log
        QPS=$(grep "Finish sending" "$LOG_FILE" | grep -oP 'actuallQPS is \K[0-9.]+')
        RECALL=$(grep "Recall10@10:" "$LOG_FILE" | grep -oP 'Recall10@10: \K[0-9.]+')
        echo "  QPS: $QPS, Recall@10: $RECALL"
    done
done

echo ""
echo "S10-S0a complete. Summary:"
echo "st,QPS_run1,QPS_run2,QPS_run3,Recall_run1,Recall_run2,Recall_run3"
for st in 4 8 12 16; do
    QPS1=$(grep "actuallQPS is" "$OUTPUT_DIR/s10_s0a_st${st}_run1.log" | grep -oP 'actuallQPS is \K[0-9.]+')
    QPS2=$(grep "actuallQPS is" "$OUTPUT_DIR/s10_s0a_st${st}_run2.log" | grep -oP 'actuallQPS is \K[0-9.]+')
    QPS3=$(grep "actuallQPS is" "$OUTPUT_DIR/s10_s0a_st${st}_run3.log" | grep -oP 'actuallQPS is \K[0-9.]+')
    R1=$(grep "Recall10@10:" "$OUTPUT_DIR/s10_s0a_st${st}_run1.log" | grep -oP 'Recall10@10: \K[0-9.]+')
    R2=$(grep "Recall10@10:" "$OUTPUT_DIR/s10_s0a_st${st}_run2.log" | grep -oP 'Recall10@10: \K[0-9.]+')
    R3=$(grep "Recall10@10:" "$OUTPUT_DIR/s10_s0a_st${st}_run3.log" | grep -oP 'Recall10@10: \K[0-9.]+')
    echo "$st,$QPS1,$QPS2,$QPS3,$R1,$R2,$R3"
done
