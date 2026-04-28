# SPANN 搜索 I/O 性能分析报告

## 输入概览
- query_rows: 10000
- disk_rows: 1284
- process_rows: 1283
- cpu_rows: 1284
- psi_rows: 1284
- log_qps: 86.600
- log_total_runtime_s: 0.000
- log_search_stage_s: 0.000
- log_read_mb: 0.000

## 瓶颈分析
- 结论: **存在混合瓶颈，I/O 不是唯一主导**
- avg_latency_ms: 46.144
- p95_latency_ms: 51.558
- p99_latency_ms: 55.830
- avg_io_wait_ms: 0.000
- io_wait_ratio: 0.000
- corr(latency, requested_bytes): 0.111
- corr(latency, avg_queue_depth): -0.099

## 系统级信号
- avg_read_bandwidth_mbs: 62.817
- avg_cpu_iowait_percent: 0.049
- psi_io_some_delta_us: 81930
- psi_io_full_delta_us: 79565
- process_read_bytes_delta: 8523010048

## 查询级效率指标
- avg_duplicate_vector_read_ratio: 0.089056
- avg_distance_eval_ratio: 0.910944
- avg_final_result_ratio: 0.006889
- avg_requested_read_bytes: 852301.005

## 备注
- 当缺少 query-csv 时，报告会退化为日志与系统级摘要。
- 相关性分析基于同一 monotonic ns 时间窗重叠 join。
