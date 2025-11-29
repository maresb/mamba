# Specification: Revise Patches for Issue #4095

<!-- markdownlint-disable MD013 MD024 -->

## Overview

This document specifies how to revise the 13 existing patches into 8 well-structured commits that:

1. Follow TDD (test before fix)
2. Keep parallel code paths (`package_fetcher.cpp` and `constructor.cpp`) together
3. Incorporate all identified fixes and clarifications
4. Maintain comprehensive commit messages

## Prerequisites

### Build Environment

Follow the micromamba static compilation instructions in `docs/source/installation/micromamba-installation.rst`:

```bash
# Create the static build environment
micromamba create -n build_env -f dev/environment-micromamba-static.yml

# Activate and build
micromamba activate build_env
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_LIBMAMBA=ON -DBUILD_STATIC=ON \
    -DBUILD_MICROMAMBA=ON -DMICROMAMBA_LINKAGE=FULL_STATIC
make -j$(nproc)
```

For development/testing with shared libraries:

```bash
micromamba create -n mamba -c conda-forge -f dev/environment-dev.yml
micromamba run -n mamba cmake -B build/ -G Ninja --preset mamba-unix-shared-debug-dev
micromamba run -n mamba cmake --build build/ --parallel
```

### Source Patches

All source patches are located in `cursor-context/patches/`:

| Patch | Description                                  |
| ----- | -------------------------------------------- |
| 0001  | test: defaulted_keys population              |
| 0002  | fix: from_url() defaulted_keys               |
| 0003  | fix: make_package_info() \_initialized       |
| 0004  | test: URL metadata + channel patches         |
| 0005  | fix: write_repodata_record()                 |
| 0006  | test: cache healing                          |
| 0007  | fix: has_valid_extracted_dir()               |
| 0008  | fix: constructor.cpp defaulted_keys          |
| 0009  | test: consistent fields                      |
| 0010  | fix: consistent fields package_fetcher       |
| 0011  | **DELETE** - pixi.lock/pixi.toml (unrelated) |
| 0012  | fix: consistent fields constructor           |
| 0013  | test: Python integration tests               |

---

## New Commit Structure

| New # | Source Patches               | Description                                                     |
| ----- | ---------------------------- | --------------------------------------------------------------- |
| 01    | 0001 (modified)              | test: defaulted_keys population (RED)                           |
| 02    | 0002 + 0003 (modified)       | fix: defaulted_keys in from_url() + make_package_info() (GREEN) |
| 03    | 0004 + 0013 tests (modified) | test: URL metadata + channel patches + constructor (RED)        |
| 04    | 0005 + 0008 (modified)       | fix: write_repodata_record() + constructor.cpp (GREEN)          |
| 05    | 0006 (modified)              | test: cache healing (RED)                                       |
| 06    | 0007 (modified)              | fix: has_valid_extracted_dir() (GREEN)                          |
| 07    | 0009 (modified + new tests)  | test: consistent fields + checksums (RED)                       |
| 08    | 0010 + 0012 (modified)       | fix: consistent fields + checksums (GREEN)                      |

---

## Commit 01: test: add tests for defaulted_keys population (RED)

### Source

- **Base:** Patch 0001

### Files to Modify

- `libmamba/tests/src/specs/test_package_info.cpp`

### Steps

1. **Apply patch 0001:**

   ```bash
   git apply cursor-context/patches/0001-test-add-tests-for-defaulted_keys-population-RED.patch
   ```

2. **Modify the git URL test section** to verify ALL expected fields.

   In the `SECTION("Git URL has correct defaulted_keys")` block, ADD these assertions after the existing ones:

   ```cpp
   // Git URLs should also have these fields defaulted (not just the common ones)
   // Git URLs only provide package_url and optionally name from #egg=
   CHECK(contains(pkg.defaulted_keys, "version"));
   CHECK(contains(pkg.defaulted_keys, "channel"));
   CHECK(contains(pkg.defaulted_keys, "subdir"));
   CHECK(contains(pkg.defaulted_keys, "fn"));
   ```

3. **Build and run tests:**

   ```bash
   micromamba run -n mamba cmake --build build/ --parallel
   micromamba run -n mamba ./build/libmamba/tests/test_libmamba "[from_url populates defaulted_keys]"
   ```

4. **Verify:** ALL tests should FAIL (defaulted_keys not populated yet)

### Commit Message

```text
test: add tests for defaulted_keys population (RED)

Add comprehensive tests verifying that PackageInfo::from_url() populates
the defaulted_keys field to mark which fields have stub/default values.

PURPOSE: Verify defaulted_keys is correctly populated for all URL types
MOTIVATION: Issue #4095 - URL-derived packages write stub defaults instead
of using correct values from index.json. The fix uses defaulted_keys with
"_initialized" sentinel for fail-hard verification.

Tests added for:
- Conda packages (.conda, .tar.bz2): Standard stub fields
- Wheel packages (.whl): Standard + build/build_string
- TarGz packages (.tar.gz): Same as wheels
- Git URLs (git+https://...): Most fields defaulted including version,
  channel, subdir, fn (git URLs provide minimal info)

Each test verifies "_initialized" sentinel presence (required for all paths)
plus appropriate stub field markers.

Test Status: ALL TESTS FAIL (expected RED phase)
- Failure: defaulted_keys is empty for all package types

Related: https://github.com/mamba-org/mamba/issues/4095
```

