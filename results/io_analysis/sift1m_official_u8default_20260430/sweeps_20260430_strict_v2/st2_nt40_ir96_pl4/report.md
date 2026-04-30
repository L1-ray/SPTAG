# SPANN 搜索 I/O 性能分析报告

## 输入概览
- query_rows: 10000
- disk_rows: 62
- process_rows: 61
- cpu_rows: 62
- psi_rows: 62
- log_qps: 1941.750
- log_total_runtime_s: 0.000
- log_search_stage_s: 0.000
- log_read_mb: 0.000

## 瓶颈分析
- 结论: **读取放大显著，且与延迟相关**
- avg_latency_ms: 1.030
- p95_latency_ms: 1.214
- p99_latency_ms: 1.278
- avg_io_wait_ms: 0.000
- io_wait_ratio: 0.000
- corr(latency, requested_bytes): 0.807
- corr(latency, avg_queue_depth): -0.052

## 系统级信号
- avg_read_bandwidth_mbs: 1124.239
- avg_cpu_iowait_percent: 9.878
- psi_io_some_delta_us: 3239688
- psi_io_full_delta_us: 3105406
- process_read_bytes_delta: 7344812032

## 查询级效率指标
- avg_duplicate_vector_read_ratio: 0.194313
- avg_distance_eval_ratio: 0.805687
- avg_final_result_ratio: 0.002553
- avg_requested_read_bytes: 727320.166

## 备注
- 当缺少 query-csv 时，报告会退化为日志与系统级摘要。
- 相关性分析基于同一 monotonic ns 时间窗重叠 join。
