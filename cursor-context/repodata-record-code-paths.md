# Code Paths Writing to `repodata_record.json`

This document provides a comprehensive analysis of all code paths in the mamba codebase that result in writes to `repodata_record.json`.

## Overview

`repodata_record.json` is written to `<pkg_extract_dir>/info/repodata_record.json` within the package cache. It serves as a record of the channel-style metadata for an installed package, which may include fixes from repodata patches and can differ from the original `index.json` inside the package tarball.

---

## Primary Write Locations

### 1. `PackageFetcher::write_repodata_record()` (Main Path)

**File:** `libmamba/src/core/package_fetcher.cpp`  
**Lines:** 431-467

```cpp
void PackageFetcher::write_repodata_record(const fs::u8path& base_path) const
{
    const fs::u8path repodata_record_path = base_path / "info" / "repodata_record.json";
    const fs::u8path index_path = base_path / "info" / "index.json";

    nlohmann::json index;
    std::ifstream index_file = open_ifstream(index_path);
    index_file >> index;

    nlohmann::json repodata_record = m_package_info;

    // For explicit spec files (URLs), m_package_info has empty depends/constrains arrays
    // that would overwrite the correct values from index.json. Remove these empty fields.
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

    // Merge index.json values (adds new fields, doesn't overwrite existing)
    repodata_record.insert(index.cbegin(), index.cend());

    if (repodata_record.find("size") == repodata_record.end() || repodata_record["size"] == 0)
    {
        repodata_record["size"] = fs::file_size(m_tarball_path);
    }

    std::ofstream repodata_record_file(repodata_record_path.std_path());
    repodata_record_file << repodata_record.dump(4);
}
```

#### Call Chain to `write_repodata_record()`

```
PackageFetcher::extract()
  └── write_repodata_record(extract_path)
```

**Trigger Conditions:**

1. **Download and Extract Flow:**
   - `PackageFetcherTask::run(downloaded_size)` is called after a successful download
   - This calls `PackageFetcher::validate()` and then `PackageFetcher::extract()`
   - Extract performs the actual archive extraction followed by `write_repodata_record()`

2. **Extract-Only Flow (Cached Tarball):**
   - When a valid tarball exists in cache but no valid extracted directory
   - `PackageFetcher::extract()` is called directly
   - Archive is extracted, then `write_repodata_record()` is called

#### Data Sources

| Field | Source | Notes |
|-------|--------|-------|
| `name`, `version`, `build`, etc. | `m_package_info` | From PackageInfo (via JSON serialization) |
| `depends`, `constrains` | `index.json` OR `m_package_info` | `m_package_info` values used if non-empty; otherwise from `index.json` |
| `size` | Tarball file size | Computed from `m_tarball_path` if missing or zero |
| All other fields | `index.json` | Merged via `insert()` (only adds new keys) |

#### Upstream Code Path: How `PackageFetcher` Gets Called

```
MTransaction::execute()
  └── execute_download_extract()
      └── [creates PackageFetcher instances]
          └── PackageFetcher::build_extract_task()
              └── PackageExtractTask::run()
                  └── PackageFetcher::extract()
                      └── write_repodata_record()
```

---

### 2. `construct()` Function (Constructor Path)

**File:** `micromamba/src/constructor.cpp`  
**Lines:** 119-185

```cpp
if (extract_conda_pkgs)
{
    // ... for each package URL in pkgs/urls file ...
    
    auto pkg_info = specs::PackageInfo::from_url(raw_url).value();
    fs::u8path base_path = extract(entry, ExtractOptions::from_context(config.context()));
    
    fs::u8path repodata_record_path = base_path / "info" / "repodata_record.json";
    fs::u8path index_path = base_path / "info" / "index.json";
    
    nlohmann::json repodata_record;
    if (fs::exists(repodata_location))
    {
        // Try to find package in cached repodata
        auto& j = repodatas[repodata_cache_name];
        repodata_record = find_package(j, pkg_info.filename);
    }
    
    nlohmann::json index;
    std::ifstream index_file{ index_path.std_path() };
    index_file >> index;
    
    if (!repodata_record.is_null())
    {
        // Merge index.json into repodata_record
        repodata_record.insert(index.cbegin(), index.cend());
    }
    else
    {
        // No cached repodata - use index.json as base
        repodata_record = index;
        repodata_record["size"] = fs::file_size(entry);
        if (!pkg_info.md5.empty()) repodata_record["md5"] = pkg_info.md5;
        if (!pkg_info.sha256.empty()) repodata_record["sha256"] = pkg_info.sha256;
    }
    
    // Always set these from pkg_info
    repodata_record["fn"] = pkg_info.filename;
    repodata_record["url"] = pkg_info.package_url;
    repodata_record["channel"] = pkg_info.channel;
    
    // Fill size if missing
    if (repodata_record.find("size") == repodata_record.end() || repodata_record["size"] == 0)
    {
        repodata_record["size"] = fs::file_size(entry);
    }
    
    std::ofstream repodata_record_of{ repodata_record_path.std_path() };
    repodata_record_of << repodata_record.dump(4);
}
```

