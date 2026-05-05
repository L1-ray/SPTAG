# Online 测试完整时间线分析

**分析日期**: 2026-05-05

---

## 一、时间线总览

```
2026-05-03 ──────────────────────────────────────────────────────
  │
  ├─ 22:27  H0.1 解耦验证 (ir=64 vs ir=128)
  ├─ 22:42  Budget Sweep (SIFT1M, B=16~128)
  ├─ 22:51  Budget Sweep (SIFT10M, B=16~128)
  ├─ 23:03  Phase 2 特征提取
  ├─ 23:14  Phase 3 Rule-based Online Test (SIFT1M)
  └─ 23:20  Phase 3 Rule-based Online Test (SIFT10M)

2026-05-04 ──────────────────────────────────────────────────────
  │
  ├─ 00:19  Phase 4 Learned Policy Online Test (SIFT1M) ★首次成功
  ├─ 09:26  SIFT10M Learned Test (单次)
  ├─ 09:57  SIFT10M Multi-run (3x baseline, 3x learned)
  ├─ 10:46  SIFT1M Multi-run (3x baseline, 3x learned)
  ├─ 10:53  SIFT1M Sweep Learned (不同 st/ir 配置)
  ├─ 21:13  SIFT1M Paper Params 测试
  ├─ 21:36  SIFT1M Threshold Sweep (Official)
  └─ 22:00  SIFT1M ir64 Retrain

2026-05-05 ──────────────────────────────────────────────────────
  │
  ├─ 09:22  SIFT10M ir64 Retrain Budget Sweep
  ├─ 09:25  SIFT10M ir64 Retrain Threshold Tests
  ├─ 09:30  SIFT1M ir64 Retrain Threshold Tests
  ├─ 09:40  SIFT1M Learned Sweep (st=2/4/8/16)
  ├─ 10:22  SIFT1M Diskstats Sweep
  ├─ 11:29  Budget Granularity A0 vs A3 (SIFT1M & SIFT10M)
  ├─ 11:40  SIFT1M A4 Conservative Tests
  ├─ 19:24  SIFT1M Timing Test
  ├─ 19:37  SIFT10M Timing Test
  └─ 20:20  SIFT1M ir Compare (ir=64 vs ir=128)
```

---

## 二、详细测试记录

### 2026-05-03: Phase 1-3 验证

#### 2.1 H0.1 解耦验证 (22:27)

| 配置 | InternalResultNum | QPS | Recall | 说明 |
|------|------------------|-----|--------|------|
| ir=64 | 64 | 5,893 | 0.9783 | Baseline |
| ir=128 | 128 | 2,951 | 0.9940 | B=128, QPS下降50% |

**发现**: HeadCandidateNum 和 PostingBudget 当时耦合，无法独立控制

**后续解耦**: 代码修改后分出两个独立参数：

| 参数 | 配置名 | 默认值 | 作用 |
|------|--------|--------|------|
| **InternalResultNum** | `SearchInternalResultNum` | 64 | Head search 返回的候选数量 |
| **PostingBudget** | `PostingBudget` | -1 (使用 InternalResultNum) | 实际读取的 posting 数量 |

代码定义 (`Options.h`):
```cpp
int m_searchInternalResultNum;           // Head search 返回多少候选
int m_postingBudget;                     // 实际读取多少 posting (<= m_searchInternalResultNum)
                                         // -1 表示使用 m_searchInternalResultNum
```

配置示例:
```ini
[SearchSSDIndex]
InternalResultNum=128      ; Head search 返回 128 个候选
PostingBudget=64           ; 但只读取前 64 个 posting
```

**解耦的意义**:
1. **更大的 head 候选集**: `InternalResultNum=128` 提供 128 个候选，提高 routing 质量
2. **可控的 I/O 预算**: `PostingBudget=64` 限制只读 64 个 posting，控制延迟
3. **为 Learned Policy 提供基础**: 可以动态调整 `PostingBudget` 而不影响 head search 行为

#### 2.2 Budget Sweep (22:42-22:51)

