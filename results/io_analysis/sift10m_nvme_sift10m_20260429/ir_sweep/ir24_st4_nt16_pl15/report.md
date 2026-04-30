# SPANN 搜索 I/O 性能分析报告

## 输入概览
- query_rows: 10000
- disk_rows: 820
- process_rows: 819
- cpu_rows: 820
- psi_rows: 820
- log_qps: 147.380
- log_total_runtime_s: 0.000
- log_search_stage_s: 0.000
- log_read_mb: 0.000

## 瓶颈分析
- 结论: **存在混合瓶颈，I/O 不是唯一主导**
- avg_latency_ms: 27.096
- p95_latency_ms: 32.166
- p99_latency_ms: 34.613
- avg_io_wait_ms: 0.000
- io_wait_ratio: 0.000
- corr(latency, requested_bytes): 0.047
- corr(latency, avg_queue_depth): -0.010

## 系统级信号
- avg_read_bandwidth_mbs: 147.250
- avg_cpu_iowait_percent: 4.613
- psi_io_some_delta_us: 13404382
- psi_io_full_delta_us: 11664656
- process_read_bytes_delta: 12773965824

## 查询级效率指标
- avg_duplicate_vector_read_ratio: 0.075407
- avg_distance_eval_ratio: 0.924593
- avg_final_result_ratio: 0.009180
- avg_requested_read_bytes: 641073.152

## 备注
- 当缺少 query-csv 时，报告会退化为日志与系统级摘要。
- 相关性分析基于同一 monotonic ns 时间窗重叠 join。
