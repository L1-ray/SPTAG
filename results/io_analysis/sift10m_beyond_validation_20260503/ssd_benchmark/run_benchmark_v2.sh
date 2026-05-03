#!/bin/bash
# NVMe SSD 性能基准测试 - 改进版
# 使用 fio 或直接读取绕过缓存

BASE_DIR="/home/ray/code/SPTAG"
OUTPUT_DIR="$BASE_DIR/results/io_analysis/sift10m_beyond_validation_20260503/ssd_benchmark"
DATA_FILE="/home/ray/data/sift10m/spann_index_u8default_20260430/SPTAGFullList.bin"
DISK="nvme0n1p8"

mkdir -p $OUTPUT_DIR
cd $BASE_DIR

echo "=========================================="
echo "NVMe SSD 性能基准测试"
echo "=========================================="
echo ""
echo "数据文件: $DATA_FILE"
echo "文件大小: $(ls -lh $DATA_FILE | awk '{print $5}')"
echo ""

# 清除页面缓存
clear_cache() {
    sync
    echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null 2>&1 || true
}

# 获取 diskstats
get_stats() {
    grep "$DISK" /proc/diskstats
}

# 计算性能
calc_perf() {
    python3 << PYEOF
before_str = """$1"""
after_str = """$2"""
duration = $3

def parse_stats(s):
    parts = s.split()
    return {
        'reads': int(parts[3]),
        'merges': int(parts[4]),
        'sectors': int(parts[5]),
        'read_ms': int(parts[6])
    }

before = parse_stats(before_str)
after = parse_stats(after_str)

delta_reads = after['reads'] - before['reads']
delta_merges = after['merges'] - before['merges']
delta_sectors = after['sectors'] - before['sectors']
delta_kb = delta_sectors // 2
delta_mb = delta_kb // 1024

if delta_reads == 0:
    print("  无读取请求")
else:
    avg_req = delta_kb / delta_reads
    merge_rate = delta_merges / delta_reads * 100
    iops = delta_reads / duration
    throughput = delta_mb / duration

    print(f"  请求数: {delta_reads:,}")
    print(f"  合并数: {delta_merges:,}")
    print(f"  合并率: {merge_rate:.2f}%")
    print(f"  读取量: {delta_mb:,} MB")
    print(f"  平均请求大小: {avg_req:.2f} KB")
    print(f"  测试时长: {duration:.2f} 秒")
    print(f"  IOPS: {iops:,.0f}")
    print(f"  吞吐量: {throughput:.0f} MB/s")
PYEOF
}

echo "=========================================="
echo "测试 1: 顺序读 (单线程, 1MB 块)"
echo "=========================================="
clear_cache
BEFORE=$(get_stats)
START=$(python3 -c "import time; print(time.time())")

dd if=$DATA_FILE of=/dev/null bs=1M count=1024 iflag=direct 2>/dev/null

END=$(python3 -c "import time; print(time.time())")
AFTER=$(get_stats)
DURATION=$(python3 -c "print(f'{$END - $START:.2f}')")

calc_perf "$BEFORE" "$AFTER" "$DURATION"

echo ""
echo "=========================================="
echo "测试 2: 顺序读 (8线程, 每线程 128MB)"
echo "=========================================="
clear_cache
BEFORE=$(get_stats)
START=$(python3 -c "import time; print(time.time())")

for i in {0..7}; do
    dd if=$DATA_FILE of=/dev/null bs=1M count=128 skip=$((i * 1024)) iflag=direct 2>/dev/null &
done
wait

END=$(python3 -c "import time; print(time.time())")
AFTER=$(get_stats)
DURATION=$(python3 -c "print(f'{$END - $START:.2f}')")

calc_perf "$BEFORE" "$AFTER" "$DURATION"

echo ""
echo "=========================================="
echo "测试 3: 4K 随机读 (单线程, 100K 次)"
echo "=========================================="

# 使用 dd 的 skip 参数模拟随机读
clear_cache
BEFORE=$(get_stats)
START=$(python3 -c "import time; print(time.time())")

# 生成随机 offset 并读取
python3 << 'PYEOF' &
import subprocess
import random
import os

filename = "/home/ray/data/sift10m/spann_index_u8default_20260430/SPTAGFullList.bin"
file_size = os.path.getsize(filename)
num_blocks = file_size // 4096

for _ in range(100000):
    offset = random.randint(0, num_blocks - 1)
    # 使用 dd 读取 4K，skip 到随机位置
    subprocess.run([
        'dd', 'if=' + filename, 'of=/dev/null',
        'bs=4K', 'skip=' + str(offset), 'count=1', 'iflag=direct'
    ], stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
PYEOF
wait

END=$(python3 -c "import time; print(time.time())")
AFTER=$(get_stats)
DURATION=$(python3 -c "print(f'{$END - $START:.2f}')")

calc_perf "$BEFORE" "$AFTER" "$DURATION"

echo ""
echo "=========================================="
echo "测试 4: 4K 随机读 (8线程, 每线程 80K 次)"
echo "模拟 SPANN 的并发模式"
echo "=========================================="

clear_cache
BEFORE=$(get_stats)
START=$(python3 -c "import time; print(time.time())")

for t in {0..7}; do
    python3 << 'PYEOF' &
import subprocess
import random
import os

filename = "/home/ray/data/sift10m/spann_index_u8default_20260430/SPTAGFullList.bin"
file_size = os.path.getsize(filename)
num_blocks = file_size // 4096
random.seed(os.getpid())  # 每个进程不同种子

for _ in range(80000):
    offset = random.randint(0, num_blocks - 1)
    subprocess.run([
        'dd', 'if=' + filename, 'of=/dev/null',
        'bs=4K', 'skip=' + str(offset), 'count=1', 'iflag=direct'
    ], stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
PYEOF
done
wait

END=$(python3 -c "import time; print(time.time())")
AFTER=$(get_stats)
DURATION=$(python3 -c "print(f'{$END - $START:.2f}')")

calc_perf "$BEFORE" "$AFTER" "$DURATION"

echo ""
echo "=========================================="
echo "测试 5: 8K 随机读 (8线程, 每线程 80K 次)"
echo "模拟 SPANN 的实际请求大小"
echo "=========================================="

clear_cache
BEFORE=$(get_stats)
START=$(python3 -c "import time; print(time.time())")

for t in {0..7}; do
    python3 << 'PYEOF' &
import subprocess
import random
import os

filename = "/home/ray/data/sift10m/spann_index_u8default_20260430/SPTAGFullList.bin"
file_size = os.path.getsize(filename)
num_blocks = file_size // 8192  # 8K blocks
random.seed(os.getpid())

for _ in range(80000):
    offset = random.randint(0, num_blocks - 1)
    subprocess.run([
        'dd', 'if=' + filename, 'of=/dev/null',
        'bs=8K', 'skip=' + str(offset), 'count=1', 'iflag=direct'
    ], stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
PYEOF
done
wait

END=$(python3 -c "import time; print(time.time())")
AFTER=$(get_stats)
DURATION=$(python3 -c "print(f'{$END - $START:.2f}')")

calc_perf "$BEFORE" "$AFTER" "$DURATION"

echo ""
echo "=========================================="
echo "测试总结与对比"
echo "=========================================="
echo ""
echo "SPANN 测试结果:"
echo "  请求数: 639,963"
echo "  平均请求大小: 7.87 KB"
echo "  合并率: 0.01%"
echo "  IOPS: 137,932"
echo "  吞吐量: 1,060 MB/s"
echo ""