**SIFT1M**:
| B | QPS | Recall | Pages |
|---|-----|--------|-------|
| 16 | 11,834 | 0.8698 | 30 |
| 32 | 9,074 | 0.9404 | 60 |
| 48 | 7,236 | 0.9657 | 90 |
| **64** | 5,828 | **0.9786** | 119 |
| 128 | 2,979 | 0.9940 | 236 |

**Oracle 分析**: 理论最优 pages saving = **50.8%**

#### 2.3 Phase 3 Rule-based Online Test (23:14-23:25)

**SIFT1M**:
| 配置 | QPS | 说明 |
|------|-----|------|
| Baseline | 5,893 | B=64 |
| Rule-based | 6,146 | margin_16 规则 |

**SIFT10M**:
| 配置 | QPS | 说明 |
|------|-----|------|
| Baseline | 5,691 | B=64 |
| Rule-based | 5,692 | 效果不明显 |

**结论**: Rule-based 在 SIFT1M 有效，SIFT10M 效果有限

---

### 2026-05-04: Learned Policy 首次验证

#### 2.4 SIFT1M Learned Test (00:19) ★首次成功

| 配置 | QPS | Recall | Postings | 说明 |
|------|-----|--------|----------|------|
| Baseline | 5,708 | 0.9786 | - | B=64 |
| Learned (B=128) | 2,943 | 0.9940 | 127.3 | 验证模型加载 |
| **Learned (B=64)** | **6,169** | **0.9782** | **101.2** | **+8.08% QPS** |

**关键里程碑**: Learned Policy 首次在线测试成功！

#### 2.5 SIFT10M Learned Test (09:26) 单次测试

| 配置 | QPS | Recall | 说明 |
|------|-----|--------|------|
| Baseline | ~5,400 | 0.949 | B=64 |
| Learned (t=0.80) | **6,761** | 0.9478 | **+24.4% QPS** ⚠️ |

**警告**: 单次测试结果波动大，需要多次验证

#### 2.6 SIFT10M Multi-run (09:57) 3次验证

| Run | Baseline QPS | Learned QPS |
|-----|--------------|-------------|
| 1 | 5,305 | 5,970 |
| 2 | 5,311 | 5,952 |
| 3 | 5,302 | 5,995 |
| **Mean** | **5,306** | **5,973** |

**修正结果**: +12.56% QPS (非 +24.4%)

#### 2.7 SIFT1M Multi-run (10:46) 3次验证

| Run | Baseline QPS | Learned QPS |
|-----|--------------|-------------|
| 1 | 5,672 | 6,277 |
| 2 | 5,780 | 6,285 |
| 3 | 5,777 | 6,258 |
| **Mean** | **5,743** | **6,274** |

**结果**: +9.2% QPS, Recall delta = -0.00037

---

### 2026-05-05: 参数调优与最终验证

#### 2.8 Budget Granularity A0 vs A3 (11:29)

**SIFT1M**:
| 配置 | QPS | Recall | 说明 |
|------|-----|--------|------|
| A0 (baseline) | 7,057 | 0.9775 | B=32/40/48/64 |
| **A3 (learned)** | **7,474** | 0.9763 | **+5.9% QPS** |

**SIFT10M**:
| 配置 | QPS | Recall | 说明 |
|------|-----|--------|------|
| A0 (baseline) | 6,636 | 0.9472 | B=32/40/48/64 |
| A3 (learned) | 6,752 | 0.9465 | +1.8% QPS |

#### 2.9 SIFT1M Timing Test (19:24)

| 配置 | Wall Time | QPS | Recall | Pages |
|------|-----------|-----|--------|-------|
| Baseline | 2.459s | 5,841 | 0.9783 | 118.7 |
| Learned | 2.311s | 6,402 | 0.9775 | 98.5 |

**结果**: -6.0% wall time, +9.6% QPS, -17.0% pages

#### 2.10 SIFT10M Timing Test (19:37)