#### Trigger Conditions

This path is triggered when running:
```bash
micromamba constructor --prefix <prefix> --extract-conda-pkgs
```

This is used by conda-constructor to set up environments from pre-downloaded packages.

#### Data Sources

| Scenario | Primary Source | Secondary Source |
|----------|----------------|------------------|
| Cached repodata exists | `pkgs/cache/<channel>.json` | `index.json` (via merge) |
| No cached repodata | `index.json` | `pkg_info` (from URL parsing) |

---

## How `PackageInfo` Gets Populated

Understanding the data flow requires tracing how `PackageInfo` objects are created:

### Path A: From Channel Repodata (Normal Install)

```
load_channels() → SubdirIndexLoader → Database::add_repo_from_repodata()
  └── Solver resolution produces PackageInfo with full metadata
      └── MTransaction → PackageFetcher(pkg_info, caches)
```

**Result:** `m_package_info` has all metadata from channel's `repodata.json`

### Path B: From URL (Explicit Lockfile / `--file explicit.txt`)

```
specs::PackageInfo::from_url(url_string)
```

**File:** `libmamba/src/specs/package_info.cpp`, lines 193-258

**What gets populated:**
- `name`, `version`, `build_string` (parsed from filename)
- `channel`, `platform` (parsed from URL path)
- `package_url`, `filename`
- `md5` or `sha256` (if present in URL hash fragment)

**What remains empty/default:**
- `dependencies` = `[]`
- `constrains` = `[]`
- `license` = `""`
- `timestamp` = `0`
- `build_number` = `0`
- `size` = `0`
- `track_features` = `[]`

### Path C: From Lockfile (conda-lock YAML or mambajs JSON)

**File:** `libmamba/src/core/env_lockfile_conda.cpp`

When parsing conda-lock format:
```cpp
package.info.version = package_node["version"].as<std::string>();
package.info.md5 = hash_node["md5"].as<std::string>();
package.info.sha256 = hash_node["sha256"].as<std::string>();
package.info.package_url = package_node["url"].as<std::string_view>();

// Then from_url parsing:
package.info.filename = maybe_parsed_info->filename;
package.info.channel = maybe_parsed_info->channel;
package.info.build_string = maybe_parsed_info->build_string;
package.info.platform = maybe_parsed_info->platform;

// Dependencies ARE populated from lockfile:
for (const auto& dependency : package_node["dependencies"]) {
    package.info.dependencies.push_back(...);
}
```

**Note:** Lockfiles contain `depends`/`constrains` so these are populated, unlike the URL path.

---

## JSON Serialization: `PackageInfo` → JSON

**File:** `libmamba/src/specs/package_info.cpp`, lines 474-526

```cpp
void to_json(nlohmann::json& j, const PackageInfo& pkg)
{
    j["name"] = pkg.name;
    j["version"] = pkg.version;
    j["channel"] = pkg.channel;
    j["url"] = pkg.package_url;
    j["subdir"] = pkg.platform;
    j["fn"] = pkg.filename;
    j["size"] = pkg.size;
    j["timestamp"] = pkg.timestamp;
    j["build"] = pkg.build_string;
    j["build_string"] = pkg.build_string;
    j["build_number"] = pkg.build_number;
    if (pkg.noarch != NoArchType::No) { j["noarch"] = pkg.noarch; }
    j["license"] = pkg.license;
    j["track_features"] = fmt::format("{}", fmt::join(pkg.track_features, ","));
    if (!pkg.md5.empty()) { j["md5"] = pkg.md5; }
    if (!pkg.sha256.empty()) { j["sha256"] = pkg.sha256; }
    // ...
    
    // Always write depends/constrains (even if empty)
    if (pkg.dependencies.empty()) {
        j["depends"] = nlohmann::json::array();
    } else {
        j["depends"] = pkg.dependencies;
    }
    
    if (pkg.constrains.empty()) {
        j["constrains"] = nlohmann::json::array();
    } else {
        j["constrains"] = pkg.constrains;
    }
}
```

**Key insight:** `to_json` always emits `depends` and `constrains` as arrays (empty or not), along with all other fields including stub values like `timestamp: 0` and `license: ""`.

---

## The Bug: URL-Derived Package Metadata Corruption

When installing from explicit URLs or lockfiles, the flow is:

```
1. PackageInfo::from_url() creates stub PackageInfo
   - timestamp = 0, license = "", build_number = 0, etc.
   - depends = [], constrains = []

2. nlohmann::json repodata_record = m_package_info;
   - Serializes stub values into JSON

3. Current fix only handles depends/constrains:
   if (depends_it->empty()) repodata_record.erase("depends");
   
4. repodata_record.insert(index.cbegin(), index.cend());
   - index.json values ONLY added if key doesn't exist
   - BUT timestamp, license, build_number etc. ALREADY exist with stub values!

Result: repodata_record.json has incorrect stub values for:
- timestamp (0 instead of real value)
- license ("" instead of real value)  
- build_number (0 instead of real value)
- track_features ("" instead of real value or absent)
```

