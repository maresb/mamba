# Plan Updates: Fix Incorrect repodata_record.json Metadata

<!-- markdownlint-disable MD013 -->

This document captures updates, clarifications, and corrections to the original plan based on patch review and analysis.

## Document Precedence

This document (`plan-updates.md`) is the **authoritative specification** that supersedes `plan.md` and the current patches. It contains:

1. **Corrections** to the original plan based on implementation findings
2. **Bug reports** for issues discovered in the current patch set
3. **Required changes** that must be implemented before the PR is ready

When this document conflicts with `plan.md` or the current patches, **this document takes precedence**.

---

## Document Structure (Meta-Goals)

This document is organized to track:

1. **Goals** — What the fix aims to achieve (functional requirements)
2. **Previously-Unspecified Requirements** — Constraints discovered during implementation that weren't in the original plan (e.g., ABI stability, commit atomicity, test documentation standards)
3. **Clarification: Prevention vs. Healing** — Critical distinction between the two mechanisms (must not be conflated)
4. **Bugs in Current Patch Set** — Issues found in the existing patches, with recommended fixes
5. **Clarifying Messages** — Code comments and commit messages that should be added or modified to improve clarity
6. **Patch Reorganization** — Suggested combining or splitting of patches to make more logical and focused changes
7. **Additional Features to Implement** — New functionality identified as necessary during review
8. **General Observations** — Architectural insights, design patterns, and other learnings

### Validation of this document

This document is valid when the following prompt succeeds with no suggested modifications to plan-updates.md.

@cursor-context/pre-plan.md @cursor-context/plan.md @cursor-context/plan-updates.md

Please thoroughly analyze @patches, which we have implemented in the previous stage. The stage we are now preparing for is outlined in @cursor-context/plan-updates.md. Please screen @cursor-context/plan-updates.md for correctness and completeness with respect to the patches and the other planning materials.

---

## 1. Goals

1. **Fix the core bug**: URL-derived packages from explicit lockfiles should use metadata from `index.json`, not stub defaults
2. **Preserve channel patches**: Solver-derived packages with intentionally empty `depends`/`constrains` must be preserved
3. **Heal corrupted caches**: Existing corrupted `repodata_record.json` files from v2.1.1–v2.4.0 should be detected and re-extracted
4. **Maintain ABI stability**: No changes to public headers or struct layouts
5. **Ensure logical consistency**: All URL-derived package types should populate `defaulted_keys` explicitly
6. **Match conda behavior**: Output `repodata_record.json` should have consistent field presence (e.g., `depends`/`constrains` always present, `track_features` only when non-empty)

---

## 2. Previously-Unspecified Requirements

### No ABI Changes

The patches must not modify any public headers or change struct layouts. This was implicit in the original plan's decision to use the existing `defaulted_keys` field, but should be stated explicitly.

**Verification**: All patches modify only `.cpp` implementation files and test files. No `.hpp` headers are touched.

### Version Range Update

After the initial patches were crafted, mamba 2.4.0 was released (still containing the bug). All references to the affected version ranges must be updated:

| Description              | In Patches      | Should Be                   |
| ------------------------ | --------------- | --------------------------- |
| Overall affected range   | `v2.1.1-v2.3.3` | `v2.1.1-v2.4.0`             |
| Full corruption range    | `v2.1.1-v2.3.2` | `v2.1.1-v2.3.2` (unchanged) |
| Partial corruption range | `v2.3.3`        | `v2.3.3-v2.4.0`             |

### Atomic Commits with Test-Fix Separation

Each commit should make exactly ONE logical change. Tests and fixes should be in separate commits to enable verification:

- Check out commit N (test): verify tests FAIL (RED)
- Check out commit N+1 (fix): verify tests PASS (GREEN)

This confirms tests actually catch the bug and the fix addresses it.

**Exception**: Red-green cadence can be violated for good reason (e.g., adding `_initialized` to solver path without a dedicated test).

**Combining repetitive changes**: Repetitive changes of the same type (e.g., adding `defaulted_keys` to multiple package types in `from_url()`) can be combined into a single commit to avoid redundancy while still maintaining logical atomicity.

### Shared Helpers Are Acceptable

Shared helper functions are OK if they:

- Increase clarity
- Don't require ABI changes (i.e., stay in `.cpp` files)

Code duplication is also acceptable with explicit comments if helpers don't improve clarity.

### Behavioral Compatibility

Changes to JSON serialization (like conditional `track_features`) affect output format but not binary interface. These are acceptable but should be documented.