| 配置 | Wall Time | QPS | Recall | Pages |
|------|-----------|-----|--------|-------|
| Baseline | 4.560s | 5,411 | 0.9491 | 125.9 |
| Learned | 4.423s | 5,824 | 0.9472 | 103.1 |

**结果**: -3.0% wall time, +7.6% QPS, -18.1% pages

#### 2.11 SIFT1M ir=64 vs ir=128 Compare (20:20)

**ir=64 系列**:
| 配置 | QPS | Recall | Postings | 说明 |
|------|-----|--------|----------|------|
| Baseline | 5,721 | 0.9783 | 63.65 | B=64 |
| **Learned (t=0.97)** | **6,365** | 0.9774 | 51.02 | **+11.3% QPS** ✅ |

**ir=128 系列**:
| 配置 | QPS | Recall | Postings | 说明 |
|------|-----|--------|----------|------|
| Baseline (B=64) | 5,640 | 0.9786 | 64.00 | - |
| Learned (t=0.90) | 6,431 | 0.9687 | 51.48 | +14.0% QPS, 但recall loss太大 ❌ |
| Learned (t=0.95) | 5,767 | 0.9763 | 60.89 | +2.3% QPS ⚠️ |
| Learned (t=0.97) | 5,609 | 0.9782 | 63.35 | -0.5% QPS ❌ |
| Baseline (B=128) | 2,937 | 0.9940 | 127.29 | 高recall但QPS低 |

**结论**: ir=64 + threshold=0.97 是最佳配置

---

## 三、关键指标汇总

### 3.1 QPS 提升对比

| 测试日期 | 数据集 | 测试类型 | Baseline QPS | Learned QPS | 提升 |
|----------|--------|----------|--------------|-------------|------|
| 05-04 00:19 | SIFT1M | 单次 | 5,708 | 6,169 | **+8.08%** |
| 05-04 10:46 | SIFT1M | 3次mean | 5,743 | 6,274 | **+9.2%** |
| 05-05 19:24 | SIFT1M | Timing | 5,841 | 6,402 | **+9.6%** |
| 05-05 20:20 | SIFT1M | ir=64 | 5,721 | 6,365 | **+11.3%** |
| 05-04 09:26 | SIFT10M | 单次 | ~5,400 | 6,761 | +24.4% (波动) |
| 05-04 09:57 | SIFT10M | 3次mean | 5,306 | 5,973 | **+12.56%** |
| 05-05 19:37 | SIFT10M | Timing | 5,411 | 5,824 | **+7.6%** |

### 3.2 Recall 变化

| 数据集 | Baseline Recall | Learned Recall | Delta | 阈值 | 状态 |
|--------|-----------------|----------------|-------|------|------|
| SIFT1M | 0.9783 | 0.9774 | -0.0009 | 0.002 | ✅ |
| SIFT10M | 0.9491 | 0.9478 | -0.0013 | 0.002 | ✅ |

### 3.3 Pages/Postings 节省

| 数据集 | Baseline Pages | Learned Pages | Saving |
|--------|---------------|---------------|--------|
| SIFT1M | 118.7 | 98.5 | **17.0%** |
| SIFT10M | 125.9 | 103.1 | **18.1%** |

---

## 四、关键发现

### 4.1 单次测试 vs 多次测试

| 数据集 | 单次 QPS提升 | 多次 QPS提升 | 差异 |
|--------|-------------|-------------|------|
| SIFT10M | +24.4% | +12.56% | **-11.8pp** |

**教训**: 单次测试结果不可靠，必须多次测试取平均

### 4.2 Threshold 敏感性

**SIFT1M ir=128**:
| Threshold | QPS提升 | Recall Delta |
|-----------|---------|--------------|
| 0.90 | +14.0% | -0.0099 ❌ |
| 0.95 | +2.3% | -0.0023 ⚠️ |
| 0.97 | -0.5% | -0.0004 |

**结论**: ir=128 模型 threshold 敏感性过高，不适合生产使用

### 4.3 ir=64 vs ir=128

