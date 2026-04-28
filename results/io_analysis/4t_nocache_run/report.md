# SPANN 搜索 I/O 性能分析报告

## 输入概览
- query_rows: 10000
- disk_rows: 1110
- process_rows: 1109
- cpu_rows: 1110
- psi_rows: 1110
- log_qps: 90.860
- log_total_runtime_s: 0.000
- log_search_stage_s: 0.000
- log_read_mb: 0.000

## 瓶颈分析
- 结论: **存在混合瓶颈，I/O 不是唯一主导**
- avg_latency_ms: 43.979
- p95_latency_ms: 48.951
- p99_latency_ms: 53.369
- avg_io_wait_ms: 0.000
- io_wait_ratio: 0.000
- corr(latency, requested_bytes): 0.092
- corr(latency, avg_queue_depth): -0.092

## 系统级信号
- avg_read_bandwidth_mbs: 67.514
- avg_cpu_iowait_percent: 0.069
- psi_io_some_delta_us: 99712
- psi_io_full_delta_us: 97412
- process_read_bytes_delta: 7920295936

## 查询级效率指标
- avg_duplicate_vector_read_ratio: 0.118684
- avg_distance_eval_ratio: 0.881316
- avg_final_result_ratio: 0.008018
- avg_requested_read_bytes: 792029.594

## 备注
- 当缺少 query-csv 时，报告会退化为日志与系统级摘要。
- 相关性分析基于同一 monotonic ns 时间窗重叠 join。
