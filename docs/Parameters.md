## **Parameters**

## **Configuration File Format**

配置文件使用 INI 格式，有以下注意事项：

### 注释语法

**重要**：只有 `;`（分号）支持作为注释符。`#` 和 `//` 不是有效注释，会导致静默解析失败。

```ini
; 正确 - 分号注释
; EnablePageCache=true

# 错误 - 井号会被当作参数名解析，导致失败
# This will break parsing

// 错误 - 双斜杠同样不支持
// This will also break parsing
```

使用错误注释符会导致：
- 该行解析失败
- 后续参数可能被跳过
- 错误是**静默的**，不会报错

### Section 和参数名

- Section 和参数名**不区分大小写**（内部会转换为小写）
- Section 格式：`[SectionName]`
- 参数格式：`ParameterName=Value`

---

> Common Parameters

|  ParametersName | type  |  default | definition|
|---|---|---|---|
| Samples | int | 1000 | how many points will be sampled to do tree node split |
|TPTNumber | int | 32 | number of TPT trees to help with graph construction |
|TPTLeafSize | int | 2000 | TPT tree leaf size |
NeighborhoodSize | int | 32 | number of neighbors each node has in the neighborhood graph |
|GraphNeighborhoodScale | int | 2 | number of neighborhood size scale in the build stage |
|CEF | int | 1000 | number of results used to construct RNG | 
|MaxCheckForRefineGraph| int | 10000 | how many nodes each node will visit during graph refine in the build stage | 
|NumberOfThreads | int | 1 | number of threads to uses for speed up the build |
|DistCalcMethod | string | Cosine | choose from Cosine and L2 |
|MaxCheck | int | 8192 | how many nodes will be visited for a query in the search stage

> BKT

|  ParametersName | type  |  default | definition|
|---|---|---|---|
| BKTNumber | int | 1 | number of BKT trees |
| BKTKMeansK | int | 32 | how many childs each tree node has |

> KDT

|  ParametersName | type  |  default | definition|
|---|---|---|---|
| KDTNumber | int | 1 | number of KDT trees |

> Parameters that will affect the index size
* NeighborhoodSize
* BKTNumber
* KDTNumber

> Parameters that will affect the index build time
* NumberOfThreads
* TPTNumber
* TPTLeafSize
* GraphNeighborhoodScale
* CEF
* MaxCheckForRefineGraph

> Parameters that will affect the index quality
* TPTNumber
* TPTLeafSize
* GraphNeighborhoodScale
* CEF
* MaxCheckForRefineGraph
* NeighborhoodSize
* KDTNumber

> Parameters that will affect search latency and recall
* MaxCheck

## **NNI for parameters tuning**

Prepare vector data file **data.tsv**, query data file **query.tsv**, and truth file **truth.txt** following the format introduced in the [Get Started](GettingStart.md). 

Install [microsoft nni](https://github.com/microsoft/nni) and write the following python code (nni_sptag.py), parameter search space configuration (search_space.json) and nni environment configuration (config.yml).

> nni_sptag.py

```Python
import nni
import os

vector_dimension = 10
vector_type = 'Float'
index_algo = 'BKT'
threads = 32
k = 3

def main():
    para = nni.get_next_parameter()
    cmd_build = "./indexbuilder -d %d -v %s -i data.tsv -o index -a %s -t %d " % (vector_dimension, vector_type, index_algo, threads)
    for p, v in para.items():
        cmd_build += "Index." + p + "=" + str(v)
    cmd_test = "./indexsearcher index Index.QueryFile=query.tsv Index.TruthFile=truth.txt Index.K=%d" % (k)
    os.system(cmd_build)
    os.system(cmd_test + " > out.txt")
    with open("out.txt", "r") as fd:
        lines = fd.readlines()
        res = lines[-2]
        segs = res.split()
        recall = float(segs[-2])
        avg_latency = float(segs[-5])
    score = recall
    nni.report_final_result(score)

if __name__ == '__main__':
    main()
```
> search_space.json

```json
{
        "BKTKmeansK": {"_type": "choice", "_value": [2, 4, 8, 16, 32]},
        "GraphNeighborhoodScale": {"_type": "choice", "_value": [2, 4, 8, 16, 32]}
}

```

> config.yml

```yaml
authorName: default

experimentName: example_sptag

trialConcurrency: 1

maxExecDuration: 1h

maxTrialNum: 10

#choice: local, remote, pai

trainingServicePlatform: local

searchSpacePath: search_space.json

#choice: true, false

useAnnotation: false

tuner:

  #choice: TPE, Random, Anneal, Evolution, BatchTuner, MetisTuner

  #SMAC (SMAC should be installed through nnictl)

  builtinTunerName: TPE

  classArgs:

    #choice: maximize, minimize

    optimize_mode: maximize

trial:

  command: python3 nni_sptag.py

  codeDir: .

  gpuNum: 0

```

Then start the tuning (tunning results can be found in the Web UI urls in the command output):
```bash
nnictl create --config config.yml
```

stop the tunning:
```bash
nnictl stop
```