# SPANN 性能测试指令

本文档提供清除缓存后的 SPANN 性能测试指令，需要 sudo 权限。

## 测试前准备

所有测试结果将保存到 `results/` 文件夹下。

---

# 一、SPANN 磁盘索引测试

## ⚠️ 重要说明

由于 SPTAG 的参数设计问题，`HashTableExponent` 参数在所有阶段之间共享：
- 搜索阶段需要 `HashTableExponent=12`（较大的哈希表提高搜索性能）
- 构建阶段需要 `HashTableExponent=4`（默认值，构建更快）

因此，**构建和搜索测试需要分开执行**：
1. 使用 `spann_build_only.ini` 进行索引构建
2. 使用 `spann_search_only.ini` 进行搜索测试

---

# 二、SPTAG 内存索引测试

SPTAG 内存索引使用 BKT 算法，构建和搜索使用独立的可执行程序。

监控脚本：`scripts/sptag_memory_monitor.sh`

## 数据准备

### Ground Truth 格式转换

SIFT1M 的 ground truth 文件需要转换为 SPTAG 兼容格式：

```bash
python3 /home/ray/code/SPTAG/scripts/convert_groundtruth.py \
    /media/ray/1tb/sift1m/sift_groundtruth.ivecs \
    /media/ray/1tb/sift1m/sift_groundtruth_sptag \
    --format default --k 100
```

---

## 2.1 索引构建测试

### 单次构建测试（8线程）

```bash
cd /home/ray/code/SPTAG

./scripts/sptag_memory_monitor.sh \
    -d /media/ray/1tb \
    -o results/build/bkt_build_8t_nocache.csv \
    -l results/build/bkt_build_8t_nocache.log \
    -C \
    -- ./Release/indexbuilder \
        -d 128 -v Float -f XVEC \
        -i /media/ray/1tb/sift1m/sift_base.fvecs \
        -o /media/ray/1tb/sift1m/bkt_memory_index_8t \
        -a BKT \
        -c config/bkt_build.ini \
        "Index.NumberOfThreads=8"
```

### 多线程对比测试（16线程）

```bash
cd /home/ray/code/SPTAG

./scripts/sptag_memory_monitor.sh \
    -d /media/ray/1tb \
    -o results/build/bkt_build_16t_nocache.csv \
    -l results/build/bkt_build_16t_nocache.log \
    -C \
    -- ./Release/indexbuilder \
        -d 128 -v Float -f XVEC \
        -i /media/ray/1tb/sift1m/sift_base.fvecs \
        -o /media/ray/1tb/sift1m/bkt_memory_index_16t \
        -a BKT \
        -c config/bkt_build.ini \
        "Index.NumberOfThreads=14"
```

### 一键执行多线程构建测试

```bash
cd /home/ray/code/SPTAG

for threads in 8 16; do
    echo "=========================================="
    echo "构建测试 ${threads} 线程..."
    echo "=========================================="
    ./scripts/sptag_memory_monitor.sh \
        -d /media/ray/1tb \
        -o results/build/bkt_build_${threads}t_nocache.csv \
        -l results/build/bkt_build_${threads}t_nocache.log \
        -C \
        -- ./Release/indexbuilder \
            -d 128 -v Float -f XVEC \
            -i /media/ray/1tb/sift1m/sift_base.fvecs \
            -o /media/ray/1tb/sift1m/bkt_memory_index_${threads}t \
            -a BKT \
            -c config/bkt_build.ini \
            "Index.NumberOfThreads=${threads}"
    echo ""
done

echo "=========================================="
echo "所有构建测试完成！"
echo "=========================================="
```

### 构建测试结果摘要

```bash
cd /home/ray/code/SPTAG

echo "=== 构建测试结果 ==="
echo "线程数,BuildTree耗时,总耗时,峰值内存"
for logfile in results/build/bkt_build_*_nocache.log; do
    threads=$(basename $logfile | grep -oP 'bkt_build_\K[0-9]+')
    tree_time=$(grep "BuildTree:" $logfile | grep -oP 'BuildTree: \K[0-9.]+')
    total_time=$(grep "总运行时间" $logfile | grep -oP '总运行时间: \K[0-9.]+')
    mem=$(grep "峰值" $logfile | grep "内存占用" | grep -oP '峰值 \K[0-9.]+')
    echo "${threads}t,${tree_time}s,${total_time}s,${mem}MB"
done
```