---

## Commit 02: fix: populate defaulted_keys in from_url() and make_package_info() (GREEN)

### Source

- **Base:** Patches 0002 + 0003

### Files to Modify

- `libmamba/src/specs/package_info.cpp`
- `libmamba/src/solver/libsolv/helpers.cpp`

### Steps

1. **Apply patch 0002:**

   ```bash
   git apply cursor-context/patches/0002-fix-populate-defaulted_keys-in-from_url-with-_initia.patch
   ```

2. **Fix the git URL defaulted_keys** in `package_info.cpp`.

   Find the git URL handling section (around line 278 after patch) and REPLACE the defaulted_keys assignment:

   ```cpp
   // Mark fields that have stub/default values for git URL packages.
   // Git URLs only provide package_url and optionally name (from #egg=).
   // All other fields use struct defaults. See issue #4095.
   pkg.defaulted_keys = { "_initialized", "version",       "channel",
                          "subdir",       "fn",            "build",
                          "build_string", "build_number",  "license",
                          "timestamp",    "track_features", "depends",
                          "constrains" };
   ```

3. **Apply patch 0003:**

   ```bash
   git apply cursor-context/patches/0003-fix-add-_initialized-to-make_package_info-for-solver.patch
   ```

4. **Enhance the comment in helpers.cpp** (the patch already has a good comment, verify it includes):

   ```cpp
   // Set "_initialized" sentinel to indicate this PackageInfo was properly constructed.
   // Solver-derived packages have authoritative metadata from channel repodata (potentially
   // patched). We set only _initialized (no stub field names) to signal "trust all fields".
   // This preserves channel patches with intentionally empty arrays.
   //
   // The asymmetry with from_url() is intentional:
   // - from_url(): {"_initialized", "license", ...} = listed fields are stubs to erase
   // - make_package_info(): {"_initialized"} = trust ALL fields, erase nothing
   //
   // See issue #4095.
   out.defaulted_keys = { "_initialized" };
   ```

5. **Build and run tests:**

   ```bash
   micromamba run -n mamba cmake --build build/ --parallel
   micromamba run -n mamba ./build/libmamba/tests/test_libmamba "[from_url populates defaulted_keys]"
   ```

6. **Verify:** ALL tests from Commit 01 should now PASS

### Commit Message

```text
fix: populate defaulted_keys in from_url() and make_package_info() (GREEN)

Populate the defaulted_keys field in PackageInfo creation paths to mark
which fields have stub/default values. This enables write_repodata_record()
to erase these stubs before merging with index.json.

The "_initialized" sentinel is placed first in defaulted_keys for all paths.
This enables fail-hard verification: write_repodata_record() will throw if
_initialized is missing, catching any code path that fails to set defaulted_keys.

Changes in from_url() (package_info.cpp):
- Conda (.conda, .tar.bz2): _initialized + build_number, license, timestamp,
  track_features, depends, constrains
- Wheel (.whl): Above + build, build_string (not in wheel filename)
- TarGz (.tar.gz): Same as wheels
- Git URLs: _initialized + version, channel, subdir, fn, build, build_string,
  build_number, license, timestamp, track_features, depends, constrains
  (git URLs provide minimal info - only package_url and optionally name)

Changes in make_package_info() (helpers.cpp):
- Set defaulted_keys = {"_initialized"} for solver-derived packages
- Solver packages have authoritative metadata from channel repodata
- Only _initialized signals "trust all fields" (including patches)

The asymmetry is intentional:
- URL-derived: list stub fields to erase
- Solver-derived: trust all fields, erase nothing

Test Status: ALL TESTS PASS (GREEN phase)
- test cases: 1 | 1 passed
- assertions: 50+ | all passed

Tests fixed:
- PackageInfo::from_url populates defaulted_keys (all 5 sections)

Related: https://github.com/mamba-org/mamba/issues/4095
```

---

## Commit 03: test: add tests for URL-derived metadata and channel patch preservation (RED)

### Source

- **Base:** Patch 0004 + test portions of 0013

### Files to Modify

- `libmamba/tests/src/core/test_package_fetcher.cpp`
- `micromamba/tests/test_constructor.py`

### Steps

1. **Apply patch 0004:**

   ```bash
   git apply cursor-context/patches/0004-test-add-tests-for-URL-derived-metadata-channel-patc.patch
   ```

2. **Update the "BUG:" comments** in `test_package_fetcher.cpp` to describe expected behavior.

   Find and replace ALL instances of comments like:

   ```cpp
   // BUG: These will FAIL - gets stub defaults instead of index.json values
   // After fix: Should use correct values from index.json
   ```

   With:

   ```cpp
   // Verify repodata_record.json contains correct values from index.json
   // Issue #4095: URL-derived packages should NOT have stub defaults
   ```

   Similarly for channel patch tests:

   ```cpp
   // BUG: This will FAIL - the empty array patch gets overwritten with index.json value
   // After fix: Should preserve the intentionally empty depends from the patch
   ```

   Replace with:

   ```cpp
   // Verify channel patch is preserved (empty depends NOT replaced by index.json)
   // Issue #4095: Solver-derived packages with defaulted_keys={"_initialized"}
   // should trust ALL fields including intentionally empty arrays
   ```

