# Repository Guidelines

## Project Structure & Module Organization
SPTAG is a C++17 ANN library built with CMake.

- `AnnService/`: core indexing/search libraries and executables (for example `indexbuilder`, `indexsearcher`, `SSDServing`).
- `Test/`: unit/integration tests (`Test/src/*Test.cpp`) and CUDA tests under `Test/cuda/`.
- `Wrappers/`: language bindings (Python/.NET/Java/WinRT).
- `GPUSupport/`: GPU-specific build targets and test binaries.
- `Tools/` and `scripts/`: helper tools and benchmark/test automation.
- `docs/`: user docs and usage examples.
- `ThirdParty/`: vendored dependencies (spdk, RocksDB, zstd, etc.).

## Build, Test, and Development Commands
Use out-of-source builds from repo root:

```bash
mkdir -p build && cd build
cmake -DSPDK=OFF -DROCKSDB=OFF ..
make -j
```

Key commands:

- `./Release/SPTAGTest`: run the main C++ test binary after build.
- `./Release/indexbuilder ...`: build an index (see `docs/GettingStart.md`).
- `./Release/indexsearcher ...`: run query/search evaluation.
- `python setup.py develop`: install Python wrapper in dev mode (after `pip install -U -r setup.txt`).
- `bash scripts/test_bkt_build.sh` / `bash scripts/test_bkt_search.sh`: benchmark-oriented regression scripts.

## Coding Style & Naming Conventions
- Formatting is governed by `.clang-format` (4-space indent, 120-column limit, no tabs, brace-heavy C++ style). Run `clang-format` on changed C++ files before submitting.
- Follow existing naming patterns: `PascalCase` for C++ types/functions in core code, `*Test.cpp` for tests, and `snake_case` for shell/python scripts.
- Keep changes localized; avoid unrelated refactors in performance-critical paths.

## Testing Guidelines
- Add/extend tests in `Test/src/` for CPU features and `Test/cuda/` for GPU changes.
- Test names should be descriptive by feature (examples: `AlgoTest.cpp`, `SPFreshTest.cpp`).
- Verify locally with `./Release/SPTAGTest`; for wrapper changes, run the relevant wrapper smoke test or example script.

## Commit & Pull Request Guidelines
- Recent commits use short imperative subjects in Chinese or English (examples: `Fix quantizer for UInt8 type (#418)`, `添加 SPANN ...`). Keep subjects concise and component-focused.
- PRs should include: problem statement, what changed, how it was tested, and any performance impact.
- Link related issues, and include logs/screenshots for behavior or benchmark changes.
- If config or dataset paths are environment-specific, document them clearly and avoid committing secrets.
