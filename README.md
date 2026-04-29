# SPTAG: A library for fast approximate nearest neighbor search

[![MIT licensed](https://img.shields.io/badge/license-MIT-yellow.svg)](https://github.com/Microsoft/SPTAG/blob/master/LICENSE)
[![Build status](https://sysdnn.visualstudio.com/SPTAG/_apis/build/status/SPTAG-GITHUB)](https://sysdnn.visualstudio.com/SPTAG/_build/latest?definitionId=2)

## **SPTAG**
 SPTAG (Space Partition Tree And Graph) is a library for large scale vector approximate nearest neighbor search scenario released by [Microsoft Research (MSR)](https://www.msra.cn/) and [Microsoft Bing](http://bing.com).

 <p align="center">
 <img src="docs/img/sptag.png" alt="architecture" width="500"/>
 </p>

## What's NEW
* Result Iterator with Relaxed Monotonicity Signal Support
* New Research Paper [SPFresh: Incremental In-Place Update for Billion-Scale Vector Search](https://dl.acm.org/doi/10.1145/3600006.3613166) - _published in SOSP 2023_
* New Research Paper [VBASE: Unifying Online Vector Similarity Search and Relational Queries via Relaxed Monotonicity](https://www.usenix.org/system/files/osdi23-zhang-qianxi_1.pdf) - _published in OSDI 2023_

## **Introduction**

This library assumes that the samples are represented as vectors and that the vectors can be compared by L2 distances or cosine distances.
Vectors returned for a query vector are the vectors that have smallest L2 distance or cosine distances with the query vector.

SPTAG provides two methods: kd-tree and relative neighborhood graph (SPTAG-KDT)
and balanced k-means tree and relative neighborhood graph (SPTAG-BKT).
SPTAG-KDT is advantageous in index building cost, and SPTAG-BKT is advantageous in search accuracy in very high-dimensional data.



## **How it works**

SPTAG is inspired by the NGS approach [[WangL12](#References)]. It contains two basic modules: index builder and searcher.
The RNG is built on the k-nearest neighborhood graph [[WangWZTG12](#References), [WangWJLZZH14](#References)]
for boosting the connectivity. Balanced k-means trees are used to replace kd-trees to avoid the inaccurate distance bound estimation in kd-trees for very high-dimensional vectors.
The search begins with the search in the space partition trees for
finding several seeds to start the search in the RNG.
The searches in the trees and the graph are iteratively conducted.

 ## **Highlights**
  * Fresh update: Support online vector deletion and insertion
  * Distributed serving: Search over multiple machines

 ## **Build**

### **Requirements**

| Dependency | Minimum Version | Required For |
|------------|-----------------|--------------|
| CMake | >= 3.12 | Build system |
| GCC (Linux) | >= 5.0 | Compiler |
| MSVC (Windows) | >= 14.0 (VS 2019) | Compiler |
| Boost | >= 1.67 | Core library |
| OpenMP | - | Parallelism (required) |
| Intel TBB | >= 2020 | Parallelism (optional, recommended) |
| SWIG | >= 4.0.2 | Python bindings (optional) |
| Python 3 | >= 3.6 | Python bindings (optional) |

### **Install Dependencies**

#### Ubuntu 24.04 / 22.04 (Recommended)
```bash
sudo apt update
sudo apt install -y build-essential cmake libboost-all-dev libtbb-dev libomp-dev swig python3-dev
```

#### Ubuntu 20.04
```bash
sudo apt update
sudo apt install -y build-essential cmake libboost-all-dev libtbb-dev libomp-dev swig python3-dev
```

#### CentOS / RHEL / Rocky Linux
```bash
sudo yum groupinstall -y "Development Tools"
sudo yum install -y cmake3 boost-devel tbb-devel libomp-devel swig python3-devel

# Create cmake symlink if needed
sudo ln -sf /usr/bin/cmake3 /usr/bin/cmake
```

#### Fedora
```bash
sudo dnf groupinstall -y "Development Tools"
sudo dnf install -y cmake boost-devel tbb-devel libomp-devel swig python3-devel
```

#### macOS (with Homebrew)
```bash
brew install cmake boost tbb swig python3 libomp
```

#### Windows (vcpkg)
```powershell
# Install vcpkg if not already installed
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
.\bootstrap-vcpkg.bat

# Install dependencies
.\vcpkg install boost tbb openmp
```

### **Quick Build (Recommended)**

For most users, the simplified build without SPDK/RocksDB dependencies:

#### Linux
```bash
mkdir build
cd build && cmake -DSPDK=OFF -DROCKSDB=OFF .. && make -j$(nproc)
```
Compiled binaries are placed in `Release/` directory.

#### Windows
```bash
mkdir build
cd build && cmake -A x64 -DSPDK=OFF -DROCKSDB=OFF ..
```
Open `SPTAGLib.sln` in Visual Studio 2019+ and build the `ALL_BUILD` project.
Binaries will be in `Release/` directory.

#### Docker
```bash
docker build -t sptag .
```
Binaries will be in `/app/Release/` inside the container.

### **Build Options (CMake)**

| Option | Default | Description |
|--------|---------|-------------|
| `GPU` | OFF | Enable GPU support |
| `ROCKSDB` | OFF | Enable RocksDB storage backend |
| `SPDK` | OFF | Enable SPDK storage backend |
| `TBB` | ON | Enable Intel TBB for parallelism |
| `URING` | OFF | Enable liburing/io_uring support (Linux) |
| `USE_ASAN` | OFF | Enable AddressSanitizer for debugging |

Example with custom options:
```bash
cmake -DSPDK=OFF -DROCKSDB=OFF -DTBB=ON -DURING=ON ..
```

### **Build Output**

After successful build, the `Release/` directory contains:

| Binary | Description |
|--------|-------------|
| `indexbuilder` | Build in-memory index (BKT/KDT) |
| `indexsearcher` | Search in-memory index |
| `aggregator` | Distributed search aggregator |
| `client` | Remote search client |
| `server` | Socket-based search service |
| `ssdserving` | SSD index build and search (SPANN) |
| `quantizer` | Train PQ/OPQ quantizers |
| `libSPTAGLib.so` | Shared library |
| `libSPTAGLibStatic.a` | Static library |
| `_SPTAG.so` | Python binding module |

### **Full Build (Advanced)**

For SPDK and RocksDB support, additional dependencies are required:

#### Compile SPDK (Linux only)
```bash
cd ThirdParty/spdk
./scripts/pkgdep.sh
CC=gcc-9 ./configure
CC=gcc-9 make -j
```

#### Compile isal-l_crypto (Linux only)
```bash
cd ThirdParty/isal-l_crypto
./autogen.sh
./configure
make -j
```

#### Build RocksDB
```bash
mkdir build && cd build
cmake -DUSE_RTTI=1 -DWITH_JEMALLOC=1 -DWITH_SNAPPY=1 \
      -DCMAKE_C_COMPILER=gcc-7 -DCMAKE_CXX_COMPILER=g++-7 \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-fPIC" ..
make -j
sudo make install
```

#### Build SPTAG with full dependencies
```bash
mkdir build
cd build && cmake -DSPDK=ON -DROCKSDB=ON .. && make -j$(nproc)
```

### **Fast Clone (Skip LFS)**

```bash
# Option 1: Environment variable
GIT_LFS_SKIP_SMUDGE=1 git clone --recurse-submodules https://github.com/microsoft/SPTAG

# Option 2: Git config
git config --global filter.lfs.smudge "git-lfs smudge --skip -- %f"
git config --global filter.lfs.process "git-lfs filter-process --skip"
git clone --recurse-submodules https://github.com/microsoft/SPTAG
```

### **Verify Build**

Run the test suite to verify the build:
```bash
# Run all tests
./Release/SPTAGTest

# Run specific test suite
./Release/SPTAGTest --run_test=TestSuiteName

# Verbose output
./Release/SPTAGTest --log_level=test_suite
```

For detailed Windows installation, see [Windows Installation Guide](docs/WindowsInstallation.md).

## **Tested Environments**

| OS | Compiler | CMake | Boost | Status |
|----|----------|-------|-------|--------|
| Ubuntu 24.04 LTS | GCC 13.3.0 | 3.28.3 | 1.83.0 | ✓ Tested |
| Ubuntu 22.04 LTS | GCC 11.x | 3.22+ | 1.74+ | ✓ Works |
| Ubuntu 20.04 LTS | GCC 9.x | 3.16+ | 1.71+ | ✓ Works |
| Windows 10/11 | VS 2019/2022 | 3.12+ | 1.67+ | ✓ Works |
| macOS 12+ | Clang 14+ | 3.12+ | 1.67+ | ✓ Works |

### **Usage**

The detailed usage can be found in [Get started](docs/GettingStart.md). There is also an end-to-end tutorial for building vector search online service using Python Wrapper in [Python Tutorial](docs/Tutorial.ipynb).
The detailed parameters tunning can be found in [Parameters](docs/Parameters.md).

## **References**
Please cite SPTAG in your publications if it helps your research:
```
@inproceedings{xu2023spfresh,
  title={SPFresh: Incremental In-Place Update for Billion-Scale Vector Search},
  author={Xu, Yuming and Liang, Hengyu and Li, Jin and Xu, Shuotao and Chen, Qi and Zhang, Qianxi and Li, Cheng and Yang, Ziyue and Yang, Fan and Yang, Yuqing and others},
  booktitle={Proceedings of the 29th Symposium on Operating Systems Principles},
  pages={545--561},
  year={2023}
}

@inproceedings{zhang2023vbase,
  title={$\{$VBASE$\}$: Unifying Online Vector Similarity Search and Relational Queries via Relaxed Monotonicity},
  author={Zhang, Qianxi and Xu, Shuotao and Chen, Qi and Sui, Guoxin and Xie, Jiadong and Cai, Zhizhen and Chen, Yaoqi and He, Yinxuan and Yang, Yuqing and Yang, Fan and others},
  booktitle={17th USENIX Symposium on Operating Systems Design and Implementation (OSDI 23)},
  year={2023}
}

@inproceedings{ChenW21,
  author = {Qi Chen and
            Bing Zhao and
            Haidong Wang and
            Mingqin Li and
            Chuanjie Liu and
            Zengzhong Li and
            Mao Yang and
            Jingdong Wang},
  title = {SPANN: Highly-efficient Billion-scale Approximate Nearest Neighbor Search},
  booktitle = {35th Conference on Neural Information Processing Systems (NeurIPS 2021)},
  year = {2021}
}

@manual{ChenW18,
  author    = {Qi Chen and
               Haidong Wang and
               Mingqin Li and
               Gang Ren and
               Scarlett Li and
               Jeffery Zhu and
               Jason Li and
               Chuanjie Liu and
               Lintao Zhang and
               Jingdong Wang},
  title     = {SPTAG: A library for fast approximate nearest neighbor search},
  url       = {https://github.com/Microsoft/SPTAG},
  year      = {2018}
}

@inproceedings{WangL12,
  author    = {Jingdong Wang and
               Shipeng Li},
  title     = {Query-driven iterated neighborhood graph search for large scale indexing},
  booktitle = {ACM Multimedia 2012},
  pages     = {179--188},
  year      = {2012}
}

@inproceedings{WangWZTGL12,
  author    = {Jing Wang and
               Jingdong Wang and
               Gang Zeng and
               Zhuowen Tu and
               Rui Gan and
               Shipeng Li},
  title     = {Scalable k-NN graph construction for visual descriptors},
  booktitle = {CVPR 2012},
  pages     = {1106--1113},
  year      = {2012}
}

@article{WangWJLZZH14,
  author    = {Jingdong Wang and
               Naiyan Wang and
               You Jia and
               Jian Li and
               Gang Zeng and
               Hongbin Zha and
               Xian{-}Sheng Hua},
  title     = {Trinary-Projection Trees for Approximate Nearest Neighbor Search},
  journal   = {{IEEE} Trans. Pattern Anal. Mach. Intell.},
  volume    = {36},
  number    = {2},
  pages     = {388--403},
  year      = {2014
}
```

## **Contribute**

This project welcomes contributions and suggestions from all the users.

We use [GitHub issues](https://github.com/Microsoft/SPTAG/issues) for tracking suggestions and bugs.

## **License**
The entire codebase is under [MIT license](https://github.com/Microsoft/SPTAG/blob/master/LICENSE)