---

## 2.2 搜索性能测试

### 单个 MaxCheck 测试（8线程）

```bash
cd /home/ray/code/SPTAG

./scripts/sptag_memory_monitor.sh \
    -d /media/ray/1tb \
    -o results/search/bkt_search_8t_m8192_nocache.csv \
    -l results/search/bkt_search_8t_m8192_nocache.log \
    -C \
    -- ./Release/indexsearcher \
        -d 128 -v Float -f XVEC \
        -i /media/ray/1tb/sift1m/sift_query.fvecs \
        -x /media/ray/1tb/sift1m/bkt_memory_index_8t \
        -r /media/ray/1tb/sift1m/sift_groundtruth_sptag.bin \
        -m 8192 \
        -k 32 \
        -t 8
```

### 多个 MaxCheck 对比测试

```bash
cd /home/ray/code/SPTAG

./scripts/sptag_memory_monitor.sh \
    -d /media/ray/1tb \
    -o results/search/bkt_search_8t_multi_nocache.csv \
    -l results/search/bkt_search_8t_multi_nocache.log \
    -C \
    -- ./Release/indexsearcher \
        -d 128 -v Float -f XVEC \
        -i /media/ray/1tb/sift1m/sift_query.fvecs \
        -x /media/ray/1tb/sift1m/bkt_memory_index_8t \
        -r /media/ray/1tb/sift1m/sift_groundtruth_sptag.bin \
        -m "1024#2048#4096#8192" \
        -k 32 \
        -t 8
```

### 不同线程数测试

```bash
cd /home/ray/code/SPTAG

for threads in 8 16; do
    echo "=========================================="
    echo "搜索测试 ${threads} 线程..."
    echo "=========================================="
    ./scripts/sptag_memory_monitor.sh \
        -d /media/ray/1tb \
        -o results/search/bkt_search_${threads}t_nocache.csv \
        -l results/search/bkt_search_${threads}t_nocache.log \
        -C \
        -- ./Release/indexsearcher \
            -d 128 -v Float -f XVEC \
            -i /media/ray/1tb/sift1m/sift_query.fvecs \
            -x /media/ray/1tb/sift1m/bkt_memory_index_${threads}t \
            -r /media/ray/1tb/sift1m/sift_groundtruth_sptag.bin \
            -m "1024#2048#4096#8192" \
            -k 32 \
            -t ${threads}
    echo ""
done

echo "=========================================="
echo "所有搜索测试完成！"
echo "=========================================="
```

### 搜索测试结果摘要

```bash
cd /home/ray/code/SPTAG

echo "=== 搜索测试结果 ==="
echo "文件,MaxCheck,Recall,QPS,平均延迟"
for logfile in results/search/bkt_search_*_nocache.log; do
    name=$(basename $logfile _nocache.log)
    grep "0-10000" $logfile 2>/dev/null | while read line; do
        maxcheck=$(echo $line | awk '{print $2}')
        recall=$(echo $line | awk '{print $6}')
        qps=$(echo $line | awk '{print $7}')
        avg=$(echo $line | awk '{print $3}')
        echo "${name},${maxcheck},${recall},${qps},${avg}"
    done
done
```

---

## 2.3 一键执行完整测试流程

