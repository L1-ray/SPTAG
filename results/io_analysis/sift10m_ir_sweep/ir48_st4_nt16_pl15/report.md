# SPANN 搜索 I/O 性能分析报告

## 输入概览
- query_rows: 10000
- disk_rows: 1309
- process_rows: 1308
- cpu_rows: 1309
- psi_rows: 1309
- log_qps: 84.880
- log_total_runtime_s: 0.000
- log_search_stage_s: 0.000
- log_read_mb: 0.000

## 瓶颈分析
- 结论: **存在混合瓶颈，I/O 不是唯一主导**
- avg_latency_ms: 47.087
- p95_latency_ms: 60.048
- p99_latency_ms: 69.016
- avg_io_wait_ms: 0.000
- io_wait_ratio: 0.000
- corr(latency, requested_bytes): 0.120
- corr(latency, avg_queue_depth): 0.069

## 系统级信号
- avg_read_bandwidth_mbs: 92.121
- avg_cpu_iowait_percent: 0.458
- psi_io_some_delta_us: 2107971
- psi_io_full_delta_us: 2067411
- process_read_bytes_delta: 12744114176

## 查询级效率指标
- avg_duplicate_vector_read_ratio: 0.110247
- avg_distance_eval_ratio: 0.889753
- avg_final_result_ratio: 0.004596
- avg_requested_read_bytes: 1274411.418

## 备注
- 当缺少 query-csv 时，报告会退化为日志与系统级摘要。
- 相关性分析基于同一 monotonic ns 时间窗重叠 join。