3. **Apply the test portions from patch 0013.**

   Extract and apply ONLY the test class additions from patch 0013 to `micromamba/tests/test_constructor.py`.

   Apply the full patch first:

   ```bash
   git apply cursor-context/patches/0013-test-add-Python-integration-tests-for-URL-derived-me.patch
   ```

4. **Fix the imports** in `test_constructor.py`:

   REMOVE the unused import:

   ```python
   # DELETE this line:
   from io import BytesIO
   ```

5. **Fix the env var restoration** in `test_constructor.py`:

   REPLACE the `teardown_class` method:

   ```python
   @classmethod
   def teardown_class(cls):
       """Clean up test directory and restore env vars."""
       shutil.rmtree(cls.temp_dir)

       # Properly restore env vars (delete if originally unset)
       if cls.orig_root_prefix is not None:
           os.environ["MAMBA_ROOT_PREFIX"] = cls.orig_root_prefix
       else:
           os.environ.pop("MAMBA_ROOT_PREFIX", None)
       if cls.orig_prefix is not None:
           os.environ["CONDA_PREFIX"] = cls.orig_prefix
       else:
           os.environ.pop("CONDA_PREFIX", None)
   ```

6. **Build and run C++ tests:**

   ```bash
   micromamba run -n mamba cmake --build build/ --parallel
   micromamba run -n mamba ./build/libmamba/tests/test_libmamba "[write_repodata_record]"
   ```

7. **Verify C++ tests:** Should FAIL (write_repodata_record fix not yet applied)

8. **Note:** Python tests cannot be verified until constructor.cpp is fixed (Commit 04)

### Commit Message

```text
test: add tests for URL-derived metadata and channel patch preservation (RED)

Add tests demonstrating bugs in write_repodata_record() and constructor.cpp:

C++ Tests (test_package_fetcher.cpp):

1. URL-derived metadata test:
   - Creates PackageInfo from URL (has stub defaults)
   - Creates tarball with correct index.json values
   - EXPECTS: repodata_record.json has correct values from index.json
   - Tests: license, timestamp, build_number

2. Channel patch preservation (depends):
   - Creates solver-derived PackageInfo with depends=[]
   - defaulted_keys = {"_initialized"} (only sentinel = trust all)
   - EXPECTS: Empty depends preserved in repodata_record.json
   - Verifies channel patches are NOT overwritten by index.json

3. Channel patch preservation (constrains):
   - Same as above but for constrains field

Python Integration Tests (test_constructor.py):

1. test_url_derived_metadata_from_index_json:
   - Verifies constructor.cpp extracts URL packages correctly
   - Checks license, timestamp, build_number, depends, constrains

2. test_consistent_field_presence:
   - Verifies depends/constrains always present as arrays

3. test_track_features_omitted_when_empty:
   - Verifies track_features omitted when empty (matches conda)

Test Status: C++ TESTS FAIL (expected RED phase)
- URL metadata: license="", timestamp=0, build_number=0 (stubs)
- Channel patches: depends overwritten by index.json

Related: https://github.com/mamba-org/mamba/issues/4095
```

---

## Commit 04: fix: use defaulted_keys in write_repodata_record() and constructor.cpp (GREEN)

### Source

- **Base:** Patches 0005 + 0008

### Files to Modify

- `libmamba/src/core/package_fetcher.cpp`
- `micromamba/src/constructor.cpp`

### Steps

1. **Apply patch 0005:**

   ```bash
   git apply cursor-context/patches/0005-fix-use-defaulted_keys-in-write_repodata_record-with.patch
   ```

2. **Enhance the comment block** at the start of the defaulted_keys handling in `package_fetcher.cpp`:

   Find the `// FAIL-HARD CHECK:` comment and REPLACE the entire comment block with:

   ```cpp
   // Write repodata_record.json with correct metadata from index.json.
   //
   // The _initialized sentinel in defaulted_keys proves this PackageInfo was
   // properly constructed. All creation paths MUST set it:
   // - from_url(): sets {"_initialized", stub_field_names...}
   // - make_package_info(): sets {"_initialized"} only (trust all fields)
   //
   // Note: from_json() does NOT set _initialized because it deserializes
   // already-written cache files for display/query purposes. Those PackageInfo
   // objects are never passed to this function - they're used for mamba list,
   // dependency computation, etc.
   //
   // If _initialized is missing, it indicates a bug in a code path that creates
   // PackageInfo objects destined for extraction. We fail hard to catch this.
   //
   // See GitHub issue #4095.
   ```

3. **Apply patch 0008:**

   ```bash
   git apply cursor-context/patches/0008-fix-apply-defaulted_keys-handling-to-constructor.cpp.patch
   ```

