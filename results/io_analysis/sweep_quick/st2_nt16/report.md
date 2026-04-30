# SPANN 搜索 I/O 性能分析报告

## 输入概览
- query_rows: 10000
- disk_rows: 606
- process_rows: 605
- cpu_rows: 606
- psi_rows: 606
- log_qps: 167.750
- log_total_runtime_s: 0.000
- log_search_stage_s: 0.000
- log_read_mb: 0.000

## 瓶颈分析
- 结论: **存在混合瓶颈，I/O 不是唯一主导**
- avg_latency_ms: 11.922
- p95_latency_ms: 13.657
- p99_latency_ms: 14.790
- avg_io_wait_ms: 0.000
- io_wait_ratio: 0.000
- corr(latency, requested_bytes): 0.148
- corr(latency, avg_queue_depth): -0.312

## 系统级信号
- avg_read_bandwidth_mbs: 132.412
- avg_cpu_iowait_percent: 15.367
- psi_io_some_delta_us: 33396907
- psi_io_full_delta_us: 32217476
- process_read_bytes_delta: 8495001600

## 查询级效率指标
- avg_duplicate_vector_read_ratio: 0.118684
- avg_distance_eval_ratio: 0.881316
- avg_final_result_ratio: 0.008018
- avg_requested_read_bytes: 792029.594

## 备注
- 当缺少 query-csv 时，报告会退化为日志与系统级摘要。
- 相关性分析基于同一 monotonic ns 时间窗重叠 join。