### Thorough Test Documentation

Every test should include inline comments explaining:

1. **Purpose**: What behavior is being tested
2. **Motivation**: Why this test exists (what bug/regression it prevents)
3. **Setup**: What preconditions are established
4. **Assertion rationale**: Why each assertion matters

Example format:

```cpp
TEST_CASE("PackageFetcher::write_repodata_record preserves channel-patched empty depends")
{
    // PURPOSE: Verify that intentionally empty depends[] from channel repodata patches
    // are preserved in repodata_record.json, not overwritten by index.json.
    //
    // MOTIVATION: Issue #4095 - the v2.3.3 partial fix unconditionally erased empty
    // depends[], which silently undid channel patches that fix broken dependencies.
    //
    // SETUP: Create a solver-derived PackageInfo with:
    // - depends = [] (intentionally empty from channel patch)
    // - defaulted_keys = {"_initialized"} (solver-derived, trust all fields)
    // - timestamp != 0 (non-stub value proves this is solver-derived)
    //
    // ASSERTIONS:
    // - repodata_record["depends"] must be empty array (patch preserved)
    // - NOT ["broken-dep"] from index.json (patch would be undone)

    // ... test implementation ...
}
```

---

## 3. Clarification: Prevention vs. Healing Are Separate Mechanisms

These two mechanisms serve **completely different purposes** and must not be conflated:

### Prevention Mechanism (for NEW extractions)

- **Purpose**: Ensure `defaulted_keys` is always explicitly populated in all code paths
- **Implementation**:
  - `from_url()`: Populates `defaulted_keys` with stub field names + `_initialized` sentinel
  - `make_package_info()`: Sets `defaulted_keys = {"_initialized"}` for solver path
  - `write_repodata_record()`: Fail-hard if `_initialized` missing; erase stub keys before merge
- **Trigger**: `_initialized` sentinel presence/absence in `defaulted_keys`
- **On failure**: Throws exception (indicates a bug in a code path that creates PackageInfo)

### Healing Mechanism (for OLD corrupted caches)

- **Purpose**: Detect and invalidate corrupted `repodata_record.json` files written by v2.1.1–v2.4.0
- **Implementation**: `has_valid_extracted_dir()` checks corruption signature in **existing cache files on disk**
- **Trigger**: `timestamp == 0 AND license == ""` in the cached `repodata_record.json` file
- **On detection**: Returns `valid=false` → triggers re-extraction → `from_url()` called fresh → prevention mechanism writes correct values

### Why They Must Remain Separate

1. **Different locations**: Healing detects corruption in **files on disk**; prevention ensures **in-memory** `PackageInfo` correctness
2. **Different triggers**: Healing uses file content signature; prevention uses `_initialized` sentinel in memory
3. **Different failure modes**: Healing silently re-extracts (user-facing recovery); prevention fails hard (developer-facing bug indicator)
4. **Healing depends on prevention**: After healing triggers re-extraction, the prevention mechanism is what actually writes the correct values
5. **No overlap**: The corruption signature is ONLY for detecting old files; it should NEVER be used as a fallback for missing `defaulted_keys`

### Flow Diagram

```text
NEW EXTRACTION (Prevention):
  from_url() → defaulted_keys = {"_initialized", "license", ...}
            → write_repodata_record()
            → check _initialized (fail hard if missing)
            → erase stub keys
            → merge with index.json
            → correct repodata_record.json

OLD CORRUPTED CACHE (Healing):
  has_valid_extracted_dir() → reads existing repodata_record.json
                           → detects timestamp=0 AND license=""
                           → returns valid=false
                           → triggers re-extraction
                           → from_url() called (NEW EXTRACTION flow above)
```

---

## 4. Bugs in Current Patch Set

### Bug 1: Git URLs Not Handled in `from_url()`

**Location**: Patch 0002 (`package_info.cpp`)

**Problem**: The `git+https://...` URL parsing path does not populate `defaulted_keys`. The fallback stub-signature detection in `write_repodata_record()` compensates for this, but it conflates prevention with healing semantics.

**Impact**: Low — git packages are handled by pip, not extracted by mamba's `PackageFetcher`. This is an internal consistency issue, not a functional bug.

**Required Fix**: Add `defaulted_keys` population for git URLs for internal consistency and future-proofing.

### Bug 2: Fallback Logic Must Be Replaced with Fail-Hard

**Location**: Patch 0004 (`package_fetcher.cpp`)

**Problem**: The current implementation contains a fallback that uses the corruption signature when `defaulted_keys` is empty:

```cpp
// CURRENT (BAD):
auto defaulted_keys = m_package_info.defaulted_keys;
if (defaulted_keys.empty() && repodata_record["timestamp"] == 0
    && repodata_record["license"] == "")
{
    // Fallback: treat as URL-derived
    defaulted_keys = { "build_number", "license", ... };
}
```

**Why this is bad practice**:

1. **Conflates prevention with healing**: The corruption signature (`timestamp==0 AND license==""`) is for detecting OLD corrupted files on disk, NOT for compensating for missing `defaulted_keys` in memory
2. **Masks bugs**: If a code path in `from_url()` fails to populate `defaulted_keys`, this fallback silently "fixes" it instead of exposing the bug
3. **Violates fail-fast principle**: Bugs in PackageInfo creation paths should be loud and immediate, not silently worked around
4. **Creates false confidence**: Tests pass even when `defaulted_keys` isn't properly populated (e.g., git URLs)

**Required Fix**: Replace the fallback with a fail-hard check:

```cpp
// CORRECT:
if (!util::contains(m_package_info.defaulted_keys, "_initialized"))
{
    throw std::logic_error(
        "PackageInfo missing _initialized sentinel in defaulted_keys. "
        "This indicates a bug in the code path that created this PackageInfo. "
        "See GitHub issue #4095."
    );
}

// Now erase stub keys (excluding _initialized)
for (const auto& key : m_package_info.defaulted_keys)
{
    if (key != "_initialized")
    {
        repodata_record.erase(key);
    }
}
```

**All code paths must explicitly set `defaulted_keys`** with `_initialized` as the first element. There is no fallback.

### Bug 3: Missing Tests for `defaulted_keys` Population

| Test                                        | Priority |
| ------------------------------------------- | -------- |
| `defaulted_keys` populated for conda URL    | Required |
| `defaulted_keys` populated for wheel URL    | Required |
| `defaulted_keys` populated for tar.gz URL   | Required |
| `defaulted_keys` populated for git URL      | Required |
| Channel patch preservation for `constrains` | Required |

### Bug 4: Patch 0007 Should Be Deleted

Patch 0007 adds `pre-plan.md` which doesn't belong in the PR.

---

## 5. Clarifying Messages

### Code Comment Requirements

| Location                                 | Required Comment                                                                                                                                                                                                                                            |
| ---------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `from_url()` — \_initialized             | "Sentinel for fail-hard verification; can be removed later if desired"                                                                                                                                                                                      |
| `make_package_info()` — \_initialized    | "Solver packages have authoritative metadata from channel repodata (potentially patched). We set only \_initialized to signal 'trust all fields' without attempting to detect absent fields, which would require libsolv wrapper changes. See issue #4095." |
| `write_repodata_record()` — fail-hard    | "All PackageInfo creation paths must set \_initialized. If missing, this is a bug."                                                                                                                                                                         |
| `has_valid_extracted_dir()` — corruption | "Detect corrupted cache from v2.1.1–v2.4.0. See issue #4095."                                                                                                                                                                                               |
| Any intentional duplication              | "Intentionally duplicated from [location] for clarity; see issue #4095"                                                                                                                                                                                     |

### Code Comment Examples (Current → Recommended)

#### In `package_fetcher.cpp` (`write_repodata_record`)

**Current** (from patch 0004):

```cpp
// Handle metadata for new extractions (both current and healing from v2.1.1-v2.3.3).
```

**Recommended**:

```cpp
// Write repodata_record.json with correct metadata from index.json.
//
// The defaulted_keys mechanism:
// - All PackageInfo creation paths MUST set defaulted_keys with "_initialized" sentinel
// - URL-derived packages: defaulted_keys contains stub field names to erase
// - Solver-derived packages: defaulted_keys = {"_initialized"} (no stubs to erase)
//
// Flow:
// 1. Verify "_initialized" sentinel is present (fail hard if not)
// 2. Erase all keys listed in defaulted_keys (except "_initialized")
// 3. Merge with index.json via insert() to fill erased keys
// 4. Solver-derived values are preserved (nothing erased)
//
// Healing of OLD corrupted caches (v2.1.1-v2.4.0):
// - Detected in has_valid_extracted_dir() via stub signature (timestamp=0, license="")
// - Cache invalidated → re-extraction triggered
// - Fresh from_url() call populates defaulted_keys correctly
// - This code then writes correct values
//
// See GitHub issue #4095 for details on the original corruption bug.
```

#### In `package_cache.cpp` (`has_valid_extracted_dir`)