4. **Enhance the comment block** in `constructor.cpp`:

   Find the defaulted_keys handling section and REPLACE the comment with:

   ```cpp
   // Handle URL-derived packages using defaulted_keys mechanism.
   //
   // NOTE: Unlike write_repodata_record() in package_fetcher.cpp, we do NOT
   // fail-hard when _initialized is missing. This is INTENTIONAL asymmetry:
   //
   // - package_fetcher.cpp: PackageInfo always comes from from_url() or
   //   make_package_info(), both of which set _initialized. Missing sentinel
   //   indicates a bug.
   //
   // - constructor.cpp: PackageInfo may come from cached repodata files
   //   (via from_json()), which don't have _initialized. This is correct -
   //   cached repodata should be trusted (it may contain channel patches).
   //
   // When _initialized IS present: URL-derived package, erase stub fields
   // When _initialized is ABSENT: cached repodata, trust all fields
   //
   // The healing mechanism in has_valid_extracted_dir() handles corrupted
   // caches from v2.1.1-v2.4.0 by detecting the corruption signature and
   // triggering re-extraction.
   //
   // See GitHub issue #4095.
   ```

5. **Build and run tests:**

   ```bash
   micromamba run -n mamba cmake --build build/ --parallel
   micromamba run -n mamba ./build/libmamba/tests/test_libmamba "[write_repodata_record]"
   ```

6. **Verify C++ tests:** All tests from Commit 03 should now PASS

7. **Run Python tests** (requires static build for micromamba):

   ```bash
   # Build static micromamba
   micromamba activate build_env
   cd build && make -j$(nproc) && cd ..

   # Run Python tests
   TEST_MAMBA_EXE=$(pwd)/build/micromamba/micromamba \
     python -m pytest micromamba/tests/test_constructor.py -v
   ```

8. **Verify Python tests:** All TestURLDerivedMetadata tests should PASS

### Commit Message

```text
fix: use defaulted_keys in write_repodata_record() and constructor.cpp (GREEN)

Fix URL-derived package corruption and preserve channel patches by using
the defaulted_keys mechanism in both write_repodata_record() and constructor.cpp.

Implementation in package_fetcher.cpp:

1. FAIL-HARD CHECK: Verify "_initialized" sentinel is present
   - Throws std::logic_error if missing
   - Catches any code path that fails to set defaulted_keys
   - Note: from_json() intentionally doesn't set _initialized (it deserializes
     cache files for display/query, not for writing new records)

2. ERASE STUB FIELDS: Remove fields listed in defaulted_keys before merge
   - URL-derived: erases build_number, license, timestamp, etc.
   - Solver-derived: defaulted_keys = {"_initialized"}, nothing erased
   - This preserves channel patches with intentionally empty arrays

3. MERGE WITH INDEX.JSON: insert() fills erased fields
   - URL-derived: gets correct values from index.json
   - Solver-derived: all fields already present, index.json ignored

Implementation in constructor.cpp:

Same defaulted_keys erasure logic BUT no fail-hard check. This is intentional:
- constructor may receive PackageInfo from cached repodata (via from_json())
- Cached repodata doesn't have _initialized, and that's correct
- Cached data should be trusted (may contain channel patches)

Why this works:
- URL packages: {"_initialized", "license", ...} → stubs erased → index.json fills
- Solver packages: {"_initialized"} → nothing erased → patch preserved
- Cached packages: no _initialized → nothing erased → all fields trusted

Test Status: ALL TESTS PASS (GREEN phase)
- C++ test cases: 4+ | all passed
- Python test cases: 4 | all passed

Tests fixed:
- write_repodata_record uses index.json for URL-derived metadata
- write_repodata_record preserves channel patched empty depends
- write_repodata_record preserves channel patched empty constrains
- TestURLDerivedMetadata (all 3 Python tests)

Related: https://github.com/mamba-org/mamba/issues/4095
```

---

## Commit 05: test: add test for cache healing (RED)

### Source

- **Base:** Patch 0006

### Files to Modify

- `libmamba/tests/src/core/test_package_fetcher.cpp`

### Steps

1. **Apply patch 0006:**

   ```bash
   git apply cursor-context/patches/0006-test-add-test-for-cache-healing-RED.patch
   ```

2. **Update the comments** in the test to describe expected behavior:

   Find and replace:

   ```cpp
   // BUG: With current code, has_valid_extracted_dir() returns true despite corruption
   // because it only validates checksums, not metadata correctness.
   // So needs_extract() returns false and the corrupted cache is used as-is.
   // After fix: should return true (cache invalid, needs re-extraction)
   ```

   With:

   ```cpp
   // Verify healing: corrupted cache should be detected and invalidated.
   // The corruption signature (timestamp=0 AND license="") triggers
   // has_valid_extracted_dir() to return false, causing re-extraction.
   // Issue #4095: This heals caches corrupted by v2.1.1-v2.4.0.
   ```

   And replace:

   ```cpp
   // BUG: These will FAIL because cache wasn't invalidated, so re-extraction didn't happen
   // After fix: correct values from index.json should be present
   ```

   With:

   ```cpp
   // After healing: correct values from index.json should be present
   ```

