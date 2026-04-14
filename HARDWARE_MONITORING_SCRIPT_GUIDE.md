# 通用硬件监控脚本编写指南

本文档提供一套通用的硬件监控脚本编写规范和模板，便于在各种实验项目中快速部署硬件监控能力。

---

## 目录

1. [设计原则](#设计原则)
2. [核心功能模块](#核心功能模块)
3. [脚本模板](#脚本模板)
4. [数据采集方法](#数据采集方法)
5. [阶段识别机制](#阶段识别机制)
6. [输出格式规范](#输出格式规范)
7. [自定义扩展](#自定义扩展)
8. [最佳实践](#最佳实践)
9. [常见问题](#常见问题)

---

## 设计原则

### 1. 最小侵入性

```
┌─────────────────────────────────────────────────────────────┐
│                      设计原则                                │
├─────────────────────────────────────────────────────────────┤
│  ✅ 不修改被测程序的代码                                     │
│  ✅ 不影响被测程序的正常运行                                 │
│  ✅ 采样开销尽量小                                          │
│  ✅ 被测程序崩溃时能正常清理                                 │
└─────────────────────────────────────────────────────────────┘
```

### 2. 数据可追溯

- 记录完整的实验配置信息
- 输出格式标准化（CSV/JSON）
- 时间戳采用 ISO 8601 格式

### 3. 灵活可配置

- 支持命令行参数配置
- 采样间隔可调
- 输出路径可自定义

### 4. 可扩展架构

- 模块化设计，便于添加新监控项
- 支持自定义阶段识别逻辑
- 支持自定义输出格式

---

## 核心功能模块

### 模块架构图

```
┌─────────────────────────────────────────────────────────────────┐
│                      监控脚本架构                                │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │                    参数解析模块                          │   │
│  │  - 解析命令行参数                                        │   │
│  │  - 验证参数有效性                                        │   │
│  │  - 设置默认值                                            │   │
│  └─────────────────────────────────────────────────────────┘   │
│                              │                                  │
│                              ▼                                  │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │                    进程管理模块                          │   │
│  │  - 启动被测程序 (新进程组)                               │   │
│  │  - 追踪进程树                                            │   │
│  │  - 清理子进程                                            │   │
│  └─────────────────────────────────────────────────────────┘   │
│                              │                                  │
│                              ▼                                  │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │                    数据采集模块                          │   │
│  │  - CPU 使用率采集                                        │   │
│  │  - 内存使用采集                                          │   │
│  │  - 磁盘 I/O 采集                                         │   │
│  │  - 自定义指标采集                                        │   │
│  └─────────────────────────────────────────────────────────┘   │
│                              │                                  │
│                              ▼                                  │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │                    阶段识别模块                          │   │
│  │  - 检测当前运行阶段                                      │   │
│  │  - 统计阶段时间                                          │   │
│  │  - 触发阶段切换回调                                      │   │
│  └─────────────────────────────────────────────────────────┘   │
│                              │                                  │
│                              ▼                                  │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │                    数据输出模块                          │   │
│  │  - CSV 文件写入                                          │   │
│  │  - LOG 文件写入                                          │   │
│  │  - 统计摘要生成                                          │   │
│  └─────────────────────────────────────────────────────────┘   │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## 脚本模板

### 基础模板

```bash
#!/bin/bash
# 通用硬件监控脚本模板
# 用于监测命令运行期间的 CPU、内存、磁盘 I/O 使用情况

set -o pipefail

# ============ 默认参数 ============
DEFAULT_DISK="/mnt/data"           # 默认监控磁盘
DEFAULT_OUTPUT="monitor.csv"       # 默认 CSV 输出
DEFAULT_LOG="monitor.log"          # 默认 LOG 输出
DEFAULT_INTERVAL=1                 # 默认采样间隔(秒)

# ============ 全局变量 ============
DISK_MOUNT=""
DISK_DEVICE=""
OUTPUT_CSV=""
OUTPUT_LOG=""
INTERVAL=""
COMMAND=()
START_TIME=0
PGID=0

# 初始计数器
INIT_READ_BYTES=0
INIT_WRITE_BYTES=0
PREV_READ_BYTES=0
PREV_WRITE_BYTES=0
PREV_SAMPLE_TIME=0

# 统计数据
SAMPLE_COUNT=0
CPU_SUM=0
CPU_MAX=0
MEM_RSS_SUM=0
MEM_RSS_MAX=0
MEM_VSZ_MAX=0

# ============ 帮助信息 ============
usage() {
    cat << EOF
用法: $0 [选项] -- <被测命令>

选项:
    -d, --disk      要监测的磁盘挂载点 (默认: $DEFAULT_DISK)
    -o, --output    输出 CSV 文件路径 (默认: $DEFAULT_OUTPUT)
    -l, --log       输出 LOG 文件路径 (默认: $DEFAULT_LOG)
    -i, --interval  采样间隔，秒 (默认: $DEFAULT_INTERVAL)
    -h, --help      显示帮助信息

示例:
    $0 -d /mnt/data -o metrics.csv -l experiment.log -- ./your_program

输出 CSV 列:
    timestamp          - 时间戳 (ISO 8601)
    elapsed_time       - 经过时间 (秒)
    cpu_percent        - CPU 使用率 (%)
    mem_rss_mb         - 物理内存占用 (MB)
    mem_vsz_mb         - 虚拟内存占用 (MB)
    process_status     - 进程状态
    threads            - 线程数
    disk_read_bytes    - 累计磁盘读取 (字节)
    disk_write_bytes   - 累计磁盘写入 (字节)
    disk_read_rate_mbs - 磁盘读取速率 (MB/s)
    disk_write_rate_mbs- 磁盘写入速率 (MB/s)
EOF
    exit 0
}

# ============ 参数解析 ============
parse_args() {
    DISK_MOUNT="$DEFAULT_DISK"
    OUTPUT_CSV="$DEFAULT_OUTPUT"
    OUTPUT_LOG="$DEFAULT_LOG"
    INTERVAL="$DEFAULT_INTERVAL"

    while [[ $# -gt 0 ]]; do
        case "$1" in
            -d|--disk)     DISK_MOUNT="$2"; shift 2 ;;
            -o|--output)   OUTPUT_CSV="$2"; shift 2 ;;
            -l|--log)      OUTPUT_LOG="$2"; shift 2 ;;
            -i|--interval) INTERVAL="$2"; shift 2 ;;
            -h|--help)     usage ;;
            --)            shift; COMMAND=("$@"); break ;;
            *)             echo "错误: 未知选项 $1" >&2; usage ;;
        esac
    done

    if [[ ${#COMMAND[@]} -eq 0 ]]; then
        echo "错误: 未指定被测命令" >&2
        usage
    fi
}

# ============ 获取磁盘设备名 ============
get_disk_device() {
    local mount_point="$1"
    local device_path
    device_path=$(df "$mount_point" 2>/dev/null | awk 'NR==2 {print $1}')

    if [[ -z "$device_path" ]]; then
        echo "错误: 无法找到挂载点 $mount_point 对应的设备" >&2
        exit 1
    fi

    local device_name
    device_name=$(basename "$device_path")

    # 处理分区名
    if [[ "$device_name" =~ ^nvme ]]; then
        DISK_DEVICE=$(echo "$device_name" | sed 's/p[0-9]*$//')
    elif [[ "$device_name" =~ ^sd || "$device_name" =~ ^hd || "$device_name" =~ ^vd ]]; then
        DISK_DEVICE=$(echo "$device_name" | sed 's/[0-9]*$//')
    else
        DISK_DEVICE="$device_name"
    fi

    echo "检测到磁盘设备: $DISK_DEVICE (挂载点: $mount_point)"
}

# ============ 读取磁盘 I/O ============
get_disk_io() {
    local device="$1"
    local stat_file="/sys/block/$device/stat"

    if [[ ! -f "$stat_file" ]]; then
        echo "0 0"
        return
    fi

    local stats
    stats=$(cat "$stat_file")
    local read_sectors write_sectors
    read_sectors=$(echo "$stats" | awk '{print $3}')
    write_sectors=$(echo "$stats" | awk '{print $7}')

    local read_bytes=$((read_sectors * 512))
    local write_bytes=$((write_sectors * 512))

    echo "$read_bytes $write_bytes"
}

# ============ 获取进程组资源使用 ============
get_process_group_stats() {
    local pgid="$1"
    local pids
    pids=$(pgrep -g "$pgid" 2>/dev/null)

    if [[ -z "$pids" ]]; then
        echo "0 0 0 S 0"
        return
    fi

    local total_rss=0
    local total_vsz=0
    local total_threads=0
    local main_status="S"

    for pid in $pids; do
        local status_file="/proc/$pid/status"

        if [[ -f "$status_file" ]]; then
            local rss vsz threads
            rss=$(awk '/VmRSS:/ {print $2}' "$status_file" 2>/dev/null || echo 0)
            vsz=$(awk '/VmSize:/ {print $2}' "$status_file" 2>/dev/null || echo 0)
            threads=$(awk '/Threads:/ {print $2}' "$status_file" 2>/dev/null || echo 1)

            total_rss=$((total_rss + rss))
            total_vsz=$((total_vsz + vsz))
            total_threads=$((total_threads + threads))
        fi
    done

    local rss_mb=$(awk "BEGIN {printf \"%.2f\", $total_rss / 1024}")
    local vsz_mb=$(awk "BEGIN {printf \"%.2f\", $total_vsz / 1024}")

    echo "$rss_mb $vsz_mb $main_status $total_threads"
}

# ============ 计算 CPU 使用率 ============
calculate_cpu_usage() {
    local pgid="$1"
    local cpu_percent
    cpu_percent=$(ps -g "$pgid" -o %cpu --no-headers 2>/dev/null | awk '{sum += $1} END {printf "%.2f", sum}')

    if [[ -z "$cpu_percent" ]] || [[ "$cpu_percent" == "" ]]; then
        cpu_percent="0.00"
    fi

    echo "$cpu_percent"
}

# ============ 采样函数 ============
sample() {
    local elapsed=$(awk "BEGIN {printf \"%.1f\", $(date +%s.%N) - $START_TIME}")
    local timestamp=$(date -Iseconds)

    # 获取进程组统计
    local proc_stats
    proc_stats=$(get_process_group_stats "$PGID")
    local rss_mb vsz_mb proc_status threads
    read -r rss_mb vsz_mb proc_status threads <<< "$proc_stats"

    # 获取磁盘 I/O
    local disk_io
    disk_io=$(get_disk_io "$DISK_DEVICE")
    local read_bytes write_bytes
    read -r read_bytes write_bytes <<< "$disk_io"

    # 计算相对值
    local rel_read=$((read_bytes - INIT_READ_BYTES))
    local rel_write=$((write_bytes - INIT_WRITE_BYTES))
    [[ $rel_read -lt 0 ]] && rel_read=0
    [[ $rel_write -lt 0 ]] && rel_write=0

    # 计算速率
    local current_time=$(date +%s.%N)
    local read_rate="0.00"
    local write_rate="0.00"

    if [[ $(awk "BEGIN {print ($PREV_SAMPLE_TIME > 0)}") -eq 1 ]]; then
        local time_diff=$(awk "BEGIN {printf \"%.6f\", $current_time - $PREV_SAMPLE_TIME}")
        if [[ $(awk "BEGIN {print ($time_diff > 0)}") -eq 1 ]]; then
            local read_diff=$((read_bytes - PREV_READ_BYTES))
            local write_diff=$((write_bytes - PREV_WRITE_BYTES))
            [[ $read_diff -lt 0 ]] && read_diff=0
            [[ $write_diff -lt 0 ]] && write_diff=0
            read_rate=$(awk "BEGIN {printf \"%.2f\", $read_diff / $time_diff / 1048576}")
            write_rate=$(awk "BEGIN {printf \"%.2f\", $write_diff / $time_diff / 1048576}")
        fi
    fi

    PREV_READ_BYTES=$read_bytes
    PREV_WRITE_BYTES=$write_bytes
    PREV_SAMPLE_TIME=$current_time

    # 获取 CPU 使用率
    local cpu_percent
    cpu_percent=$(calculate_cpu_usage "$PGID")

    # 写入 CSV
    echo "$timestamp,$elapsed,$cpu_percent,$rss_mb,$vsz_mb,$proc_status,$threads,$rel_read,$rel_write,$read_rate,$write_rate" >> "$OUTPUT_CSV"

    # 更新统计
    SAMPLE_COUNT=$((SAMPLE_COUNT + 1))
    CPU_SUM=$(awk "BEGIN {print $CPU_SUM + $cpu_percent}")
    [[ $(awk "BEGIN {print ($cpu_percent > $CPU_MAX)}") -eq 1 ]] && CPU_MAX=$cpu_percent
    MEM_RSS_SUM=$(awk "BEGIN {print $MEM_RSS_SUM + $rss_mb}")
    [[ $(awk "BEGIN {print ($rss_mb > $MEM_RSS_MAX)}") -eq 1 ]] && MEM_RSS_MAX=$rss_mb
    [[ $(awk "BEGIN {print ($vsz_mb > $MEM_VSZ_MAX)}") -eq 1 ]] && MEM_VSZ_MAX=$vsz_mb
}

# ============ 监控循环 ============
monitor_loop() {
    while kill -0 -$PGID 2>/dev/null; do
        sample
        sleep "$INTERVAL"
    done
    sample  # 最后一次采样
}

# ============ 输出统计摘要 ============
print_summary() {
    local end_time=$(date '+%Y-%m-%d %H:%M:%S')
    local total_time=$(awk "BEGIN {printf \"%.1f\", $(date +%s.%N) - $START_TIME}")

    local cpu_avg mem_rss_avg
    [[ $SAMPLE_COUNT -gt 0 ]] && cpu_avg=$(awk "BEGIN {printf \"%.1f\", $CPU_SUM / $SAMPLE_COUNT}")
    [[ $SAMPLE_COUNT -gt 0 ]] && mem_rss_avg=$(awk "BEGIN {printf \"%.1f\", $MEM_RSS_SUM / $SAMPLE_COUNT}")

    cat << EOF >> "$OUTPUT_LOG"

========================================
实验统计摘要
========================================
结束时间: $end_time
总运行时间: ${total_time} 秒
采样次数: $SAMPLE_COUNT

CPU 使用率: 平均 ${cpu_avg}%, 峰值 ${CPU_MAX}%
内存占用 (RSS): 平均 ${mem_rss_avg} MB, 峰值 ${MEM_RSS_MAX} MB
内存占用 (VSZ): 峰值 ${MEM_VSZ_MAX} MB
========================================
EOF

    echo ""
    echo "CSV 文件: $OUTPUT_CSV"
    echo "LOG 文件: $OUTPUT_LOG"
}

# ============ 清理函数 ============
cleanup() {
    if [[ $PGID -gt 0 ]] && kill -0 -$PGID 2>/dev/null; then
        echo "正在终止进程组 $PGID..."
        kill -- -"$PGID" 2>/dev/null
    fi
}

# ============ 主函数 ============
main() {
    trap cleanup EXIT INT TERM

    parse_args "$@"

    # 确保输出目录存在
    local output_dir
    output_dir=$(dirname "$OUTPUT_CSV")
    [[ ! -d "$output_dir" ]] && mkdir -p "$output_dir"
    output_dir=$(dirname "$OUTPUT_LOG")
    [[ ! -d "$output_dir" ]] && mkdir -p "$output_dir"

    get_disk_device "$DISK_MOUNT"

    # 记录初始值
    local initial_io
    initial_io=$(get_disk_io "$DISK_DEVICE")
    read -r INIT_READ_BYTES INIT_WRITE_BYTES <<< "$initial_io"
    PREV_READ_BYTES=$INIT_READ_BYTES
    PREV_WRITE_BYTES=$INIT_WRITE_BYTES

    # 写入 CSV 表头
    echo "timestamp,elapsed_time,cpu_percent,mem_rss_mb,mem_vsz_mb,process_status,threads,disk_read_bytes,disk_write_bytes,disk_read_rate_mbs,disk_write_rate_mbs" > "$OUTPUT_CSV"

    # 初始化 LOG 文件
    cat << EOF > "$OUTPUT_LOG"
========================================
实验参数
========================================
开始时间: $(date '+%Y-%m-%d %H:%M:%S')
磁盘挂载点: $DISK_MOUNT
磁盘设备: $DISK_DEVICE
被测命令: ${COMMAND[*]}
采样间隔: ${INTERVAL} 秒
========================================
EOF

    echo "开始监测..."
    START_TIME=$(date +%s.%N)

    # 启动被测程序 (新的进程组)
    setsid "${COMMAND[@]}" >> "$OUTPUT_LOG" 2>&1 &
    local cmd_pid=$!
    PGID=$(ps -o pgid= -p "$cmd_pid" | tr -d ' ')

    sleep 0.5

    if [[ -z "$PGID" ]] || [[ "$PGID" == "0" ]]; then
        PGID=$cmd_pid
    fi

    echo "被测进程 PID: $cmd_pid, PGID: $PGID"

    monitor_loop
    print_summary
}

main "$@"
```

---

## 数据采集方法

### 1. CPU 使用率

#### 方法一：使用 `ps` 命令（推荐）

```bash
# 获取进程组的 CPU 使用率
get_cpu_usage() {
    local pgid="$1"
    ps -g "$pgid" -o %cpu --no-headers 2>/dev/null | awk '{sum += $1} END {printf "%.2f", sum}'
}
```

**优点**：简单可靠，适合大多数场景
**缺点**：精度较低，可能遗漏短时间内的 CPU 突发

#### 方法二：读取 `/proc/stat`

```bash
# 更精确的 CPU 计算
get_cpu_precise() {
    local pid="$1"
    local stat_file="/proc/$pid/stat"
    
    # utime(14) + stime(15)
    local utime stime
    utime=$(awk '{print $14}' "$stat_file")
    stime=$(awk '{print $15}' "$stat_file")
    
    echo $((utime + stime))
}
```

**优点**：精度高，可获取累积 CPU 时间
**缺点**：需要多次采样计算差值

### 2. 内存使用

```bash
# 获取进程内存信息
get_memory_info() {
    local pid="$1"
    local status_file="/proc/$pid/status"
    
    # VmRSS: 物理内存 (kB)
    # VmSize: 虚拟内存 (kB)
    local rss vsz
    rss=$(awk '/VmRSS:/ {print $2}' "$status_file")
    vsz=$(awk '/VmSize:/ {print $2}' "$status_file")
    
    echo "$rss $vsz"
}
```

### 3. 磁盘 I/O

```bash
# 读取磁盘统计
# /sys/block/<device>/stat 格式:
# Field 3: read sectors
# Field 7: write sectors
get_disk_io() {
    local device="$1"
    local stat_file="/sys/block/$device/stat"
    
    local stats
    stats=$(cat "$stat_file")
    
    local read_sectors write_sectors
    read_sectors=$(echo "$stats" | awk '{print $3}')
    write_sectors=$(echo "$stats" | awk '{print $7}')
    
    # 每个扇区 512 字节
    local read_bytes=$((read_sectors * 512))
    local write_bytes=$((write_sectors * 512))
    
    echo "$read_bytes $write_bytes"
}
```


---

## 阶段识别机制

### 1. 基于进程名识别

```bash
# 检测当前运行阶段
detect_stage_by_process() {
    local pgid="$1"
    local pids
    pids=$(pgrep -g "$pgid" 2>/dev/null)
    
    for pid in $pids; do
        local cmdline
        cmdline=$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null)
        
        # 根据命令行关键词识别阶段
        case "$cmdline" in
            *"build_index"*) echo "indexing"; return ;;
            *"search"*)      echo "search"; return ;;
            *"train"*)       echo "training"; return ;;
        esac
    done
    
    echo "unknown"
}
```

### 2. 基于日志文件识别

```bash
# 通过监控日志文件识别阶段
detect_stage_by_log() {
    local log_file="$1"
    local last_line
    last_line=$(tail -1 "$log_file" 2>/dev/null)
    
    case "$last_line" in
        *"开始构建索引"*) echo "indexing" ;;
        *"开始搜索"*)     echo "search" ;;
        *"开始训练"*)     echo "training" ;;
        *)                echo "unknown" ;;
    esac
}
```

### 3. 基于资源使用模式识别

```bash
# 根据资源使用特征识别阶段（高级）
detect_stage_by_pattern() {
    local cpu="$1"
    local mem="$2"
    local disk_rate="$3"
    
    # I/O 密集型阶段
    if [[ $(awk "BEGIN {print ($disk_rate > 50)}") -eq 1 ]]; then
        echo "io_intensive"
    # CPU 密集型阶段
    elif [[ $(awk "BEGIN {print ($cpu > 80)}") -eq 1 ]]; then
        echo "cpu_intensive"
    # 内存密集型阶段
    elif [[ $(awk "BEGIN {print ($mem > 1024)}") -eq 1 ]]; then
        echo "memory_intensive"
    else
        echo "idle"
    fi
}
```

---

## 输出格式规范

### 1. CSV 文件格式

#### 标准列定义

```csv
timestamp,elapsed_time,stage,cpu_percent,mem_rss_mb,mem_vsz_mb,process_status,threads,disk_read_bytes,disk_write_bytes,disk_read_rate_mbs,disk_write_rate_mbs
2025-04-12T10:30:00+08:00,0.5,unknown,0.5,2.1,10.5,S,1,0,0,0.00,0.00
2025-04-12T10:30:01+08:00,1.5,indexing,78.5,1024.3,2048.5,R,16,52428800,10485760,50.00,10.00
```

#### 必需列

| 列名 | 类型 | 说明 |
|:---|:---|:---|
| `timestamp` | string | ISO 8601 时间戳 |
| `elapsed_time` | float | 经过时间（秒） |
| `cpu_percent` | float | CPU 使用率 |
| `mem_rss_mb` | float | 物理内存（MB） |

#### 可选列

| 列名 | 类型 | 说明 |
|:---|:---|:---|
| `stage` | string | 当前阶段 |
| `mem_vsz_mb` | float | 虚拟内存（MB） |
| `threads` | int | 线程数 |
| `disk_read_bytes` | int | 累计读取字节 |
| `disk_write_bytes` | int | 累计写入字节 |
| `disk_read_rate_mbs` | float | 读取速率 |
| `disk_write_rate_mbs` | float | 写入速率 |

### 2. LOG 文件格式

```
========================================
实验参数
========================================
开始时间: 2025-04-12 10:30:00
被测命令: ./your_program --config config.yaml
采样间隔: 1 秒

