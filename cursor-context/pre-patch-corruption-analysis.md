# Pre-Patch Corruption Analysis: What Goes Wrong Without the Fixes

<!-- markdownlint-disable MD013 MD024 -->

This document traces through each input pathway to show **exactly what corruption occurs** when the patches are reverted (the pre-fix state).

## Core Problem: The Original `write_repodata_record()`

**File:** `libmamba/src/core/package_fetcher.cpp`

### Original Code (v2.3.3 partial fix)

```cpp
void PackageFetcher::write_repodata_record(const fs::u8path& base_path) const
{
    // ... file setup ...

    nlohmann::json repodata_record = m_package_info;  // Converts to JSON via to_json()

    // v2.3.3 "fix": only handles empty depends/constrains
    if (auto depends_it = repodata_record.find("depends");
        depends_it != repodata_record.end() && depends_it->empty())
    {
        repodata_record.erase("depends");
    }
    if (auto constrains_it = repodata_record.find("constrains");
        constrains_it != repodata_record.end() && constrains_it->empty())
    {
        repodata_record.erase("constrains");
    }

    // insert() only adds MISSING keys - does not overwrite existing ones!
    repodata_record.insert(index.cbegin(), index.cend());

    // ... size handling, write file ...
}
```

### The Critical Flaw

The `to_json()` function in `package_info.cpp` **ALWAYS** writes these fields:

```cpp
void to_json(nlohmann::json& j, const PackageInfo& pkg)
{
    // These are ALWAYS written, even with stub/default values:
    j["build_number"] = pkg.build_number;  // 0 (stub)
    j["license"] = pkg.license;            // "" (stub)
    j["timestamp"] = pkg.timestamp;        // 0 (stub)
    j["track_features"] = ...;             // "" (stub)
    j["depends"] = pkg.dependencies;       // [] (stub) 
    j["constrains"] = pkg.constrains;      // [] (stub)
    // ...
}
```

Since `insert()` only adds **missing** keys, and these keys are **already present** with stub values, the correct values from `index.json` are **never used**.

---

## Corruption by Input Pathway

### 1. Explicit URL Installation (`from_url()` → `write_repodata_record()`)

**Command:** `micromamba install https://.../.../pkg-1.0-h0.conda`

**Flow:**
```
URL string
    ↓
PackageInfo::from_url()    // defaulted_keys = [] (empty!)
    ↓
PackageFetcher            // Stores m_package_info
    ↓
extract() → write_repodata_record()
    ↓
to_json(m_package_info)   // Writes stub values for ALL fields
    ↓
insert(index)             // Does nothing for stub fields (already present)
    ↓
CORRUPTED repodata_record.json
```

**Fields Corrupted:**

| Field | In index.json | Written to repodata_record.json |
|-------|---------------|--------------------------------|
| `depends` | `["python >=3.8", "numpy"]` | `[]` |
| `constrains` | `["scipy <2.0"]` | `[]` |
| `license` | `"MIT"` | `""` |
| `timestamp` | `1700000000` | `0` |
| `build_number` | `5` | `0` |
| `track_features` | `"mkl"` | `""` |

**Impact:**
- Package appears to have no dependencies → environment resolution broken
- License tracking impossible
- Package provenance lost (no timestamp)
- Build number lost (version disambiguation broken)

---

### 2. Explicit Lockfile Installation (`.txt` with `@EXPLICIT`)

**Command:** `micromamba install -f explicit.lock`

**File contents:**
```
@EXPLICIT
https://conda.anaconda.org/conda-forge/linux-64/pkg-1.0-h0.conda#abc123...
```

**Flow:** Same as #1 - URL is parsed via `from_url()`, same corruption occurs.

---

### 3. Conda-Lock YAML Format (`env_lockfile_conda.cpp`)

**Command:** `micromamba install -f environment-lock.yml`

**Original Code (before patch):**

```cpp
auto read_package_info(const YAML::Node& package_node)
{
    Package package{ .info = specs::PackageInfo{ name }, ... };
    
    // Copy fields from from_url()
    auto maybe_parsed_info = specs::PackageInfo::from_url(package.info.package_url);
    package.info.filename = maybe_parsed_info->filename;
    package.info.channel = maybe_parsed_info->channel;
    package.info.build_string = maybe_parsed_info->build_string;
    package.info.platform = maybe_parsed_info->platform;
    // NOTE: defaulted_keys NOT copied!  <-- BUG
    
    // Dependencies from lockfile
    for (const auto& dependency : package_node["dependencies"]) {
        package.info.dependencies.push_back(...);
    }
    
    return package;
}
```

