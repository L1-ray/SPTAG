#!/bin/bash
# SPTAG BKT 内存索引完整性能测试流程
# 包含构建和搜索两个阶段

set -e

SPTAG_ROOT="/home/ray/code/SPTAG"
SCRIPTS_DIR="${SPTAG_ROOT}/scripts"
RESULTS_DIR="${SPTAG_ROOT}/results"

# ============ 帮助信息 ============
usage() {
    cat << EOF
用法: $0 [选项]

选项:
    -t, --threads    构建线程数 (默认: 16)
    -m, --maxcheck   搜索 MaxCheck 值（可多次指定，默认: 8192）
    -k, --knn        K近邻数量 (默认: 32)
    -s, --search-threads  搜索线程数 (默认: 16)
    -C, --clear-cache  测试前清除缓存
    --build-only     仅执行构建测试
    --search-only    仅执行搜索测试 (需要已有索引)
    -x, --index      索引目录（仅 --search-only 时使用）
    -h, --help       显示帮助信息

示例:
    $0                          # 完整测试（构建+搜索）
    $0 -t 16 -m 8192            # 指定线程和 MaxCheck
    $0 --build-only -t 32       # 仅构建，32线程
    $0 --search-only -m 4096    # 仅搜索，MaxCheck=4096
    $0 --search-only -x /path/to/index  # 使用指定索引搜索
EOF
    exit 0
}

# ============ 参数解析 ============
BUILD_THREADS=16
MAX_CHECKS=()
KNN=32
SEARCH_THREADS=16
CLEAR_CACHE=false
BUILD_ONLY=false
SEARCH_ONLY=false
INDEX_DIR=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -t|--threads)
            BUILD_THREADS="$2"
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
        -s|--search-threads)
            SEARCH_THREADS="$2"
            shift 2
            ;;
        -C|--clear-cache)
            CLEAR_CACHE=true
            shift
            ;;
        --build-only)
            BUILD_ONLY=true
            shift
            ;;
        --search-only)
            SEARCH_ONLY=true
            shift
            ;;
        -x|--index)
            INDEX_DIR="$2"
            shift 2
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
    MAX_CHECKS=(8192)
fi

# ============ 执行测试 ============

TEST_START=$(date '+%Y-%m-%d %H:%M:%S')
echo "========================================"
echo "SPTAG BKT 内存索引性能测试"
echo "========================================"
echo "开始时间: ${TEST_START}"
echo "构建线程: ${BUILD_THREADS}"
echo "搜索线程: ${SEARCH_THREADS}"
echo "MaxCheck: ${MAX_CHECKS[*]}"
echo "KNN: ${KNN}"
echo "清除缓存: ${CLEAR_CACHE}"
echo ""

DATA_DIR="/media/ray/1tb/sift1m"

# 构建测试
if [[ "$SEARCH_ONLY" != true ]]; then
    echo "[阶段 1/2] 构建索引..."
    echo ""

    build_opts=(-t "$BUILD_THREADS")
    [[ "$CLEAR_CACHE" == true ]] && build_opts+=(-C)

    "${SCRIPTS_DIR}/test_bkt_build.sh" "${build_opts[@]}"

    # 设置索引目录供搜索使用
    INDEX_DIR="${DATA_DIR}/bkt_memory_index_${BUILD_THREADS}t"
fi

# 搜索测试
if [[ "$BUILD_ONLY" != true ]]; then
    echo ""
    echo "[阶段 2/2] 搜索测试..."
    echo ""

    # 如果没有指定索引目录，使用默认构建输出
    if [[ -z "$INDEX_DIR" ]]; then
        INDEX_DIR="${DATA_DIR}/bkt_memory_index_${BUILD_THREADS}t"
    fi

    # 验证索引目录存在
    if [[ ! -d "$INDEX_DIR" ]]; then
        echo "错误: 索引目录不存在: ${INDEX_DIR}" >&2
        echo "请先运行构建测试，或使用 -x 指定已有索引目录" >&2
        exit 1
    fi

    search_opts=(-x "$INDEX_DIR" -t "$SEARCH_THREADS" -k "$KNN")
    for mc in "${MAX_CHECKS[@]}"; do
        search_opts+=(-m "$mc")
    done
    [[ "$CLEAR_CACHE" == true ]] && search_opts+=(-C)

    "${SCRIPTS_DIR}/test_bkt_search.sh" "${search_opts[@]}"
fi

echo ""
echo "========================================"
echo "测试全部完成"
echo "========================================"
echo "开始时间: ${TEST_START}"
echo "结束时间: $(date '+%Y-%m-%d %H:%M:%S')"
echo "结果目录: ${RESULTS_DIR}"