3. **Build and run tests:**

   ```bash
   micromamba run -n mamba cmake --build build/ --parallel
   micromamba run -n mamba ./build/libmamba/tests/test_libmamba "[heals existing corrupted cache]"
   ```

4. **Verify:** Test should FAIL (healing not yet implemented)

### Commit Message

```text
test: add test for cache healing (RED)

Add test demonstrating that EXISTING corrupted caches (from v2.1.1-v2.4.0)
are NOT detected and healed by current code.

The bug: has_valid_extracted_dir() only validates checksums, not metadata
correctness. Corrupted caches with stub values (timestamp=0, license="")
are considered valid and used as-is.

Test scenario:
1. Create package tarball with CORRECT index.json (MIT license, etc.)
2. Create CORRUPTED repodata_record.json in cache (simulating bug)
   - timestamp=0, license="", build_number=0
3. Create PackageFetcher
4. EXPECT: needs_extract() returns true (corruption detected)
5. ACTUAL: returns false (corruption NOT detected)

Healing flow (after fix):
1. has_valid_extracted_dir() detects corruption signature
2. Returns valid=false → needs_extract() returns true
3. Package re-extracted from tarball
4. write_repodata_record() writes correct values

Test Status: TEST FAILS (expected RED phase)
- needs_extract() returns false (should return true)

Related: https://github.com/mamba-org/mamba/issues/4095
```

---

## Commit 06: fix: detect corrupted cache in has_valid_extracted_dir() (GREEN)

### Source

- **Base:** Patch 0007

### Files to Modify

- `libmamba/src/core/package_cache.cpp`

### Steps

1. **Apply patch 0007:**

   ```bash
   git apply cursor-context/patches/0007-fix-detect-corrupted-cache-in-has_valid_extracted_di.patch
   ```

2. **Enhance the comment** explaining false-positive safety:

   Find the healing comment and REPLACE with:

   ```cpp
   // HEALING: Detect corrupted cache from buggy versions (v2.1.1-v2.4.0).
   //
   // Corruption signature: timestamp == 0 AND license == ""
   //
   // Why this signature is safe (low false-positive risk):
   // - Both conditions must be true (very specific)
   // - Modern build tools virtually always set timestamps
   // - Packages typically have license information
   // - The combination is extremely rare for legitimate packages
   //
   // Even if a false positive occurs:
   // - The only consequence is unnecessary re-extraction
   // - This is wasted work, NOT data corruption
   // - The re-extracted package will have correct metadata
   //
   // When corruption is detected:
   // 1. Return valid=false to invalidate this cache entry
   // 2. Caller triggers re-extraction from tarball
   // 3. write_repodata_record() writes correct values using index.json
   //
   // See GitHub issue #4095.
   if (valid && repodata_record.contains("timestamp")
       && repodata_record["timestamp"] == 0 && repodata_record.contains("license")
       && repodata_record["license"] == "")
   {
       LOG_INFO << "Detected corrupted metadata in cache (v2.1.1-v2.4.0 bug, "
                   "issue #4095), will re-extract: "
                << extracted_dir.string();
       valid = false;
   }
   ```

3. **Build and run tests:**

   ```bash
   micromamba run -n mamba cmake --build build/ --parallel
   micromamba run -n mamba ./build/libmamba/tests/test_libmamba "[heals existing corrupted cache]"
   ```

4. **Verify:** Test from Commit 05 should now PASS

### Commit Message

```text
fix: detect corrupted cache in has_valid_extracted_dir() (GREEN)

Add corruption detection to has_valid_extracted_dir() to heal caches
corrupted by v2.1.1-v2.4.0 bug.

Corruption signature: timestamp == 0 AND license == ""

When detected:
1. Log info message about the corruption detection
2. Return valid=false to invalidate the cache entry
3. This triggers re-extraction from the tarball
4. write_repodata_record() then writes correct values using index.json

Why this signature is reliable (low false-positive risk):
- timestamp=0 is extremely rare for legitimate packages (build tools set it)
- Both conditions must be true (reduces false positives significantly)
- license="" alone could be legitimate (some packages have no license)
- timestamp=0 alone could theoretically be legitimate
- Together they reliably indicate v2.1.1-v2.4.0 corruption

Even if a false positive occurs:
- Only consequence is unnecessary re-extraction (wasted work)
- NOT data corruption - re-extracted package has correct values
- This is acceptable given the rarity of the combination

The detection is placed AFTER all other validations to ensure it's the
final check before returning valid status.

Test Status: ALL TESTS PASS (GREEN phase)
- test cases: 5+ | all passed

Tests fixed:
- PackageFetcher heals existing corrupted cache

Related: https://github.com/mamba-org/mamba/issues/4095
```

---

## Commit 07: test: add tests for consistent field presence and checksums (RED)

### Source

- **Base:** Patch 0009 + new tests

### Files to Modify

- `libmamba/tests/src/core/test_package_fetcher.cpp`

### Steps

1. **Apply patch 0009:**

   ```bash
   git apply cursor-context/patches/0009-test-add-tests-for-consistent-field-presence-and-che.patch
   ```

