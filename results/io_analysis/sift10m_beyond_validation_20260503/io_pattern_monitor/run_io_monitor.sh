#!/bin/bash
# I/O 模式监控测试脚本
# 通过采样 /proc/diskstats 和 pread 参数来分析 I/O 模式

BASE_DIR="/home/ray/code/SPTAG"
OUTPUT_DIR="$BASE_DIR/results/io_analysis/sift10m_beyond_validation_20260503/io_pattern_monitor"
DISK="nvme0n1"
DATA_FILE="/home/ray/data/sift10m/spann_index_u8default_20260430/SPTAGFullList.bin"

mkdir -p $OUTPUT_DIR
cd $BASE_DIR

# 函数：读取 diskstats
read_diskstats() {
    local output_file=$1
    echo "timestamp=$(date +%s.%N)" > $output_file
    grep "$DISK" /proc/diskstats >> $output_file
}

# 函数：计算两个时间点之间的 I/O 统计
calc_io_stats() {
    local before=$1
    local after=$2

    # diskstats 字段说明 (从第4个字段开始，索引从0开始):
    # 0: reads completed successfully
    # 1: reads merged
    # 2: sectors read
    # 3: time spent reading (ms)
    # 4: writes completed
    # 5: writes merged
    # 6: sectors written
    # 7: time spent writing (ms)
    # 8: I/Os currently in progress
    # 9: time spent doing I/Os (ms)
    # 10: weighted time spent doing I/Os (ms)

    local before_data=$(grep "$DISK" $before | awk '{print $4, $5, $6, $7, $8}')
    local after_data=$(grep "$DISK" $after | awk '{print $4, $5, $6, $7, $8}')

    local before_arr=($before_data)
    local after_arr=($after_data)

    local reads_before=${before_arr[0]}
    local reads_after=${after_arr[0]}
    local merges_before=${before_arr[1]}
    local merges_after=${after_arr[1]}
    local sectors_before=${before_arr[2]}
    local sectors_after=${after_arr[2]}
    local read_ms_before=${before_arr[3]}
    local read_ms_after=${after_arr[3]}

    local reads=$((reads_after - reads_before))
    local merges=$((merges_after - merges_before))
    local sectors=$((sectors_after - sectors_before))
    local read_ms=$((read_ms_after - read_ms_before))

    local read_kb=$((sectors / 2))
    local avg_read_size_kb=$((read_kb / reads))
    local avg_latency_ms=$(echo "scale=3; $read_ms / $reads" | bc)

    echo "=== I/O 统计 ==="
    echo "总读请求数: $reads"
    echo "合并读请求数: $merges"
    echo "合并率: $(echo "scale=1; $merges * 100 / $reads" | bc)%"
    echo "总读取量: $read_kb KB ($((read_kb / 1024)) MB)"
    echo "平均请求大小: $avg_read_size_kb KB"
    echo "平均延迟: $avg_latency_ms ms"
}

echo "=== I/O 模式监控测试 ==="
echo ""
echo "数据文件: $DATA_FILE"
echo "监控磁盘: $DISK"
echo ""

# 1. 测试前的 diskstats
echo "记录测试前的 diskstats..."
read_diskstats "$OUTPUT_DIR/diskstats_before.txt"
cat "$OUTPUT_DIR/diskstats_before.txt"

# 2. 创建测试配置
cat > "$OUTPUT_DIR/test_monitor.ini" << EOF
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
SearchResult=$OUTPUT_DIR/search_results.bin
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
EnablePageCache=false
EnableDetailedIOStats=true
DetailedIOStatsOutput=$OUTPUT_DIR/query_io_stats.csv
EOF

# 3. 运行测试 (同时在后台采样 diskstats)
echo ""
echo "运行测试 (st=8, cache=off)..."
./Release/ssdserving "$OUTPUT_DIR/test_monitor.ini" > "$OUTPUT_DIR/test.log" 2>&1 &
TEST_PID=$!

# 后台采样 diskstats (每100ms采样一次)
while kill -0 $TEST_PID 2>/dev/null; do
    echo "$(date +%s.%N) $(grep "$DISK" /proc/diskstats)" >> "$OUTPUT_DIR/diskstats_timeseries.txt"
    sleep 0.1