```bash
cd /home/ray/code/SPTAG

# 创建输出目录
mkdir -p results/build results/search

echo "=========================================="
echo "1. 构建索引 (8线程)..."
echo "=========================================="
./scripts/sptag_memory_monitor.sh \
    -d /media/ray/1tb \
    -o results/build/bkt_build_8t_nocache.csv \
    -l results/build/bkt_build_8t_nocache.log \
    -C \
    -- ./Release/indexbuilder \
        -d 128 -v Float -f XVEC \
        -i /media/ray/1tb/sift1m/sift_base.fvecs \
        -o /media/ray/1tb/sift1m/bkt_memory_index_8t \
        -a BKT \
        -c config/bkt_build.ini \
        "Index.NumberOfThreads=8"

echo ""
echo "=========================================="
echo "2. 搜索测试 (多 MaxCheck)..."
echo "=========================================="
./scripts/sptag_memory_monitor.sh \
    -d /media/ray/1tb \
    -o results/search/bkt_search_8t_nocache.csv \
    -l results/search/bkt_search_8t_nocache.log \
    -C \
    -- ./Release/indexsearcher \
        -d 128 -v Float -f XVEC \
        -i /media/ray/1tb/sift1m/sift_query.fvecs \
        -x /media/ray/1tb/sift1m/bkt_memory_index_8t \
        -r /media/ray/1tb/sift1m/sift_groundtruth_sptag.bin \
        -m "1024#2048#4096#8192" \
        -k 32 \
        -t 8

echo ""
echo "=========================================="
echo "所有测试完成！"
echo "=========================================="
```

---

## 2.4 监控脚本参数说明

```bash
./scripts/sptag_memory_monitor.sh [选项] -- <被测命令>
```

| 选项 | 说明 | 默认值 |
|------|------|--------|
| `-d, --disk` | 监控的磁盘挂载点 | `/media/ray/1tb` |
| `-o, --output` | 输出 CSV 文件路径 | `sptag_memory_monitor.csv` |
| `-l, --log` | 输出 LOG 文件路径 | `sptag_memory_monitor.log` |
| `-i, --interval` | 采样间隔（秒） | `1` |
| `-C, --clear-cache` | 测试前清除系统缓存 | 否 |
| `-h, --help` | 显示帮助信息 | - |

---

## 2.5 在线模式测试（带监控）

在线模式特点：构建和搜索在同一进程中完成，无需保存索引到磁盘。

### C++ 在线模式测试（推荐）

```bash
cd /home/ray/code/SPTAG

# 带监控的在线模式测试
./scripts/sptag_memory_monitor.sh \
    -d /media/ray/1tb \
    -o results/build/online_test_8t.csv \
    -l results/build/online_test_8t.log \
    -- ./Release/onlinetest \
        -d 128 -v Float -f XVEC \
        -b /media/ray/1tb/sift1m/sift_base.fvecs \
        -q /media/ray/1tb/sift1m/sift_query.fvecs \
        -t /media/ray/1tb/sift1m/sift_groundtruth.ivecs \
        -a BKT -m 8192 -k 32 \
        -t 8
```

### Python 在线模式测试

```bash
cd /home/ray/code/SPTAG

# 单个搜索模式（性能较低）
PYTHONPATH=/home/ray/code/SPTAG/Release:$PYTHONPATH \
python3 scripts/test_online_mode.py \
    --base /media/ray/1tb/sift1m/sift_base.fvecs \
    --query /media/ray/1tb/sift1m/sift_query.fvecs \
    --truth /media/ray/1tb/sift1m/sift_groundtruth.ivecs \
    --threads 8 --k 32 --maxcheck 8192

# 批量搜索模式（性能较高）
PYTHONPATH=/home/ray/code/SPTAG/Release:$PYTHONPATH \
python3 scripts/test_online_mode.py \
    --base /media/ray/1tb/sift1m/sift_base.fvecs \
    --query /media/ray/1tb/sift1m/sift_query.fvecs \
    --truth /media/ray/1tb/sift1m/sift_groundtruth.ivecs \
    --threads 8 --k 32 --maxcheck 8192 \
    --batch
```

### 性能对比（8线程，MaxCheck=8192）

| 模式 | 构建时间 | 搜索 QPS | Recall@32 |
|------|---------|---------|-----------|
| 离线模式 (indexsearcher) | ~660s | 5,149 | 99.85% |
| 在线模式 C++ (onlinetest) | ~543s | 4,673 | 99.85% |
| 在线模式 Python Batch | ~545s | 4,158 | 99.85% |
| 在线模式 Python 单个 | ~545s | 958 | 99.84% |

