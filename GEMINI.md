# SPTAG (Space Partition Tree And Graph)

## Project Overview
SPTAG is a C++ library developed by Microsoft Research and Microsoft Bing for large-scale vector approximate nearest neighbor (ANN) search. It assumes samples are represented as vectors and can be compared using L2 or Cosine distances. It provides algorithms such as KD-tree (SPTAG-KDT) for lower index building cost, and balanced k-means tree (SPTAG-BKT) for higher search accuracy in very high-dimensional data. The repository also includes systems for billion-scale vector search (SPANN) and incremental in-place updates (SPFresh).

## Building and Running

### Requirements
- CMake >= 3.12.0
- GCC >= 5.0 (Linux) or MSVC 14+ (Windows)
- Boost >= 1.67.0
- SWIG >= 4.0.2 (for Python bindings)
- OpenMP (Required)

### Build Instructions

**Linux:**
```bash
mkdir build
cd build && cmake -DSPDK=OFF -DROCKSDB=OFF .. && make
```
The compiled binaries and libraries will be output to the `Release/` directory.

**Windows:**
```cmd
mkdir build
cd build && cmake -A x64 -DSPDK=OFF -DROCKSDB=OFF ..
```
Then compile `SPTAGLib.sln` in Visual Studio 2019+.

### Testing
The project uses the Boost.Test framework. Test files are located in `Test/src/`.

**Run all tests:**
```bash
./Release/SPTAGTest
```

**Run a specific test suite:**
```bash
./Release/SPTAGTest --run_test=TestSuiteName
```

## Development Conventions

### Code Formatting
The project uses `clang-format` based on a Microsoft style.
- Column limit: 120
- Indent width: 4
- Standard: C++17
- Custom brace wrapping (after functions, classes, namespaces)

To format a file:
```bash
clang-format -i <file>
```

### Architecture Highlights
- **Core Algorithms:** `AnnService/inc/Core/` contains implementations for BKT, KDT, SPANN, and common tools (SIMD, Distance computation, Quantization).
- **Executables:** `AnnService/src/` contains the entry points for IndexBuilder, IndexSearcher, Server, Client, Aggregator, SSDServing, SPFresh, and Quantizer.
