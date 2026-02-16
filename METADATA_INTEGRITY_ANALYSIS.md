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
of Principle 1 suggests a potentially simpler implementation:

```cpp
enum class PackageProvenance {
    Unknown,         // Not yet classified (error if it reaches write_repodata_record)
    ChannelRepodata, // All fields authoritative (from solver + channel repodata)
    UrlStub,         // Only URL-derivable fields are real (from from_url(), lockfiles)
};
```

The merge logic in `write_repodata_record()` would become:

```cpp
switch (m_package_info.provenance) {
    case PackageProvenance::ChannelRepodata:
        // PackageInfo is authoritative (solver, channel patches).
        // Start from PackageInfo, only backfill missing from index.json.
        repodata_record = m_package_info;   // to_json()
        repodata_record.insert(index.cbegin(), index.cend());
        break;
    case PackageProvenance::UrlStub:
        // PackageInfo has stubs for most fields.
        // Start from index.json, overlay the few URL-known fields.
        repodata_record = index;
        repodata_record["url"] = m_package_info.package_url;
        repodata_record["channel"] = m_package_info.channel;
        repodata_record["fn"] = m_package_info.filename;
        if (!m_package_info.md5.empty())
            repodata_record["md5"] = m_package_info.md5;
        if (!m_package_info.sha256.empty())
            repodata_record["sha256"] = m_package_info.sha256;
        break;
    case PackageProvenance::Unknown:
        throw std::logic_error("...");
}
// Then normalization (Principle 3)...
```

### Critical Assessment: Weaknesses of the Provenance Enum

**Weakness 1: There are TWO merge points, not one.**

`write_repodata_record()` in `package_fetcher.cpp` is only one merge point. The
second is in `micromamba/src/constructor.cpp`, which has completely different logic:
it reads cached channel repodata from `pkgs/cache/*.json` files (a third data
source that `write_repodata_record()` never sees). The constructor path:

- If cached channel repodata exists → use as base, backfill from `index.json`
- If missing → use `index.json`, overlay `md5`/`sha256`/`fn`/`url`/`channel`
  from the parsed URL

The constructor already had the correct merge logic before this PR (it never
confused URL stubs with channel repodata because it keeps them separate). The
PR only added normalization (depends/constrains/track_features) to this path.

A provenance enum only helps `write_repodata_record()`, not the constructor. The
normalization code would still need to be shared between both paths regardless.

**Weakness 2: The UrlStub merge is not perfectly symmetrical with defaulted_keys.**

The current PR's approach is:
1. `repodata_record = to_json(PackageInfo)` — full serialization including stubs
2. Erase stub fields listed in `defaulted_keys`
3. `insert()` from `index.json` (only adds missing)

The provenance enum approach would be:
1. `repodata_record = index` — start from `index.json`
2. Overlay URL-known fields from `PackageInfo`

These are NOT always equivalent. The `to_json()` serialization can produce fields
that `index.json` doesn't have and the explicit overlay list doesn't cover. For
example, `to_json()` always emits `"size": 0` and `"subdir": "linux-64"` for URL
packages. With the `defaulted_keys` approach, these are preserved (they're not in
the defaulted list). With the provenance enum approach, they'd need to be in the
explicit overlay list, or they'd come from `index.json` instead.

In practice, the differences are minor (`subdir` should match between URL and
`index.json`, and `size` is handled separately). But the overlay list in the
enum approach becomes the new fragile point — it's just centralized to one
location instead of seven, which is better but not zero-maintenance.

**Weakness 3: The provenance enum still needs to survive the libsolv round-trip.**

There is a real code path where `from_url()`-derived packages enter the solver:
`channel_loader.cpp` line 177-183 handles "package channels" (when a channel URL
points to a `.conda` file instead of a repository). These packages are created
via `from_url()`, pushed into the solver database via `add_repo_from_packages()`,
and after solving, extracted via `make_package_info()`.

So even with the enum approach, we still need to:
- Store the provenance in the solv-cpp solvable wrapper
- Retrieve it after the solver round-trip

However, storing a single integer is far simpler than a comma-separated string
in `SOLVABLE_KEYWORDS`. A single bit in `SOLVABLE_INSTALLSTATUS` (already
repurposed for `SolvableType`) or a small integer would suffice.

**Weakness 4: The "URL-known fields" list varies by package type.**

For a `.conda` URL, `name`, `version`, and `build_string` are parsed from the
filename. For a `.whl` URL, `build_string` is NOT available. For git URLs,
almost nothing is available. With `defaulted_keys`, each `from_url()` sub-path
declares exactly which fields are stubs. With the enum, the merge logic either:

