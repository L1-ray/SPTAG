#!/bin/bash
# SPTAG BKT 内存索引构建测试脚本
# 支持多线程性能对比测试

set -e

# ============ 配置区域 ============
SPTAG_ROOT="/home/ray/code/SPTAG"
DATA_DIR="/media/ray/1tb/sift1m"
CONFIG_DIR="${SPTAG_ROOT}/config"
SCRIPTS_DIR="${SPTAG_ROOT}/scripts"
RESULTS_DIR="${SPTAG_ROOT}/results/build"

# 数据文件
VECTOR_FILE="${DATA_DIR}/sift_base.fvecs"
INDEX_BASE_DIR="${DATA_DIR}/bkt_memory_index"

# 构建配置
BUILD_CONFIG="bkt_build.ini"

# ============ 帮助信息 ============
usage() {
    cat << EOF
用法: $0 [选项]

选项:
    -t, --threads    线程数（可多次指定，默认: 8 16）
    -c, --config     构建配置文件 (默认: bkt_build.ini)
    -o, --output     结果输出目录 (默认: results/build)
    -C, --clear-cache  构建前清除系统缓存
    -h, --help       显示帮助信息

示例:
    $0 -t 8 -t 16              # 测试 8 和 16 线程
    $0 -t 16 -C                 # 16线程，清除缓存后测试
    $0 --threads 8 --threads 16 --threads 32  # 测试多种线程配置
EOF
    exit 0
}

# ============ 参数解析 ============
THREAD_COUNTS=()
CLEAR_CACHE=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        -t|--threads)
            THREAD_COUNTS+=("$2")
            shift 2
            ;;
        -c|--config)
            BUILD_CONFIG="$2"
            shift 2
            ;;
        -o|--output)
            RESULTS_DIR="$2"
            shift 2
            ;;
        -C|--clear-cache)
            CLEAR_CACHE=true
            shift
            ;;
        -h|--help)
            usage
            ;;
        *)
            echo "错误: 未知选项 $1" >&2
            usage
            ;;
    esac
done

# 默认线程数
if [[ ${#THREAD_COUNTS[@]} -eq 0 ]]; then
    THREAD_COUNTS=(8 16)
fi

# ============ 函数定义 ============

# 运行单次构建测试
run_build_test() {
    local threads=$1
    local timestamp=$(date +%Y%m%d_%H%M%S)
    local index_dir="${INDEX_BASE_DIR}_${threads}t"
    local result_prefix="${RESULTS_DIR}/bkt_build_${threads}t_${timestamp}"

    echo "========================================"
    echo "开始构建测试: ${threads} 线程"
    echo "索引目录: ${index_dir}"
    echo "结果前缀: ${result_prefix}"
    echo "========================================"

    # 清除旧索引
    if [[ -d "$index_dir" ]]; then
        echo "清除旧索引目录..."
        rm -rf "$index_dir"
    fi

    # 创建索引目录
    mkdir -p "$index_dir"
    mkdir -p "$RESULTS_DIR"

    # 构建监控命令选项
    local monitor_opts=(
        -d "${DATA_DIR}"
        -o "${result_prefix}_monitor.csv"
        -l "${result_prefix}_monitor.log"
    )

    if [[ "$CLEAR_CACHE" == true ]]; then
        monitor_opts+=(-C)
    fi

    # 构建命令行参数（覆盖线程数）
    local build_cmd=(
        "${SPTAG_ROOT}/Release/indexbuilder"
        -d 128
        -v Float
        -f XVEC
        -i "${VECTOR_FILE}"
        -o "${index_dir}"
        -a BKT
        -c "${CONFIG_DIR}/${BUILD_CONFIG}"
        "Index.NumberOfThreads=${threads}"
    )

    # 运行监控脚本
    "${SCRIPTS_DIR}/sptag_memory_monitor.sh" \
        "${monitor_opts[@]}" \
        -- "${build_cmd[@]}"

    echo "构建测试完成: ${threads} 线程"
    echo ""

    # 返回索引目录路径供后续使用
    echo "$index_dir"
}

# ============ 主程序 ============

echo "========================================"
echo "SPTAG BKT 内存索引构建测试"
echo "========================================"
echo "测试线程数: ${THREAD_COUNTS[*]}"
echo "配置文件: ${BUILD_CONFIG}"
echo "清除缓存: ${CLEAR_CACHE}"
echo "结果目录: ${RESULTS_DIR}"
echo ""

# 创建结果目录
mkdir -p "$RESULTS_DIR"

# 记录测试开始时间
TEST_START_TIME=$(date '+%Y-%m-%d %H:%M:%S')

# 运行各线程配置测试
declare -a INDEX_DIRS
for threads in "${THREAD_COUNTS[@]}"; do
    index_dir=$(run_build_test "$threads")
    INDEX_DIRS+=("$index_dir")
done

# 输出汇总信息
echo "========================================"
echo "所有构建测试完成"
echo "========================================"
echo "开始时间: ${TEST_START_TIME}"
echo "结束时间: $(date '+%Y-%m-%d %H:%M:%S')"
echo ""
echo "生成的索引目录:"
for i in "${!THREAD_COUNTS[@]}"; do
    echo "  ${THREAD_COUNTS[$i]} 线程: ${INDEX_DIRS[$i]}"
done
echo ""
echo "监控文件保存在: ${RESULTS_DIR}"
