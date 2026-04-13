# SPANN 性能测试指令

本文档提供清除缓存后的 SPANN 性能测试指令，需要 sudo 权限。

## 测试前准备

所有测试结果将保存到 `results/` 文件夹下。

## ⚠️ 重要说明

由于 SPTAG 的参数设计问题，`HashTableExponent` 参数在所有阶段之间共享：
- 搜索阶段需要 `HashTableExponent=12`（较大的哈希表提高搜索性能）
- 构建阶段需要 `HashTableExponent=4`（默认值，构建更快）

因此，**构建和搜索测试需要分开执行**：
1. 使用 `spann_build_only.ini` 进行索引构建
2. 使用 `spann_search_only.ini` 进行搜索测试

---

# 一、索引构建测试

使用 `spann_build_only.ini` 从头构建索引。

**注意**：构建测试不包含搜索阶段，仅测试构建性能。

---

## 1.1 单次构建测试

```bash
/home/ray/code/SPTAG/spann_monitor.sh -c /home/ray/code/SPTAG/spann_build_only.ini -C -o /home/ray/code/SPTAG/results/spann_build_nocache.csv -l /home/ray/code/SPTAG/results/spann_build_nocache.log
```

---

## 1.2 构建测试结果摘要

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

# 二、搜索性能测试

使用已构建的索引，测试不同线程数下的搜索性能。

配置文件：`spann_search_only.ini`

---

## 2.1 单独测试各线程数

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

## 2.2 一键执行全部搜索测试

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

# 三、查看测试结果摘要

## 搜索测试结果

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

# 四、配置文件说明

| 配置文件 | 用途 | 阶段 | HashTableExponent |
|----------|------|------|-------------------|
| `spann_build_only.ini` | 仅构建索引 | SelectHead → BuildHead → BuildSSDIndex | 4（默认） |
| `spann_search_only.ini` | 仅搜索测试 | SearchSSDIndex | 12 |

---

# 五、输出文件

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

# 六、一键执行完整测试流程

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
