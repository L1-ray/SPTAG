# SIFT1M Baseline vs Learned Policy 时间对比测试

测试日期: 2026-05-05 19:23:11

## 测试配置

- SearchThreadNum=8
- NumberOfThreads=16
- InternalResultNum=64
- 每次测试前清空缓存 (sync && echo 3 > /proc/sys/vm/drop_caches)

## 结果汇总

| 配置 | 耗时(秒) | QPS | Recall@10 |
|------|----------|-----|-----------|
| Baseline (B=64) | [2026-05-05 19:23:03] 开始测试: baseline
./scripts/run_sift1m_timing_test.sh: 行 37: ./Release/ssdserving: 没有那个文件或目录
[2026-05-05 19:23:07] 测试完成: baseline
耗时:  秒 |  |  |
| Learned A0 (B={32,40,48}, t=0.95) | [2026-05-05 19:23:07] 开始测试: learned_a0
./scripts/run_sift1m_timing_test.sh: 行 37: ./Release/ssdserving: 没有那个文件或目录
[2026-05-05 19:23:09] 测试完成: learned_a0
耗时:  秒 |  |  |
| Learned A3 (B={32,40,48,56}, 分阈值) | [2026-05-05 19:23:09] 开始测试: learned_a3
./scripts/run_sift1m_timing_test.sh: 行 37: ./Release/ssdserving: 没有那个文件或目录
[2026-05-05 19:23:11] 测试完成: learned_a3
耗时:  秒 |  |  |

## 耗时对比

- A0 vs Baseline: %
- A3 vs Baseline: %
