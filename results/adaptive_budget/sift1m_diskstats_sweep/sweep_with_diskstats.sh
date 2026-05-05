#!/bin/bash
# sweep_with_diskstats.sh - Test Learned Policy with diskstats monitoring
# Collects both application-level and physical-level I/O stats

set -e

BASE_DIR="/home/ray/code/SPTAG"
RESULT_DIR="$BASE_DIR/results/adaptive_budget/sift1m_diskstats_sweep"
BIN="$BASE_DIR/Release/ssdserving"
DISK_DEVICE="nvme0n1p8"

mkdir -p $RESULT_DIR
cd $BASE_DIR

# Function to get diskstats
get_diskstats() {
    local output_file=$1
    cat /proc/diskstats | grep $DISK_DEVICE | awk '{print $3, $4, $5, $6, $7, $8, $9, $10}' > "$output_file"
}

STS="2 4 8 16"
NT=16
IR=64
THRESHOLD=0.95

echo "=========================================="
echo "SIFT1M Learned Policy Sweep with diskstats"
echo "Config: st in {2,4,8,16}, nt=16, ir=64, threshold=0.95"
echo "Disk device: $DISK_DEVICE"
echo "=========================================="
echo "Start time: $(date)"
echo ""

# Results summary file
SUMMARY_FILE="$RESULT_DIR/diskstats_summary.csv"
echo "config,mode,qps,recall,test_time_s,disk_reads,disk_merges,disk_sectors,disk_read_ms,disk_total_mb,disk_avg_req_kb,merge_rate_pct,disk_iops,disk_bw_mbs,app_bw_mbs" > "$SUMMARY_FILE"

# Baseline tests
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
SearchResult=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_diskstats_sweep/search_results_baseline_${CONFIG_NAME}.bin
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
DetailedIOStatsOutput=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_diskstats_sweep/baseline_${CONFIG_NAME}.csv
DetailedIOStatsSampleRate=1.0
EnablePageCache=false
EnableInFlightCoalescing=true
EOF

    # Clear cache
    sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
    sleep 2

    # Get diskstats before
    get_diskstats "$RESULT_DIR/diskstats_before_baseline_${CONFIG_NAME}.txt"

    # Run test
    START_TIME=$(date +%s.%N)
    $BIN "$RESULT_DIR/baseline_${CONFIG_NAME}.ini" 2>&1 | tee "$RESULT_DIR/baseline_${CONFIG_NAME}.log" | grep -E "actuallQPS|Recall@10"
    END_TIME=$(date +%s.%N)

    # Get diskstats after
    get_diskstats "$RESULT_DIR/diskstats_after_baseline_${CONFIG_NAME}.txt"

    # Calculate all stats using Python
    python3 << PYEOF
import csv

# Read diskstats
def read_diskstats(filepath):
    with open(filepath) as f:
        parts = f.read().strip().split()
        return {
            'reads': int(parts[1]),
            'merges': int(parts[2]),
            'sectors': int(parts[3]),
            'read_ms': int(parts[4])
        }

before = read_diskstats('$RESULT_DIR/diskstats_before_baseline_${CONFIG_NAME}.txt')
after = read_diskstats('$RESULT_DIR/diskstats_after_baseline_${CONFIG_NAME}.txt')

delta_reads = after['reads'] - before['reads']
delta_merges = after['merges'] - before['merges']
delta_sectors = after['sectors'] - before['sectors']
delta_read_ms = after['read_ms'] - before['read_ms']

# Parse log for QPS and recall
with open('$RESULT_DIR/baseline_${CONFIG_NAME}.log') as f:
    log = f.read()
    import re
    qps_match = re.search(r'actuallQPS is ([\d.]+)', log)
    recall_match = re.search(r'Recall@10: ([\d.]+)', log)
    qps = float(qps_match.group(1)) if qps_match else 0
    recall = float(recall_match.group(1)) if recall_match else 0

# Test time
import time
start_time = $START_TIME
end_time = $END_TIME
test_time = end_time - start_time

# Calculate disk metrics
total_mb = delta_sectors * 512 / 1024 / 1024
avg_req_kb = delta_sectors * 512 / 1024 / delta_reads if delta_reads > 0 else 0
merge_rate = delta_merges * 100 / delta_reads if delta_reads > 0 else 0
disk_iops = delta_reads / test_time
disk_bw = total_mb / test_time

# Get app-level bandwidth
import pandas as pd
df = pd.read_csv('$RESULT_DIR/baseline_${CONFIG_NAME}.csv')
app_total_bytes = df['requested_read_bytes'].sum()
app_bw = app_total_bytes / 1024 / 1024 / test_time

print(f"Disk stats: reads={delta_reads}, merges={delta_merges}, sectors={delta_sectors}")
print(f"Disk IOPS: {disk_iops:.0f}, Disk BW: {disk_bw:.2f} MB/s")
print(f"App BW: {app_bw:.2f} MB/s")

