# H0.1 HeadCandidateNum/PostingBudget 解耦验证报告

## 实验结果

| 指标 | ir=64 (baseline) | ir=128 | 变化 |
|------|------------------|--------|------|
| **Recall@10** | 0.978319 | 0.993960 | **+1.56%** |
| **QPS** | 5892.75 | 2950.72 | -50% |
| **Pages/query** | 118.67 | 236.43 | +99% |
| **Postings touched** | 63.65 | 127.29 | +100% |
| **Head Latency** | 0.329 ms | 0.394 ms | +20% |
| **Ex Latency** | 1.026 ms | 2.315 ms | +126% |
| **Total Latency** | 1.356 ms | 2.709 ms | +100% |

## 关键发现

### 1. HeadCandidateNum=128 可以产生更长 posting order ✅

- 平均 127.29 postings/query (接近 128)
- 说明 head search 可以返回更多候选

### 2. Recall 提升显著 ✅

- Recall@10: 0.978 → 0.994 (+1.56%)
- 说明有约 1.5% 的 query 的真实近邻在 posting 64-128 之间

### 3. Head Search 延迟增量可接受 ✅

- Head latency: 0.329 → 0.394 ms (+20%)
- 仅增加 0.065 ms，相对于 1.36 ms 总延迟可忽略

### 4. 当前实现无法解耦 ❌

- `InternalResultNum` 同时控制:
  1. Head search 返回的 candidate 数量
  2. ExtraSearcher 读取的 posting 数量

- 无法实现: HeadCandidateNum=128, PostingBudget=64

## 分析

### 低 recall query 是否被 ir=128 改善？

需要对比 query-level recall 分布来判断：
- ir=64 的低 recall query 在 ir=128 下是否改善了？

### Adaptive Budget 的空间

从数据看：
- ir=128 比 ir=64 多读了 64 个 postings
- Recall 提升了 1.56%
- 说明 98.44% 的 query 在 B=64 时已达到目标 recall

如果 adaptive budget 正确识别 easy query：
- 可以在 B=32/48 时节省 I/O
- 在 B=64 时保持 baseline
- 在 B=96/128 时提升 hard query recall

## 下一步

### Option A: 修改代码实现解耦

需要添加新参数:
- `HeadCandidateNum`: head search 返回的 candidate 数量
- `PostingBudget`: 实际读取的 posting 数量

### Option B: 纯离线分析

使用现有 ir=128 的数据，在离线模拟不同 posting budget:
- 对每个 query 取前 B 个 postings (B ∈ {16,32,48,64,96,128})
- 计算 recall 和 pages
- 得到 oracle upper bound

## 建议

**推荐 Option B**：先做离线 oracle 分析，确认 adaptive budget 有足够空间后再决定是否修改代码。

如果 oracle 显示 pages/query 可以节省 >= 15%，再实现解耦。
