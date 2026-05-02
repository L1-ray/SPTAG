# S0 Diagnosis Summary (2026-05-02)

## Bug Fix

修复了 `ExtraStaticSearcher.h` 第 793 行的 null pointer dereference bug：
- 原代码：`if (truth && truth->count(vectorID)) (*found)[curPostingID].insert(vectorID);`
- 问题：当 `truth` 非空但 `found` 为空时，解引用 `(*found)` 导致 SEGFAULT
- 修复：添加 `&& found` 检查

## Trace Collection Status

| Format | Threads | Index | Recall@10 | Avg Latency (ms) | QPS (approx) |
|--------|---------|-------|-----------|------------------|--------------|
| Legacy | st8 | official_u8default | 0.978319 | ~1.34 | ~5945 |
| Two-stage | st1 | m2_u8default | 0.977299 | 3.970 | 252 |
| Two-stage | st8 | m2_u8default | 0.977299 | 4.412 | 227 |

## S0 Diagnosis Results (Two-stage st8)

### Key Metrics

| Metric | Value | Interpretation |
|--------|-------|----------------|
| Payload wait / ex latency | 0.602 | I/O wait 占 extra latency 60% |
| Avg candidates per unique payload page | 4.197 | Page locality 较弱 |
| Gini(payload wait) | 0.100 | 分布相对均匀 |
| Top10 query share of payload wait | 13.7% | 无极端热点 |

### Direction Ranking

| Direction | Score | Reason |
|-----------|-------|--------|
| M1 (Global I/O broker + page cache) | 2 | payload_read_wait_over_ex is high; read wait is likely dominant |
| M2-H (Hybrid selective code-first) | 2 | candidates per unique payload page is low; page fanout/locality is weak |
| M4 (Primary-secondary dedupe) | 1 | Default fallback |

## Next Steps

1. **M1 Prototype**: 实现 global page cache + in-flight coalescing
   - 预期收益：降低 payload read wait，特别是并发场景
   - 成功指标：read wait 下降 >=10%，QPS 提升 >=5%

2. **M2-H Analysis**: 分析 bad postings 分布
   - 需要：posting-level bytes/wait 贡献分析
   - 决策点：top 10% postings 是否贡献 >=30% read wait

3. **Legacy vs Two-stage Comparison**:
   - Legacy QPS 显著高于 Two-stage (~5945 vs ~227 st8 approx)
   - 原因分析：Two-stage 的额外开销（scan code, merge, build read plan）超过了 payload savings
   - 结论：**不在 two-stage 方向上继续优化，转而在 legacy 上叠加 M1 cache/coalescing**
