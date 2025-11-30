# Analysis: Explicit URL Spec Execution Flow and Package Cache Error

## Overview

This document provides a thorough analysis of the execution flow when running:

```bash
mamba create https://conda.anaconda.org/conda-forge/linux-64/_libgcc_mutex-0.1-main.tar.bz2 \
    --no-env -n env-create-from-explicit-url --override-channels --json -y --no-rc
```

## 1. The Execution Flow

### 1.1 Entry Point: `create()` in `libmamba/src/api/create.cpp`

```cpp
void create(Configuration& config)
{
    // ...
    auto& create_specs = config.at("specs").value<std::vector<std::string>>();
    auto& use_explicit = config.at("explicit_install").value<bool>();  // DEFAULT: false
    // ...
    
    if (!create_specs.empty())
    {
        if (use_explicit)  // FALSE for command-line URL specs
        {
            install_explicit_specs(ctx, channel_context, create_specs, ...);
        }
        else
        {
            install_specs(ctx, channel_context, config, create_specs, ...);  // ← THIS PATH
        }
    }
}
```

**Key Insight**: When a URL is passed directly on the command line (not via a spec file with `@EXPLICIT`), `use_explicit` remains `false` (its default value). This routes the request to `install_specs()` instead of `install_explicit_specs()`.

### 1.2 The Solver Path: `install_specs_impl()` in `libmamba/src/api/install.cpp`

```cpp
void install_specs_impl(Context& ctx, ChannelContext& channel_context, ...)
{
    // Step 1: Extract channels from URL specs
    populate_context_channels_from_specs(raw_specs, ctx);  // Adds URL's channel to ctx.channels
    
    // Step 2: Create solver database
    solver::libsolv::Database db{...};
    
    // Step 3: Load channels (including URL packages)
    auto maybe_load = load_channels(ctx, channel_context, db, package_caches);
    
    // Step 4: Create solver request (parses URL as MatchSpec)
    auto request = create_install_request(prefix_data, raw_specs, freeze_installed);
    
    // Step 5: SOLVER IS INVOKED!
    auto outcome = solver::libsolv::Solver().solve(db, request, ...);
    
    // Step 6: Create transaction from solver solution
    auto trans = MTransaction(ctx, database, request, std::get<solver::Solution>(outcome), ...);
    
    // Step 7: Execute transaction (download, extract, link)
    trans.execute(ctx, channel_context, prefix_data);
}
```

### 1.3 Channel Loading with URL Packages: `load_channels_impl()` in `libmamba/src/api/channel_loader.cpp`

```cpp
for (const auto& location : ctx.channels)
{
    for (auto channel : channel_context.make_channel(location))
    {
        if (channel.is_package())  // TRUE for package URLs
        {
            // Creates PackageInfo via from_url() with FULL defaulted_keys
            auto pkg_info = specs::PackageInfo::from_url(channel.url().str()).value();
            // pkg_info.defaulted_keys = {"_initialized", "build_number", "license", 
            //                            "timestamp", "track_features", "depends", "constrains"}
            packages.push_back(pkg_info);
            continue;
        }
        // ... handle normal channels
    }
}

if (!packages.empty())
{
    // Adds packages to solver database
    database.add_repo_from_packages(packages, "packages");
}
```

### 1.4 The Data Loss: `add_repo_from_packages()` → `set_solvable()`

When `add_repo_from_packages()` is called:

```cpp
void Database::add_repo_from_packages_impl_loop(const RepoInfo& repo, const specs::PackageInfo& pkg)
{
    auto [id, solv] = s_repo.add_solvable();
    set_solvable(pool(), solv, pkg, settings().matchspec_parser);
    // set_solvable() copies most fields but NOT defaulted_keys (libsolv doesn't have this field)
}
```

The `defaulted_keys` field is **lost** because libsolv's solvable structure doesn't have a corresponding field.

### 1.5 Solver Returns Solution with `make_package_info()`

When the solver returns a solution, packages are converted back via `make_package_info()`:

```cpp
auto make_package_info(const solv::ObjPool& pool, solv::ObjSolvableViewConst s)
    -> specs::PackageInfo
{
    specs::PackageInfo out = {};
    out.name = s.name();
    out.version = s.version();
    out.license = s.license();      // May be empty if not in solvable
    out.timestamp = s.timestamp();  // May be 0 if not in solvable
    // ...
    
    // ONLY sets _initialized, NOT the original stub field names
    out.defaulted_keys = { "_initialized" };
    
    return out;
}
```

**The Problem**: The `PackageInfo` that enters `write_repodata_record()` has:
- `defaulted_keys = ["_initialized"]` (only)
- `license = ""` (stub value from solvable)
- `timestamp = 0` (stub value from solvable)

### 1.6 Package Extraction and `write_repodata_record()`

During `MTransaction::execute()`:

```cpp
// In PackageFetcher::extract()
write_repodata_record(base_path);
```