### 监控输出示例

```
========================================
实验统计摘要
========================================
总运行时间: 509.4 秒

CPU 使用率: 平均 617.7%, 峰值 746.00%
内存占用 (RSS): 平均 790.5 MB, 峰值 1039.63 MB

阶段耗时:
  BuildTree: 43.6s
========================================
```

---

## 2.7 配置文件说明

SPTAG 内存索引使用 `config/bkt_build.ini` 配置文件：

| 配置文件 | 用途 | 使用程序 | 索引类型 |
|----------|------|---------|---------|
| `config/bkt_build.ini` | BKT 构建配置 | `indexbuilder` | 内存索引 |

**配置文件关键参数：**

```ini
[Index]
IndexAlgoType=BKT          # BKT 算法
NeighborhoodSize=32        # 图邻居数
TPTNumber=8                # TPT 树数量
RefineIterations=1         # 图精化迭代次数
NumberOfThreads=8          # 构建线程数
```

**注意**：
- 配置文件使用 `;` 作为注释符号，不支持 `#`
- 配置参数放在 `[Index]` section
- 可通过命令行参数覆盖配置：`"Index.NumberOfThreads=8"`
- 内存索引构建和搜索分离，使用不同程序（`indexbuilder` 和 `indexsearcher`）

---

## 2.8 输出文件

所有结果文件保存在 `results/` 目录下：

### 构建测试输出

| 文件 | 说明 |
|------|------|
| `results/build/bkt_build_8t_nocache.csv` | 8线程构建 CSV 数据 |
| `results/build/bkt_build_8t_nocache.log` | 8线程构建 LOG 日志 |
| `results/build/bkt_build_16t_nocache.csv` | 16线程构建 CSV 数据 |
| `results/build/bkt_build_16t_nocache.log` | 16线程构建 LOG 日志 |

### 搜索测试输出

| 文件 | 说明 |
|------|------|
| `results/search/bkt_search_8t_nocache.csv` | 8线程搜索 CSV 数据 |
| `results/search/bkt_search_8t_nocache.log` | 8线程搜索 LOG 日志 |

### CSV 输出格式

```
timestamp,elapsed_time,stage,cpu_percent,mem_rss_mb,mem_vsz_mb,process_status,threads,disk_read_bytes,disk_write_bytes,disk_read_rate_mbs,disk_write_rate_mbs
```

---

## 2.9 监控脚本阶段识别

SPTAG 内存索引监控脚本自动识别以下阶段：

| 阶段 | 识别关键词 |
|------|-----------|
| BuildTree | `Start to build BKTree`, `Building BKTree` |
| BuildGraph | `build RNG graph`, `BuildGraph` |
| Search | `[query]`, `[maxcheck]`, `[recall]`, `搜索测试` |
| Finished | `Output results finish`, `测试总结` |

---

## 2.10 在线模式测试（Python API）

在线模式特点：构建和搜索在同一进程中完成，无需保存索引到磁盘。

### 测试脚本

脚本路径：`scripts/test_online_mode.py`

### 基本用法

```bash
cd /home/ray/code/SPTAG

# 设置 PYTHONPATH
export PYTHONPATH=/home/ray/code/SPTAG/Release:$PYTHONPATH

# 完整测试（构建 + 搜索）
python3 scripts/test_online_mode.py \
    --base /media/ray/1tb/sift1m/sift_base.fvecs \
    --query /media/ray/1tb/sift1m/sift_query.fvecs \
    --truth /media/ray/1tb/sift1m/sift_groundtruth.ivecs \
    --threads 8 \
    --k 32 \
    --maxcheck 8192
```

### 仅构建测试

```bash
python3 scripts/test_online_mode.py \
    --base /media/ray/1tb/sift1m/sift_base.fvecs \
    --query /media/ray/1tb/sift1m/sift_query.fvecs \
    --truth /media/ray/1tb/sift1m/sift_groundtruth.ivecs \
    --threads 8 \
    --no-search
```

