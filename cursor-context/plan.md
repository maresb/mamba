# Plan: Fix Incorrect repodata_record.json Metadata

<!-- markdownlint-disable MD013 MD024 -->

## Executive Summary

This plan addresses GitHub issue #4095: when installing packages from explicit lockfiles (URLs), mamba writes incorrect metadata to `repodata_record.json`. The bug affects versions v2.1.1–v2.3.2 (full corruption) and v2.3.3 (partial fix for `depends`/`constrains` only).

**Root Cause**: `PackageInfo::from_url()` creates a "stub" object with only URL-derived fields populated. When `PackageFetcher::write_repodata_record()` converts this stub to JSON, stub/default values for `build_number`, `license`, `timestamp`, `track_features`, `depends`, and `constrains` overwrite the correct values from `index.json` because `nlohmann::json::insert()` only adds _missing_ keys—it doesn't overwrite existing keys.

**Solution**: Leverage the existing but unused `defaulted_keys` field in `PackageInfo` to track which fields have stub values. When writing `repodata_record.json`, erase all defaulted keys from the initial JSON before merging with `index.json`, allowing the correct values to flow through.

## Provenance: All Code Paths Creating PackageInfo

Understanding how `PackageInfo` objects are created is essential for this fix:

| #   | Code Path                             | Source                                                                              | Trust Level                                                                                                                                                                                                                 |
| --- | ------------------------------------- | ----------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1   | **URL-derived**                       | `specs::PackageInfo::from_url()` → parse URL/filename                               | Trust: name, version, build_string (conda), platform, channel, filename, package_url, package_type, md5/sha256 (if hash fragment). **Stub defaults**: build_number, license, timestamp, track_features, depends, constrains |
| 2   | **Channel-repodata-derived (solver)** | channel JSON → libsolv repo → `solver::libsolv::make_package_info()`                | Trust **all fields** (including patches such as empty depends/constrains). `defaulted_keys`: empty (wrappers don't expose field-absence)                                                                                    |
| 3   | **Installed cache read**              | `from_json()` in `PrefixData::load_single_record`                                   | Used to display/compute, not directly written back; healing happens when package is re-extracted                                                                                                                            |
| 4   | **Constructor path**                  | Reads repodata cache and index.json; chooses authoritative base then inserts others | Does not need changes for this fix                                                                                                                                                                                          |

## Field Audit Summary

### Fields Always Written by `to_json()`

- `build_number` (always, even if 0)
- `license` (always, even if "")
- `timestamp` (always, even if 0)
- `depends` (always as `[]` if empty)
- `constrains` (always as `[]` if empty)
- `track_features` (always as "" if empty)

### Fields Conditionally Written by `to_json()`

- `md5` (only if non-empty)
- `sha256` (only if non-empty)
- `signatures` (only if non-empty)
- `python_site_packages_path` (only if non-empty)
- `noarch` (only if not `NoArchType::No`)

### Fields Always Trusted from `parse_url()` (Conda packages)

- `name`, `version`, `build`/`build_string`, `channel`, `url`, `subdir`, `fn`

### Special Handling

- `size`: Set from file if missing or 0

### `defaulted_keys` Per Package Type

| Package Type                     | `defaulted_keys` Contents                                                                                                                                                                |
| -------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Conda** (`.tar.bz2`, `.conda`) | `[build_number, license, timestamp, track_features, depends, constrains]`                                                                                                                |
| **Wheel** (`.whl`)               | Above + `[build, build_string]`                                                                                                                                                          |
| **TarGz** (`.tar.gz`)            | Same as Wheel                                                                                                                                                                            |
| **Git** (`git+https`)            | Nearly all fields: `[version, build, build_string, build_number, channel, subdir, fn, license, timestamp, track_features, depends, constrains, noarch, python_site_packages_path, size]` |

## Research Verification

### Codebase Structure Verified

1. **`PackageInfo::from_url()`** at `libmamba/src/specs/package_info.cpp:193-258`:
   - ✅ Confirmed: Creates stub PackageInfo with only URL-derived fields
   - ✅ Confirmed: For conda packages, populates: `name`, `version`, `build_string`, `channel`, `package_url`, `platform`, `filename`, `package_type`
   - ✅ Confirmed: Does NOT populate: `build_number`, `license`, `timestamp`, `track_features`, `dependencies`, `constrains`, `defaulted_keys`
   - ✅ Confirmed: Hash suffixes can populate `md5` or `sha256`

2. **`defaulted_keys` field** at `libmamba/include/mamba/specs/package_info.hpp:55-57`:
   - ✅ Confirmed: `std::vector<std::string> defaulted_keys = {};`
   - ✅ Confirmed: Warning comment about `make_package_info` behavior exists
   - ✅ Confirmed: Currently never populated anywhere in the codebase
   - ✅ Confirmed: Read only in `json_signable()` at `package_info.cpp:306-312`

3. **`write_repodata_record()`** at `libmamba/src/core/package_fetcher.cpp:431-467`:
   - ✅ Confirmed: Line 440 creates JSON from `m_package_info`
   - ✅ Confirmed: Lines 444-453 only handle empty `depends`/`constrains` (v2.3.3 partial fix)
   - ✅ Confirmed: Line 458 uses `insert()` which doesn't overwrite existing keys
   - ✅ Confirmed: Missing handling for: `build_number`, `license`, `timestamp`, `track_features`

4. **`make_package_info()`** at `libmamba/src/solver/libsolv/helpers.cpp:107-150`:
   - ✅ Confirmed: Creates PackageInfo from libsolv solvables
   - ✅ Confirmed: Never touches `defaulted_keys` (intentionally—solver-derived packages are authoritative)

5. **`to_json()`** at `libmamba/src/specs/package_info.cpp:474-526`:
   - ✅ Confirmed: Always writes `build_number`, `license`, `timestamp`, `track_features`, `depends`, `constrains`
   - ✅ Confirmed: Conditionally writes `md5`, `sha256`, `signatures`, `python_site_packages_path`, `noarch`

6. **`constructor.cpp`** at `micromamba/src/constructor.cpp:107-185`:
   - ✅ Confirmed: Different code path for repodata_record.json
   - ✅ Confirmed: Uses channel repodata cache when available (lines 134-155)
   - ✅ Confirmed: Falls back to index.json and overrides specific fields (lines 157-180)
   - ⚠️ Note: Constructor doesn't suffer from URL-derived stub pollution because it only overrides specific fields (`fn`, `url`, `channel`, `size`, `md5`, `sha256`)

### Test Infrastructure Verified

1. **C++ tests**: `libmamba/tests/src/core/test_package_fetcher.cpp`
   - ✅ Confirmed: Uses Catch2 framework
   - ✅ Confirmed: Existing test `extract_creates_repodata_record_with_dependencies` at lines 97-195
   - ✅ Confirmed: Test creates a minimal package, extracts it, and verifies repodata_record.json

2. **PackageInfo tests**: `libmamba/tests/src/specs/test_package_info.cpp`
   - ✅ Confirmed: Tests for `from_url()` parsing
   - ✅ Confirmed: Tests for serialization (`to_json`/`from_json`)

3. **Development environment**: `docs/source/developer_zone/dev_environment.rst`
   - ✅ Confirmed: `micromamba create -n mamba -c conda-forge -f dev/environment-dev.yml`
   - ✅ Confirmed: `cmake -B build/ -G Ninja --preset mamba-unix-shared-debug-dev`
   - ✅ Confirmed: `cmake --build build/ --parallel`
   - ✅ Confirmed: `./build/libmamba/tests/test_libmamba`

### Pre-commit Configuration Verified

- ✅ Confirmed: `.pre-commit-config.yaml` exists with clang-format, cmake-format, and other hooks

## Current Bug Behavior Analysis

### The JSON Insert Problem

```cpp
// package_fetcher.cpp:431-467 (current code)
void PackageFetcher::write_repodata_record(const fs::u8path& base_path) const
{
    // ...
    nlohmann::json repodata_record = m_package_info;  // Line 440
    // m_package_info from URL has stub values:
    // - build_number=0, license="", timestamp=0
    // - track_features=[], depends=[], constrains=[]

    // Lines 444-453: v2.3.3 partial fix - only handles empty depends/constrains
    if (/* depends empty */) repodata_record.erase("depends");
    if (/* constrains empty */) repodata_record.erase("constrains");

    // Line 458: insert() only adds MISSING keys
    repodata_record.insert(index.cbegin(), index.cend());
    // Problem: build_number, license, timestamp, track_features still have stub values!
}
```

### Why Channel Patches Are Lost

When a package comes from the solver (with patched repodata that sets `depends: []`):

1. `m_package_info` has `depends: []` from patched channel repodata
2. Lines 444-448 detect `depends` is empty and **erase it**
3. Line 458 `insert()` adds `depends` from original `index.json`
4. **Result**: Patched empty `depends` is silently replaced with original broken dependencies

### Solution: Use `defaulted_keys` to Distinguish

- **URL-derived PackageInfo**: `defaulted_keys = ["build_number", "license", "timestamp", "track_features", "depends", "constrains"]`
- **Solver-derived PackageInfo**: `defaulted_keys = []` (empty, meaning all fields are authoritative)

When writing `repodata_record.json`:

1. Erase all keys listed in `defaulted_keys` from the JSON
2. Then `insert()` from `index.json` to fill in correct values
3. This preserves solver-derived values (including intentionally-empty arrays)

## Implementation Plan

### Phase 0: Environment Setup

#### Step 0.1: Set Up Development Environment

```bash
# Create dev environment
micromamba create -n mamba -c conda-forge -f dev/environment-dev.yml
# Build the project
micromamba run -n mamba cmake -B build/ -G Ninja --preset mamba-unix-shared-debug-dev
micromamba run -n mamba cmake --build build/ --parallel
```

**Verification**: Run existing tests to establish baseline:

```bash
micromamba run -n mamba ./build/libmamba/tests/test_libmamba
```

**Exit Criteria**: All existing tests pass.

#### Step 0.2: Install Pre-commit Hooks

```bash
micromamba run -n mamba pre-commit install
```

**Verification**: Run pre-commit on all files to verify setup:

```bash
micromamba run -n mamba pre-commit run --all-files
```

**Exit Criteria**: Pre-commit hooks are installed and pass (or only report pre-existing issues).

### Phase 1: Write Failing Tests (RED)

We write **10 tests** covering all scenarios. Some tests validate our assumptions (should pass initially), others demonstrate bugs (should fail initially).

#### Test 1: `defaulted_keys_populated_for_conda_url`

**File**: `libmamba/tests/src/specs/test_package_info.cpp`

**Purpose**: Verify `defaulted_keys` is correctly populated when parsing a conda URL.

**Test Logic**:

1. Call `PackageInfo::from_url()` with a conda URL
2. **Assert** `defaulted_keys` contains: `["build_number", "license", "timestamp", "track_features", "depends", "constrains"]`

**Expected Initial Outcome**: FAILS (defaulted_keys not populated yet)

#### Test 2: `defaulted_keys_populated_for_wheel_url`

**File**: `libmamba/tests/src/specs/test_package_info.cpp`

**Purpose**: Verify `defaulted_keys` includes `build`/`build_string` for wheel packages.

**Test Logic**:

1. Call `PackageInfo::from_url()` with a wheel URL
2. **Assert** `defaulted_keys` contains the conda fields PLUS `["build", "build_string"]`

**Expected Initial Outcome**: FAILS (defaulted_keys not populated yet)

#### Test 3: `repodata_record_preserves_all_metadata_from_index_json`

**File**: `libmamba/tests/src/core/test_package_fetcher.cpp`

**Purpose**: Verify that when extracting a URL-derived package, ALL metadata fields from `index.json` are preserved in `repodata_record.json`.

**Test Logic**:

1. Create a PackageInfo from URL (has stub values)
2. Create a mock package with `index.json` containing:
   - `license = "MIT"`
   - `timestamp = 1234567890`
   - `build_number = 5`
   - `track_features = ["mkl"]`
   - `depends = ["python >=3.7"]`
   - `constrains = ["pytz"]`
3. Call `write_repodata_record()` (via PackageFetcher extraction)
4. Read `repodata_record.json`
5. **Assert** all fields match `index.json` values (not stub values)

**Expected Initial Outcome**: FAILS (bug: stub values override index.json)

#### Test 4: `repodata_record_preserves_channel_patched_empty_depends`

**File**: `libmamba/tests/src/core/test_package_fetcher.cpp`

**Purpose**: Verify intentionally empty `depends` from channel patches is preserved.

**Test Logic**:

1. Create a solver-derived PackageInfo with:
   - `dependencies = []` (intentionally empty from channel patch)
   - `build_number = 3` (non-zero, non-stub)
   - `timestamp = 1234567890` (non-zero, non-stub)
   - `defaulted_keys = []` (empty—solver-derived)
2. Create mock package with `index.json` containing `depends = ["broken-dep"]`
3. Call `write_repodata_record()`
4. Read `repodata_record.json`
5. **Assert** `depends` is still empty (patch preserved)

**Expected Initial Outcome**: FAILS (bug: empty depends erased unconditionally)

#### Test 5: `repodata_record_preserves_channel_patched_empty_constrains`

**File**: `libmamba/tests/src/core/test_package_fetcher.cpp`

**Purpose**: Verify intentionally empty `constrains` from channel patches is preserved (separate from depends).

**Test Logic**: Same as Test 4 but for `constrains`.

**Expected Initial Outcome**: FAILS (bug: empty constrains erased unconditionally)

#### Test 6: `repodata_record_backfills_noarch_and_python_site_packages_path`

**File**: `libmamba/tests/src/core/test_package_fetcher.cpp`

**Purpose**: Verify `noarch` and `python_site_packages_path` are backfilled from `index.json` for URL-derived packages (these are NOT in `defaulted_keys` because `to_json()` omits them when unset).

**Test Logic**:

1. Create a URL-derived PackageInfo (noarch and python_site_packages_path not set)
2. Create mock package with `index.json` containing `noarch = "python"` and `python_site_packages_path = "lib/python3.11/site-packages"`
3. Call `write_repodata_record()`
4. **Assert** both fields are present in `repodata_record.json`

**Expected Initial Outcome**: PASSES (validates existing merge behavior for conditionally-written fields)

#### Test 7: `repodata_record_size_filled_from_tarball_when_zero`

**File**: `libmamba/tests/src/core/test_package_fetcher.cpp`

**Purpose**: Verify `size` is filled from actual tarball size when 0 or missing.

**Test Logic**:

1. Create PackageInfo with `size = 0`
2. Create a tarball of known size
3. Call `write_repodata_record()`
4. **Assert** `size` in `repodata_record.json` equals actual tarball size

**Expected Initial Outcome**: PASSES (existing behavior works correctly)

#### Test 8: `repodata_record_preserves_url_hash_over_index_json`

**File**: `libmamba/tests/src/core/test_package_fetcher.cpp`

**Purpose**: Verify `md5`/`sha256` from URL hash fragment takes precedence over `index.json`.

**Test Logic**:

1. Create PackageInfo from URL with hash suffix (e.g., `#sha256:abc123...`)
2. Create mock `index.json` with different hash
3. Call `write_repodata_record()`
4. **Assert** hash from URL is preserved (not overwritten by index.json)

**Expected Initial Outcome**: PASSES (URL-derived hashes are authoritative)

#### Test 9: `repodata_record_heals_corrupted_cache_entries`

**File**: `libmamba/tests/src/core/test_package_fetcher.cpp`

**Purpose**: Verify corrupted cache entries from v2.1.1-v2.3.2 get healed.

**Test Logic**:

1. Create PackageInfo simulating corrupted cache:
   - `timestamp = 0`
   - `build_number = 0`
   - `license = ""`
   - `dependencies = []`
   - `defaulted_keys = []` (empty—old versions didn't populate it)
2. Create mock package with `index.json` containing correct values
3. Call `write_repodata_record()`
4. **Assert** fields are healed from `index.json`

**Expected Initial Outcome**: FAILS (healing logic doesn't exist yet)

#### Test 10: `repodata_record_no_false_positive_healing`

**File**: `libmamba/tests/src/core/test_package_fetcher.cpp`

**Purpose**: Verify healing doesn't trigger on legitimate packages with `timestamp=0` but non-stub values.

**Test Logic**:

1. Create PackageInfo with:
   - `timestamp = 0` (rare but legitimate)
   - `license = "MIT"` (non-empty—NOT a stub)
   - `dependencies = ["foo"]` (non-empty—NOT a stub)
   - `defaulted_keys = []`
2. Create mock `index.json` with different values
3. Call `write_repodata_record()`
4. **Assert** PackageInfo values are preserved (no healing occurred)

**Expected Initial Outcome**: PASSES initially (no healing logic exists), must still PASS after fix

### Phase 2: Verify Initial Test Outcomes (Critical TDD Step)

**Command**:

```bash
micromamba run -n mamba cmake --build build/ --parallel
micromamba run -n mamba ./build/libmamba/tests/test_libmamba "[defaulted_keys],[repodata_record]"
```

**Expected Outcomes**:

| Test | Expected | Reason                                                    |
| ---- | -------- | --------------------------------------------------------- |
| 1    | FAIL     | defaulted_keys not populated yet                          |
| 2    | FAIL     | defaulted_keys not populated yet                          |
| 3    | FAIL     | Bug: stub values override index.json                      |
| 4    | FAIL     | Bug: empty depends erased unconditionally                 |
| 5    | FAIL     | Bug: empty constrains erased unconditionally              |
| 6    | PASS     | Validates existing merge for conditionally-written fields |
| 7    | PASS     | Validates existing size handling                          |
| 8    | PASS     | URL hashes are already authoritative                      |
| 9    | FAIL     | Healing logic doesn't exist                               |
| 10   | PASS     | No healing logic = no false positives (yet)               |

**Exit Criteria**:

- Tests 1, 2, 3, 4, 5, 9 → FAIL
- Tests 6, 7, 8, 10 → PASS

**If outcomes don't match**: Re-examine test logic; fix tests to properly validate assumptions or demonstrate bugs.

### Phase 3: Implement Fix (GREEN)

#### Step 3.1: Populate `defaulted_keys` in `PackageInfo::from_url()`

**File**: `libmamba/src/specs/package_info.cpp`

**Changes**: In `parse_url()` function, after populating fields from URL:

```cpp
// For conda packages (.tar.bz2, .conda)
if (out.package_type == PackageType::Conda)
{
    out.defaulted_keys = {
        "build_number",
        "license",
        "timestamp",
        "track_features",
        "depends",
        "constrains"
    };
}
// For wheel packages (.whl)
else if (out.package_type == PackageType::Wheel)
{
    out.defaulted_keys = {
        "build",
        "build_string",
        "build_number",
        "license",
        "timestamp",
        "track_features",
        "depends",
        "constrains"
    };
}
// For tar.gz packages
else if (out.package_type == PackageType::TarGz)
{
    out.defaulted_keys = {
        "build",
        "build_string",
        "build_number",
        "license",
        "timestamp",
        "track_features",
        "depends",
        "constrains"
    };
}
```

For git URLs (in the `git+https` handling section):

```cpp
out.defaulted_keys = {
    "version",
    "build",
    "build_string",
    "build_number",
    "channel",
    "subdir",
    "fn",
    "license",
    "timestamp",
    "track_features",
    "depends",
    "constrains",
    "noarch",
    "python_site_packages_path",
    "size"
};
```

**Verification**: Build and run tests 1-2. They should now PASS.

```bash
micromamba run -n mamba cmake --build build/ --parallel
micromamba run -n mamba ./build/libmamba/tests/test_libmamba "[defaulted_keys]"
```

#### Step 3.2: Fix `write_repodata_record()` to Use `defaulted_keys` and Healing

**File**: `libmamba/src/core/package_fetcher.cpp`

**Changes**: Replace the current partial fix (lines 444-453) with comprehensive logic:

```cpp
void PackageFetcher::write_repodata_record(const fs::u8path& base_path) const
{
    const fs::u8path repodata_record_path = base_path / "info" / "repodata_record.json";
    const fs::u8path index_path = base_path / "info" / "index.json";

    nlohmann::json index;
    std::ifstream index_file = open_ifstream(index_path);
    index_file >> index;

    nlohmann::json repodata_record = m_package_info;

    // Determine which keys to erase before merging with index.json
    std::vector<std::string> keys_to_erase;

    if (!m_package_info.defaulted_keys.empty())
    {
        // Normal path: URL-derived PackageInfo with populated defaulted_keys
        keys_to_erase = m_package_info.defaulted_keys;
    }
    else
    {
        // HEALING: Detect corrupted cache entries from v2.1.1-v2.3.2
        // These have timestamp=0, empty defaulted_keys, AND stub indicators
        const bool has_stub_timestamp = (m_package_info.timestamp == 0);
        const bool has_stub_indicators = m_package_info.license.empty()
                                         || m_package_info.dependencies.empty()
                                         || m_package_info.track_features.empty()
                                         || m_package_info.build_number == 0;

        if (has_stub_timestamp && has_stub_indicators)
        {
            // Corrupted cache: heal by treating as URL-derived
            keys_to_erase = {
                "build_number",
                "license",
                "timestamp",
                "track_features",
                "depends",
                "constrains"
            };
        }
        // else: Solver-derived PackageInfo → trust all fields (including empty arrays)
    }

    // Erase keys that should be filled from index.json
    for (const auto& key : keys_to_erase)
    {
        repodata_record.erase(key);
    }

    // Insert index.json values for missing keys
    // (This fills in erased stub fields with correct values from index.json)
    repodata_record.insert(index.cbegin(), index.cend());

    // URL-authoritative fields: ensure these come from PackageInfo, not index.json
    // (They were set correctly by to_json and should NOT be overwritten by insert)
    // fn, url, channel are already authoritative from m_package_info

    // Handle size: fill from tarball if missing or 0
    if (repodata_record.find("size") == repodata_record.end() || repodata_record["size"] == 0)
    {
        repodata_record["size"] = fs::file_size(m_tarball_path);
    }

    std::ofstream repodata_record_file(repodata_record_path.std_path());
    repodata_record_file << repodata_record.dump(4);
}
```

**Verification**: Build and run all tests.

```bash
micromamba run -n mamba cmake --build build/ --parallel
micromamba run -n mamba ./build/libmamba/tests/test_libmamba "[defaulted_keys],[repodata_record]"
```

**Expected Outcome**: ALL 10 tests PASS.

#### Step 3.3: Verify All Tests Pass

**Command**:

```bash
micromamba run -n mamba ./build/libmamba/tests/test_libmamba
```

**Exit Criteria**: All tests pass, including both new and existing tests.

### Phase 4: Pre-commit and Cleanup

#### Step 4.1: Run Pre-commit on Changed Files

```bash
micromamba run -n mamba pre-commit run --files \
    libmamba/src/specs/package_info.cpp \
    libmamba/src/core/package_fetcher.cpp \
    libmamba/tests/src/specs/test_package_info.cpp \
    libmamba/tests/src/core/test_package_fetcher.cpp
```

**Exit Criteria**: All pre-commit checks pass.

#### Step 4.2: Final Test Run

```bash
micromamba run -n mamba ./build/libmamba/tests/test_libmamba
```

**Exit Criteria**: All tests pass.

### Phase 5: Additional Verifications

#### Step 5.1: Verify `defaulted_keys` Not Serialized

**Check**: Ensure `defaulted_keys` is NOT written to `repodata_record.json`.

Review `to_json()` in `package_info.cpp` to confirm `defaulted_keys` is not serialized. (It isn't—the current implementation doesn't include it.)

#### Step 5.2: Test with Missing Index Fields

**Manual Test**: Create a package with an `index.json` missing some fields (e.g., no `license` key). Verify:

- No crash
- Missing fields get default values
- Present fields are correctly merged

#### Step 5.3: Real Package Smoke Test (Optional)

**Manual Test**: Install a real package from conda-forge using explicit URL and verify `repodata_record.json` has correct metadata.

```bash
micromamba run -n mamba ./build/micromamba/micromamba create -p /tmp/test-env \
    https://conda.anaconda.org/conda-forge/noarch/tzdata-2024a-h0c530f3_0.conda
cat /tmp/test-env/pkgs/tzdata-*/info/repodata_record.json
```

Verify `timestamp`, `build_number`, `license` are correct (not 0/"").

## Commit Structure (Atomic Commits)

### Commit 1: Add tests for repodata_record.json metadata handling

**Files**:

- `libmamba/tests/src/specs/test_package_info.cpp`
- `libmamba/tests/src/core/test_package_fetcher.cpp`

**Message**:

```text
test(libmamba): add tests for repodata_record.json metadata handling

Add 10 tests covering repodata_record.json metadata scenarios:

Failing tests (demonstrate bugs):
1. defaulted_keys_populated_for_conda_url
2. defaulted_keys_populated_for_wheel_url
3. repodata_record_preserves_all_metadata_from_index_json
4. repodata_record_preserves_channel_patched_empty_depends
5. repodata_record_preserves_channel_patched_empty_constrains
9. repodata_record_heals_corrupted_cache_entries

Passing tests (validate assumptions):
6. repodata_record_backfills_noarch_and_python_site_packages_path
7. repodata_record_size_filled_from_tarball_when_zero
8. repodata_record_preserves_url_hash_over_index_json
10. repodata_record_no_false_positive_healing

Tests 1-5, 9 currently FAIL, demonstrating issue #4095.

Ref: https://github.com/mamba-org/mamba/issues/4095
```

### Commit 2: Populate defaulted_keys in PackageInfo::from_url()

**Files**:

- `libmamba/src/specs/package_info.cpp`

**Message**:

```text
fix(specs): populate defaulted_keys in PackageInfo::from_url()

When creating a PackageInfo from a URL (explicit install), populate
the defaulted_keys field to track which fields have stub/default
values rather than real metadata.

Per package type:
- Conda: [build_number, license, timestamp, track_features, depends, constrains]
- Wheel/TarGz: above + [build, build_string]
- Git: nearly all fields (15 total)

The defaulted_keys infrastructure was introduced in PR #1120 for
signature verification but fell out of use in 2023 during the libsolv
wrapper refactor. This commit revives its use for distinguishing
"field has default value" from "field intentionally set to this value".

Tests fixed:
- defaulted_keys_populated_for_conda_url
- defaulted_keys_populated_for_wheel_url

This is part 1/2 of the fix for #4095.
```

### Commit 3: Fix write_repodata_record() with defaulted_keys and healing

**Files**:

- `libmamba/src/core/package_fetcher.cpp`

**Message**:

```text
fix(core): use defaulted_keys in write_repodata_record() with healing

Replace the v2.3.3 partial fix (which only handled empty depends/constrains)
with a complete fix using defaulted_keys:

1. Erase all keys listed in defaulted_keys before merging with index.json.
   This allows correct values from index.json to fill stub fields.

2. Preserve channel patches: solver-derived PackageInfo has empty
   defaulted_keys, so their values (including empty arrays) are trusted.

3. Cache healing: detect corrupted entries from v2.1.1-v2.3.2 using a
   conservative predicate (timestamp==0 AND stub indicators) to avoid
   false positives on legitimate packages with timestamp=0.

Healing predicate:
- timestamp == 0
- AND defaulted_keys.empty()
- AND (license.empty() OR dependencies.empty() OR track_features.empty()
       OR build_number == 0)

Tests fixed:
- repodata_record_preserves_all_metadata_from_index_json
- repodata_record_preserves_channel_patched_empty_depends
- repodata_record_preserves_channel_patched_empty_constrains
- repodata_record_heals_corrupted_cache_entries

Test still passing (no false positives):
- repodata_record_no_false_positive_healing

Closes: https://github.com/mamba-org/mamba/issues/4095
```

## Risk Assessment

### Low Risk

- Uses existing infrastructure (`defaulted_keys`) rather than introducing new fields
- No ABI changes required
- Solver-derived packages (normal install flow) are unaffected—they have empty `defaulted_keys`

### Medium Risk

- Cache healing heuristic could theoretically match legitimate packages, but:
  - Predicate requires BOTH `timestamp=0` AND stub indicators
  - Real packages virtually always have build timestamps
  - Test 10 explicitly verifies no false positives
  - Worst case: metadata is refreshed from `index.json`, which is correct anyway

### Mitigations

- **10 comprehensive tests** covering all scenarios
- **Conservative healing predicate** with stub indicators
- **Test 10** explicitly verifies no false-positive healing
- Code is localized to `write_repodata_record()`, limiting blast radius

## Healing Scope & Safety

- **Heals both v2.1.1–v2.3.2 and v2.3.3** corrupted caches
- **Conservative predicate** reduces false positives:
  - Solver-derived packages almost always have non-zero timestamps
  - If a rare package has `timestamp=0` but non-stub values, Test 10 ensures no healing
- **Healing occurs during natural cache churn** (re-extraction); no migration needed

## PR Description Template

```markdown
## Summary

Fixes #4095: Incorrect metadata in `repodata_record.json` when installing packages from explicit lockfiles (URLs).

## Problem

When packages are installed from URLs (explicit installs), `PackageInfo::from_url()` creates a "stub" object with only URL-derived fields. Other fields (`build_number`, `license`, `timestamp`, `track_features`, `depends`, `constrains`) get default/stub values.

The current code converts this stub to JSON, then uses `json.insert()` to merge with `index.json`. However, `insert()` only adds _missing_ keys—it doesn't overwrite existing keys with stub values. This causes:

1. **v2.1.1–v2.3.2**: All stub fields written to `repodata_record.json`
2. **v2.3.3**: Partial fix for `depends`/`constrains`, but still wrong values for `build_number`, `license`, `timestamp`, `track_features`
3. **Channel patches lost**: The v2.3.3 fix unconditionally erases empty `depends`/`constrains`, silently undoing channel repodata patches that intentionally set these to empty arrays

## Solution

Leverage the existing `defaulted_keys` field in `PackageInfo` (introduced in PR #1120 for signature verification, unused since 2023):

1. **Populate `defaulted_keys` in `from_url()`**: Track which fields have stub values per package type
2. **Erase defaulted keys before merge**: In `write_repodata_record()`, erase all keys listed in `defaulted_keys` before calling `insert()`, allowing `index.json` to provide correct values
3. **Preserve channel patches**: Solver-derived `PackageInfo` has empty `defaulted_keys`, so their values (including intentionally empty arrays) are preserved
4. **Cache healing**: Detect corrupted entries from v2.1.1–v2.3.2 using conservative predicate (`timestamp=0` AND stub indicators) and heal them

## Testing

Added 10 tests following TDD:

**Tests demonstrating bugs (failed initially, pass after fix):**

- `defaulted_keys_populated_for_conda_url`
- `defaulted_keys_populated_for_wheel_url`
- `repodata_record_preserves_all_metadata_from_index_json`
- `repodata_record_preserves_channel_patched_empty_depends`
- `repodata_record_preserves_channel_patched_empty_constrains`
- `repodata_record_heals_corrupted_cache_entries`

**Tests validating assumptions (passed throughout):**

- `repodata_record_backfills_noarch_and_python_site_packages_path`
- `repodata_record_size_filled_from_tarball_when_zero`
- `repodata_record_preserves_url_hash_over_index_json`
- `repodata_record_no_false_positive_healing`

## Historical Context

The `defaulted_keys` field was introduced in PR #1120 (September 2021) by @adriendelsalle for signature verification. It was populated by `make_package_info()` using libsolv's `solvable_lookup_deparray()` return value to track field presence.

In May 2023, libsolv wrappers were refactored and the boolean return value was discarded, making it impossible to detect field presence. Since then, `defaulted_keys` was never populated.

This PR revives `defaulted_keys` for a different but related purpose: distinguishing "URL-derived stub" from "solver-derived authoritative value".
```

## Abort Conditions

The plan MUST abort and notify the user if:

1. Development environment cannot be set up
2. Build fails
3. Existing tests fail before any changes
4. Tests cannot be executed
5. **RED phase**: Expected failing tests pass, or expected passing tests fail
6. **GREEN phase**: Tests still fail after implementation
7. Pre-commit checks fail and cannot be fixed