2. **Update comments** to describe expected behavior (not bugs):

   Replace "BUG:" style comments with descriptive ones throughout.

3. **Add new test cases** after the existing ones in `test_package_fetcher.cpp`:

   ```cpp
   /**
    * Test that noarch and python_site_packages_path are backfilled from index.json
    *
    * PURPOSE: Verify fields that to_json() conditionally writes are correctly
    * filled from index.json via insert().
    *
    * These fields are NOT in defaulted_keys because to_json() omits them when
    * unset, allowing insert() to add them naturally from index.json.
    */
   TEST_CASE("PackageFetcher::write_repodata_record backfills noarch")
   {
       auto& ctx = mambatests::context();
       TemporaryDirectory temp_dir;
       MultiPackageCache package_caches{ { temp_dir.path() / "pkgs" }, ctx.validation_params };

       static constexpr std::string_view url = "https://conda.anaconda.org/conda-forge/noarch/noarchpkg-1.0-pyhd8ed1ab_0.conda";
       auto pkg_info = specs::PackageInfo::from_url(url).value();

       const std::string pkg_basename = "noarchpkg-1.0-pyhd8ed1ab_0";

       auto pkg_extract_dir = temp_dir.path() / "pkgs" / pkg_basename;
       auto info_dir = pkg_extract_dir / "info";
       fs::create_directories(info_dir);

       // Create index.json with noarch field
       nlohmann::json index_json;
       index_json["name"] = "noarchpkg";
       index_json["version"] = "1.0";
       index_json["build"] = "pyhd8ed1ab_0";
       index_json["noarch"] = "python";

       {
           std::ofstream index_file((info_dir / "index.json").std_path());
           index_file << index_json.dump(2);
       }

       {
           std::ofstream paths_file((info_dir / "paths.json").std_path());
           paths_file << R"({"paths": [], "paths_version": 1})";
       }

       auto tarball_path = temp_dir.path() / "pkgs" / (pkg_basename + ".tar.bz2");
       create_archive(pkg_extract_dir, tarball_path, compression_algorithm::bzip2, 1, 1, nullptr);
       REQUIRE(fs::exists(tarball_path));

       auto modified_pkg_info = pkg_info;
       modified_pkg_info.filename = pkg_basename + ".tar.bz2";

       fs::remove_all(pkg_extract_dir);

       PackageFetcher pkg_fetcher{ modified_pkg_info, package_caches };

       ExtractOptions options;
       options.sparse = false;
       options.subproc_mode = extract_subproc_mode::mamba_package;

       bool extract_success = pkg_fetcher.extract(options);
       REQUIRE(extract_success);

       auto repodata_record_path = pkg_extract_dir / "info" / "repodata_record.json";
       REQUIRE(fs::exists(repodata_record_path));

       std::ifstream repodata_file(repodata_record_path.std_path());
       nlohmann::json repodata_record;
       repodata_file >> repodata_record;

       // noarch should be backfilled from index.json
       REQUIRE(repodata_record.contains("noarch"));
       CHECK(repodata_record["noarch"] == "python");
   }

   /**
    * Test that size is filled from tarball when zero
    *
    * PURPOSE: Verify existing size handling behavior continues to work.
    */
   TEST_CASE("PackageFetcher::write_repodata_record fills size from tarball")
   {
       auto& ctx = mambatests::context();
       TemporaryDirectory temp_dir;
       MultiPackageCache package_caches{ { temp_dir.path() / "pkgs" }, ctx.validation_params };

       static constexpr std::string_view url = "https://conda.anaconda.org/conda-forge/linux-64/sizepkg-1.0-h0_0.conda";
       auto pkg_info = specs::PackageInfo::from_url(url).value();

       // Verify precondition: size is 0 from URL parsing
       REQUIRE(pkg_info.size == 0);

       const std::string pkg_basename = "sizepkg-1.0-h0_0";

       auto pkg_extract_dir = temp_dir.path() / "pkgs" / pkg_basename;
       auto info_dir = pkg_extract_dir / "info";
       fs::create_directories(info_dir);

       nlohmann::json index_json;
       index_json["name"] = "sizepkg";
       index_json["version"] = "1.0";
       index_json["build"] = "h0_0";

       {
           std::ofstream index_file((info_dir / "index.json").std_path());
           index_file << index_json.dump(2);
       }

       {
           std::ofstream paths_file((info_dir / "paths.json").std_path());
           paths_file << R"({"paths": [], "paths_version": 1})";
       }

       auto tarball_path = temp_dir.path() / "pkgs" / (pkg_basename + ".tar.bz2");
       create_archive(pkg_extract_dir, tarball_path, compression_algorithm::bzip2, 1, 1, nullptr);
       REQUIRE(fs::exists(tarball_path));

       auto tarball_size = fs::file_size(tarball_path);
       REQUIRE(tarball_size > 0);

       auto modified_pkg_info = pkg_info;
       modified_pkg_info.filename = pkg_basename + ".tar.bz2";

       fs::remove_all(pkg_extract_dir);

       PackageFetcher pkg_fetcher{ modified_pkg_info, package_caches };

       ExtractOptions options;
       options.sparse = false;
       options.subproc_mode = extract_subproc_mode::mamba_package;

       bool extract_success = pkg_fetcher.extract(options);
       REQUIRE(extract_success);

       auto repodata_record_path = pkg_extract_dir / "info" / "repodata_record.json";
       std::ifstream repodata_file(repodata_record_path.std_path());
       nlohmann::json repodata_record;
       repodata_file >> repodata_record;

       // Size should be filled from tarball
       REQUIRE(repodata_record.contains("size"));
       CHECK(repodata_record["size"] == tarball_size);
   }

   /**
    * Test no false positive healing
    *
    * PURPOSE: Verify packages with timestamp=0 but valid metadata are NOT healed.
    * A package with timestamp=0 but license="MIT" should NOT be re-extracted.
    */
   TEST_CASE("PackageFetcher no false positive cache healing")
   {
       auto& ctx = mambatests::context();
       TemporaryDirectory temp_dir;
       MultiPackageCache package_caches{ { temp_dir.path() / "pkgs" }, ctx.validation_params };

       static constexpr std::string_view url = "https://conda.anaconda.org/conda-forge/linux-64/nofp-1.0-h0_0.tar.bz2";
       auto pkg_info = specs::PackageInfo::from_url(url).value();

       const std::string pkg_basename = "nofp-1.0-h0_0";

       auto pkg_extract_dir = temp_dir.path() / "pkgs" / pkg_basename;
       auto info_dir = pkg_extract_dir / "info";
       fs::create_directories(info_dir);

       nlohmann::json index_json;
       index_json["name"] = "nofp";
       index_json["version"] = "1.0";
       index_json["build"] = "h0_0";

       {
           std::ofstream index_file((info_dir / "index.json").std_path());
           index_file << index_json.dump(2);
       }

       {
           std::ofstream paths_file((info_dir / "paths.json").std_path());
           paths_file << R"({"paths": [], "paths_version": 1})";
       }

       auto tarball_path = temp_dir.path() / "pkgs" / (pkg_basename + ".tar.bz2");
       create_archive(pkg_extract_dir, tarball_path, compression_algorithm::bzip2, 1, 1, nullptr);
       REQUIRE(fs::exists(tarball_path));

       // Create a repodata_record.json with timestamp=0 BUT license="MIT"
       // This should NOT trigger healing because license is not empty
       nlohmann::json special_repodata;
       special_repodata["name"] = "nofp";
       special_repodata["version"] = "1.0";
       special_repodata["build"] = "h0_0";
       special_repodata["timestamp"] = 0;       // Could trigger healing...
       special_repodata["license"] = "MIT";     // ...but this prevents it
       special_repodata["build_number"] = 5;    // Non-stub value
       special_repodata["fn"] = pkg_basename + ".tar.bz2";
       special_repodata["depends"] = nlohmann::json::array({ "python" });
       special_repodata["constrains"] = nlohmann::json::array();

       {
           std::ofstream repodata_file((info_dir / "repodata_record.json").std_path());
           repodata_file << special_repodata.dump(2);
       }

       auto modified_pkg_info = pkg_info;
       modified_pkg_info.filename = pkg_basename + ".tar.bz2";

       PackageFetcher pkg_fetcher{ modified_pkg_info, package_caches };

       // Should NOT need extraction (not corrupted despite timestamp=0)
       CHECK_FALSE(pkg_fetcher.needs_extract());
   }
   ```