### 保存索引

```bash
python3 scripts/test_online_mode.py \
    --base /media/ray/1tb/sift1m/sift_base.fvecs \
    --query /media/ray/1tb/sift1m/sift_query.fvecs \
    --truth /media/ray/1tb/sift1m/sift_groundtruth.ivecs \
    --threads 8 \
    --output /media/ray/1tb/sift1m/online_index
```

### 参数说明

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `--base` | 基础向量文件路径 | `/media/ray/1tb/sift1m/sift_base.fvecs` |
| `--query` | 查询向量文件路径 | `/media/ray/1tb/sift1m/sift_query.fvecs` |
| `--truth` | Ground truth 文件路径 | `/media/ray/1tb/sift1m/sift_groundtruth.ivecs` |
| `--threads` | 构建线程数 | 8 |
| `--k` | 返回的最近邻数量 | 32 |
| `--maxcheck` | 搜索时最大检查节点数 | 8192 |
| `--output` | 保存索引到指定目录 | None |
| `--no-search` | 仅构建索引，不执行搜索 | False |

### 在线模式 vs 离线模式性能对比

| 指标 | 在线模式（Python 单个） | 在线模式（Python Batch） | 在线模式（C++） | 离线模式 |
|------|------------------------|------------------------|----------------|----------|
| 构建时间 | ~545s | ~545s | ~543s | ~660s |
| 搜索 QPS (8线程) | 958 | 4,158 | **4,673** | 5,149 |
| Recall@32 | 99.84% | 99.85% | 99.85% | 99.85% |
| 平均延迟 | - | - | 1.71ms | 1.6ms |

### C++ 在线模式测试

```bash
cd /home/ray/code/SPTAG

./Release/onlinetest \
    -d 128 -v Float -f XVEC \
    -b /media/ray/1tb/sift1m/sift_base.fvecs \
    -q /media/ray/1tb/sift1m/sift_query.fvecs \
    -t /media/ray/1tb/sift1m/sift_groundtruth.ivecs \
    -a BKT -m 8192 -k 32 \
    -t 8
```

### C++ 在线模式参数说明

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-d` | 向量维度 | 必填 |
| `-v` | 向量类型 (Float, Int8, Int16) | Float |
| `-f` | 文件类型 (XVEC, DEFAULT, TXT) | XVEC |
| `-b` | 基础向量文件 | 必填 |
| `-q` | 查询向量文件 | 必填 |
| `-t` | Ground truth 文件 / 线程数 | - |
| `-o` | 保存索引目录 | 可选 |
| `-a` | 索引算法 (BKT, KDT) | BKT |
| `-m` | MaxCheck | 8192 |
| `-k` | 返回近邻数 | 32 |

### 批量搜索测试（推荐）

```bash
python3 scripts/test_online_mode.py \
    --base /media/ray/1tb/sift1m/sift_base.fvecs \
    --query /media/ray/1tb/sift1m/sift_query.fvecs \
    --truth /media/ray/1tb/sift1m/sift_groundtruth.ivecs \
    --threads 8 \
    --k 32 \
    --maxcheck 8192 \
    --batch