The `write_repodata_record()` function (before fix):

```cpp
void PackageFetcher::write_repodata_record(const fs::u8path& base_path) const
{
    nlohmann::json repodata_record = m_package_info;
    
    // Only erases fields in defaulted_keys (except _initialized)
    for (const auto& key : m_package_info.defaulted_keys)
    {
        if (key != "_initialized")
            repodata_record.erase(key);
    }
    // Since defaulted_keys = ["_initialized"], NOTHING is erased!
    
    // insert() only adds MISSING keys, doesn't overwrite
    repodata_record.insert(index.cbegin(), index.cend());
    // license and timestamp already exist with stub values, so NOT replaced!
    
    // WRITES STUB VALUES: timestamp=0, license=""
    repodata_record_file << repodata_record.dump(4);
}
```

### 1.7 The Linking Phase Triggers Healing Code

During the linking phase in `MTransaction::execute()`:

```cpp
for (const specs::PackageInfo& pkg : m_solution.packages_to_install())
{
    const fs::u8path cache_path(m_multi_cache.get_extracted_dir_path(pkg, false));
    // ↑ This calls has_valid_extracted_dir() which triggers healing
    
    LinkPackage lp(pkg, cache_path, &transaction_context);
    lp.execute();
}
```

### 1.8 Healing Code Detects Corruption

In `has_valid_extracted_dir()` in `libmamba/src/core/package_cache.cpp`:

```cpp
// HEALING: Detect corrupted cache from buggy versions (v2.1.1-v2.4.0).
// Corruption signature: timestamp == 0 AND license == ""
if (valid && repodata_record.contains("timestamp")
    && repodata_record["timestamp"] == 0 
    && repodata_record.contains("license")
    && repodata_record["license"] == "")
{
    LOG_INFO << "Detected corrupted metadata in cache, will re-extract...";
    valid = false;  // INVALIDATES THE CACHE WE JUST WROTE!
}
```

### 1.9 The "Package cache error"

Since `get_extracted_dir_path(pkg, false)` is called with `return_empty=false`:

```cpp
fs::u8path MultiPackageCache::get_extracted_dir_path(const specs::PackageInfo& s, bool return_empty)
{
    for (auto& c : m_caches)
    {
        if (c.has_valid_extracted_dir(s, m_params))  // Returns FALSE (healing triggered)
        {
            return c.path();
        }
    }
    
    if (return_empty)
        return fs::u8path();
    else
    {
        LOG_ERROR << "Cannot find a valid extracted directory cache for '" << s.filename << "'";
        throw std::runtime_error("Package cache error.");  // ← THIS ERROR
    }
}
```

## 2. Why the Solver is Invoked for URL Specs

### 2.1 The Design Decision

The mamba code has two paths for installing packages:

1. **Solver Path** (`install_specs()` → `install_specs_impl()`): 
   - Parses specs as `MatchSpec`
   - Invokes the solver to resolve dependencies
   - Handles complex dependency resolution
   
2. **Explicit Path** (`install_explicit_specs()` → `create_explicit_transaction_from_urls()`):
   - Creates `PackageInfo` directly via `from_url()`
   - No solver invocation
   - No dependency resolution

### 2.2 How to Choose the Explicit Path

The explicit path is chosen when:

1. **`--explicit` flag is passed**: Sets `explicit_install=true` directly
2. **Spec file with `@EXPLICIT` marker**: Triggers `config.at("explicit_install").set_value(true)`

Without either of these, even pure URL specs go through the solver.

### 2.3 Why This Design?

The solver path allows for:
- Dependency resolution if the URL package has dependencies
- Integration with existing environment packages
- Pin and freeze handling

The issue is that the solver path loses the `defaulted_keys` information because libsolv doesn't preserve it.

## 3. Why Previous Tests Didn't Detect This

### 3.1 Test Coverage Gaps

1. **C++ Unit Tests**: Test `write_repodata_record()` with `PackageInfo` created directly via `from_url()` or with manually set `defaulted_keys`. They don't test the full solver → `make_package_info()` flow.

2. **Python Integration Tests**: Most tests use:
   - Package names (not URLs) that go through repodata
   - Explicit spec files with `@EXPLICIT` marker
   - The `--explicit` flag

3. **The Specific Gap**: No test covered the case of URL specs passed directly on the command line without `--explicit`.

### 3.2 Branches That Were Missed

| Branch | Tested? | Description |
|--------|---------|-------------|
| URL via spec file with `@EXPLICIT` | ✅ | Uses `install_explicit_specs()` |
| URL with `--explicit` flag | ✅ | Uses `install_explicit_specs()` |
| URL directly on command line | ❌ | Uses `install_specs()` → solver |
| Package name via solver | ✅ | Gets metadata from repodata |

## 4. Mixed Specs: URLs + Package Names + Spec Files

### 4.1 How Mamba Handles Mixed Input

When you run:
```bash
mamba create numpy https://example.com/pkg.conda -f specs.txt
```

