# Pixi Build and Test Guide

This document describes how to build and test the mamba project using [pixi](https://pixi.sh/).

## Initial Setup

```bash
# 1. Install pixi (if not already installed)
curl -fsSL https://pixi.sh/install.sh | bash
export PATH="$HOME/.pixi/bin:$PATH"

# 2. Install dependencies
pixi install
```

## Key Configuration Note

The pixi environment provides **shared libraries**, but micromamba's default CMake configuration expects **static** libraries. The `pixi.toml` uses the `mamba-unix-shared-debug-dev` preset to handle this:

```bash
cmake -B build -G Ninja --preset mamba-unix-shared-debug-dev
```

## Available Tasks

### Build Tasks

| Task | Description |
|------|-------------|
| `pixi run configure` | Configure CMake with shared library preset |
| `pixi run build` | Build all targets |
| `pixi run build-tests` | Build only C++ test executable |
| `pixi run build-mamba` | Build only mamba executable |
| `pixi run clean` | Remove build directory |

### Test Tasks

| Task | Description |
|------|-------------|
| `pixi run test-cpp-package-info` | Run C++ PackageInfo tests |
| `pixi run test-cpp-package-fetcher` | Run C++ PackageFetcher tests |
| `pixi run test-py-constructor` | Run Python TestURLDerivedMetadata tests |
| `pixi run test-defaulted-keys` | Run all defaulted_keys feature tests |
| `pixi run list-tests` | List all available C++ test names |

### CI Tasks

| Task | Description |
|------|-------------|
| `pixi run ci-test-defaulted-keys` | Full build + all defaulted_keys tests |

## Quick Start

```bash
# Fresh build and test everything
pixi run ci-test-defaulted-keys

# Or step by step:
pixi run build-tests
pixi run build-mamba
pixi run test-defaulted-keys
```

## Commit-by-Commit Test Verification

The 8 commits follow TDD (RED-GREEN) cycles. Here's which tasks to run at each commit:

| Commit | Phase | Tasks | Expected Result |
|--------|-------|-------|-----------------|
| 01 | RED | `build-tests`, `test-cpp-package-info` | ❌ 5 assertions fail |
| 02 | GREEN | `build-tests`, `test-cpp-package-info` | ✅ 149 assertions pass |
| 03 | RED | `build-tests`, `test-cpp-package-fetcher` | ❌ 3 test cases fail |
| 04 | GREEN | `build-tests`, `test-cpp-package-fetcher` | ✅ 19 assertions pass |
| 05 | RED | `build-tests`, `test-cpp-package-fetcher` | ❌ 1 test case fails |
| 06 | GREEN | `build-tests`, `test-cpp-package-fetcher` | ✅ 26 assertions pass |
| 07 | RED | `build-tests`, `test-cpp-package-fetcher` | ❌ 2 test cases fail |
| 08 | GREEN | `build-tests`, `build-mamba`, `test-defaulted-keys` | ✅ All pass |

### One-Liner Verification Commands

```bash
# Commit 01 (RED)
pixi run clean && pixi run build-tests && pixi run test-cpp-package-info

# Commit 02 (GREEN)
pixi run clean && pixi run build-tests && pixi run test-cpp-package-info

# Commit 03 (RED)
pixi run clean && pixi run build-tests && pixi run test-cpp-package-fetcher

# Commit 04 (GREEN)
pixi run clean && pixi run build-tests && pixi run test-cpp-package-fetcher

# Commit 05 (RED)
pixi run clean && pixi run build-tests && pixi run test-cpp-package-fetcher

# Commit 06 (GREEN)
pixi run clean && pixi run build-tests && pixi run test-cpp-package-fetcher

# Commit 07 (RED)
pixi run clean && pixi run build-tests && pixi run test-cpp-package-fetcher

# Commit 08 (GREEN) - Final
pixi run clean && pixi run ci-test-defaulted-keys
```

## Environment Variables for Python Tests

The Python tests require these environment variables (automatically set by `test-py-constructor` task):

| Variable | Value | Purpose |
|----------|-------|---------|
| `MAMBA_ROOT_PREFIX` | `/tmp/mamba_root` | Root prefix for test environment |
| `CONDA_PREFIX` | `/tmp/mamba_root` | Conda prefix (same as root) |
| `TEST_MAMBA_EXE` | `build/micromamba/mamba` | Path to built mamba executable |

## Troubleshooting

### CMake Configuration Fails with Library Type Mismatch

```
CMake Error: Expected type "STATIC_LIBRARY" for target "yaml-cpp::yaml-cpp" but found "SHARED_LIBRARY"
```

**Solution:** Use the shared library preset (already configured in `pixi.toml`):
```bash
cmake -B build -G Ninja --preset mamba-unix-shared-debug-dev
```

### Test Filter Doesn't Match

Catch2 uses test name wildcards, not tag syntax:
```bash
# Wrong (tag syntax)
./test_libmamba "[PackageFetcher]"

# Correct (wildcard on name)
./test_libmamba "PackageFetcher*"
```

### Python Tests Fail with "Mamba/Micromamba not found"

Ensure `TEST_MAMBA_EXE` is set and the mamba executable is built:
```bash
pixi run build-mamba
export TEST_MAMBA_EXE=build/micromamba/mamba
```
