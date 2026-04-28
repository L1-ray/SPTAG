# SPANN SIFT1M SATA SSD 瓶颈分析

## 背景

- 数据集：SIFT1M，100万个向量，128维度。
- 存储：SATA SSD。
- 索引：位于 `/media/ray/1tb/sift1m/spann_index` 的 SPANN 磁盘索引。
- 测试模式：启用 BATCH_READ。
- 结果路径：`results/io_analysis/sweep_quick`。

## 主要发现

目前的快速扫描（quick sweep）主要受限于通过 SPANN 批量读取路径（batch-read path）从 SATA SSD 进行的小文件的随机读取。它主要不是受 `SearchThreadNum` 限制，一旦 `NumberOfThreads >= SearchThreadNum`，增加 `NumberOfThreads` 并没有实质性地提高吞吐量。

对于 SIFT1M 这个数据集，此基准测试设置并不能很好地代表 SPANN 旨在发挥的优势。数据集相对较小，导致基于磁盘的 SPANN 引入的读放大相对于有用的结果大小来说非常高。

## 证据

- SPANN 索引目录大约为 `3.2 GB`；`SPTAGFullList.bin` 大约为 `3.0 GB`。
- 每个查询平均读取约 `792 KB`。
- 每个查询接触约 `31.7` 个倒排列表并读取约 `193` 个 4KB 页面。
- 每个查询评估约 `1248` 个向量，但仅返回 `10` 个结果。
- `final_result_ratio` 约为 `0.008`，这意味着大多数读取和评估的数据不会直接成为最终输出。
- 已完成的扫描组合的 QPS 保持在 `78-82` 左右。
- 磁盘读取带宽保持在 `58-61 MB/s` 左右。
- `80 QPS * 0.792 MB/query ~= 63 MB/s`，这与观察到的磁盘读取带宽相匹配。

这指向了 4KB 随机读取瓶颈：

```text
60 MB/s / 4 KB ~= 15K 页面读取/秒
```

对于这种访问模式来说，这是一个合理的 SATA SSD 随机读取限制。

## 线程观察

- 将 `SearchThreadNum` 从 `2` 增加到 `8` 并没有显著提高 QPS。
- 查询延迟随并发性大致成正比上升，而吞吐量保持平稳。
- 当 `SearchThreadNum=16` 时，CPU 空闲率降得非常低并且 QPS 可能会降低，这表明在 I/O 限制之上存在额外的 CPU 冲突。
- 当出现 `SearchThreadNum > NumberOfThreads` 这种无效组合时，会产生不完整的运行，在大约 `Sent 0.00%...` 时停止。

有效的基准测试组合应保持：

```text
NumberOfThreads >= SearchThreadNum
```

## 测量说明

- 在 BATCH_READ 模式下，预期 `io_wait_ms` 保持在 `0` 附近。
- 相关的 I/O 计时字段是 `batch_read_total_ms`。
- 系统级磁盘读取带宽和每个查询请求的字节数在判断此瓶颈时更为可靠。
- `Head Latency` 更适合用于分析单 query 延迟构成，不能单独用来判断系统吞吐瓶颈。

## SATA SSD 随机读解读

正常 SATA SSD 的 4KB 随机读需要区分队列深度：

- QD1 单队列深度通常约为 `8K-15K IOPS`，对应 `32-60 MB/s`。
- QD32 高队列深度下，较好的 SATA SSD 可达到 `70K-100K IOPS`，对应 `280-400 MB/s` 的 4KB 随机读等效带宽。

当前测试使用的 Samsung 870 QVO 标称 4KB 随机读为 `QD1 up to 11K IOPS`、`QD32 up to 98K IOPS`。本次 SPANN 测试约为：

```text
80 QPS * 193 pages/query ~= 15.4K 4KB page reads/s
15.4K * 4KB ~= 60 MB/s
```

因此，当前结果没有达到 SATA SSD 在高队列深度下的标称随机读上限，但已经接近或超过 QD1 随机读量级。更准确地说，当前达到的是 SPANN 现有访问模式下的有效上限，而不是硬盘物理极限。

这说明问题不应简单归因于“SSD 太慢”。更可能的原因是：SIFT1M + 当前 SPANN 配置产生了较大的 4KB/page 级随机读放大，但应用层的查询流程、batch read 路径、AIO 提交方式和队列深度没有把 SATA SSD 的高队列深度随机读能力充分打满。

## Head Latency 解读

快速压测中，`Head Latency` 是单 query 总延迟的最大组成部分：

```text
SearchThreadNum=2:  Head 占约 64%
SearchThreadNum=4:  Head 占约 72%
SearchThreadNum=8:  Head 占约 75%
SearchThreadNum=16: Head 占约 78%
```

这说明 Head 阶段对单 query 延迟很重要，但不能直接得出“系统吞吐瓶颈是 Head Search”。原因是：

- `SearchThreadNum` 从 `2` 增加到 `8` 时，QPS 基本保持在 `80` 左右，没有随并发提升。
- 磁盘读带宽稳定在 `58-61 MB/s`，与 `QPS * 每查询读取量` 基本吻合。
- 并发提高后，query 在系统内的排队、CPU 争用和调度成本会被计入阶段延迟，使 Head Latency 膨胀。

因此更准确的结论是：`Head Latency` 是单 query 延迟构成中的最大部分；但从吞吐角度看，当前 QPS 主要受每 query 的磁盘 page 读取量和 batch-read 访问模式限制。

## InternalResultNum 扫频补充

`InternalResultNum` 扫频结果显示，降低 IR 可以近似线性地减少每 query 的 Posting List 数和读取量：

```text
IR=32: ~792 KB/query, ~31.7 posting lists, Recall@10 0.942
IR=16: ~398 KB/query, ~15.9 posting lists, Recall@10 0.873
IR=8:  ~200 KB/query, ~7.9 posting lists,  Recall@10 0.771
```

但 QPS 只从 `62.69` 提升到最高约 `69.43`，提升有限。原因不是简单的“SSD 性能同比衰减”，而是：

- IR 降低后，每 query 可并行提交的 batch read 也变少。
- 应用层提供给 SSD 的 I/O 队列深度下降。
- 观测读带宽从 `46.9 MB/s` 降到 `13.0 MB/s`。
- Head 搜索工作量基本不变，`Head Latency` 仍在 `20-23 ms` 左右。

所以单独降低 `InternalResultNum` 不是有效优化手段：它显著牺牲 Recall，但 QPS 只小幅提升。更合理的方向是同时调整 `InternalResultNum`、`SearchPostingPageLimit`、`MaxCheck` 等参数，观察 Recall/QPS 的整体折中。

## 结论

主要问题是，对于一个小数据集，此 SIFT1M SPANN 配置每次查询读取了太多的小磁盘页面。当前达到的是 SPANN 现有访问模式下的有效吞吐上限，而不是 SATA SSD 的物理随机读上限。因此，单纯添加更多搜索线程主要只会增加延迟，而不会明显提高 QPS。

对于此数据集，更好的后续步骤是：

- 使用 `SearchThreadNum=2` 或 `4`，并将 `NumberOfThreads >= SearchThreadNum` 作为基线。
- 同时调整 `SearchPostingPageLimit`、`InternalResultNum`、`MaxCheck` 并测量召回率/QPS 之间的权衡，避免只单独降低 IR。
- 与 SIFT1M 的内存索引进行基准对比。
- 如果目标是评估 SPANN 的磁盘索引可扩展性，请使用更大型的数据集或更快的 NVMe 存储。