The following happens:

1. **Command-line specs** (`numpy`, `https://example.com/pkg.conda`) are added to `specs`
2. **File specs** (`specs.txt`) are parsed and added
3. **If any file contains `@EXPLICIT`**: ALL specs go through explicit path
4. **Otherwise**: ALL specs go through solver path

### 4.2 The All-or-Nothing Problem

The current design is **all-or-nothing**:
- Either ALL specs go through the solver, or
- ALL specs go through the explicit path (if `@EXPLICIT` is detected)

**There's no hybrid path** that could:
- Use the solver for package names needing dependency resolution
- Use `from_url()` for explicit URLs to preserve `defaulted_keys`

### 4.3 What Happens with Mixed Specs Through Solver

When mixed specs go through the solver:

| Spec Type | Source | `defaulted_keys` | Result |
|-----------|--------|------------------|--------|
| Package name | Channel repodata | `["_initialized"]` | ✅ Full metadata from repodata |
| URL package | `from_url()` → solver | `["_initialized"]` | ❌ Lost stub field names |

For package names, the repodata typically has complete metadata (license, timestamp, etc.), so stub values aren't an issue.

For URL packages, the solvable may not have complete metadata, leading to stub values in `write_repodata_record()`.

## 5. The Fix and Its Implications

### 5.1 The Applied Fix

The fix adds detection in `write_repodata_record()` for solver-path URL packages:

```cpp
// HEAL SOLVER-PATH URL PACKAGES: Detect when a solver-derived package has stub values.
const bool has_only_initialized = (m_package_info.defaulted_keys.size() == 1);
const bool has_stub_signature = (m_package_info.timestamp == 0 && m_package_info.license.empty());

if (has_only_initialized && has_stub_signature)
{
    // Solver-derived URL package with stub values: erase stub fields
    static const std::vector<std::string> stub_fields = {
        "build_number", "license", "timestamp", "track_features", "depends", "constrains"
    };
    for (const auto& field : stub_fields)
    {
        repodata_record.erase(field);
    }
}
// Now insert() will add correct values from index.json
```

### 5.2 Safety of the Fix

The fix is safe because:

1. **Narrow Detection**: Only triggers when:
   - `defaulted_keys` has exactly 1 element (only `_initialized`)
   - AND `timestamp == 0`
   - AND `license == ""`

2. **Low False-Positive Risk**:
   - Channel repodata virtually always has `timestamp` and `license`
   - The combination is extremely rare for legitimate packages
   - Worst case: metadata is refreshed from `index.json` (correct anyway)

3. **Preserves Channel Patches**:
   - Packages from channel repodata have full metadata
   - They won't match the stub signature

## 6. Recommended Long-Term Solutions

### 6.1 Option A: Preserve `defaulted_keys` Through Solver

Add a custom attribute to libsolv solvables to store `defaulted_keys`:

```cpp
// In set_solvable():
solv.set_custom_attribute("defaulted_keys", serialize(pkg.defaulted_keys));

// In make_package_info():
out.defaulted_keys = deserialize(s.get_custom_attribute("defaulted_keys"));
```

**Pros**: Clean solution, preserves information flow
**Cons**: Requires libsolv wrapper changes

### 6.2 Option B: Hybrid Solver/Explicit Path

Detect URL specs early and route them differently:

```cpp
void install_specs_impl(...)
{
    std::vector<std::string> url_specs, other_specs;
    for (const auto& spec : raw_specs)
    {
        if (has_archive_extension(spec))
            url_specs.push_back(spec);
        else
            other_specs.push_back(spec);
    }
    
    // Solve other_specs with solver
    // Process url_specs via from_url() directly
    // Merge results
}
```

**Pros**: Cleaner separation of concerns
**Cons**: Complex to implement, may have edge cases

### 6.3 Option C: Detect URL-Derived Packages Post-Solver (Current Fix)

Use heuristics to detect URL-derived packages after the solver returns and handle them specially.

**Pros**: Minimal changes, works with current architecture
**Cons**: Relies on heuristics (stub signature detection)

## 7. Summary

### Root Cause
When URL specs are passed without `--explicit`, they go through the solver path. The `PackageInfo` is converted to a libsolv solvable (losing `defaulted_keys`), then back to `PackageInfo` via `make_package_info()` (which only sets `["_initialized"]`). The stub values (timestamp=0, license="") are then written to `repodata_record.json`, triggering the healing code to invalidate the cache during linking.

### The Error Flow
1. Download & extract package
2. `write_repodata_record()` writes stub values
3. Linking phase calls `has_valid_extracted_dir()`
4. Healing code detects corruption signature → invalidates cache
5. `get_extracted_dir_path()` can't find valid cache → throws "Package cache error"

### The Fix
Detect solver-path URL packages by the stub signature (timestamp=0 AND license="") combined with `defaulted_keys = ["_initialized"]`, and erase stub fields before merging with `index.json`.
