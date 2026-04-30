# SPANN 搜索 I/O 性能分析报告

## 输入概览
- query_rows: 10000
- disk_rows: 653
- process_rows: 652
- cpu_rows: 653
- psi_rows: 653
- log_qps: 153.740
- log_total_runtime_s: 0.000
- log_search_stage_s: 0.000
- log_read_mb: 0.000

## 瓶颈分析
- 结论: **存在混合瓶颈，I/O 不是唯一主导**
- avg_latency_ms: 13.005
- p95_latency_ms: 15.839
- p99_latency_ms: 37.503
- avg_io_wait_ms: 0.000
- io_wait_ratio: 0.000
- corr(latency, requested_bytes): 0.129
- corr(latency, avg_queue_depth): 0.036

## 系统级信号
- avg_read_bandwidth_mbs: 0.178
- avg_cpu_iowait_percent: 9.040
- psi_io_some_delta_us: 23690987
- psi_io_full_delta_us: 21239712
- process_read_bytes_delta: 11857920

## 查询级效率指标
- avg_duplicate_vector_read_ratio: 0.159716
- avg_distance_eval_ratio: 0.840284
- avg_final_result_ratio: 0.003891
- avg_requested_read_bytes: 480232.653

## 备注
- 当缺少 query-csv 时，报告会退化为日志与系统级摘要。
- 相关性分析基于同一 monotonic ns 时间窗重叠 join。