done

wait $TEST_PID

# 4. 测试后的 diskstats
echo ""
echo "记录测试后的 diskstats..."
read_diskstats "$OUTPUT_DIR/diskstats_after.txt"
cat "$OUTPUT_DIR/diskstats_after.txt"

# 5. 计算 I/O 统计
echo ""
calc_io_stats "$OUTPUT_DIR/diskstats_before.txt" "$OUTPUT_DIR/diskstats_after.txt"

# 6. 分析 diskstats 时间序列
echo ""
echo "=== 分析 I/O 时间序列 ==="
python3 << 'PYTHON_SCRIPT'
import re

# 读取时间序列数据
data = []
with open("$OUTPUT_DIR/diskstats_timeseries.txt", "r") as f:
    for line in f:
        parts = line.strip().split()
        if len(parts) >= 14:
            data.append({
                'time': float(parts[0]),
                'reads': int(parts[3]),
                'merges': int(parts[4]),
                'sectors': int(parts[5]),
                'read_ms': int(parts[6])
            })

if len(data) < 2:
    print("数据不足，无法分析")
    exit()

# 计算采样间隔的 I/O 速率
intervals = []
for i in range(1, len(data)):
    dt = data[i]['time'] - data[i-1]['time']
    if dt > 0:
        d_reads = data[i]['reads'] - data[i-1]['reads']
        d_sectors = data[i]['sectors'] - data[i-1]['sectors']
        d_merges = data[i]['merges'] - data[i-1]['merges']

        reads_per_sec = d_reads / dt
        kb_per_sec = (d_sectors / 2) / dt  # sectors to KB
        avg_req_size = (d_sectors / 2) / d_reads if d_reads > 0 else 0
        merge_rate = d_merges / d_reads * 100 if d_reads > 0 else 0

        intervals.append({
            'reads_per_sec': reads_per_sec,
            'kb_per_sec': kb_per_sec,
            'avg_req_size': avg_req_size,
            'merge_rate': merge_rate
        })

# 统计分析
if intervals:
    avg_reads_per_sec = sum(i['reads_per_sec'] for i in intervals) / len(intervals)
    avg_kb_per_sec = sum(i['kb_per_sec'] for i in intervals) / len(intervals)
    avg_req_size = sum(i['avg_req_size'] for i in intervals) / len(intervals)
    avg_merge_rate = sum(i['merge_rate'] for i in intervals) / len(intervals)

    # 计算方差
    import statistics
    if len(intervals) > 1:
        var_reads = statistics.variance(i['reads_per_sec'] for i in intervals)
        var_kb = statistics.variance(i['kb_per_sec'] for i in intervals)
    else:
        var_reads = var_kb = 0

    print(f"平均 IOPS: {avg_reads_per_sec:.0f}")
    print(f"平均吞吐量: {avg_kb_per_sec:.0f} KB/s ({avg_kb_per_sec/1024:.1f} MB/s)")
    print(f"平均请求大小: {avg_req_size:.1f} KB")
    print(f"平均合并率: {avg_merge_rate:.1f}%")
    print(f"IOPS 方差: {var_reads:.0f}")
    print(f"吞吐量方差: {var_kb:.0f}")

    # 判断 I/O 模式
    print("")
    print("=== I/O 模式判断 ===")
    if avg_req_size > 32:  # 平均请求 > 32KB
        print("倾向顺序读: 平均请求大小较大")
    elif avg_req_size < 8:  # 平均请求 < 8KB
        print("倾向随机读: 平均请求大小接近 4KB")
    else:
        print("混合模式: 平均请求大小在 8-32KB 之间")

    if avg_merge_rate > 30:
        print("倾向顺序读: 合并率较高 (请求连续)")
    elif avg_merge_rate < 10:
        print("倾向随机读: 合并率较低 (请求分散)")
PYTHON_SCRIPT

echo ""
echo "=== 分析完成 ==="
echo "详细数据保存在: $OUTPUT_DIR/"