4. **Build and run tests:**

   ```bash
   micromamba run -n mamba cmake --build build/ --parallel
   micromamba run -n mamba ./build/libmamba/tests/test_libmamba "[ensures depends/constrains],[ensures both checksums]"
   ```

5. **Verify:**
   - depends/constrains test: FAILS (not yet adding empty arrays)
   - checksums test: FAILS (not yet computing)
   - noarch test: PASSES (validates existing behavior)
   - size test: PASSES (validates existing behavior)
   - false positive test: PASSES (validates healing specificity)

### Commit Message

```text
test: add tests for consistent field presence and checksums (RED)

Add tests verifying repodata_record.json consistency and checksum handling:

New/Modified Tests:

1. depends/constrains presence test:
   - Tests that depends and constrains are always present as arrays
   - Even when index.json lacks these fields (like nlohmann_json-abi)
   - Matches conda behavior for field consistency
   - Status: FAILS (depends key not present when missing from index.json)

2. track_features omission test:
   - Tests that track_features is omitted when empty
   - Matches conda behavior to reduce JSON noise
   - Status: PASSES (current behavior already correct)

3. both checksums presence test:
   - Tests that both md5 and sha256 are always present
   - Should compute checksums from tarball when not available
   - Status: FAILS (md5/sha256 not computed when missing)

4. noarch backfill test:
   - Tests that noarch field is backfilled from index.json
   - Validates existing merge behavior for conditionally-written fields
   - Status: PASSES (validates assumption)

5. size from tarball test:
   - Tests that size=0 is filled from actual tarball file size
   - Validates existing special-case handling
   - Status: PASSES (validates assumption)

6. no false positive healing test:
   - Tests that timestamp=0 with license="MIT" is NOT healed
   - Verifies corruption signature requires BOTH conditions
   - Status: PASSES (validates healing specificity)

Test Status: PARTIAL FAILURES (expected RED phase)
- depends/constrains: FAIL
- checksums: FAIL
- Others: PASS (validate assumptions)

Related: https://github.com/mamba-org/mamba/issues/4095
```

