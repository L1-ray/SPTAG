#!/bin/bash
# SPANN 索引性能测试硬件监控脚本
# 用于监测 ssdserving 运行期间的 CPU、内存、磁盘 I/O 使用情况
# 支持自动识别构建阶段（SelectHead、BuildHead、BuildSSDIndex、SearchSSDIndex）

set -o pipefail

# ============ 默认参数 ============
DEFAULT_DISK="/media/ray/1tb"        # 默认监控磁盘（索引存储位置）
DEFAULT_OUTPUT="spann_monitor.csv"   # 默认 CSV 输出
DEFAULT_LOG="spann_monitor.log"      # 默认 LOG 输出
DEFAULT_INTERVAL=1                   # 默认采样间隔(秒)
SPTAG_ROOT="/home/ray/code/SPTAG"    # SPTAG 项目根目录

# ============ 全局变量 ============
DISK_MOUNT=""
DISK_DEVICE=""
OUTPUT_CSV=""
OUTPUT_LOG=""
INTERVAL=""
CONFIG_FILE=""
CLEAR_CACHE=false
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

# 阶段统计
CURRENT_STAGE="init"
STAGE_START_TIME=0
STAGE_STATS=()

# 阶段名称映射
declare -A STAGE_NAMES=(
    ["select_head"]="SelectHead"
    ["build_head"]="BuildHead"
    ["build_ssd"]="BuildSSDIndex"
    ["search"]="SearchSSDIndex"
    ["warmup"]="Warmup"
    ["unknown"]="Unknown"
)

# ============ 帮助信息 ============
usage() {
    cat << EOF
用法: $0 [选项] -- <被测命令或配置文件>

选项:
    -d, --disk      要监测的磁盘挂载点 (默认: $DEFAULT_DISK)
    -o, --output    输出 CSV 文件路径 (默认: $DEFAULT_OUTPUT)
    -l, --log       输出 LOG 文件路径 (默认: $DEFAULT_LOG)
    -i, --interval  采样间隔，秒 (默认: $DEFAULT_INTERVAL)
    -c, --config    SPANN 配置文件路径（自动构建命令）
    -C, --clear-cache  清除系统缓存（需要 sudo 权限）
    -h, --help      显示帮助信息

清除缓存功能说明:
    使用 -C 选项会在测试前清除以下缓存，确保测试结果的准确性：
    - 页面缓存 (Page Cache)
    - 目录项缓存 (Dentries)
    - Inode 缓存 (Inodes)

    注意：清除缓存需要 root 权限，脚本会使用 sudo 执行。

示例:
    # 方式 1: 直接指定命令
    $0 -d /media/ray/1tb -o metrics.csv -l experiment.log -- ./Release/ssdserving config.ini

    # 方式 2: 指定配置文件（自动构建命令）
    $0 -c spann_sift1m_config.ini

    # 方式 3: 仅监控搜索阶段（清除缓存）
    $0 -c spann_search_only.ini -o search_metrics.csv -C

    # 方式 4: 清除缓存并测试
    $0 -c spann_search_only.ini -C

输出 CSV 列:
    timestamp          - 时间戳 (ISO 8601)
    elapsed_time       - 经过时间 (秒)
    stage              - 当前阶段 (SelectHead/BuildHead/BuildSSDIndex/SearchSSDIndex)
    cpu_percent        - CPU 使用率 (%)
    mem_rss_mb         - 物理内存占用 (MB)
    mem_vsz_mb         - 虚拟内存占用 (MB)
    process_status     - 进程状态
    threads            - 线程数
    disk_read_bytes    - 累计磁盘读取 (字节)
    disk_write_bytes   - 累计磁盘写入 (字节)
    disk_read_rate_mbs - 磁盘读取速率 (MB/s)
    disk_write_rate_mbs- 磁盘写入速率 (MB/s)

阶段识别关键词:
    SelectHead    - "Begin Select Head"
    BuildHead     - "Begin Build Head"
    BuildSSDIndex - "Begin Build SSDIndex"
    SearchSSDIndex- "Start ANN Search", "Start warmup"
EOF
    exit 0
}