| 配置 | 最佳 QPS提升 | Recall Delta | 稳定性 |
|------|-------------|--------------|--------|
| ir=64 | +11.3% | -0.0009 | ✅ 稳定 |
| ir=128 | +14.0% | -0.0099 | ❌ 不稳定 |

**结论**: ir=64 是最佳选择

### 4.4 不同数据集的 Threshold

| 数据集 | 推荐 Threshold | QPS提升 | 说明 |
|--------|---------------|---------|------|
| SIFT1M | 0.97 | +8~11% | 保守，recall loss 小 |
| SIFT10M | 0.80~0.85 | +7~12% | 激进，但仍达标 |

---

## 五、测试配置演进

### 5.1 参数变化

| 阶段 | InternalResultNum | SearchThreadNum | Threshold | 说明 |
|------|------------------|-----------------|-----------|------|
| Phase 3 | 128 | 8 | - | Rule-based |
| Phase 4 | 128 | 8 | 0.97 | Learned, ir=128 |
| 05-04 | 128 | 8 | 0.97/0.80 | SIFT1M/SIFT10M |
| 05-05 | **64** | 8 | 0.97 | **ir=64 确定为最佳** |

### 5.2 Budget 分布变化

**SIFT1M (threshold=0.97)**:
| Budget | Phase 4 | 05-05 Final |
|--------|---------|-------------|
| B=32 | 18.6% | 21.3% |
| B=40 | 8.2% | 12.5% |
| B=48 | 12.4% | 15.1% |
| B=64 | 60.8% | 33.5% |

**SIFT10M (threshold=0.80)**:
| Budget | Phase 4 | 05-05 Final |
|--------|---------|-------------|
| B=32 | 26.9% | 26.9% |
| B=40 | 12.7% | 12.7% |
| B=48 | 23.9% | 23.9% |
| B=64 | 36.4% | 36.4% |

---

## 六、最终推荐配置

### SIFT1M (1M vectors)

```ini
[SearchSSDIndex]
InternalResultNum=64
PostingBudget=64
EnableLearnedBudget=true
LearnedBudgetThreshold=0.97
LearnedBudgetDefault=64
LearnedBudgetMin=32
```

**预期效果**:
- QPS: +8~11%
- Recall delta: -0.0004~-0.0009
- Pages saving: 15~20%

### SIFT10M (10M vectors)

```ini
[SearchSSDIndex]
InternalResultNum=64
PostingBudget=64
EnableLearnedBudget=true
LearnedBudgetThreshold=0.80
LearnedBudgetDefault=64
LearnedBudgetMin=32
```

**预期效果**:
- QPS: +7~12%
- Recall delta: -0.0013~-0.0019
- Pages saving: 18~22%

---

## 七、测试文件索引

### 关键日志文件

| 日期 | 文件 | 说明 |
|------|------|------|
| 05-03 23:14 | `phase3_online_test/adaptive.log` | Rule-based 首次测试 |
| 05-04 00:19 | `sift1m_learned_test/learned_qps.log` | Learned 首次成功 ★ |
| 05-04 09:57 | `sift10m_multi_run/*.log` | SIFT10M 3次验证 |
| 05-04 10:46 | `sift1m_multi_run/*.log` | SIFT1M 3次验证 |
| 05-05 19:24 | `sift1m_timing_test/*.log` | SIFT1M Timing |
| 05-05 19:37 | `sift10m_timing_test/*.log` | SIFT10M Timing |
| 05-05 20:20 | `sift1m_ir_compare/*.log` | ir=64 vs ir=128 |

### 关键报告文件

| 文件 | 说明 |
|------|------|
| `sift1m_learned_test/EVALUATION_REPORT.md` | SIFT1M 首次评估 |
| `sift10m_multi_run/EVALUATION_REPORT.md` | SIFT10M 多次验证 |
| `sift1m_timing_test/RESULTS.md` | SIFT1M Timing 结果 |
| `sift10m_timing_test/RESULTS.md` | SIFT10M Timing 结果 |
| `sift1m_ir_compare/RESULTS.md` | ir 对比最终结论 |
