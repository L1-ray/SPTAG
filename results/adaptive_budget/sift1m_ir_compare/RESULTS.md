# SIFT1M ir=64 vs ir=128 完整对比报告

## 测试配置
- **Dataset**: SIFT1M (1M vectors, 128 dimensions)
- **Threads**: SearchThreadNum=8
- **Head Candidates**: InternalResultNum = 64 or 128
- **Posting Budget**: B = 64 or 128

## 参数说明

| 参数 | 作用 | 影响 |
|------|------|------|
| InternalResultNum | Head search 返回的候选数量 | 更多候选 = 更高 recall，但 head search 更慢 |
| PostingBudget | 每个查询最多读取的 posting 数量 | 更多 posting = 更高 recall，但 I/O 更多 |

## 完整测试结果

### 第一组：相同 PostingBudget (B=64)，不同 InternalResultNum

| Config | ir | B | Policy | Threshold | QPS | Recall | Postings | Posting Save | QPS Δ |
|--------|----|----|--------|-----------|-----|--------|----------|--------------|-------|
| ir64_baseline | 64 | 64 | Fixed | - | 5,721 | 0.9783 | 63.65 | - | - |
| **ir64_learned** | 64 | 64 | Learned | 0.97 | **6,365** | 0.9774 | 51.02 | **19.9%** | **+11.3%** |
| ir128_baseline | 128 | 64 | Fixed | - | 5,640 | 0.9786 | 64.00 | - | - |
| ir128_learned_t90 | 128 | 64 | Learned | 0.90 | 6,431 | 0.9687 | 51.48 | 19.5% | +14.0% |
| ir128_learned_t95 | 128 | 64 | Learned | 0.95 | 5,767 | 0.9763 | 60.89 | 4.8% | +2.3% |
| ir128_learned_t97 | 128 | 64 | Learned | 0.97 | 5,609 | 0.9782 | 63.35 | 1% | -0.5% |

### 第二组：相同 InternalResultNum (ir=128)，不同 PostingBudget

| Config | ir | B | Policy | Threshold | QPS | Recall | Postings | Posting Save | QPS Δ |
|--------|----|----|--------|-----------|-----|--------|----------|--------------|-------|
| ir128_baseline | 128 | 64 | Fixed | - | 5,640 | 0.9786 | 64.00 | - | - |
| ir128_b128_baseline | 128 | 128 | Fixed | - | 2,937 | 0.9940 | 127.29 | - | - |
| ir128_b128_learned* | 128 | 128 | Learned | 0.90 | 4,337 | 0.9837 | 83.53 | 34.4% | +47.7% |

*注：ir128_b128_learned 使用 B=64 baseline 的模型，default=128，对比不公平

## 关键发现

### 1. ir=64 vs ir=128 (相同 B=64)

**ir=64 learned 是最佳配置**：
- QPS: +11.3%
- Recall delta: -0.0009 (可接受)
- Posting saving: 19.9%

**ir=128 模型问题**：
- threshold=0.90: Recall loss 太大 (-0.0099) ❌
- threshold=0.97: 几乎没有节省效果 ❌
- threshold=0.95: 中等效果，但不如 ir=64 ⚠️

### 2. B=64 vs B=128 (相同 ir=128)

**B=128 baseline**：
- Recall: 0.9940 (vs 0.9786, +0.0154)
- QPS: 2,937 (vs 5,640, **-48%**)
- Postings: 127.29 (vs 64.00, **+99%**)

**Tradeoff**：B=128 用 2x I/O 换取 1.5% recall 提升，代价太高。

### 3. ir=128 baseline 性能分析

| Metric | ir=64 baseline | ir=128 baseline | Delta |
|--------|---------------|-----------------|-------|
| QPS | 5,721 | 5,640 | -1.4% |
| Recall | 0.9783 | 0.9786 | +0.0003 |
| Head Latency | ~0.33ms | ~0.33ms | 相同 |
| Posting Budget | 64 | 64 | 相同 |

ir=128 baseline 比 ir=64 慢 1.4%，但 recall 略高。差异很小因为 PostingBudget 相同。

## 推荐配置

### 场景 1：追求 QPS（推荐）

```
InternalResultNum=64
PostingBudget=64 (baseline)
EnableLearnedBudget=true
LearnedBudgetThreshold=0.97
```

**效果**：+11.3% QPS, -0.0009 recall delta

### 场景 2：追求 Recall

```
InternalResultNum=128
PostingBudget=128
```

**效果**：Recall=0.994, QPS=2,937 (比默认配置慢 48%)

### 场景 3：平衡 QPS 和 Recall

```
InternalResultNum=64
PostingBudget=64
```

**效果**：Recall=0.978, QPS=5,721 (默认配置)

## 结论

1. **ir=64 learned (threshold=0.97) 是最佳选择**
   - QPS 提升 11.3%
   - Recall 损失可接受 (-0.0009)
   - 配置简单，模型已验证

2. **ir=128 learned 模型需要重新训练**
   - 当前模型 threshold 敏感性过高
   - 需要针对目标 miss rate 重新训练

3. **B=128 baseline 代价过高**
   - 2x I/O 换取 1.5% recall 提升不划算
   - 如果需要高 recall，应考虑其他方法（如重新排序）

4. **ir=128 主要用于高 recall 场景**
   - 如果目标 recall >= 0.99，使用 ir=128 + B=128
   - 否则 ir=64 更高效