# ============ 参数解析 ============
parse_args() {
    DISK_MOUNT="$DEFAULT_DISK"
    OUTPUT_CSV="$DEFAULT_OUTPUT"
    OUTPUT_LOG="$DEFAULT_LOG"
    INTERVAL="$DEFAULT_INTERVAL"
    CONFIG_FILE=""
    CLEAR_CACHE=false

    while [[ $# -gt 0 ]]; do
        case "$1" in
            -d|--disk)        DISK_MOUNT="$2"; shift 2 ;;
            -o|--output)      OUTPUT_CSV="$2"; shift 2 ;;
            -l|--log)         OUTPUT_LOG="$2"; shift 2 ;;
            -i|--interval)    INTERVAL="$2"; shift 2 ;;
            -c|--config)      CONFIG_FILE="$2"; shift 2 ;;
            -C|--clear-cache) CLEAR_CACHE=true; shift ;;
            -h|--help)        usage ;;
            --)               shift; COMMAND=("$@"); break ;;
            *)                echo "错误: 未知选项 $1" >&2; usage ;;
        esac
    done

    # 如果指定了配置文件，自动构建命令
    if [[ -n "$CONFIG_FILE" ]]; then
        if [[ ! -f "$CONFIG_FILE" ]]; then
            echo "错误: 配置文件不存在: $CONFIG_FILE" >&2
            exit 1
        fi
        COMMAND=("${SPTAG_ROOT}/Release/ssdserving" "$CONFIG_FILE")

        # 生成默认输出文件名
        if [[ "$OUTPUT_CSV" == "$DEFAULT_OUTPUT" ]]; then
            local config_basename=$(basename "$CONFIG_FILE" .ini)
            OUTPUT_CSV="${SPTAG_ROOT}/${config_basename}_monitor.csv"
        fi
        if [[ "$OUTPUT_LOG" == "$DEFAULT_LOG" ]]; then
            local config_basename=$(basename "$CONFIG_FILE" .ini)
            OUTPUT_LOG="${SPTAG_ROOT}/${config_basename}_monitor.log"
        fi
    fi

    if [[ ${#COMMAND[@]} -eq 0 ]]; then
        echo "错误: 未指定被测命令或配置文件" >&2
        echo "使用 -c <config.ini> 或 -- <command> 指定" >&2
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

# ============ 阶段识别 ============
detect_stage() {
    local log_file="$1"

    if [[ ! -f "$log_file" ]]; then
        echo "unknown"
        return
    fi

    # 读取最后100行日志
    local recent_logs
    recent_logs=$(tail -100 "$log_file" 2>/dev/null)

    # 按优先级检测阶段（后出现的阶段覆盖前面的）
    local stage="unknown"

    # 检测各阶段关键词
    if echo "$recent_logs" | grep -q "Begin Select Head"; then
        stage="select_head"
    fi
    if echo "$recent_logs" | grep -q "Begin Build Head"; then
        stage="build_head"
    fi
    if echo "$recent_logs" | grep -q "Begin Build SSDIndex"; then
        stage="build_ssd"
    fi
    if echo "$recent_logs" | grep -q "Start warmup"; then
        stage="warmup"
    fi
    if echo "$recent_logs" | grep -q "Start ANN Search"; then
        stage="search"
    fi

    # 检测是否已完成
    if echo "$recent_logs" | grep -q "Finish ANN Search\|测试完成"; then
        stage="finished"
    fi

    echo "$stage"
}

# ============ 阶段名称转换 ============
get_stage_display_name() {
    local stage="$1"
    echo "${STAGE_NAMES[$stage]:-$stage}"
}

# ============ 记录阶段统计 ============
record_stage_stats() {
    local old_stage="$1"
    local new_stage="$2"
    local current_time=$(date +%s.%N)

    if [[ "$old_stage" != "init" && "$old_stage" != "unknown" && "$old_stage" != "$new_stage" ]]; then
        local stage_duration=$(awk "BEGIN {printf \"%.1f\", $current_time - $STAGE_START_TIME}")
        STAGE_STATS+=("$(get_stage_display_name $old_stage): ${stage_duration}s")
        echo "  [阶段切换] $(get_stage_display_name $old_stage) -> $(get_stage_display_name $new_stage) (耗时: ${stage_duration}s)"
    fi

    STAGE_START_TIME=$current_time
}

# ============ 采样函数 ============
sample() {
    local elapsed=$(awk "BEGIN {printf \"%.1f\", $(date +%s.%N) - $START_TIME}")
    local timestamp=$(date -Iseconds)

    # 检测当前阶段
    local new_stage
    new_stage=$(detect_stage "$OUTPUT_LOG")

    if [[ "$new_stage" != "$CURRENT_STAGE" ]]; then
        record_stage_stats "$CURRENT_STAGE" "$new_stage"
        CURRENT_STAGE=$new_stage
    fi

    local stage_display
    stage_display=$(get_stage_display_name "$CURRENT_STAGE")

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
    echo "$timestamp,$elapsed,$stage_display,$cpu_percent,$rss_mb,$vsz_mb,$proc_status,$threads,$rel_read,$rel_write,$read_rate,$write_rate" >> "$OUTPUT_CSV"

    # 更新统计
    SAMPLE_COUNT=$((SAMPLE_COUNT + 1))
    CPU_SUM=$(awk "BEGIN {print $CPU_SUM + $cpu_percent}")
    [[ $(awk "BEGIN {print ($cpu_percent > $CPU_MAX)}") -eq 1 ]] && CPU_MAX=$cpu_percent
    MEM_RSS_SUM=$(awk "BEGIN {print $MEM_RSS_SUM + $rss_mb}")
    [[ $(awk "BEGIN {print ($rss_mb > $MEM_RSS_MAX)}") -eq 1 ]] && MEM_RSS_MAX=$rss_mb
    [[ $(awk "BEGIN {print ($vsz_mb > $MEM_VSZ_MAX)}") -eq 1 ]] && MEM_VSZ_MAX=$vsz_mb

    # 实时显示进度
    printf "\r[%8.1fs] 阶段: %-15s CPU: %5.1f%%  内存: %6.1f MB  磁盘读取: %6.2f MB/s  写入: %6.2f MB/s" \
        "$elapsed" "$stage_display" "$cpu_percent" "$rss_mb" "$read_rate" "$write_rate"
}

# ============ 监控循环 ============
monitor_loop() {
    while kill -0 -$PGID 2>/dev/null; do
        sample
        sleep "$INTERVAL"
    done
    sample  # 最后一次采样
    echo ""  # 换行
}

# ============ 记录环境信息 ============
record_environment() {
    cat << EOF >> "$OUTPUT_LOG"

========================================
实验环境
========================================
内核版本: $(uname -r)
操作系统: $(lsb_release -d 2>/dev/null | cut -d':' -f2 | xargs || cat /etc/os-release | grep PRETTY_NAME | cut -d'"' -f2)
CPU: $(lscpu | grep "Model name" | cut -d':' -f2 | xargs)
CPU 核心数: $(nproc)
内存总量: $(free -h | awk '/Mem:/ {print $2}')
磁盘信息:
$(lsblk -d -o NAME,SIZE,TYPE | grep -E "disk|NAME")

========================================
实验配置
========================================
配置文件: ${CONFIG_FILE:-未指定}
被测命令: ${COMMAND[*]}
采样间隔: ${INTERVAL} 秒
监控磁盘: $DISK_MOUNT ($DISK_DEVICE)
索引目录: $(grep -E "^IndexDirectory=" "${CONFIG_FILE:-}" 2>/dev/null | cut -d'=' -f2 || echo "未指定")

EOF
}

# ============ 输出统计摘要 ============
print_summary() {
    local end_time=$(date '+%Y-%m-%d %H:%M:%S')
    local total_time=$(awk "BEGIN {printf \"%.1f\", $(date +%s.%N) - $START_TIME}")

    local cpu_avg mem_rss_avg
    [[ $SAMPLE_COUNT -gt 0 ]] && cpu_avg=$(awk "BEGIN {printf \"%.1f\", $CPU_SUM / $SAMPLE_COUNT}")
    [[ $SAMPLE_COUNT -gt 0 ]] && mem_rss_avg=$(awk "BEGIN {printf \"%.1f\", $MEM_RSS_SUM / $SAMPLE_COUNT}")

    # 获取最终磁盘 I/O
    local final_io
    final_io=$(get_disk_io "$DISK_DEVICE")
    local final_read final_write
    read -r final_read final_write <<< "$final_io"
    local total_read_mb=$(( (final_read - INIT_READ_BYTES) / 1048576 ))
    local total_write_mb=$(( (final_write - INIT_WRITE_BYTES) / 1048576 ))

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

磁盘 I/O:
  累计读取: ${total_read_mb} MB
  累计写入: ${total_write_mb} MB

阶段耗时:
$(for stat in "${STAGE_STATS[@]}"; do echo "  $stat"; done)

========================================
输出文件
========================================
CSV 文件: $OUTPUT_CSV
LOG 文件: $OUTPUT_LOG
========================================
EOF

    echo ""
    echo "========================================"
    echo "实验统计摘要"
    echo "========================================"
    echo "总运行时间: ${total_time} 秒"
    echo "CPU 使用率: 平均 ${cpu_avg}%, 峰值 ${CPU_MAX}%"
    echo "内存占用: 平均 ${mem_rss_avg} MB, 峰值 ${MEM_RSS_MAX} MB"
    echo "磁盘 I/O: 读取 ${total_read_mb} MB, 写入 ${total_write_mb} MB"
    echo ""
    echo "阶段耗时:"
    for stat in "${STAGE_STATS[@]}"; do
        echo "  $stat"
    done
    echo ""
    echo "CSV 文件: $OUTPUT_CSV"
    echo "LOG 文件: $OUTPUT_LOG"
}

# ============ 清理函数 ============
cleanup() {
    if [[ $PGID -gt 0 ]] && kill -0 -$PGID 2>/dev/null; then
        echo ""
        echo "正在终止进程组 $PGID..."
        kill -- -"$PGID" 2>/dev/null
    fi
}

# ============ 清除系统缓存 ============
clear_system_cache() {
    echo "========================================"
    echo "清除系统缓存"
    echo "========================================"

    # 检查是否有 sudo 权限
    if ! sudo -n true 2>/dev/null; then
        echo "需要 sudo 权限来清除缓存，请输入密码：" >&2
    fi

    # 显示清除前的内存状态
    echo "清除前内存状态:"
    free -h | head -2

    # 同步文件系统缓冲区
    echo ""
    echo "同步文件系统缓冲区..."
    sudo sync

    # 清除页面缓存、目录项和 inode 缓存
    # 1: 清除页面缓存
    # 2: 清除目录项和 inode
    # 3: 清除所有缓存
    echo "清除页面缓存、目录项和 inode 缓存..."
    echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null

    # 等待缓存清除完成
    sleep 1

    # 显示清除后的内存状态
    echo ""
    echo "清除后内存状态:"
    free -h | head -2

    # 显示缓存统计
    local cache_info
    cache_info=$(grep -E "^Cached:|^Buffers:" /proc/meminfo)
    echo ""
    echo "缓存信息:"
    echo "$cache_info"

    echo ""
    echo "缓存清除完成！"
    echo "========================================"
    echo ""
}

# ============ 主函数 ============
main() {
    trap cleanup EXIT INT TERM

    parse_args "$@"

    # 清除缓存（如果指定）
    if [[ "$CLEAR_CACHE" == true ]]; then
        clear_system_cache
    fi

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
    echo "timestamp,elapsed_time,stage,cpu_percent,mem_rss_mb,mem_vsz_mb,process_status,threads,disk_read_bytes,disk_write_bytes,disk_read_rate_mbs,disk_write_rate_mbs" > "$OUTPUT_CSV"

    # 初始化 LOG 文件
    local cache_status="否"
    [[ "$CLEAR_CACHE" == true ]] && cache_status="是"

    cat << EOF > "$OUTPUT_LOG"
========================================
SPANN 索引性能测试监控
========================================
开始时间: $(date '+%Y-%m-%d %H:%M:%S')
已清除缓存: $cache_status
EOF

    record_environment

    echo ""
    echo "开始监测..."
    echo "被测命令: ${COMMAND[*]}"
    START_TIME=$(date +%s.%N)
    STAGE_START_TIME=$START_TIME

    # 启动被测程序 (新的进程组)
    setsid "${COMMAND[@]}" >> "$OUTPUT_LOG" 2>&1 &
    local cmd_pid=$!
    PGID=$(ps -o pgid= -p "$cmd_pid" | tr -d ' ')

    sleep 0.5

    if [[ -z "$PGID" ]] || [[ "$PGID" == "0" ]]; then
        PGID=$cmd_pid
    fi

    echo "被测进程 PID: $cmd_pid, PGID: $PGID"
    echo ""

    monitor_loop
    print_summary
}

main "$@"
