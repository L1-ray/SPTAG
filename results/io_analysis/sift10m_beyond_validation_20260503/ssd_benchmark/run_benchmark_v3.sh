#!/bin/bash
# NVMe SSD 性能基准测试 - 使用预编译的 C 程序

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

# 编译随机读测试程序
cat > $OUTPUT_DIR/randread.c << 'EOF'
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <errno.h>

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <filename> <block_size_kb> <num_reads> [num_threads]\n", argv[0]);
        return 1;
    }

    char *filename = argv[1];
    int block_size = atoi(argv[2]) * 1024;  // KB to bytes
    int num_reads = atoi(argv[3]);
    int num_threads = argc > 4 ? atoi(argv[4]) : 1;

    int fd = open(filename, O_RDONLY | O_DIRECT);
    if (fd < 0) {
        perror("open (O_DIRECT failed, trying without)");
        fd = open(filename, O_RDONLY);
        if (fd < 0) {
            perror("open");
            return 1;
        }
    }

    // Get file size
    off_t file_size = lseek(fd, 0, SEEK_END);
    off_t num_blocks = file_size / block_size;

    // Allocate aligned buffer
    void *buf;
    if (posix_memalign(&buf, 4096, block_size) != 0) {
        perror("posix_memalign");
        close(fd);
        return 1;
    }

    // Random reads
    srand(time(NULL) ^ getpid());
    for (int i = 0; i < num_reads; i++) {
        off_t offset = (rand() % num_blocks) * block_size;
        lseek(fd, offset, SEEK_SET);
        ssize_t n = read(fd, buf, block_size);
        if (n != block_size) {
            fprintf(stderr, "read error at offset %ld: %d (%s)\n", offset, errno, strerror(errno));
        }
    }

    free(buf);
    close(fd);
    return 0;
}
EOF

echo "编译测试程序..."
gcc -O3 -o $OUTPUT_DIR/randread $OUTPUT_DIR/randread.c
if [ $? -ne 0 ]; then
    echo "编译失败!"
    exit 1
fi

# 清除缓存
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
import sys
before_str = """$1"""
after_str = """$2"""
duration = $3
test_name = """$4"""

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

print(f"  {test_name}")
print(f"  ----------------------------------")
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

echo ""
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

calc_perf "$BEFORE" "$AFTER" "$DURATION" "顺序读 1MB 块"

echo ""
echo "=========================================="
echo "测试 2: 顺序读 (8线程, 1MB 块)"
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

calc_perf "$BEFORE" "$AFTER" "$DURATION" "顺序读 1MB 块, 8线程"

echo ""
echo "=========================================="
echo "测试 3: 4K 随机读 (单线程, 100K 次)"
echo "=========================================="
clear_cache
BEFORE=$(get_stats)
START=$(python3 -c "import time; print(time.time())")

$OUTPUT_DIR/randread $DATA_FILE 4 100000 1

END=$(python3 -c "import time; print(time.time())")
AFTER=$(get_stats)
DURATION=$(python3 -c "print(f'{$END - $START:.2f}')")

calc_perf "$BEFORE" "$AFTER" "$DURATION" "4K 随机读"

echo ""
echo "=========================================="
echo "测试 4: 4K 随机读 (8线程, 每线程 80K 次)"
echo "=========================================="
clear_cache
BEFORE=$(get_stats)
START=$(python3 -c "import time; print(time.time())")

for t in {0..7}; do
    $OUTPUT_DIR/randread $DATA_FILE 4 80000 1 &
done
wait

END=$(python3 -c "import time; print(time.time())")
AFTER=$(get_stats)
DURATION=$(python3 -c "print(f'{$END - $START:.2f}')")

calc_perf "$BEFORE" "$AFTER" "$DURATION" "4K 随机读, 8线程"

echo ""
echo "=========================================="
echo "测试 5: 8K 随机读 (8线程, 每线程 80K 次)"
echo "模拟 SPANN 的请求大小"
echo "=========================================="
clear_cache
BEFORE=$(get_stats)
START=$(python3 -c "import time; print(time.time())")

for t in {0..7}; do
    $OUTPUT_DIR/randread $DATA_FILE 8 80000 1 &
done
wait

END=$(python3 -c "import time; print(time.time())")
AFTER=$(get_stats)
DURATION=$(python3 -c "print(f'{$END - $START:.2f}')")

calc_perf "$BEFORE" "$AFTER" "$DURATION" "8K 随机读, 8线程"

echo ""
echo "=========================================="
echo "测试总结与对比"
echo "=========================================="
echo ""
echo "SPANN 实测结果:"
echo "  请求数: 639,963"
echo "  平均请求大小: 7.87 KB"
echo "  合并率: 0.01%"
echo "  IOPS: 137,932"
echo "  吞吐量: 1,060 MB/s"
