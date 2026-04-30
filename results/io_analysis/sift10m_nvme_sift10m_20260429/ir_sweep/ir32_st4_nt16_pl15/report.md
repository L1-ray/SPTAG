# SPANN 搜索 I/O 性能分析报告

## 输入概览
- query_rows: 10000
- disk_rows: 787
- process_rows: 786
- cpu_rows: 787
- psi_rows: 787
- log_qps: 154.700
- log_total_runtime_s: 0.000
- log_search_stage_s: 0.000
- log_read_mb: 0.000

## 瓶颈分析
- 结论: **存在混合瓶颈，I/O 不是唯一主导**
- avg_latency_ms: 25.813
- p95_latency_ms: 30.208
- p99_latency_ms: 33.477
- avg_io_wait_ms: 0.000
- io_wait_ratio: 0.000
- corr(latency, requested_bytes): 0.061
- corr(latency, avg_queue_depth): 0.011

## 系统级信号
- avg_read_bandwidth_mbs: 178.884
- avg_cpu_iowait_percent: 4.704
- psi_io_some_delta_us: 13922604
- psi_io_full_delta_us: 12475328
- process_read_bytes_delta: 14899609600

## 查询级效率指标
- avg_duplicate_vector_read_ratio: 0.089056
- avg_distance_eval_ratio: 0.910944
- avg_final_result_ratio: 0.006889
- avg_requested_read_bytes: 852301.005

## 备注
- 当缺少 query-csv 时，报告会退化为日志与系统级摘要。
- 相关性分析基于同一 monotonic ns 时间窗重叠 join。
