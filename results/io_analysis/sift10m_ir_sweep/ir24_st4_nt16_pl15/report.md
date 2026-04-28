# SPANN 搜索 I/O 性能分析报告

## 输入概览
- query_rows: 10000
- disk_rows: 1291
- process_rows: 1290
- cpu_rows: 1291
- psi_rows: 1291
- log_qps: 86.580
- log_total_runtime_s: 0.000
- log_search_stage_s: 0.000
- log_read_mb: 0.000

## 瓶颈分析
- 结论: **存在混合瓶颈，I/O 不是唯一主导**
- avg_latency_ms: 46.159
- p95_latency_ms: 50.987
- p99_latency_ms: 55.054
- avg_io_wait_ms: 0.000
- io_wait_ratio: 0.000
- corr(latency, requested_bytes): 0.075
- corr(latency, avg_queue_depth): -0.123

## 系统级信号
- avg_read_bandwidth_mbs: 46.961
- avg_cpu_iowait_percent: 0.092
- psi_io_some_delta_us: 374227
- psi_io_full_delta_us: 365742
- process_read_bytes_delta: 6410731520

## 查询级效率指标
- avg_duplicate_vector_read_ratio: 0.075407
- avg_distance_eval_ratio: 0.924593
- avg_final_result_ratio: 0.009180
- avg_requested_read_bytes: 641073.152

## 备注
- 当缺少 query-csv 时，报告会退化为日志与系统级摘要。
- 相关性分析基于同一 monotonic ns 时间窗重叠 join。
