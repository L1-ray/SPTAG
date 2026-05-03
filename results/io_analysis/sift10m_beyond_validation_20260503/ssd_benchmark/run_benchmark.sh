#!/bin/bash
# NVMe SSD 性能基准测试
# 测试顺序读和 4K 随机读性能

BASE_DIR="/home/ray/code/SPTAG"
OUTPUT_DIR="$BASE_DIR/results/io_analysis/sift10m_beyond_validation_20260503/ssd_benchmark"
DATA_FILE="/home/ray/data/sift10m/spann_index_u8default_20260430/SPTAGFullList.bin"
DISK="nvme0n1p8"

mkdir -p $OUTPUT_DIR
cd $BASE_DIR

# 函数：读取 diskstats
read_diskstats() {
    grep "$DISK" /proc/diskstats
}

# 函数：计算性能指标
calc_perf() {
    local before="$1"
    local after="$2"
    local duration="$3"

    local before_arr=($before)
    local after_arr=($after)

    # 字段索引: 3=reads, 4=merges, 5=sectors, 6=read_ms
    local reads_before=${before_arr[3]}
    local reads_after=${after_arr[3]}
    local merges_before=${before_arr[4]}
    local merges_after=${after_arr[4]}
    local sectors_before=${before_arr[5]}
    local sectors_after=${after_arr[5]}

    local delta_reads=$((reads_after - reads_before))
    local delta_merges=$((merges_after - merges_before))
    local delta_sectors=$((sectors_after - sectors_before))

    local delta_kb=$((delta_sectors / 2))
    local delta_mb=$((delta_kb / 1024))

    local iops=$(echo "$delta_reads / $duration" | bc 2>/dev/null || python3 -c "print(int($delta_reads / $duration))")
    local throughput=$(echo "$delta_mb / $duration" | bc 2>/dev/null || python3 -c "print(f'{$delta_mb / $duration:.0f}')")
    local avg_req_size=$(python3 -c "print(f'{$delta_kb / $delta_reads:.2f}')")
    local merge_rate=$(python3 -c "print(f'{$delta_merges / $delta_reads * 100:.2f}')")

    echo "  请求数: $delta_reads"
    echo "  合并数: $delta_merges"
    echo "  合并率: ${merge_rate}%"
    echo "  读取量: ${delta_mb} MB"
    echo "  平均请求大小: ${avg_req_size} KB"
    echo "  测试时长: ${duration} 秒"
    echo "  IOPS: ${iops}"
    echo "  吞吐量: ${throughput} MB/s"
}

echo "=========================================="
echo "NVMe SSD 性能基准测试"
echo "=========================================="
echo ""
echo "数据文件: $DATA_FILE"
echo "文件大小: $(ls -lh $DATA_FILE | awk '{print $5}')"
echo "监控分区: $DISK"
echo ""

# 获取文件大小
FILE_SIZE=$(stat -c %s $DATA_FILE)
FILE_SIZE_MB=$((FILE_SIZE / 1024 / 1024))

echo "=========================================="
echo "测试 1: 顺序读 (1MB 块，单线程)"
echo "=========================================="
echo ""

BEFORE=$(read_diskstats)
START_TIME=$(date +%s.%N)

# 使用 dd 进行顺序读测试
dd if=$DATA_FILE of=/dev/null bs=1M count=1024 2>/dev/null

END_TIME=$(date +%s.%N)
AFTER=$(read_diskstats)
DURATION=$(python3 -c "print(f'{$END_TIME - $START_TIME:.2f}')")

echo "结果:"
calc_perf "$BEFORE" "$AFTER" "$DURATION"

echo ""
echo "=========================================="
echo "测试 2: 顺序读 (1MB 块，8 线程并发)"
echo "=========================================="
echo ""

BEFORE=$(read_diskstats)
START_TIME=$(date +%s.%N)

# 8 线程并发顺序读，每个线程读取不同区域
for i in {0..7}; do
    offset=$((i * 1024 * 1024 * 128))  # 每个 thread 从不同 offset 开始
    (dd if=$DATA_FILE of=/dev/null bs=1M count=256 skip=$((i * 256)) 2>/dev/null) &
done
wait

END_TIME=$(date +%s.%N)
AFTER=$(read_diskstats)
DURATION=$(python3 -c "print(f'{$END_TIME - $START_TIME:.2f}')")

echo "结果:"
calc_perf "$BEFORE" "$AFTER" "$DURATION"

echo ""
echo "=========================================="
echo "测试 3: 4K 随机读 (单线程)"
echo "=========================================="
echo ""

# 创建随机读测试程序
cat > /tmp/random_read_test.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