**Flow:**
```
YAML lockfile
    ↓
read_package_info()
    ↓
PackageInfo with:
  - depends: from lockfile (CORRECT)
  - constrains: from lockfile (CORRECT)
  - defaulted_keys: [] (empty - NOT copied from from_url!)
    ↓
PackageFetcher → write_repodata_record()
    ↓
to_json():
  - depends: from lockfile → [] or correct values
  - license: "" (stub)
  - timestamp: 0 (stub)
  - etc.
    ↓
PARTIALLY CORRUPTED repodata_record.json
```

**Fields Corrupted:**

| Field | Source | Written Value |
|-------|--------|---------------|
| `depends` | Lockfile | ✓ Correct (from lockfile) |
| `constrains` | Lockfile | ✓ Correct (from lockfile) |
| `license` | Stub | `""` ✗ WRONG |
| `timestamp` | Stub | `0` ✗ WRONG |
| `build_number` | Stub | `0` ✗ WRONG |
| `track_features` | Stub | `""` ✗ WRONG |

**Key Insight:** Conda-lock is "better" than explicit URLs because it provides dependencies, but still corrupts metadata fields.

---

### 4. MambaJS JSON Format (`env_lockfile_mambajs.cpp`)

**Command:** `micromamba install -f mamba-lock.json`

**Original Code (before patch):**

```cpp
auto read_package_info(/* ... */)
{
    Package package{ .info = specs::PackageInfo{ name }, ... };
    
    package.info.version = package_value["version"].get<std::string>();
    package.info.md5 = hash_value["md5"].get<std::string>();
    package.info.channel = package_value["channel"].get<std::string>();
    package.info.build_string = package_value["build"].get<std::string>();
    // NOTE: No dependencies in mambajs format!
    // NOTE: defaulted_keys NOT set!  <-- BUG
    
    return package;
}
```

**Flow:**
```
JSON lockfile
    ↓
read_package_info()
    ↓
PackageInfo with:
  - depends: [] (empty - NOT in lockfile!)
  - constrains: [] (empty)
  - defaulted_keys: [] (empty - NOT set!)
    ↓
PackageFetcher → write_repodata_record()
    ↓
v2.3.3 "fix" erases empty depends/constrains
    ↓
insert(index) adds them back from index.json
    ↓
MOSTLY CORRECT for depends/constrains, BUT:
  - license: ""
  - timestamp: 0
  - build_number: 0
```

**Fields Corrupted:**

| Field | Source | Written Value |
|-------|--------|---------------|
| `depends` | index.json | ✓ Correct (v2.3.3 fix works here) |
| `constrains` | index.json | ✓ Correct (v2.3.3 fix works here) |
| `license` | Stub | `""` ✗ WRONG |
| `timestamp` | Stub | `0` ✗ WRONG |
| `build_number` | Stub | `0` ✗ WRONG |
| `track_features` | Stub | `""` ✗ WRONG |

---

### 5. Normal Solver Installation (`make_package_info()`)

**Command:** `micromamba install numpy`

**Original Code (before patch):**

```cpp
auto make_package_info(const solv::ObjPool& pool, solv::ObjSolvableViewConst s)
    -> specs::PackageInfo
{
    specs::PackageInfo out = {};
    
    out.name = s.name();
    out.version = s.version();
    out.dependencies = ...; // From repodata.json (potentially patched!)
    out.constrains = ...;   // From repodata.json (potentially patched!)
    // ... all fields from solver ...
    
    // NOTE: defaulted_keys NOT set!  <-- NOT A BUG (but causes other issues)
    
    return out;
}
```

**Flow:**
```
repodata.json (with channel patches)
    ↓
libsolv
    ↓
make_package_info()
    ↓
PackageInfo with ALL correct values from channel repodata
  - depends: [] (intentionally empty from channel patch!)
  - defaulted_keys: [] (empty)
    ↓
PackageFetcher → write_repodata_record()
    ↓
v2.3.3 "fix": if depends.empty() → erase("depends")  <-- DESTROYS PATCH!
    ↓
insert(index) adds depends from index.json  <-- OVERWRITES PATCH!
    ↓
repodata_record.json has WRONG depends (from index, not patch)
```

**The Channel Patch Problem:**

Conda-forge sometimes patches `repodata.json` to fix dependency issues:

```json
// repodata.json (patched by conda-forge)
{
  "broken-pkg-1.0-h0.conda": {
    "depends": [],  // Intentionally empty! The original was wrong.
    ...
  }
}
```

