# SPANN 搜索 I/O 性能分析报告

## 输入概览
- query_rows: 10000
- disk_rows: 72
- process_rows: 71
- cpu_rows: 72
- psi_rows: 72
- log_qps: 1607.200
- log_total_runtime_s: 0.000
- log_search_stage_s: 0.000
- log_read_mb: 0.000

## 瓶颈分析
- 结论: **读取放大显著，且与延迟相关**
- avg_latency_ms: 1.244
- p95_latency_ms: 1.481
- p99_latency_ms: 1.557
- avg_io_wait_ms: 0.000
- io_wait_ratio: 0.000
- corr(latency, requested_bytes): 0.824
- corr(latency, avg_queue_depth): 0.010

## 系统级信号
- avg_read_bandwidth_mbs: 1283.921
- avg_cpu_iowait_percent: 9.870
- psi_io_some_delta_us: 3708020
- psi_io_full_delta_us: 3519169
- process_read_bytes_delta: 9740738560

## 查询级效率指标
- avg_duplicate_vector_read_ratio: 0.218413
- avg_distance_eval_ratio: 0.781587
- avg_final_result_ratio: 0.001914
- avg_requested_read_bytes: 968433.254

## 备注
- 当缺少 query-csv 时，报告会退化为日志与系统级摘要。
- 相关性分析基于同一 monotonic ns 时间窗重叠 join。
