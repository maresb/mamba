# `test_create_with_explicit_url` Test Failure

## The Problem

The test `micromamba/tests/test_create.py::test_create_with_explicit_url` is failing with "Package cache error" after our changes to fix [GitHub issue #4095](https://github.com/mamba-org/mamba/issues/4095).

## How to Reproduce

```bash
pixi run build
export TEST_MAMBA_EXE=$(pwd)/build/micromamba/mamba
pixi run pytest micromamba/tests/test_create.py::test_create_with_explicit_url -x -v
```

## What We Know

1. **The test was passing on `main`** before our commits, so our changes broke something.

2. **The healing code** in `libmamba/src/core/package_cache.cpp` (lines 363-393) detects corrupted cache entries using signature: `timestamp == 0 AND license == ""`

3. **Real corrupted records have `size > 0`** - example from user:

   ```json
   {
     "size": 488279, // NOT zero - always computed from tarball
     "timestamp": 0, // corrupted stub
     "license": "", // corrupted stub
     "depends": [] // corrupted - missing real deps
   }
   ```

4. **The `_libgcc_mutex` package** (used in the test) has in its `index.json`:
   - `"license": "None"` (not empty!)
   - `"timestamp": 1575233841774` (not zero!)

5. **We expected our fix in `write_repodata_record()`** (`libmamba/src/core/package_fetcher.cpp` lines 482-499) should:
   - Erase stub fields listed in `defaulted_keys`
   - Insert correct values from `index.json`
   - Result: `license: "None"` and `timestamp: 1575233841774`

6. **But the test shows `timestamp: 0`** in the output, suggesting our fix isn't working correctly.

## Key Files

- `libmamba/src/core/package_fetcher.cpp` - `write_repodata_record()` function (lines ~440-530)
- `libmamba/src/core/package_cache.cpp` - healing code in `has_valid_extracted_dir()` (lines ~363-393)
- `libmamba/src/specs/package_info.cpp` - `from_url()` sets `defaulted_keys` (line ~103)

No uncommitted changes to the fix code.

## The Question

Why is `write_repodata_record()` not correctly replacing stub values with values from `index.json`? The healing code then triggers on the (incorrectly) written `timestamp: 0` and `license: ""`.

## Debugging Steps to Try

1. Run the explicit URL install manually and inspect the written `repodata_record.json`:

   ```bash
   export TEST_MAMBA_EXE=$(pwd)/build/micromamba/mamba
   export MAMBA_ROOT_PREFIX=/tmp/test_debug_$$
   $TEST_MAMBA_EXE create "https://conda.anaconda.org/conda-forge/linux-64/_libgcc_mutex-0.1-main.tar.bz2" \
       --no-env -n test-env --override-channels -y --no-rc -v
   cat $MAMBA_ROOT_PREFIX/pkgs/_libgcc_mutex-0.1-main/info/repodata_record.json
   ```

2. Check if `defaulted_keys` is being passed through correctly from `from_url()` to `write_repodata_record()`

3. Verify `index.json` contents in the extracted package:

   ```bash
   cat $MAMBA_ROOT_PREFIX/pkgs/_libgcc_mutex-0.1-main/info/index.json
   ```

4. Add debug logging to `write_repodata_record()` to trace what's happening