# Write to summary
with open('$SUMMARY_FILE', 'a') as f:
    f.write(f"baseline_${CONFIG_NAME},baseline,{qps:.2f},{recall},{test_time:.3f},{delta_reads},{delta_merges},{delta_sectors},{delta_read_ms},{total_mb:.3f},{avg_req_kb:.2f},{merge_rate:.4f},{disk_iops:.0f},{disk_bw:.2f},{app_bw:.2f}\n")
PYEOF

    sleep 3
done

# Learned policy tests
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
SearchResult=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_diskstats_sweep/search_results_learned_${CONFIG_NAME}.bin
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
DetailedIOStatsOutput=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_diskstats_sweep/learned_${CONFIG_NAME}.csv
DetailedIOStatsSampleRate=1.0
EnablePageCache=false
EnableInFlightCoalescing=true
EnableLearnedBudget=true
LearnedBudgetModelPath=/home/ray/code/SPTAG/results/adaptive_budget/sift1m_ir64_retrain
LearnedBudgetThreshold=${THRESHOLD}
LearnedBudgetDefault=64
LearnedBudgetMin=32
EOF

    # Clear cache
    sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
    sleep 2

    # Get diskstats before
    get_diskstats "$RESULT_DIR/diskstats_before_learned_${CONFIG_NAME}.txt"

    # Run test
    START_TIME=$(date +%s.%N)
    $BIN "$RESULT_DIR/learned_${CONFIG_NAME}.ini" 2>&1 | tee "$RESULT_DIR/learned_${CONFIG_NAME}.log" | grep -E "actuallQPS|Recall@10"
    END_TIME=$(date +%s.%N)

    # Get diskstats after
    get_diskstats "$RESULT_DIR/diskstats_after_learned_${CONFIG_NAME}.txt"

    # Calculate all stats using Python
    python3 << PYEOF
import csv

# Read diskstats
def read_diskstats(filepath):
    with open(filepath) as f:
        parts = f.read().strip().split()
        return {
            'reads': int(parts[1]),
            'merges': int(parts[2]),
            'sectors': int(parts[3]),
            'read_ms': int(parts[4])
        }

before = read_diskstats('$RESULT_DIR/diskstats_before_learned_${CONFIG_NAME}.txt')
after = read_diskstats('$RESULT_DIR/diskstats_after_learned_${CONFIG_NAME}.txt')

delta_reads = after['reads'] - before['reads']
delta_merges = after['merges'] - before['merges']
delta_sectors = after['sectors'] - before['sectors']
delta_read_ms = after['read_ms'] - before['read_ms']

# Parse log for QPS and recall
with open('$RESULT_DIR/learned_${CONFIG_NAME}.log') as f:
    log = f.read()
    import re
    qps_match = re.search(r'actuallQPS is ([\d.]+)', log)
    recall_match = re.search(r'Recall@10: ([\d.]+)', log)
    qps = float(qps_match.group(1)) if qps_match else 0
    recall = float(recall_match.group(1)) if recall_match else 0

# Test time
import time
start_time = $START_TIME
end_time = $END_TIME
test_time = end_time - start_time

# Calculate disk metrics
total_mb = delta_sectors * 512 / 1024 / 1024
avg_req_kb = delta_sectors * 512 / 1024 / delta_reads if delta_reads > 0 else 0
merge_rate = delta_merges * 100 / delta_reads if delta_reads > 0 else 0
disk_iops = delta_reads / test_time
disk_bw = total_mb / test_time

# Get app-level bandwidth
import pandas as pd
df = pd.read_csv('$RESULT_DIR/learned_${CONFIG_NAME}.csv')
app_total_bytes = df['requested_read_bytes'].sum()
app_bw = app_total_bytes / 1024 / 1024 / test_time

print(f"Disk stats: reads={delta_reads}, merges={delta_merges}, sectors={delta_sectors}")
print(f"Disk IOPS: {disk_iops:.0f}, Disk BW: {disk_bw:.2f} MB/s")
print(f"App BW: {app_bw:.2f} MB/s")

# Write to summary
with open('$SUMMARY_FILE', 'a') as f:
    f.write(f"learned_${CONFIG_NAME},learned,{qps:.2f},{recall},{test_time:.3f},{delta_reads},{delta_merges},{delta_sectors},{delta_read_ms},{total_mb:.3f},{avg_req_kb:.2f},{merge_rate:.4f},{disk_iops:.0f},{disk_bw:.2f},{app_bw:.2f}\n")
PYEOF

    sleep 3
done

echo ""
echo "=========================================="
echo "All tests completed!"
echo "Results saved to: $SUMMARY_FILE"
echo "End time: $(date)"
echo "=========================================="

# Print summary
echo ""
echo "=== Diskstats Summary ==="
cat "$SUMMARY_FILE" | column -t -s','
