#!/bin/bash
# SPTAG BKT 内存索引搜索测试脚本
# 支持多种 MaxCheck 参数测试

set -e

# ============ 配置区域 ============
SPTAG_ROOT="/home/ray/code/SPTAG"
DATA_DIR="/media/ray/1tb/sift1m"
SCRIPTS_DIR="${SPTAG_ROOT}/scripts"
RESULTS_DIR="${SPTAG_ROOT}/results/search"

# 数据文件
QUERY_FILE="${DATA_DIR}/sift_query.fvecs"
TRUTH_FILE="${DATA_DIR}/sift_groundtruth_sptag.bin"

# 搜索参数
DEFAULT_INDEX_DIR="${DATA_DIR}/bkt_memory_index"
KNN=32
THREADS=16

# ============ 帮助信息 ============
usage() {
    cat << EOF
用法: $0 [选项]

选项:
    -x, --index      索引目录 (默认: ${DEFAULT_INDEX_DIR})
    -m, --maxcheck   MaxCheck值（可多次指定，默认: 1024 2048 4096 8192）
    -k, --knn        K近邻数量 (默认: 32)
    -t, --threads    搜索线程数 (默认: 16)
    -o, --output     结果输出目录 (默认: results/search)
    -C, --clear-cache  搜索前清除系统缓存
    -h, --help       显示帮助信息

示例:
    $0 -x /path/to/index -m 8192
    $0 -m 1024 -m 4096 -m 8192    # 多个 MaxCheck 测试
    $0 -t 32 -C                    # 32线程搜索，清除缓存
EOF
    exit 0
}

# ============ 参数解析 ============
INDEX_DIR="$DEFAULT_INDEX_DIR"
MAX_CHECKS=()
THREADS=16
KNN=32
CLEAR_CACHE=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        -x|--index)
            INDEX_DIR="$2"
            shift 2
            ;;
        -m|--maxcheck)
            MAX_CHECKS+=("$2")
            shift 2
            ;;
        -k|--knn)
            KNN="$2"
            shift 2
            ;;
        -t|--threads)
            THREADS="$2"
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

# 默认 MaxCheck 值
if [[ ${#MAX_CHECKS[@]} -eq 0 ]]; then
    MAX_CHECKS=(1024 2048 4096 8192)
fi

# 验证索引目录
if [[ ! -d "$INDEX_DIR" ]]; then
    echo "错误: 索引目录不存在: ${INDEX_DIR}" >&2
    exit 1
fi

# ============ 主程序 ============

echo "========================================"
echo "SPTAG BKT 内存索引搜索测试"
echo "========================================"
echo "索引目录: ${INDEX_DIR}"
echo "查询文件: ${QUERY_FILE}"
echo "Ground Truth: ${TRUTH_FILE}"
echo "MaxCheck 值: ${MAX_CHECKS[*]}"
echo "KNN: ${KNN}"
echo "搜索线程: ${THREADS}"
echo "清除缓存: ${CLEAR_CACHE}"
echo ""

# 创建结果目录
mkdir -p "$RESULTS_DIR"

timestamp=$(date +%Y%m%d_%H%M%S)
result_prefix="${RESULTS_DIR}/bkt_search_${THREADS}t_${timestamp}"

# 构建监控命令选项
monitor_opts=(
    -d "${DATA_DIR}"
    -o "${result_prefix}_monitor.csv"
    -l "${result_prefix}_monitor.log"
)

if [[ "$CLEAR_CACHE" == true ]]; then
    monitor_opts+=(-C)
fi

# 构建搜索命令
# MaxCheck 可以用 # 分隔多个值
maxcheck_str=$(IFS='#'; echo "${MAX_CHECKS[*]}")

search_cmd=(
    "${SPTAG_ROOT}/Release/indexsearcher"
    -d 128
    -v Float
    -f XVEC
    -i "${QUERY_FILE}"
    -x "${INDEX_DIR}"
    -r "${TRUTH_FILE}"
    -m "${maxcheck_str}"
    -k "${KNN}"
    -t "${THREADS}"
)

echo "开始搜索测试..."
echo "命令: ${search_cmd[*]}"
echo ""

# 运行监控脚本
"${SCRIPTS_DIR}/sptag_memory_monitor.sh" \
    "${monitor_opts[@]}" \
    -- "${search_cmd[@]}"

echo ""
echo "========================================"
echo "搜索测试完成"
echo "========================================"
echo "监控文件: ${result_prefix}_monitor.csv"
echo "日志文件: ${result_prefix}_monitor.log"

# 提取关键性能指标
echo ""
echo "性能指标摘要:"
grep -E "\[query\]|\[maxcheck\]|\[recall\]|\[qps\]" "${result_prefix}_monitor.log" 2>/dev/null || true