- Needs sub-type awareness (undermining the simplicity)
- Or treats all URL types uniformly (which works in practice because
  `index.json` provides all the correct values for `name`, `version`,
  `build_string` anyway — overlaying from PackageInfo is harmless when they match)

The second option is viable: since the enum's UrlStub merge STARTS from
`index.json`, all tarball-internal fields (name, version, build, etc.) come from
the correct source. Only the external-origin fields (url, channel, fn, checksums)
need overlaying. This is actually a smaller and more obvious list than the
`defaulted_keys` approach's "fields to erase" list.

**Weakness 5: Future extensibility.**

If a future lockfile format provides richer metadata (e.g., `depends` from the
resolver plus `license` from some other source), a binary enum won't be
sufficient. You'd need either more enum variants or to go back to per-field
tracking. However, this is a YAGNI concern — the current needs are clearly binary.

### Advantages (still valid)

Despite the weaknesses, the provenance enum approach has real advantages:

1. **Centralizes the "what to trust" logic** to one location in
   `write_repodata_record()` instead of scattering it across 7 creation sites
2. **Simpler libsolv storage**: a single int instead of a comma-separated string
3. **Eliminates dead code**: no need for `defaulted_keys` in `history.cpp`
   (which never flows to `write_repodata_record()` anyway)
4. **Smaller attack surface**: the overlay list in the merge logic is small and
   obvious (`url`, `channel`, `fn`, `md5`, `sha256`) and corresponds to fields
   that only exist outside the tarball

### Test Retainability

Most tests verify *outcomes* (correct `repodata_record.json` content), not the
*mechanism* (`defaulted_keys` field contents). Here's the breakdown:

| Test | Retainable? | Notes |
|---|---|---|
| `write_repodata_record uses index.json for URL-derived metadata` | **Yes** | Outcome test |
| `write_repodata_record preserves channel patched empty depends` | **Yes** | Outcome test |
| `write_repodata_record preserves channel patched empty constrains` | **Yes** | Outcome test |
| `write_repodata_record fails without _initialized` | **Rewrite** | Change to test `Unknown` provenance |
| Cache healing test | **Yes** | Independent mechanism |
| `write_repodata_record ensures depends/constrains present` | **Yes** | Outcome test |
| `write_repodata_record omits empty track_features` | **Yes** | Outcome test |
| `write_repodata_record ensures both checksums` | **Yes** | Outcome test |
| `write_repodata_record backfills noarch` | **Yes** | Outcome test |
| `write_repodata_record fills size from tarball` | **Yes** | Outcome test |
| `PackageInfo::from_url populates defaulted_keys` (all sections) | **Rewrite** | Change to test provenance enum |
| solv-cpp `defaulted_keys` storage test | **Rewrite** | Change to test provenance storage |
| libsolv `defaulted_keys` round-trip tests | **Rewrite** | Change to test provenance round-trip |
| lockfile `_initialized` in `defaulted_keys` | **Rewrite** | Change to test provenance set |
| history `defaulted_keys` test | **Remove** | Dead code |
| Constructor `TestURLDerivedMetadata` (Python) | **Yes** | Outcome tests |
| Constructor `TestChannelPatchPreservation` (Python) | **Yes** | Outcome test |

**Bottom line**: ~15 of 20+ test cases are fully retainable or trivially
rewritable. The "rewrite" tests become simpler (checking an enum value instead
of verifying field name lists). Only the history test is eliminated.

---

## Summary

If reimplementing from scratch based on the 3 principles:

1. Add a `PackageProvenance` enum to `PackageInfo`
2. Set provenance in `from_url()` → `UrlStub`
3. Set provenance in `make_package_info()` / JSON solvable creation → `ChannelRepodata`
4. Set provenance in lockfile parsers → `UrlStub`
5. Store/retrieve provenance through solv-cpp wrapper (simple int, not string)
6. Modify `write_repodata_record()` to branch on provenance (Principle 2)
7. Add normalization (Principle 3) to `write_repodata_record()` and extract a
   shared helper for `constructor.cpp`
8. Optionally add cache healing — this is independent and could be a separate PR

The provenance enum is NOT a magic bullet — it still requires libsolv round-trip
support, still can't help the constructor path (which has different data sources),
and still has a list of "URL-known fields" to maintain. But that list is small,
obvious, centralized, and corresponds to a clear semantic concept ("fields that
only exist outside the tarball").

The real question is whether the simplification is worth the reimplementation
cost, or whether it's better to fix the specific bugs in the current PR
(dead code in history.cpp, DRY violation in normalization) and ship it.