```

### QPS 差距原因分析

**实测性能对比（8线程，MaxCheck=8192）**

| 模式 | QPS | 相对离线模式 | 开销来源 |
|------|-----|-------------|---------|
| 离线模式 (indexsearcher) | 5,149 | 100% | 纯 C++，最优 |
| 在线模式 C++ | 4,673 | 91% | C++，在同一进程内构建+搜索 |
| 在线模式 Python Batch | 4,158 | 81% | Python 调用 + C++ 多线程搜索 |
| 在线模式 Python 单个 | 958 | 19% | Python 循环 + 每次跨语言调用 |

**关键发现**：

1. **C++ 在线模式性能接近离线模式**（91%），差距仅 9%
2. **Python BatchSearch 性能良好**（81%），适合 Python 用户
3. **Python 单个 Search 性能较差**（19%），应避免在生产环境使用

**性能开销分解**：

| 开销来源 | Python 单个 | Python Batch | C++ 在线 | 离线 |
|---------|------------|--------------|---------|------|
| Python→C++ 数据转换 | 每次查询 | 一次批量 | 无 | 无 |
| Python GIL 锁 | 阻塞并行 | 仅调用时 | 无 | 无 |
| 控制流程 | Python | Python | C++ | C++ |
| 内存拷贝 | 多次小块 | 一次大块 | 最优 | 最优 |

**为什么 C++ 在线模式比离线模式慢 9%？**

两种模式代码几乎相同，都使用 C++ 多线程搜索。差距可能来自：
- 在线模式先构建索引，内存状态可能影响搜索缓存
- 离线模式从磁盘加载索引，内存布局更优
- 测试误差（543s vs 660s 构建时间差异）

### 建议

| 场景 | 推荐方式 | QPS |
|------|---------|-----|
| 生产环境、最高性能 | 离线模式（`indexsearcher`） | 5,149 |
| C++ 在线构建+搜索 | 在线模式 C++（`onlinetest`） | 4,673 |
| Python 批量查询 | 在线模式 Python + `--batch` | 4,158 |
| Python 交互式调试 | 在线模式 Python 单个 `Search()` | 958 |

---

# 三、SPANN 磁盘索引详细测试

使用 `spann_build_only.ini` 从头构建索引。

---

## 3.1 单次构建测试

```bash
/home/ray/code/SPTAG/spann_monitor.sh -c /home/ray/code/SPTAG/spann_build_only.ini -C -o /home/ray/code/SPTAG/results/spann_build_nocache.csv -l /home/ray/code/SPTAG/results/spann_build_nocache.log
```

---

## 3.2 构建测试结果摘要

```bash
cd /home/ray/code/SPTAG

echo "=== 构建测试结果 ==="
echo "SelectHead耗时,BuildHead耗时,BuildSSDIndex耗时,总耗时,峰值内存"
select_time=$(grep "select head time" results/spann_build_nocache.log 2>/dev/null | grep -oP 'select head time: \K[0-9]+')
build_time=$(grep "build head time" results/spann_build_nocache.log 2>/dev/null | grep -oP 'build head time: \K[0-9]+')
ssd_time=$(grep "build ssd time" results/spann_build_nocache.log 2>/dev/null | grep -oP 'build ssd time: \K[0-9]+')
total_time=$(awk -F',' 'NR>1 {print $2}' results/spann_build_nocache.csv 2>/dev/null | tail -1 | cut -d'.' -f1)
mem=$(awk -F',' 'NR>1 {print $5}' results/spann_build_nocache.csv 2>/dev/null | sort -n | tail -1)
echo "${select_time}s,${build_time}s,${ssd_time}s,${total_time}s,${mem}MB"
```

---

# 四、SPANN 搜索性能测试

使用已构建的索引，测试不同线程数下的搜索性能。

配置文件：`spann_search_only.ini`

---

## 4.1 单独测试各线程数

### 16 线程测试

```bash
sed -i 's/SearchThreadNum=.*/SearchThreadNum=16/' /home/ray/code/SPTAG/spann_search_only.ini
/home/ray/code/SPTAG/spann_monitor.sh -c /home/ray/code/SPTAG/spann_search_only.ini -C -o /home/ray/code/SPTAG/results/spann_search_16t_nocache.csv -l /home/ray/code/SPTAG/results/spann_search_16t_nocache.log
```

### 8 线程测试

```bash
sed -i 's/SearchThreadNum=.*/SearchThreadNum=8/' /home/ray/code/SPTAG/spann_search_only.ini
/home/ray/code/SPTAG/spann_monitor.sh -c /home/ray/code/SPTAG/spann_search_only.ini -C -o /home/ray/code/SPTAG/results/spann_search_8t_nocache.csv -l /home/ray/code/SPTAG/results/spann_search_8t_nocache.log
```

### 4 线程测试

```bash
sed -i 's/SearchThreadNum=.*/SearchThreadNum=4/' /home/ray/code/SPTAG/spann_search_only.ini
/home/ray/code/SPTAG/spann_monitor.sh -c /home/ray/code/SPTAG/spann_search_only.ini -C -o /home/ray/code/SPTAG/results/spann_search_4t_nocache.csv -l /home/ray/code/SPTAG/results/spann_search_4t_nocache.log
```

### 2 线程测试

```bash
sed -i 's/SearchThreadNum=.*/SearchThreadNum=2/' /home/ray/code/SPTAG/spann_search_only.ini
/home/ray/code/SPTAG/spann_monitor.sh -c /home/ray/code/SPTAG/spann_search_only.ini -C -o /home/ray/code/SPTAG/results/spann_search_2t_nocache.csv -l /home/ray/code/SPTAG/results/spann_search_2t_nocache.log
```

---

## 4.2 一键执行全部搜索测试

```bash
cd /home/ray/code/SPTAG