#define BLOCK_SIZE 4096
#define NUM_READS 100000

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
        return 1;
    }

    int fd = open(argv[1], O_RDONLY | O_DIRECT);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    // 获取文件大小
    off_t file_size = lseek(fd, 0, SEEK_END);
    off_t num_blocks = file_size / BLOCK_SIZE;

    // 分配对齐的缓冲区
    void *buf;
    if (posix_memalign(&buf, BLOCK_SIZE, BLOCK_SIZE) != 0) {
        perror("posix_memalign");
        return 1;
    }

    // 随机读
    srand(time(NULL));
    for (int i = 0; i < NUM_READS; i++) {
        off_t offset = (rand() % num_blocks) * BLOCK_SIZE;
        lseek(fd, offset, SEEK_SET);
        read(fd, buf, BLOCK_SIZE);
    }

    free(buf);
    close(fd);
    return 0;
}
EOF

# 编译
gcc -o /tmp/random_read_test /tmp/random_read_test.c 2>/dev/null

if [ -f /tmp/random_read_test ]; then
    BEFORE=$(read_diskstats)
    START_TIME=$(date +%s.%N)

    /tmp/random_read_test $DATA_FILE

    END_TIME=$(date +%s.%N)
    AFTER=$(read_diskstats)
    DURATION=$(python3 -c "print(f'{$END_TIME - $START_TIME:.2f}')")

    echo "结果 (100,000 次 4K 随机读):"
    calc_perf "$BEFORE" "$AFTER" "$DURATION"
else
    echo "编译失败，使用 Python 替代方案..."

    BEFORE=$(read_diskstats)
    START_TIME=$(date +%s.%N)

    python3 << 'PYEOF'
import os
import random

filename = os.environ.get('DATA_FILE', '/home/ray/data/sift10m/spann_index_u8default_20260430/SPTAGFullList.bin')
block_size = 4096
num_reads = 100000

file_size = os.path.getsize(filename)
num_blocks = file_size // block_size

with open(filename, 'rb') as f:
    for _ in range(num_reads):
        offset = random.randint(0, num_blocks - 1) * block_size
        f.seek(offset)
        f.read(block_size)
PYEOF

    END_TIME=$(date +%s.%N)
    AFTER=$(read_diskstats)
    DURATION=$(python3 -c "print(f'{$END_TIME - $START_TIME:.2f}')")

    echo "结果 (100,000 次 4K 随机读, 使用 Python):"
    calc_perf "$BEFORE" "$AFTER" "$DURATION"
fi

echo ""
echo "=========================================="
echo "测试 4: 4K 随机读 (8 线程并发)"
echo "=========================================="
echo ""

BEFORE=$(read_diskstats)
START_TIME=$(date +%s.%N)

# 8 个进程并发随机读
for i in {0..7}; do
    (python3 << 'PYEOF' &
import os
import random

filename = "/home/ray/data/sift10m/spann_index_u8default_20260430/SPTAGFullList.bin"
block_size = 4096
num_reads = 50000  # 每个进程 50K 次，总共 400K 次

file_size = os.path.getsize(filename)
num_blocks = file_size // block_size

with open(filename, 'rb') as f:
    for _ in range(num_reads):
        offset = random.randint(0, num_blocks - 1) * block_size
        f.seek(offset)
        f.read(block_size)
PYEOF
    ) &
done
wait

END_TIME=$(date +%s.%N)
AFTER=$(read_diskstats)
DURATION=$(python3 -c "print(f'{$END_TIME - $START_TIME:.2f}')")

echo "结果 (400,000 次 4K 随机读, 8 线程):"
calc_perf "$BEFORE" "$AFTER" "$DURATION"

echo ""
echo "=========================================="
echo "测试 5: 8KB 随机读 (8 线程并发, 模拟 SPANN)"
echo "=========================================="
echo ""

BEFORE=$(read_diskstats)
START_TIME=$(date +%s.%N)

# 8 个进程并发 8KB 随机读，模拟 SPANN 的请求大小
for i in {0..7}; do
    (python3 << 'PYEOF' &
import os
import random

filename = "/home/ray/data/sift10m/spann_index_u8default_20260430/SPTAGFullList.bin"
block_size = 8192  # 8KB，接近 SPANN 的平均请求大小
num_reads = 80000  # 每个进程 80K 次，总共 640K 次 (接近 SPANN 的 640K 次请求)

file_size = os.path.getsize(filename)
num_blocks = file_size // block_size

with open(filename, 'rb') as f:
    for _ in range(num_reads):
        offset = random.randint(0, num_blocks - 1) * block_size
        f.seek(offset)
        f.read(block_size)
PYEOF
    ) &
done
wait

END_TIME=$(date +%s.%N)
AFTER=$(read_diskstats)
DURATION=$(python3 -c "print(f'{$END_TIME - $START_TIME:.2f}')")

echo "结果 (640,000 次 8KB 随机读, 8 线程):"
calc_perf "$BEFORE" "$AFTER" "$DURATION"

echo ""
echo "=========================================="
echo "测试总结"
echo "=========================================="
echo ""
echo "对比 SPANN 测试结果:"
echo "  请求数: 639,963"
echo "  平均请求大小: 7.87 KB"
echo "  合并率: 0.01%"
echo "  IOPS: 137,932"
echo "  吞吐量: 1,060 MB/s"
