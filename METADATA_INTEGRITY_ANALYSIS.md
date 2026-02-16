# Metadata Integrity Analysis: PR #4110

## Executive Summary

The PR addresses a real and well-defined bug (issue #4095), but the implementation
has grown into a complex multi-tentacled change touching 19 files across 5 subsystems.
The core problem can be distilled to **3 precise principles**, and most of the PR's
complexity comes from propagating a single mechanism (`defaulted_keys`) through
every code path rather than addressing the root cause at the single point where
the merge actually happens.

---

## The 3 Fundamental Principles of Metadata Integrity

### Principle 1: Provenance Determines Trust

A `PackageInfo` object's field reliability depends entirely on **where it came from**:

| Provenance | Trustworthy Fields | Stub/Default Fields |
|---|---|---|
| **Channel repodata** (solver) | ALL fields (including repodata patches) | None |
| **URL string** (`from_url()`) | name, version, build_string, channel, subdir, filename, md5/sha256 (if in URL) | build_number, license, timestamp, track_features, depends, constrains |
| **Lockfile** (conda format) | name, version, build, channel, platform, md5/sha256, url | build_number, license, timestamp, track_features |
| **Lockfile** (mambajs format) | name, version, build, channel, subdir, hash | build_number, license, timestamp, track_features, depends, constrains |
| **History entry** | name, version, build, channel | Everything else |

### Principle 2: The Merge Rule for `repodata_record.json`

When writing `repodata_record.json`, two data sources are merged:
1. **The `PackageInfo`** (from solver or URL)
2. **The `index.json`** (from inside the extracted tarball)

The merge rule is:
- **Channel-repodata-derived packages**: `PackageInfo` fields take precedence over
  `index.json`. Use `insert()` (only add missing keys). This preserves repodata
  patches, including intentionally empty `depends`/`constrains`.
- **URL-derived packages**: `index.json` fields take precedence over `PackageInfo`
  stub fields. Erase stub fields first, then `insert()` from both sources.

### Principle 3: Normalization at the Write Boundary

Regardless of provenance, the final `repodata_record.json` should satisfy:
- `depends` and `constrains` are always present as JSON arrays (even if empty)
- `track_features` is omitted when empty (matching conda behavior)
- `md5` and `sha256` are always present (computed from tarball if needed)
- `size` is always present (computed from tarball if needed)

---

## How the PR Implements These Principles

The PR uses the `defaulted_keys` field on `PackageInfo` as a **per-field provenance
tracker**:

- `defaulted_keys = {"_initialized"}` → channel-repodata provenance (trust all fields)
- `defaulted_keys = {"_initialized", "field1", "field2", ...}` → URL provenance
  (listed fields are stubs)
- `defaulted_keys = {}` (empty) → **error** (PackageInfo not properly constructed)

The `_initialized` sentinel enables a fail-hard check in `write_repodata_record()`.

---

## Bugs and Gaps Identified

### Bug 1: Unnecessary Dead Code in `history.cpp`

`read_history_url_entry()` sets `defaulted_keys` (commit `ebf9f138`), but
its results are **only used for `PackageDiff` computation** (history display /
`mamba env diff`), never for `PackageFetcher` or `write_repodata_record()`. The
`defaulted_keys` set here is dead code that adds confusion.

**Evidence**: `read_history_url_entry()` is called at lines 416 and 422 of
`history.cpp`, storing results into `rev.removed_pkg` / `rev.installed_pkg` maps
that feed into `PackageDiff`. These never flow into `build_fetchers()` or
`PackageFetcher`.

### Bug 2: `SOLVABLE_KEYWORDS` Repurposing Risk

The PR stores `defaulted_keys` as a comma-separated string in libsolv's
`SOLVABLE_KEYWORDS` field. Concerns:

1. **Persisted in `.solv` cache files**: `repo_write()` serializes all solvable
   attributes. New mamba writing `.solv` caches with `SOLVABLE_KEYWORDS` populated
   will produce caches that old mamba versions might interpret differently if they
   ever start using this field.
2. **Comma in key names**: If any future key name contains a comma, the
   serialization breaks (low risk but fragile).
3. **No validation on read**: `defaulted_keys()` parses whatever string is in
   `SOLVABLE_KEYWORDS` without checking it's actually `defaulted_keys` data vs.
   some other use of the field.

### Bug 3: Constructor Path Has No `_initialized` Verification

`micromamba/src/constructor.cpp` writes `repodata_record.json` with its own
independent code path that does NOT use `write_repodata_record()` and has no
`_initialized` check. This path trusts whatever `repodata_record` JSON it finds
in the cached channel data (`pkgs/cache/*.json`). If that cache is missing, it
falls back to `index.json` — which is actually the correct behavior for that
scenario.

The constructor path is mostly correct because it sources its repodata from
cached channel data files (not from `PackageInfo` stubs). However, the
normalization logic (depends/constrains/track_features) is **copy-pasted** from
`package_fetcher.cpp`, creating a DRY violation.

### Gap 4: Fragile Per-Field Name Lists

The `defaulted_keys` field name lists are hardcoded in **7 separate locations**:

1. `package_info.cpp` — `parse_url()` conda packages
2. `package_info.cpp` — `parse_url()` wheel packages (2 locations)
3. `package_info.cpp` — `parse_url()` tar.gz packages
4. `package_info.cpp` — `from_url()` git packages
5. `env_lockfile_mambajs.cpp`
6. `history.cpp` (dead code, see Bug 1)

If a new field is added to `PackageInfo` in the future, all these lists must be
updated. If one is missed, stub values will silently leak into
`repodata_record.json`.

### Gap 5: Cache Healing Heuristic False Positives

The corruption detection in `package_cache.cpp` uses `timestamp == 0 AND
license == ""` as the corruption signature. While the PR's comments acknowledge
the low false-positive risk, there are legitimate packages where this could
trigger unnecessarily:

- Packages built with older tooling that didn't set timestamps
- Packages with no license information (e.g., internal/proprietary)

The consequence is only unnecessary re-extraction (not data corruption), but
this could cause unexpected performance degradation in large environments with
many such packages.

### Gap 6: `defaulted_keys` in `env_lockfile_conda.cpp` Relies on `from_url()` Indirectly

In `env_lockfile_conda.cpp`, `defaulted_keys` is copied from the parsed URL
info (`maybe_parsed_info->defaulted_keys`). This works because the conda
lockfile format stores URLs that are parsed with `from_url()`. However, if the
lockfile URL parsing changes or additional fields are available in the lockfile
format, this indirect coupling could break.

---

## A Simpler Alternative: Provenance Enum

Instead of tracking individual field names in `defaulted_keys`, the core insight
of Principle 1 suggests a much simpler implementation:

```cpp
enum class PackageProvenance {
    Unknown,       // Not yet classified (error if it reaches write_repodata_record)
    ChannelRepodata, // All fields authoritative
    UrlStub,       // Only URL-derivable fields are real
};
```

The merge logic in `write_repodata_record()` becomes:

```cpp
switch (m_package_info.provenance) {
    case PackageProvenance::ChannelRepodata:
        // repodata_record = PackageInfo; insert missing from index.json
        repodata_record.insert(index.cbegin(), index.cend());
        break;
    case PackageProvenance::UrlStub:
        // repodata_record = index.json; overwrite with URL-known fields
        repodata_record = index;
        repodata_record["url"] = m_package_info.package_url;
        repodata_record["channel"] = m_package_info.channel;
        // ... other URL-known fields
        break;
    case PackageProvenance::Unknown:
        throw std::logic_error("...");
}
```

**Advantages over `defaulted_keys`**:
1. No per-field name lists to maintain (eliminates Gap 4 entirely)
2. No need to store/serialize through libsolv (eliminates Bug 2 entirely) —
   just store an enum/int in `SOLVABLE_KEYWORDS` or `SOLVABLE_INSTALLSTATUS`
3. Single point of truth: the provenance is set once at creation, merge logic
   is centralized
4. Much smaller diff: only need to add the enum field, set it in `from_url()`
   and `make_package_info()`, and modify `write_repodata_record()`
5. The merge logic explicitly documents the "URL-known fields" in one place

**What it preserves**:
- Principle 2 (provenance-dependent merge) — same outcome
- Principle 3 (normalization) — same normalization logic
- Fail-hard on `Unknown` — same safety net
- Channel patch preservation — same behavior for `ChannelRepodata`

**What it eliminates**:
- 7 separate hardcoded field-name lists → 0
- `SOLVABLE_KEYWORDS` serialization/deserialization → trivial int
- `defaulted_keys` propagation through lockfile parsers → simple enum set
- History.cpp dead code → not needed at all

---

## Summary of Recommended Changes

If reimplementing from scratch based on the 3 principles:

1. Add a `PackageProvenance` enum to `PackageInfo` (or use `defaulted_keys` with
   just `"_initialized"` vs empty as a boolean signal — even that would be simpler)
2. Set provenance in `from_url()` → `UrlStub`
3. Set provenance in `make_package_info()` / JSON solvable creation → `ChannelRepodata`
4. Set provenance in lockfile parsers → `UrlStub` (since they use URL-derived data)
5. Modify `write_repodata_record()` to branch on provenance
6. Add the normalization (Principle 3) to `write_repodata_record()` — this part
   of the PR is correct and needed
7. Optionally add cache healing — this is independent and could be a separate PR
8. Extract shared normalization logic between `package_fetcher.cpp` and
   `constructor.cpp` into a helper function

Total touch points: ~5 files instead of 19. The core logic change is ~50 lines
instead of ~2100.