**Current** (from patch 0004):

```cpp
// Detect corruption from buggy versions (v2.1.1-v2.3.3)
```

**Recommended**:

```cpp
// HEALING: Detect corrupted cache from buggy versions (v2.1.1-v2.4.0).
// These versions wrote stub values (timestamp=0, license="") to repodata_record.json
// for URL-derived packages. By returning valid=false here, we trigger re-extraction,
// and write_repodata_record() will write correct values using index.json.
// See GitHub issue #4095 for details.
```

### Commit Message Documentation

`defaulted_keys` behavior and historical context should be documented in commit messages since this is most relevant during review. Key points to include:

- Historical context (PR #1120, libsolv wrapper refactor in 2023)
- Why `_initialized` sentinel exists
- Why solver packages only get `_initialized`
- Interaction with `json_signable()`

#### Example: Fix Commit for `write_repodata_record()`

```text
fix(core): use defaulted_keys to erase stub fields in write_repodata_record

When writing repodata_record.json, erase fields listed in defaulted_keys
before merging with index.json. This allows correct values from index.json
to replace stub defaults from URL parsing.

The "_initialized" sentinel in defaulted_keys proves the field was
explicitly set. If missing, we fail hard to catch bugs in PackageInfo
creation paths.

Technical details:
- Check for "_initialized" sentinel (throw if missing)
- Filter out "_initialized" from keys to erase
- Erase remaining keys from JSON before insert()
- Solver-derived packages have {"_initialized"} only (nothing erased)

Tests fixed:
- repodata_record_preserves_all_metadata_from_index_json (RED → GREEN)
- repodata_record_preserves_channel_patched_empty_depends (RED → GREEN)

Related: https://github.com/mamba-org/mamba/issues/4095
```

#### Example: Fix Commit for Cache Invalidation

```text
fix(core): detect corrupted cache in has_valid_extracted_dir

Detect repodata_record.json files corrupted by v2.1.1-v2.4.0 bug.
These have stub values (timestamp=0, license="") that should have
been replaced by index.json metadata.

When corruption is detected, return valid=false to trigger
re-extraction. The write_repodata_record() fix will then write
correct values using the defaulted_keys mechanism.

Corruption signature: timestamp == 0 AND license == ""

Tests fixed:
- repodata_record_heals_corrupted_cache_entries (RED → GREEN)

Related: https://github.com/mamba-org/mamba/issues/4095
```

#### Example: Test Commit

```text
test(core): add failing test for URL-derived metadata preservation

Add test demonstrating that URL-derived packages incorrectly write
stub defaults (timestamp=0, license="", build_number=0) instead of
using correct values from index.json.

PURPOSE: Verify that repodata_record.json gets metadata from index.json
MOTIVATION: Issue #4095 - explicit lockfile installs have wrong metadata
EXPECTED: Test FAILS on current code (demonstrates the bug)

Test verifies:
- license should be "MIT" (not "")
- timestamp should be 1234567890 (not 0)
- build_number should be 42 (not 0)

Related: https://github.com/mamba-org/mamba/issues/4095
```

---

## 6. Patch Reorganization

### Why the `_initialized` Fail-Fast Approach?

| Without Fail-Fast                                     | With Fail-Fast                                |
| ----------------------------------------------------- | --------------------------------------------- |
| Add code to paths, run tests, hope nothing was missed | Add check, ALL missing paths immediately fail |
| Missing path = silent incorrect behavior              | Missing path = loud exception                 |
| "Are we done?" → uncertain                            | "Are we done?" → all tests pass               |
| Fallback hides bugs                                   | No fallback, bugs are visible                 |

### Key Differences from Current Patches

1. **`_initialized` sentinel** in all code paths (from_url + make_package_info)
2. **Git URLs explicitly handled** — no fallback needed
3. **Solver path updated** — `make_package_info()` sets `{"_initialized"}`
4. **Fail-hard check** — throws exception if `_initialized` missing
5. **No fallback** — fail-hard catches missing paths

### Proposed Commit Structure (11 commits)

The structure maintains test-fix separation to enable verification by selecting the appropriate commit.

#### Commit 01: test: add tests for defaulted_keys population (RED)

Tests that `from_url()` populates `defaulted_keys` for:

- Conda packages (`.conda`, `.tar.bz2`)
- Wheel packages (`.whl`)
- TarGz packages (`.tar.gz`)
- Git URLs (`git+https://...`)

**Expected**: All tests FAIL (defaulted_keys not populated yet)

#### Commit 02: fix: populate defaulted_keys in from_url() with \_initialized sentinel (GREEN)

- Populate `defaulted_keys` for all four package types
- `_initialized` as first element in all cases
- Code comment: "Sentinel for fail-hard verification; can be removed later if desired"

**Expected**: Tests from commit 01 now PASS

#### Commit 03: fix: add \_initialized to make_package_info() for solver path

- Add `defaulted_keys = {"_initialized"}` to `make_package_info()`
- Code comment explaining why only `_initialized`

**Expected**: No test changes (prepares for fail-hard check)

#### Commit 04: test: add tests for URL-derived metadata + channel patch preservation (RED)

Tests:

1. URL-derived packages get correct metadata from `index.json` (not stubs)
2. Empty `depends` from solver (channel patch) is preserved (not overwritten by `index.json`)
3. Empty `constrains` from solver (channel patch) is preserved

**Expected**: All tests FAIL (write_repodata_record doesn't use defaulted_keys yet)

#### Commit 05: fix: use defaulted_keys in write_repodata_record() with fail-hard (GREEN)

- Throw exception if `_initialized` not present in `defaulted_keys`
- Erase keys in `defaulted_keys` (except `_initialized`) before merging with `index.json`
- This fixes both URL metadata AND channel patch preservation

**Expected**: Tests from commit 04 now PASS

#### Commit 06: test: add test for cache healing (RED)

Test that existing corrupted caches (from v2.1.1–v2.4.0) are detected, invalidated, and re-extracted with correct metadata.

**Expected**: Test FAILS (no corruption detection yet)

#### Commit 07: fix: detect corrupted cache in has_valid_extracted_dir() (GREEN)

- Detect corruption signature: `timestamp == 0 AND license == ""`
- Return `valid = false` to trigger re-extraction
- Log message indicating healing

**Expected**: Test from commit 06 now PASS

#### Commit 08: test: add test for constructor URL-derived metadata (RED)

Python integration test verifying constructor properly handles URL-derived packages.

**Expected**: Test FAILS (constructor.cpp not yet fixed)

#### Commit 09: fix: apply healing and prevention to constructor.cpp (GREEN)

- Same fail-hard check for `_initialized`
- Same `defaulted_keys` erasure logic
- Same corruption detection (if applicable to constructor path)

**Expected**: Test from commit 08 now PASS

#### Commit 10: test: add tests for consistent field presence + checksums (RED)

Tests:

1. `depends` and `constrains` always present as arrays (even if missing from `index.json`)
2. `track_features` only included when non-empty
3. Both `md5` and `sha256` always present (computed from tarball if missing)

**Expected**: Tests FAIL

#### Commit 11: fix: ensure consistent fields + compute missing checksums (GREEN)

- Add empty `depends`/`constrains` arrays if missing
- Remove empty `track_features` (post-merge cleanup in `write_repodata_record()`)
- Make `to_json()` conditionally emit `track_features` (only when non-empty) — fixes at source
- Compute missing checksums from tarball

**Expected**: Tests from commit 10 now PASS

**Note on `track_features` handling**: The current patches implement BOTH:

1. **Post-merge cleanup** (patch 0008): Erase empty `track_features` after merge in `write_repodata_record()`
2. **Source fix** (patch 0009): Change `to_json()` to not emit `track_features` when empty

These are redundant — if `to_json()` doesn't emit empty `track_features`, the post-merge cleanup won't find anything to erase. Both approaches are valid; the `to_json()` fix is cleaner but has broader impact (affects ALL JSON serialization, not just `repodata_record.json`).

---

## 7. Additional Features to Implement

### The `_initialized` Sentinel

We use an `_initialized` sentinel as the first element of `defaulted_keys` in ALL code paths:

- **Purpose**: Enables fail-hard verification that all code paths properly initialize `defaulted_keys`
- **Droppable**: Can be removed in a future refactor if desired; inline comments should note this
- **Fail-hard**: `write_repodata_record()` throws an exception if `_initialized` is not present

### `defaulted_keys` Semantics

This table explains what different `defaulted_keys` values **mean** (interpretation):

| `defaulted_keys` Value             | Meaning                                              |
| ---------------------------------- | ---------------------------------------------------- |
| Missing `_initialized`             | **BUG** — fail hard with exception                   |
| `{"_initialized"}`                 | Solver-derived; trust ALL fields (including patches) |
| `{"_initialized", "license", ...}` | URL-derived; listed fields are stubs to erase        |

### Population by Package Source

This table specifies what each code path **produces** (implementation):

The `defaulted_keys` field should contain JSON key names for fields that:

1. Are **always written** by `to_json()` (even when empty/default)
2. Have **stub/default values** from URL parsing

Fields that are **conditionally written** by `to_json()` (only when non-empty/non-default) do NOT need to be in `defaulted_keys` because `insert()` will add them from `index.json` naturally.

| Source                         | `defaulted_keys` Contents                                                                                                                                            |
| ------------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `from_url()` — conda           | `{"_initialized", "build_number", "license", "timestamp", "track_features", "depends", "constrains"}`                                                                |
| `from_url()` — wheel           | Above + `"build"`, `"build_string"`                                                                                                                                  |
| `from_url()` — tar.gz          | Same as wheel                                                                                                                                                        |
| `from_url()` — git URL         | `{"_initialized", "version", "channel", "subdir", "fn", "timestamp", "build", "build_string", "build_number", "license", "track_features", "depends", "constrains"}` |
| `make_package_info()` — solver | `{"_initialized"}` (trust all fields; no absent-field detection)                                                                                                     |

**Git URL Rationale**: Git URLs (`git+https://...#egg=name`) only populate `package_url` and optionally `name` (from `#egg=`). All other fields use struct defaults. The list above includes all fields that `to_json()` always writes.

**NOT in git URL `defaulted_keys`**:

- `name` — can be set from `#egg=` marker
- `url` — always set (to the git URL itself)
- `size` — has special handling in `write_repodata_record()` (filled from tarball)
- `noarch`, `md5`, `sha256`, `python_site_packages_path` — conditionally written by `to_json()`

**Correction**: The original `plan.md` incorrectly listed `noarch`, `python_site_packages_path`, and `size` for git URLs. These fields are either conditionally written or have special handling, so they should NOT be in `defaulted_keys`.

### Why Solver Packages Only Get `_initialized`

Solver-derived packages have authoritative metadata from channel repodata (potentially patched). We set only `_initialized` without attempting to detect absent fields because:

1. Detecting absent fields would require libsolv wrapper changes
2. Empty `defaulted_keys` (except sentinel) correctly signals "trust all fields"
3. This preserves channel patches with intentionally empty arrays

### Missing Tests from Original Plan

| Test                                                             | Status     | Priority     |
| ---------------------------------------------------------------- | ---------- | ------------ |
| `defaulted_keys_populated_for_conda_url`                         | ❌ Missing | **Required** |
| `defaulted_keys_populated_for_wheel_url`                         | ❌ Missing | **Required** |
| `defaulted_keys_populated_for_tar_gz_url`                        | ❌ Missing | **Required** |
| `defaulted_keys_populated_for_git_url`                           | ❌ Missing | **Required** |
| `repodata_record_preserves_channel_patched_empty_constrains`     | ❌ Missing | **Required** |
| `repodata_record_no_false_positive_healing`                      | ❌ Missing | Medium       |
| `repodata_record_backfills_noarch_and_python_site_packages_path` | ❌ Missing | Medium       |
| `repodata_record_size_filled_from_tarball_when_zero`             | ❌ Missing | Medium       |
| `repodata_record_preserves_url_hash_over_index_json`             | ❌ Missing | Medium       |

### False-Positive Healing Analysis

The false-positive healing test ensures legitimate packages with `timestamp=0` but valid metadata aren't incorrectly "healed."

**Risk Assessment**:

The corruption signature `timestamp == 0 AND license == ""` provides strong protection:

1. **Both conditions required**: A package must have BOTH `timestamp=0` AND `license=""` to trigger healing
2. **Very rare for legitimate packages**: Modern build tools always set timestamps, and packages typically have license info
3. **Harmless worst case**: If a false positive occurs, the package is unnecessarily re-extracted — this is wasted work, not data corruption

**Where false positives can occur**:

- In `has_valid_extracted_dir()`: Checks existing cache entries on disk. A legitimate package with the signature would be unnecessarily re-extracted.

**Note**: The corruption signature is ONLY used in `has_valid_extracted_dir()` for healing. It is NOT used in `write_repodata_record()` — that function relies solely on `defaulted_keys` with fail-hard verification (see Bug 2).

**Conclusion**: A false-positive test is valuable as **documentation and regression safety**, but is not strictly required for correctness. The priority is downgraded from "Critical" to "Medium" because the impact of false positives is benign (unnecessary re-extraction, not data loss).

### Constructor Test Coverage

The constructor test (patch 0005) only verifies `license`, `timestamp`, `build_number`. Should also test:

- `track_features`
- `depends`
- `constrains`

### Test Documentation

All tests must include comprehensive documentation:

- **Purpose**: What behavior is being verified
- **Motivation**: What bug/regression this prevents (reference issue #4095)
- **Setup explanation**: Why preconditions are set the way they are
- **Assertion rationale**: Why each check matters

See "Thorough Test Documentation" requirement in Section 2 for template.

---

## 8. General Observations

### Separation of Concerns

The fix has two distinct mechanisms that should be understood separately:

1. **Prevention** (`from_url()` + `make_package_info()` + `write_repodata_record()`)
   - Populates `defaulted_keys` during URL parsing (with `_initialized` sentinel)
   - Solver path sets `defaulted_keys = {"_initialized"}` (trust all fields)
   - Erases stub keys before merging with `index.json`
   - Applies to NEW extractions
   - **Fails hard** if `_initialized` sentinel is missing

2. **Healing** (`has_valid_extracted_dir()`)
   - Detects OLD corrupted cache entries **on disk** via corruption signature
   - Corruption signature: `timestamp == 0 AND license == ""`
   - Invalidates cache to trigger re-extraction
   - Prevention mechanism then fixes the values during re-extraction

These are **independent and must not be conflated**:

- The corruption signature is ONLY for detecting old files on disk
- The `_initialized` sentinel is ONLY for verifying in-memory `defaulted_keys` initialization
- There is **no fallback** — missing `_initialized` is always a bug

**⚠️ Current patches violate this**: Bug 2 documents a fallback in `write_repodata_record()` that incorrectly uses the corruption signature when `defaulted_keys` is empty. This must be fixed.

### Coverage Assessment (After Fixes)

| Scenario                               | Covered?                                         |
| -------------------------------------- | ------------------------------------------------ |
| Conda URLs from explicit lockfiles     | ✅ Yes                                           |
| Wheel packages                         | ✅ Yes                                           |
| TarGz packages                         | ✅ Yes                                           |
| Git URLs                               | ✅ Yes (explicit `defaulted_keys`, not fallback) |
| Solver-derived packages                | ✅ Yes (`{"_initialized"}` = trust all)          |
| Healing old corruption (v2.1.1–v2.4.0) | ✅ Yes                                           |
| Channel patches preserved              | ✅ Yes                                           |
| Unknown code paths                     | ✅ Fail-hard catches bugs                        |

### Additional Improvements Beyond Original Plan

The patches include valuable improvements not in the original plan:

| Improvement                  | Patches    | Description                                                                          |
| ---------------------------- | ---------- | ------------------------------------------------------------------------------------ |
| Consistent field presence    | 0008       | `depends`/`constrains` always present as arrays, `arch`/`platform` omitted when null |
| Conditional `track_features` | 0008, 0009 | Only included when non-empty (matches conda behavior)                                |
| Both checksums computed      | 0010, 0011 | `md5` and `sha256` always present (computed from tarball if missing)                 |

**Patch 0009 Detail**: Changes `to_json()` in `package_info.cpp` to conditionally emit `track_features`:

```cpp
// Before: always emits, even when empty
j["track_features"] = fmt::format("{}", fmt::join(pkg.track_features, ","));

// After: only emits when non-empty
if (!pkg.track_features.empty())
{
    j["track_features"] = fmt::format("{}", fmt::join(pkg.track_features, ","));
}
```

This change affects ALL `PackageInfo` JSON serialization, not just `repodata_record.json`. Combined with the post-merge cleanup in patch 0008, this ensures `track_features` is never empty in output.

These are good additions but should be documented as scope expansion in the PR description.

### Background: Metadata Overview

Conda packages cached under the package cache contain metadata under `info/`:

- **`info/index.json`** is written at build time by `conda-build`. It describes the package as shipped in the artifact (name, version, build, depends, constrains, ...). This is the "as-built" view. Since the SHA256 and MD5 checksums of the build archive can only be computed once the build is complete, it is impossible to include them within `index.json`.

- **`info/repodata_record.json`** is written (or backfilled) at install/extract time by conda/mamba. Its purpose is to store the channel-style record that the solver actually used — i.e. the entry from the channel's `repodata.json`, which may include later fixes (repodata patches, e.g. [from conda-forge](https://github.com/conda-forge/conda-forge-repodata-patches)) and therefore can differ from `index.json`. If the package didn't already have such a file, conda will synthesize one from `index.json` to make the cache uniform.

### Background: Corruption Patterns

When installing packages from an explicit lockfile (e.g., `micromamba install -f explicit.lock`), affected versions write incorrect metadata to `repodata_record.json` files in the package cache.

#### v2.1.1–v2.3.2 (Full Corruption)

The following fields are corrupted in `repodata_record.json`:

| Field            | Corrupted Value |
| ---------------- | --------------- |
| `depends`        | `[]` (emptied)  |
| `constrains`     | `[]` (emptied)  |
| `license`        | `""` (emptied)  |
| `timestamp`      | `0`             |
| `build_number`   | `0`             |
| `track_features` | `""`            |

The corresponding `info/index.json` files remain correct; only `repodata_record.json` is corrupted.

#### v2.3.3–v2.4.0 (Partial Corruption)

Upstream fixed how `depends` and `constrains` are populated: they are now copied from `info/index.json`. Other fields remain zeroed/emptied.

| Field            | Behavior                                         |
| ---------------- | ------------------------------------------------ |
| `depends`        | Copied from `index.json` (key omitted if absent) |
| `constrains`     | Copied from `index.json` (empty lists omitted)   |
| `license`        | `""` (still corrupted)                           |
| `timestamp`      | `0` (still corrupted)                            |
| `build_number`   | `0` (still corrupted)                            |
| `track_features` | `""` (still corrupted)                           |

#### Field-by-field Summary

| Field          | v2.1.0 (good) | v2.1.1–v2.3.2 (full) | v2.3.3–v2.4.0 (partial) |
| -------------- | ------------- | -------------------- | ----------------------- |
| depends        | correct       | `[]`                 | from `index.json`       |
| constrains     | correct       | `[]`                 | from `index.json`       |
| license        | correct       | `""`                 | `""`                    |
| timestamp      | correct       | `0`                  | `0`                     |
| build_number   | correct       | `0`                  | `0`                     |
| track_features | correct       | `""`                 | `""`                    |

See PR `mamba-org/mamba#4071` for details.

### Corruption Signature Analysis

The corruption signature `timestamp == 0 AND license == ""` is sufficient to detect corrupted caches. We do NOT need partial corruption detection because:

1. **The pattern is distinctive**: Real packages virtually always have build timestamps and often have licenses
2. **Full corruption always has both**: v2.1.1–v2.3.2 zeroed both fields
3. **Partial corruption also has both**: v2.3.3–v2.4.0 still zeroes both fields
4. **If license is non-empty, the record is not corrupt**: A non-empty license proves the record came from proper channel repodata, not URL-derived stubs

This means we can use a simple predicate without worrying about edge cases like "timestamp=0 but license='MIT'" (which would indicate a legitimate package, not corruption).

### Background: Comparison with Conda

Common behavior (both conda and mamba):

- Always include: `build`, `build_number`, `channel`, `constrains`, `depends`, `fn`, `license`, `md5`, `name`, `size`, `subdir`, `timestamp`, `url`, `version`
- Include `python_site_packages_path` for python package
- Include `license_family` when present

Differences:

| Field            | Conda               | Mamba                   |
| ---------------- | ------------------- | ----------------------- |
| `channel`        | URL                 | channel name            |
| `build_string`   | never               | always                  |
| `sha256`         | always              | never (until this fix)  |
| `track_features` | only when non-empty | always (until this fix) |
| `package_type`   | for noarch packages | never                   |
| `arch`           | always string       | can be null for noarch  |

### Signature Verification Interaction

The `defaulted_keys` field is actively used by `json_signable()`:

```cpp
// package_info.cpp lines 306-317
if (dependencies.empty())
{
    if (!contains(defaulted_keys, "depends"))
    {
        j["depends"] = nlohmann::json::array();
    }
}
```

This means:

- If `depends` is empty AND "depends" is NOT in `defaulted_keys` → include `[]` in signable JSON
- If `depends` is empty AND "depends" IS in `defaulted_keys` → omit from signable (it's a stub)

**Our changes maintain correct behavior**:

- URL-derived packages: `defaulted_keys` includes "depends" → empty depends not signed (correct)
- Solver-derived packages: `defaulted_keys` only has `_initialized` → empty depends IS signed (correct for patches)

### Serialization Rules

**`defaulted_keys` must NEVER be serialized to JSON.**

- It is an in-memory field for controlling merge behavior during extraction
- It is not written to `repodata_record.json`
- It is not read from JSON (always empty when deserialized)
- The current `to_json()` implementation correctly omits it

### ABI Stability

All changes are implementation-only (`.cpp` files). No headers are modified. The `_initialized` sentinel is a runtime convention, not a struct change.