for threads in 16 8 4 2; do
    echo "=========================================="
    echo "搜索测试 ${threads} 线程..."
    echo "=========================================="
    sed -i "s/SearchThreadNum=.*/SearchThreadNum=${threads}/" spann_search_only.ini
    ./spann_monitor.sh -c spann_search_only.ini -C -o results/spann_search_${threads}t_nocache.csv -l results/spann_search_${threads}t_nocache.log
    echo ""
done

echo "=========================================="
echo "所有搜索测试完成！"
echo "=========================================="
```

---

# 五、查看测试结果摘要

## SPANN 搜索测试结果

```bash
cd /home/ray/code/SPTAG

echo "=== 搜索测试结果 ==="
echo "线程数,QPS,耗时,峰值内存"
for threads in 16 8 4 2; do
    qps=$(grep "actuallQPS" results/spann_search_${threads}t_nocache.log 2>/dev/null | tail -1 | grep -oP 'actuallQPS is \K[0-9.]+')
    time=$(grep "Finish sending" results/spann_search_${threads}t_nocache.log 2>/dev/null | tail -1 | grep -oP 'in \K[0-9.]+')
    mem=$(awk -F',' 'NR>1 {print $5}' results/spann_search_${threads}t_nocache.csv 2>/dev/null | sort -n | tail -1)
    echo "${threads},${qps},${time}s,${mem}MB"
