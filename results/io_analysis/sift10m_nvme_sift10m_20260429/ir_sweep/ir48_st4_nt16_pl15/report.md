# SPANN 搜索 I/O 性能分析报告

## 输入概览
- query_rows: 10000
- disk_rows: 768
- process_rows: 767
- cpu_rows: 768
- psi_rows: 768
- log_qps: 158.480
- log_total_runtime_s: 0.000
- log_search_stage_s: 0.000
- log_read_mb: 0.000

## 瓶颈分析
- 结论: **存在混合瓶颈，I/O 不是唯一主导**
- avg_latency_ms: 25.196
- p95_latency_ms: 28.519
- p99_latency_ms: 30.630
- avg_io_wait_ms: 0.000
- io_wait_ratio: 0.000
- corr(latency, requested_bytes): 0.096
- corr(latency, avg_queue_depth): -0.006

## 系统级信号
- avg_read_bandwidth_mbs: 234.178
- avg_cpu_iowait_percent: 4.887
- psi_io_some_delta_us: 14180840
- psi_io_full_delta_us: 12797035
- process_read_bytes_delta: 19045347328

## 查询级效率指标
- avg_duplicate_vector_read_ratio: 0.110247
- avg_distance_eval_ratio: 0.889753
- avg_final_result_ratio: 0.004596
- avg_requested_read_bytes: 1274411.418

## 备注
- 当缺少 query-csv 时，报告会退化为日志与系统级摘要。
- 相关性分析基于同一 monotonic ns 时间窗重叠 join。
