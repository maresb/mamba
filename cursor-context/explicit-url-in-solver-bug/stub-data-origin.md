# Where Stub Data Comes From: Complete Trace

## The Short Answer

The stub values (`timestamp=0`, `license=""`, `build_number=0`) come from **C++ struct default
member initializers** in `PackageInfo`. They are set when the struct is constructed and never
overwritten because `from_url()` cannot extract these values from a URL.

## The Complete Data Flow

### Step 1: C++ Struct Defaults (`package_info.hpp` lines 37, 47, 62)

```cpp
class PackageInfo
{
public:
    std::string name = {};
    std::string version = {};
    std::string build_string = {};
    std::size_t build_number = 0;        // ← DEFAULT: 0
    std::string channel = {};
    std::string package_url = {};
    std::string filename = {};
    std::string license = {};            // ← DEFAULT: "" (empty string)
    std::string md5 = {};
    std::string sha256 = {};
    std::vector<std::string> track_features = {};   // ← DEFAULT: empty vector
    std::vector<std::string> dependencies = {};      // ← DEFAULT: empty vector
    std::vector<std::string> constrains = {};        // ← DEFAULT: empty vector
    std::vector<std::string> defaulted_keys = {};
    std::size_t size = 0;
    std::size_t timestamp = 0;           // ← DEFAULT: 0
    // ...
};
```

These are **C++11 default member initializers**. When `PackageInfo()` is default-constructed, these values are automatically set.

### Step 2: `from_url()` Creates PackageInfo (`package_info.cpp` line 47)

```cpp
auto parse_url(std::string_view spec) -> expected_parse_t<PackageInfo>
{
    auto out = PackageInfo();  // ← Constructs with ALL defaults

    // URL: https://conda.anaconda.org/conda-forge/linux-64/_libgcc_mutex-0.1-main.tar.bz2

    out.package_url = util::path_or_url_to_url(spec);  // Set from URL
    out.filename = url.package();                       // "_libgcc_mutex-0.1-main.tar.bz2"
    out.platform = url.platform_name();                 // "linux-64"
    out.channel = url.str(...);                         // "https://conda.anaconda.org/conda-forge"
    out.build_string = tail;                            // "main"
    out.version = tail;                                 // "0.1"
    out.name = head.value();                            // "_libgcc_mutex"

    // These fields are NOT set - they CANNOT be extracted from a URL:
    // - out.timestamp  → stays 0 (default)
    // - out.license    → stays "" (default)
    // - out.build_number → stays 0 (default)
    // - out.dependencies → stays [] (default)
    // - out.constrains → stays [] (default)
    // - out.track_features → stays [] (default)

    out.defaulted_keys = { "_initialized", "build_number", "license", "timestamp",
                          "track_features", "depends", "constrains" };

    return out;
}
```

**At this point**, the `PackageInfo` has:

- `timestamp = 0` (struct default, NOT from URL)
- `license = ""` (struct default, NOT from URL)
- `build_number = 0` (struct default, NOT from URL)
- `defaulted_keys = ["_initialized", "build_number", "license", ...]`

### Step 3: Channel Loading Adds Package to Solver Database

In `channel_loader.cpp`:

```cpp
if (channel.is_package())  // True for URL packages
{
    auto pkg_info = specs::PackageInfo::from_url(channel.url().str()).value();
    // pkg_info has: timestamp=0, license="", defaulted_keys=[...]

    packages.push_back(pkg_info);
}

database.add_repo_from_packages(packages, "packages");
```

### Step 4: `set_solvable()` Copies to libsolv (`helpers.cpp` lines 47-105)

```cpp
void set_solvable(solv::ObjPool& pool, solv::ObjSolvableView solv, const specs::PackageInfo& pkg, ...)
{
    solv.set_name(pkg.name);           // "_libgcc_mutex"
    solv.set_version(pkg.version);     // "0.1"
    solv.set_build_string(pkg.build_string);  // "main"
    solv.set_build_number(pkg.build_number);  // 0 ← STUB VALUE COPIED
    solv.set_channel(pkg.channel);     // "https://conda.anaconda.org/conda-forge"
    solv.set_url(pkg.package_url);     // full URL
    solv.set_license(pkg.license);     // "" ← STUB VALUE COPIED
    solv.set_timestamp(pkg.timestamp); // 0 ← STUB VALUE COPIED
    // ...

    // NOTE: defaulted_keys is NOT copied - libsolv has no such field!
}
```

**libsolv now stores**:

- `SOLVABLE_LICENSE` = "" (empty string)
- `SOLVABLE_BUILDTIME` = 0

### Step 5: Solver Runs and Returns Solution

The solver finds the package in its database and returns it as part of the solution.

### Step 6: `make_package_info()` Creates New PackageInfo from Solvable (`helpers.cpp` lines 107-162)

```cpp
auto make_package_info(const solv::ObjPool& pool, solv::ObjSolvableViewConst s)
    -> specs::PackageInfo
{
    specs::PackageInfo out = {};  // ← Constructs with ALL defaults (timestamp=0, license="")

    out.name = s.name();           // "_libgcc_mutex"
    out.version = s.version();     // "0.1"
    out.build_string = s.build_string();  // "main"
    out.build_number = s.build_number();  // 0 ← READ FROM SOLVABLE (was 0)
    out.channel = s.channel();     // "https://conda.anaconda.org/conda-forge"
    out.package_url = s.url();     // full URL
    out.license = s.license();     // "" ← READ FROM SOLVABLE (was "")
    out.timestamp = s.timestamp(); // 0 ← READ FROM SOLVABLE (was 0)
    // ...

    // Only sets _initialized - DOES NOT KNOW original defaulted_keys!
    out.defaulted_keys = { "_initialized" };

    return out;
}
```

**The resulting `PackageInfo` has**:

- `timestamp = 0` (from solvable, which got it from struct default)
- `license = ""` (from solvable, which got it from struct default)
- `build_number = 0` (from solvable, which got it from struct default)
- `defaulted_keys = ["_initialized"]` **← The original list is LOST!**

### Step 7: `write_repodata_record()` Writes Stub Values

```cpp
void PackageFetcher::write_repodata_record(const fs::u8path& base_path) const
{
    nlohmann::json repodata_record = m_package_info;
    // JSON now contains: {"timestamp": 0, "license": "", "build_number": 0, ...}

    // Erase fields in defaulted_keys (except _initialized)
    for (const auto& key : m_package_info.defaulted_keys)  // Only ["_initialized"]
    {
        if (key != "_initialized")
            repodata_record.erase(key);
    }
    // NOTHING is erased because defaulted_keys only has "_initialized"!

    // insert() only adds MISSING keys
    repodata_record.insert(index.cbegin(), index.cend());
    // timestamp, license, build_number already exist with stub values
    // They are NOT replaced by index.json values!

    // WRITES: {"timestamp": 0, "license": "", ...}
    repodata_record_file << repodata_record.dump(4);
}
```

## Visual Summary

```text
┌─────────────────────────────────────────────────────────────────────────────┐
│                           DATA FLOW DIAGRAM                                  │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  URL: https://conda.anaconda.org/.../pkg-1.0-main.tar.bz2                   │
│                           │                                                  │
│                           ▼                                                  │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │ from_url() → PackageInfo                                              │   │
│  │   name: "pkg"           (from URL)                                    │   │
│  │   version: "1.0"        (from URL)                                    │   │
│  │   timestamp: 0          (STRUCT DEFAULT - not in URL)                 │   │
│  │   license: ""           (STRUCT DEFAULT - not in URL)                 │   │
│  │   defaulted_keys: ["_initialized", "timestamp", "license", ...]       │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                           │                                                  │
│                           ▼                                                  │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │ set_solvable() → libsolv Solvable                                     │   │
│  │   name: "pkg"                                                         │   │
│  │   SOLVABLE_BUILDTIME: 0     (copied from PackageInfo)                 │   │
│  │   SOLVABLE_LICENSE: ""      (copied from PackageInfo)                 │   │
│  │   defaulted_keys: ❌ NOT STORED (libsolv has no such field)           │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                           │                                                  │
│                           ▼                                                  │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │ SOLVER RUNS                                                           │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                           │                                                  │
│                           ▼                                                  │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │ make_package_info() → PackageInfo                                     │   │
│  │   name: "pkg"           (from solvable)                               │   │
│  │   timestamp: 0          (from solvable - was 0)                       │   │
│  │   license: ""           (from solvable - was "")                      │   │
│  │   defaulted_keys: ["_initialized"]  ← ORIGINAL LIST LOST!             │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                           │                                                  │
│                           ▼                                                  │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │ write_repodata_record()                                               │   │
│  │   JSON has: {"timestamp": 0, "license": ""}                           │   │
│  │   Only "_initialized" in defaulted_keys → nothing erased              │   │
│  │   insert(index) doesn't overwrite existing keys                       │   │
│  │   WRITES STUB VALUES TO repodata_record.json                          │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```