---

## Commit 08: fix: ensure consistent field presence and compute checksums (GREEN)

### Source

- **Base:** Patches 0010 + 0012

### Files to Modify

- `libmamba/src/core/package_fetcher.cpp`
- `micromamba/src/constructor.cpp`

### Steps

1. **Apply patch 0010:**

   ```bash
   git apply cursor-context/patches/0010-fix-ensure-consistent-field-presence-and-compute.patch
   ```

2. **Fix the md5/sha256 type checking** in `package_fetcher.cpp`:

   Find the checksum handling and REPLACE with:

   ```cpp
   // Ensure both md5 and sha256 checksums are always present.
   // Compute from tarball if not available from PackageInfo or index.json.
   // Use is_string() check to handle null or non-string values safely.
   // See GitHub issue #4095.
   auto needs_md5 = !repodata_record.contains("md5")
                    || !repodata_record["md5"].is_string()
                    || repodata_record["md5"].get<std::string>().empty();
   if (needs_md5)
   {
       repodata_record["md5"] = validation::md5sum(m_tarball_path);
   }

   auto needs_sha256 = !repodata_record.contains("sha256")
                       || !repodata_record["sha256"].is_string()
                       || repodata_record["sha256"].get<std::string>().empty();
   if (needs_sha256)
   {
       repodata_record["sha256"] = validation::sha256sum(m_tarball_path);
   }
   ```

3. **Apply patch 0012:**

   ```bash
   git apply cursor-context/patches/0012-fix-add-consistent-field-handling-to-constructor.cpp.patch
   ```

4. **Build and run all tests:**

   ```bash
   micromamba run -n mamba cmake --build build/ --parallel
   micromamba run -n mamba ./build/libmamba/tests/test_libmamba
   ```

5. **Verify:** ALL tests should PASS

6. **Run Python tests:**

   ```bash
   TEST_MAMBA_EXE=$(pwd)/build/micromamba/micromamba \
     python -m pytest micromamba/tests/test_constructor.py -v
   ```

7. **Verify:** ALL Python tests should PASS

### Commit Message

```text
fix: ensure consistent field presence and compute missing checksums (GREEN)

Ensure repodata_record.json has consistent field presence and checksums
matching conda behavior.

Changes in package_fetcher.cpp write_repodata_record():

1. depends/constrains always present:
   - Add empty arrays if fields are missing after merge
   - Some packages (like nlohmann_json-abi) lack these in index.json
   - Matches conda's expected record structure

2. track_features conditionally included:
   - Remove field if empty (null, "", or [])
   - Reduces JSON noise for majority of packages
   - Matches conda behavior

3. Both checksums always present:
   - Compute md5 from tarball if missing/empty/non-string
   - Compute sha256 from tarball if missing/empty/non-string
   - Added is_string() check to handle null values safely
   - Ensures cache validation always has checksums

Changes in constructor.cpp:

Same depends/constrains/track_features handling for consistency.

Note: Checksum computation NOT added to constructor.cpp because it
handles checksums differently:
- md5 comes from URL fragment in urls file (e.g., url#hash)
- If repodata cache exists, checksums come from there
- Only fallback case uses index.json (which typically lacks checksums)

Test Status: ALL TESTS PASS (GREEN phase)
- C++ tests: 10+ test cases, 50+ assertions
- Python tests: 4 test cases

Tests fixed:
- ensures depends/constrains present
- ensures both checksums
- All PackageFetcher tests pass
- All TestURLDerivedMetadata tests pass

Related: https://github.com/mamba-org/mamba/issues/4095
```

---

## Final Verification

After all commits are applied:

1. **Run full C++ test suite:**

   ```bash
   micromamba run -n mamba ./build/libmamba/tests/test_libmamba
   ```

2. **Run Python tests:**

   ```bash
   TEST_MAMBA_EXE=$(pwd)/build/micromamba/micromamba \
     python -m pytest micromamba/tests/test_constructor.py -v
   ```

3. **Run pre-commit:**

   ```bash
   micromamba run -n mamba pre-commit run --all-files
   ```

4. **Verify commit count:**

   ```bash
   git log --oneline | head -8
   # Should show 8 commits
   ```

---

## Summary: Patches Mapping

| New Commit | Source Patches         | Dropped           |
| ---------- | ---------------------- | ----------------- |
| 01         | 0001 (modified)        | —                 |
| 02         | 0002 + 0003 (modified) | —                 |
| 03         | 0004 + 0013 (modified) | —                 |
| 04         | 0005 + 0008 (modified) | —                 |
| 05         | 0006 (modified)        | —                 |
| 06         | 0007 (modified)        | —                 |
| 07         | 0009 (modified + new)  | —                 |
| 08         | 0010 + 0012 (modified) | —                 |
| —          | —                      | 0011 (pixi files) |