========================================
实验输出
========================================
[程序标准输出...]

========================================
实验统计摘要
========================================
结束时间: 2025-04-12 10:35:00
总运行时间: 300.0 秒
采样次数: 300

CPU 使用率: 平均 65.2%, 峰值 95.3%
内存占用 (RSS): 平均 1024.0 MB, 峰值 2048.0 MB
========================================
```

---

## 最佳实践

### 1. 采样间隔选择

```
┌─────────────────────────────────────────────────────────────┐
│                    采样间隔建议                              │
├─────────────────────────────────────────────────────────────┤
│  实验类型              │ 推荐间隔   │ 说明                   │
├────────────────────────┼────────────┼────────────────────────┤
│  快速测试 (<1分钟)     │ 0.1-0.5s   │ 捕捉快速变化           │
│  标准测试 (1-30分钟)   │ 1s         │ 平衡精度和开销         │
│  长时间测试 (>30分钟)  │ 2-5s       │ 减少数据量             │
│  I/O 密集型测试        │ 0.5-1s     │ 捕捉 I/O 突发          │
│  内存敏感测试          │ 0.5-1s     │ 捕捉内存泄漏           │
└─────────────────────────────────────────────────────────────┘
```

### 2. 清理缓存

```bash
# 实验前清理系统缓存（需要 sudo 权限）
clear_cache() {
    sync
    echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
    echo "系统缓存已清理"
}

