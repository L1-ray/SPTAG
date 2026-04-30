# SPANN 搜索 I/O 性能分析报告

## 输入概览
- query_rows: 10000
- disk_rows: 1222
- process_rows: 1221
- cpu_rows: 1222
- psi_rows: 1222
- log_qps: 81.220
- log_total_runtime_s: 0.000
- log_search_stage_s: 0.000
- log_read_mb: 0.000

## 瓶颈分析
- 结论: **存在混合瓶颈，I/O 不是唯一主导**
- avg_latency_ms: 98.312
- p95_latency_ms: 113.672
- p99_latency_ms: 121.169
- avg_io_wait_ms: 0.000
- io_wait_ratio: 0.000
- corr(latency, requested_bytes): 0.076
- corr(latency, avg_queue_depth): -0.359

## 系统级信号
- avg_read_bandwidth_mbs: 60.418
- avg_cpu_iowait_percent: 0.079
- psi_io_some_delta_us: 92584
- psi_io_full_delta_us: 84378
- process_read_bytes_delta: 7920295936

## 查询级效率指标
- avg_duplicate_vector_read_ratio: 0.118684
- avg_distance_eval_ratio: 0.881316
- avg_final_result_ratio: 0.008018
- avg_requested_read_bytes: 792029.594

## 备注
- 当缺少 query-csv 时，报告会退化为日志与系统级摘要。
- 相关性分析基于同一 monotonic ns 时间窗重叠 join。
