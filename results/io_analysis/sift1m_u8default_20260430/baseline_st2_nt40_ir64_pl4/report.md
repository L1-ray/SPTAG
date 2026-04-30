# SPANN 搜索 I/O 性能分析报告

## 输入概览
- query_rows: 0
- disk_rows: 8
- process_rows: 7
- cpu_rows: 8
- psi_rows: 8

## 瓶颈分析
- 结论: **暂无法判定（缺少查询级数据）**
- avg_latency_ms: 0.000
- p95_latency_ms: 0.000
- p99_latency_ms: 0.000
- avg_io_wait_ms: 0.000
- io_wait_ratio: 0.000
- corr(latency, requested_bytes): 0.000
- corr(latency, avg_queue_depth): 0.000

## 系统级信号
- avg_read_bandwidth_mbs: 0.000
- avg_cpu_iowait_percent: 9.788
- psi_io_some_delta_us: 608165
- psi_io_full_delta_us: 594232
- process_read_bytes_delta: 0

## 查询级效率指标

## 备注
- 当缺少 query-csv 时，报告会退化为日志与系统级摘要。
- 相关性分析基于同一 monotonic ns 时间窗重叠 join。