done
```

---

# 六、SPANN 配置文件说明

## 6.1 配置文件对比

SPTAG 项目包含 4 个主要配置文件，用于不同类型的测试：

| 配置文件 | 索引类型 | 使用程序 | SelectHead | BuildHead | BuildSSDIndex | SearchSSDIndex | HashTableExponent | 使用频率 |
|----------|---------|---------|------------|-----------|---------------|----------------|-------------------|---------|
| `config/bkt_build.ini` | 内存 BKT | `indexbuilder` | - | - | - | - | - | ✅ 高 |
| `spann_build_only.ini` | 磁盘 SPANN | `ssdserving` | ✅ | ✅ | ✅ | ❌ | 4（默认） | ✅ 高 |
| `spann_search_only.ini` | 磁盘 SPANN | `ssdserving` | ❌ | ❌ | ❌ | ✅ | 12 | ✅ 高 |
| `spann_build.ini` | 磁盘 SPANN | `ssdserving` | ✅ | ✅ | ✅ | ✅ | 4 | ⚠️ 低 |

## 6.2 为什么需要分离 `spann_build_only.ini` 和 `spann_search_only.ini`？

### 问题：`HashTableExponent` 参数冲突

由于 SPTAG 的参数设计问题，`HashTableExponent` 参数在所有阶段之间共享，无法在构建和搜索时使用不同的值：

| 阶段 | 最优 HashTableExponent | 原因 |
|------|----------------------|------|
| **构建** | 4 | 较小的哈希表，构建更快 |
| **搜索** | 12 | 较大的哈希表，搜索性能更高 |

### 解决方案

```
┌─────────────────────────────────────────────────────────────┐
│  方案一：使用 spann_build.ini（不推荐）                       │
│  ├─ 构建：HashTableExponent=4                               │
│  └─ 搜索：HashTableExponent=4（性能次优）                    │
├─────────────────────────────────────────────────────────────┤
│  方案二：分离配置（推荐）                                     │
│  ├─ spann_build_only.ini：构建时 HashTableExponent=4        │
│  └─ spann_search_only.ini：搜索时 HashTableExponent=12      │
└─────────────────────────────────────────────────────────────┘
```

## 6.3 各配置文件详细说明

### `config/bkt_build.ini` - BKT 内存索引构建配置

用于构建 BKT 内存索引，配合 `indexbuilder` 使用：

```bash
./Release/indexbuilder -c config/bkt_build.ini -a BKT ...
```

### `spann_build_only.ini` - SPANN 磁盘索引构建配置

仅构建 SPANN 索引，不执行搜索：

```bash
./spann_monitor.sh -c spann_build_only.ini ...
```

### `spann_search_only.ini` - SPANN 磁盘索引搜索配置

仅搜索已构建的索引，使用优化的 `HashTableExponent=12`：

```bash
./spann_monitor.sh -c spann_search_only.ini ...
```

### `spann_build.ini` - SPANN 构建+搜索一体化配置

构建索引后立即搜索，由于 HashTableExponent 冲突，搜索性能不是最优，**不推荐使用**。

## 6.4 配置文件阶段执行对照

| 配置文件 | SelectHead | BuildHead | BuildSSDIndex | SearchSSDIndex |
|----------|:----------:|:---------:|:-------------:|:--------------:|
| `config/bkt_build.ini` | - | - | - | - |
| `spann_build_only.ini` | ✅ 执行 | ✅ 执行 | ✅ 执行 | ❌ 不执行 |
| `spann_search_only.ini` | ❌ 不执行 | ❌ 不执行 | ❌ 不执行 | ✅ 执行 |
| `spann_build.ini` | ✅ 执行 | ✅ 执行 | ✅ 执行 | ✅ 执行 |

---

# 七、SPANN 输出文件

所有结果文件保存在 `results/` 目录下：

## 构建测试输出

| 文件 | 说明 |
|------|------|
| `results/spann_build_nocache.csv` | 构建 CSV 数据 |
| `results/spann_build_nocache.log` | 构建 LOG 日志 |

## 搜索测试输出

| 文件 | 说明 |
|------|------|
| `results/spann_search_16t_nocache.csv` | 16 线程搜索 CSV 数据 |
| `results/spann_search_16t_nocache.log` | 16 线程搜索 LOG 日志 |
| `results/spann_search_8t_nocache.csv` | 8 线程搜索 CSV 数据 |
| `results/spann_search_8t_nocache.log` | 8 线程搜索 LOG 日志 |
| `results/spann_search_4t_nocache.csv` | 4 线程搜索 CSV 数据 |
| `results/spann_search_4t_nocache.log` | 4 线程搜索 LOG 日志 |
| `results/spann_search_2t_nocache.csv` | 2 线程搜索 CSV 数据 |
| `results/spann_search_2t_nocache.log` | 2 线程搜索 LOG 日志 |

---

# 八、SPANN 一键执行完整测试流程

```bash
cd /home/ray/code/SPTAG

echo "=========================================="
echo "1. 构建索引..."
echo "=========================================="
./spann_monitor.sh -c spann_build_only.ini -C -o results/spann_build_nocache.csv -l results/spann_build_nocache.log

echo ""
echo "=========================================="
echo "2. 搜索测试..."
echo "=========================================="
for threads in 16 8 4 2; do
    echo "搜索测试 ${threads} 线程..."
    sed -i "s/SearchThreadNum=.*/SearchThreadNum=${threads}/" spann_search_only.ini
    ./spann_monitor.sh -c spann_search_only.ini -C -o results/spann_search_${threads}t_nocache.csv -l results/spann_search_${threads}t_nocache.log
done

echo ""
echo "=========================================="
echo "所有测试完成！"
echo "=========================================="
```
