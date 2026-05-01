# P0/P1 优化记忆：UInt8 + DEFAULT 格式

## 数据格式约束

**必须使用 strict `UInt8 + DEFAULT` 格式**，与官方 AE 配置对齐：

- `ValueType=UInt8`
- `VectorType=DEFAULT`
- `QueryType=DEFAULT`
- `TruthType=DEFAULT`

### 数据文件

- Base vectors: `/home/ray/data/sift1m/bigann1m_base.u8bin` (1M x 128 UInt8)
- Query vectors: `/home/ray/data/sift1m/query.public.10K.u8bin` (10K x 128 UInt8)
- Ground truth: `/home/ray/data/sift1m/bigann-1M.bin` (10K x 100)

### 索引目录

- Legacy baseline: `/home/ray/data/sift1m/spann_index_official_u8default_20260430`
- Two-stage 索引需要重新构建（使用 UInt8 数据）

## Legacy Baseline 性能

`SearchThreadNum=8, NumberOfThreads=16, InternalResultNum=64, SearchPostingPageLimit=4`:

- QPS ≈ 5945
- Recall@10 ≈ 0.978
- avg latency ≈ 1.34 ms
- requested bytes ≈ 486 KB/query

## P0: 并发 Recall 复核

### 目标

验证历史 `SearchThreadNum=8` recall 下降现象是否仍存在

### 测试配置

```
ValueType=UInt8
VectorType=DEFAULT
PostingTopRPerPosting=64
PostingTopRGlobal=256
EnableChunkedPosting=false
PostingChunkPruneMode=None
InternalResultNum=64
SearchPostingPageLimit=4
IndexDirectory=/home/ray/data/sift1m/spann_index_official_u8default_20260430
```

### 测试矩阵

SearchThreadNum = 1, 2, 4, 8

### 测试结果 (2026-05-01)

| SearchThreadNum | QPS | Recall@10 |
|---|---|---|
| 1 | 951 | 0.978319 |
| 2 | 1946 | 0.978319 |
| 4 | 3764 | 0.978319 |
| 8 | 5889 | 0.978319 |

### 验收结论

**历史并发 recall 下降现象已不存在！**
- st=1 到 st=8 的 Recall@10 均稳定在 0.978319
- QPS 随并发线性增长，st=8 时达到 ~5.9k QPS
- 并发扩展性良好，无需额外优化

## P1: Payload Physical Page 优化

### 当前问题

- PayloadPhysicalBytesRead ≈ 388 KB/query
- PagesRead ≈ 95/query
- payload logical bytes 很小，但物理页读取量大

### 优化方向

1. Build-side payload 重排：按 coarse-friendly 顺序写 payload
2. 减少 payload page 分散
3. 提高 candidatesPerPayloadPage

## 注意事项

- 所有 benchmark 必须使用 UInt8 + DEFAULT
- 不再使用 Float + XVEC 格式
- 每轮测试需要记录 raw log 来源