---

## Validation and Reading of `repodata_record.json`

### Cache Validation

**File:** `libmamba/src/core/package_cache.cpp`  
**Function:** `PackageCacheData::has_valid_extracted_dir()` (lines 236-398)

This function reads `repodata_record.json` to validate if a cached extraction is still valid:

```cpp
auto repodata_record_path = extracted_dir / "info" / "repodata_record.json";
if (fs::exists(repodata_record_path))
{
    std::ifstream repodata_record_f(repodata_record_path.std_path());
    nlohmann::json repodata_record;
    repodata_record_f >> repodata_record;
    
    // Validate size, sha256, md5, url, channel...
}
```

**Validation checks:**
1. Size matches expected
2. SHA256 or MD5 checksum matches
3. URL or channel matches

### Package Linking

**File:** `libmamba/src/core/link.cpp`  
**Function:** `LinkPackage::execute()` (lines 858-861)

```cpp
std::ifstream repodata_f = open_ifstream(m_source / "info" / "repodata_record.json");
repodata_f >> index_json;
```

This reads the cached `repodata_record.json` during package installation to the target prefix.

### Offline Mode

**File:** `libmamba/src/api/channel_loader.cpp`  
**Function:** `create_repo_from_pkgs_dir()` (lines 40-48)

```cpp
for (const auto& entry : fs::directory_iterator(pkgs_dir))
{
    fs::u8path repodata_record_json = entry.path() / "info" / "repodata_record.json";
    if (!fs::exists(repodata_record_json)) continue;
    prefix_data.load_single_record(repodata_record_json);
}
```

---

## Complete Call Graph

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        WRITE PATHS                                       │
└─────────────────────────────────────────────────────────────────────────┘

[User Command]
    │
    ├──► mamba install <pkg>
    │       │
    │       └──► MTransaction::execute()
    │               └──► execute_download_extract()
    │                       └──► PackageFetcher::extract()
    │                               └──► write_repodata_record()  ◄── WRITE #1
    │
    ├──► mamba install --file explicit.txt
    │       │
    │       └──► create_explicit_transaction_from_urls()
    │               └──► PackageInfo::from_url()  [stub values!]
    │                       └──► MTransaction(ctx, db, {}, specs_to_install, caches)
    │                               └──► PackageFetcher::extract()
    │                                       └──► write_repodata_record()  ◄── WRITE #1
    │
    ├──► mamba install --file lockfile.yml
    │       │
    │       └──► create_explicit_transaction_from_lockfile()
    │               └──► read_environment_lockfile()
    │                       └──► read_conda_environment_lockfile()
    │                               └──► read_package_info()  [has deps from lockfile]
    │                                       └──► PackageInfo::from_url()
    │                                               └──► MTransaction(ctx, db, {}, conda_packages, caches)
    │                                                       └──► PackageFetcher::extract()
    │                                                               └──► write_repodata_record()  ◄── WRITE #1
    │
    └──► micromamba constructor --extract-conda-pkgs
            │
            └──► construct()
                    └──► for each URL in pkgs/urls:
                            └──► extract()
                                    └──► [direct JSON write]  ◄── WRITE #2

┌─────────────────────────────────────────────────────────────────────────┐
│                        READ/VALIDATION PATHS                             │
└─────────────────────────────────────────────────────────────────────────┘

[Cache Validation]
    │
    └──► MultiPackageCache::get_extracted_dir_path()
            └──► PackageCacheData::has_valid_extracted_dir()
                    └──► [reads repodata_record.json for validation]

[Package Linking]
    │
    └──► LinkPackage::execute()
            └──► [reads repodata_record.json from cache]

[Offline Mode]
    │
    └──► create_repo_from_pkgs_dir()
            └──► prefix_data.load_single_record(repodata_record_json)
```

---

## Summary of Write Conditions

| Condition | Write Location | Trigger |
|-----------|---------------|---------|
| Package download + extract | `PackageFetcher::write_repodata_record()` | After successful tarball extraction |
| Cached tarball, needs extract | `PackageFetcher::write_repodata_record()` | Tarball valid but no extracted dir |
| Constructor mode | `construct()` | `--extract-conda-pkgs` flag |

---

## File Locations

- **Write destination:** `<cache_path>/<pkg_name>-<version>-<build>/info/repodata_record.json`
- **Source files:**
  - `libmamba/src/core/package_fetcher.cpp` (main write)
  - `micromamba/src/constructor.cpp` (constructor write)
  - `libmamba/src/specs/package_info.cpp` (JSON serialization)
  - `libmamba/src/core/package_cache.cpp` (validation/read)
  - `libmamba/src/core/link.cpp` (read during link)