# 清理页缓存
echo 1 | sudo tee /proc/sys/vm/drop_caches > /dev/null

# 清理 inode/dentry
echo 2 | sudo tee /proc/sys/vm/drop_caches > /dev/null

# 清理全部
echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
```

### 3. 环境记录

```bash
# 记录实验环境信息
record_environment() {
    cat << EOF

========================================
实验环境
========================================
内核版本: $(uname -r)
操作系统: $(lsb_release -d 2>/dev/null | cut -d':' -f2 | xargs || cat /etc/os-release | grep PRETTY_NAME | cut -d'"' -f2)
CPU: $(lscpu | grep "Model name" | cut -d':' -f2 | xargs)
CPU 核心数: $(nproc)
内存总量: $(free -h | awk '/Mem:/ {print $2}')
磁盘信息:
$(lsblk -d -o NAME,SIZE,TYPE | grep disk)

========================================
EOF
}
```

### 4. 错误处理

```bash
# 健壮的错误处理
set -euo pipefail

# 错误处理函数
error_handler() {
    local line_no=$1
    local error_code=$2
    echo "错误: 在第 $line_no 行发生错误 (退出码: $error_code)"
    cleanup
    exit $error_code
}

trap 'error_handler ${LINENO} $?' ERR

# 安全的进程终止
safe_kill() {
    local pgid="$1"
    local timeout=10
    
    # 先发送 SIGTERM
    kill -- -"$pgid" 2>/dev/null
    
    # 等待进程结束
    local count=0
    while kill -0 -"$pgid" 2>/dev/null && [[ $count -lt $timeout ]]; do
        sleep 1
        count=$((count + 1))
    done
    
    # 如果还在运行，发送 SIGKILL
    if kill -0 -"$pgid" 2>/dev/null; then
        echo "强制终止进程组 $pgid"
        kill -9 -- -"$pgid" 2>/dev/null
    fi
}
```

### 5. 日志轮转

```bash
# 自动管理日志文件
rotate_logs() {
    local output_dir="$1"
    local max_files=10
    
    # 按时间排序，删除旧文件
    local files=($(ls -t "$output_dir"/*.csv 2>/dev/null))
    local count=${#files[@]}
    
    if [[ $count -gt $max_files ]]; then
        for ((i = max_files; i < count; i++)); do
            rm -f "${files[$i]}"
            echo "删除旧日志: ${files[$i]}"
        done
    fi
}
```

---

## 常见问题

### Q1: 为什么监控不到磁盘 I/O？

**可能原因：**
1. 磁盘挂载点路径错误
2. 设备名解析失败
3. 权限不足

**解决方案：**
```bash
# 检查挂载点
df -h | grep "/your/mount/point"

# 手动检查设备
lsblk
ls -la /sys/block/

# 验证 stat 文件可读
cat /sys/block/sda/stat
```

### Q2: CPU 使用率显示不正确？

**可能原因：**
1. 进程组未正确追踪
2. 多线程进程统计不完整

**解决方案：**
```bash
# 检查进程组
ps -o pid,pgid,cmd -g <PGID>

# 使用更精确的方法
top -H -p <PID>
```

### Q3: 监控脚本影响被测程序性能？

**可能原因：**
1. 采样间隔过短
2. 采样操作本身开销大

**解决方案：**
```bash
# 增加采样间隔
-i 2  # 2秒间隔

# 使用更轻量的方法
# 避免在采样中调用外部命令
```

### Q4: 如何监控容器内的进程？

**解决方案：**
```bash
# 使用 --pid=host 运行容器
docker run --pid=host ...

# 或者在宿主机上监控容器进程
# 获取容器进程 ID
docker inspect --format '{{.State.Pid}}' <container_id>
```

---

## 快速启动检查清单

```
□ 1. 确认监控脚本可执行
     chmod +x monitor.sh

□ 2. 确认被测命令可独立运行
     ./your_program --test

□ 3. 确认磁盘挂载点正确
     df -h | grep "/your/mount"

□ 4. 创建输出目录
     mkdir -p results

□ 5. (可选) 清理系统缓存
     sync && echo 3 | sudo tee /proc/sys/vm/drop_caches

□ 6. 运行监控
     ./monitor.sh -d /mount -o results/test.csv -l results/test.log -- ./your_program

□ 7. 检查输出文件
     head results/test.csv
     cat results/test.log
```

---


