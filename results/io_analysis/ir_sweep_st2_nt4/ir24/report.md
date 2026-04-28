# SPANN 搜索 I/O 性能分析报告

## 输入概览
- query_rows: 10000
- disk_rows: 1559
- process_rows: 1558
- cpu_rows: 1559
- psi_rows: 1559
- log_qps: 64.360
- log_total_runtime_s: 0.000
- log_search_stage_s: 0.000
- log_read_mb: 0.000

## 瓶颈分析
- 结论: **存在混合瓶颈，I/O 不是唯一主导**
- avg_latency_ms: 31.066
- p95_latency_ms: 44.888
- p99_latency_ms: 55.948
- avg_io_wait_ms: 0.000
- io_wait_ratio: 0.000
- corr(latency, requested_bytes): 0.019
- corr(latency, avg_queue_depth): -0.526

## 系统级信号
- avg_read_bandwidth_mbs: 36.158
- avg_cpu_iowait_percent: 0.349
- psi_io_some_delta_us: 382427
- psi_io_full_delta_us: 340251
- process_read_bytes_delta: 5953445888

## 查询级效率指标
- avg_duplicate_vector_read_ratio: 0.100775
- avg_distance_eval_ratio: 0.899225
- avg_final_result_ratio: 0.010461
- avg_requested_read_bytes: 595344.589

## 备注
- 当缺少 query-csv 时，报告会退化为日志与系统级摘要。
- 相关性分析基于同一 monotonic ns 时间窗重叠 join。