```json
// Original index.json inside the package
{
  "depends": ["some-broken-dep"],  // This is WRONG
  ...
}
```

The v2.3.3 "fix" **destroys** the channel patch because it can't distinguish:
- Empty `depends` from URL-derived stub (should be replaced)
- Empty `depends` from channel patch (should be preserved)

---

### 6. Constructor Path (`constructor.cpp`)

**Command:** `micromamba constructor --extract-conda-pkgs`

**Original Code (before patch):**

```cpp
// In construct() function
auto pkg_info = specs::PackageInfo::from_url(raw_url).value();
// defaulted_keys is empty!

nlohmann::json repodata_record;
if (fs::exists(repodata_location)) {
    repodata_record = find_package(j, pkg_info.filename);
}

if (!repodata_record.is_null()) {
    // NO defaulted_keys handling here!  <-- BUG
    repodata_record.insert(index.cbegin(), index.cend());
}
```

**Same corruption as explicit URL installation.**

---

## Summary: Pre-Patch Corruption Matrix

| Input Source | depends | constrains | license | timestamp | build_number | track_features |
|--------------|---------|------------|---------|-----------|--------------|----------------|
| Explicit URL | ✗ `[]` | ✗ `[]` | ✗ `""` | ✗ `0` | ✗ `0` | ✗ `""` |
| Explicit lockfile | ✗ `[]` | ✗ `[]` | ✗ `""` | ✗ `0` | ✗ `0` | ✗ `""` |
| Conda-lock YAML | ✓ | ✓ | ✗ `""` | ✗ `0` | ✗ `0` | ✗ `""` |
| MambaJS JSON | ✓* | ✓* | ✗ `""` | ✗ `0` | ✗ `0` | ✗ `""` |
| Solver (normal) | ✓** | ✓** | ✓ | ✓ | ✓ | ✓ |
| Solver (patched) | ✗*** | ✗*** | ✓ | ✓ | ✓ | ✓ |
| Constructor | ✗ `[]` | ✗ `[]` | ✗ `""` | ✗ `0` | ✗ `0` | ✗ `""` |

**Legend:**
- ✓ = Correct value written
- ✗ = Corrupted (stub value written)
- `*` = v2.3.3 fix works because deps not in lockfile
- `**` = Correct because solver provides all values
- `***` = Channel patch DESTROYED by v2.3.3 "fix"

---

## The Two Bugs in v2.3.3

### Bug 1: Incomplete Field Coverage

The v2.3.3 fix only handles `depends` and `constrains`:

```cpp
// v2.3.3 "fix" - only 2 fields!
if (depends.empty()) repodata_record.erase("depends");
if (constrains.empty()) repodata_record.erase("constrains");
```

Missing fields: `license`, `timestamp`, `build_number`, `track_features`

### Bug 2: Cannot Distinguish Stub from Patch

```cpp
// This erases BOTH:
// - Stub empty arrays (should erase) ✓
// - Channel patch empty arrays (should NOT erase) ✗
if (depends.empty()) repodata_record.erase("depends");
```

The `defaulted_keys` mechanism solves both bugs:
- Lists ALL stub fields (not just depends/constrains)
- Only URL-derived packages have fields listed
- Solver-derived packages have empty list → nothing erased → patches preserved

---

## Real-World Impact

### Broken Environment Resolution

When `depends: []` is written instead of actual dependencies:

```bash
$ micromamba install https://conda.anaconda.org/.../numpy-1.24.0-py311h0.conda
$ micromamba list
numpy                     1.24.0               py311h0  conda-forge

$ python -c "import numpy"
ModuleNotFoundError: No module named 'numpy.core'
# numpy's internal dependencies weren't installed because depends was empty!
```

### Lost Channel Patches

When a channel patch sets `depends: []` to fix a broken package:

```bash
# Channel patched broken-pkg to have no depends (the original deps were wrong)
$ micromamba create -n test broken-pkg

# Without the fix: index.json's wrong deps get written
# Environment resolution uses wrong deps → broken environment
```

### Corrupted Cache Persistence

The corruption persists in the package cache:

```bash
$ cat ~/micromamba/pkgs/numpy-1.24.0-py311h0/info/repodata_record.json
{
  "depends": [],      # WRONG - should have dependencies
  "license": "",      # WRONG - should be "BSD-3-Clause"
  "timestamp": 0,     # WRONG - should be build timestamp
  ...
}
```

Every future installation uses this corrupted cache until:
1. User manually cleans cache: `micromamba clean -a`
2. Healing mechanism detects and re-extracts (with patches)
