# **复现所有实验结果（结果已复现）**

在这个文件夹中，我们提供了用于复现论文中图表的脚本。需要一台 Standard_L16s_v3 实例来复现所有结果。

每个脚本的名称对应我们论文中每个图表的编号。由于此目录中的某些脚本需要很长的计算时间（如我们在下方标注的那样），我们强烈建议您创建一个 tmux 会话，以避免由于网络不稳定导致脚本中断（在 tmux 会话里开启的任务是运行在服务器内存中的。即使你关掉本地电脑或断网，tmux 会话依然在服务器后台运行）。

## **数据集和基准测试**
> **对于 Artifact Evaluation（AE 评估），我们强烈建议审稿人使用提供的 Lsv3 实例上 `~/data/` 文件夹中预先下载和构建好的数据集和基准测试。因为提供的索引和数据集需要大量的计算资源，在一台 80 核的机器上生成这些数据大约需要两周时间（我们在离线状态下构建了这些数据）。**

有关如何生成数据，您可以参考 [./Script_AE/iniFile/README.md](./Script_AE_iniFile)。

## **附加设置**
以下是评估的附加设置

### **如何检查当前状态**
> 如果在执行以下命令后看不到 `/dev/nvme0n1`，这意味着该磁盘已被 SPDK 接管。
> 使用 SPDK 会将被磁盘绑定到 SPDK，且 "lsblk" 命令后不会显示 `/dev/nvme0n1`。
```bash
lsblk
```

> 如果设备已经绑定到 SPDK，执行以下命令可以重置：
```bash
sudo ./SPFresh/ThirdParty/spdk/scripts/setup.sh reset
```

### **SPFresh & SPANN+**
> 由于我们使用 SPDK 构建存储，我们需要将 PCI 设备绑定到 SPDK
```bash
sudo nvme format /dev/nvme0n1
sudo ./SPFresh/ThirdParty/spdk/scripts/setup.sh
cp bdev.json /home/sosp/SPFresh/
```

> **补充说明：关于 SPANN、SPANN+ 与 SPFresh 的区别**
> - **SPANN**：原生静态索引方案，不支持高效增量更新，面临数据更新需重写大块索引或全量重建的缺陷。
> - **SPANN+**：作为实验对比的增强版基线，支持了**简单的原地追加（In-place Append）**，但禁用了页分裂（Split）、路由重分配（Reassign）和垃圾回收（GC）。其倒排页会随着数据追加无限膨胀，严重拖慢检索速度。
> - **SPFresh**：本方案的完整实现。不仅限制了倒排页面的大小（例如 `PostingPageLimit=4`），一旦页面写满便会触发 KMeans 分裂，并将周围的向量重新动态路由分配，同时辅以异步 GC 清理碎片，从而在此动态演化过程中维持优异的检索性能。


### **DiskANN**
> DiskANN 基线使用内核提供的文件系统来维护其磁盘上的文件，因此如果我们想运行 DiskANN，我们需要将磁盘从 SPDK 释放出来。
```bash
sudo ./SPFresh/ThirdParty/spdk/scripts/setup.sh reset
```

> 为评估准备 DiskANN
```bash
sudo mkfs.ext4 /dev/nvme0n1
sudo mount /dev/nvme0n1 /home/sosp/testbed
sudo chmod 777 /home/sosp/testbed
```

> 如果您想将评估从 DiskANN 切换到 SPFresh，必须首先卸载文件系统，然后遵循上面的 SPDK 命令。
```bash
sudo umount /home/sosp/testbed
sudo nvme format /dev/nvme0n1
sudo ./SPFresh/ThirdParty/spdk/scripts/setup.sh
```

## **开始运行**

> 运行的所有文件已经设置在用于 AE 的 Azure VM 的相关文件夹中。

### **动机实验 (图 1)**
> 大约需要 22 分钟
```bash
bash motivation.sh
```
> 绘制结果
```bash
bash plot_motivation_result.sh
```

### **整体性能 (图 6)**
> 复现此图大约需要 6 天
#### **SPFresh**
> 大约需要 43 小时 50 分钟
```bash
bash overall_spacev_spfresh.sh
```
#### **SPANN+**
> 大约需要 43 小时 30 分钟
```bash
bash overall_spacev_spann.sh
```
#### **DiskANN**
> 运行前，将 DiskANN 索引移动到磁盘上（如果已被 SPDK 绑定，请先重置并执行 mkfs 挂载，参考上述说明）
```bash
cp -r /home/sosp/data/store_diskann_100m /home/sosp/testbed
```
> 大约需要 35 小时 44 分钟
```bash
bash overall_spacev_diskann.sh
```
#### **绘制基线结果**
```bash
bash plot_overall_result.sh
```

### **磁盘 IOPS 限制 (图 7)**
> 大约需要 5 分钟
```bash
bash iops_limitation.sh
```
> 绘制结果
```bash
bash plot_iops_result.sh
```

### **压力测试 (图 8)**
> 大约需要 35 小时 26 分钟
```bash
bash stress_spfresh.sh
```
> 绘制结果
```bash
bash plot_stress_result.sh
```

### **数据分布偏移微基准测试 (图 9)**
> 大约需要 1 小时 10 分钟
```bash
bash data_shifting.sh
```
> 绘制结果
```bash
bash plot_shifting_result.sh
```
### **参数研究：重分配范围 (图 10)**
> 大约需要 1 小时 30 分钟
```bash
bash parameter_study_range.sh
```
> 绘制结果
```bash
bash plot_range_result.sh
```
### **前台可扩展性 (图 11)**
> 大约需要 11 分钟
```bash
bash parameter_study_balance.sh
```
> 绘制结果
```bash
bash plot_balance_result.sh
```
