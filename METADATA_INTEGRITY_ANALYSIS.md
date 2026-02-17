# Metadata Integrity Analysis: PR #4110

## Executive Summary

PR #4110 fixes issue #4095: incorrect `repodata_record.json` metadata for
explicit installs. The core mechanism (`defaulted_keys` for per-field trust
tracking) is sound and necessary. However, the implementation has a bug in the
conda lockfile path and some minor issues worth addressing. This document
distills the comprehensive principles behind the changes and catalogs the
findings.

---

## The Principles of Metadata Integrity

### Principle 1: Provenance Determines Per-Field Trust

A `PackageInfo` object's field reliability depends on **where it came from**,
and trust is **per-field**, not per-object:

| Provenance | Trustworthy Fields | Stub/Default Fields |
|---|---|---|
| **Channel repodata** (solver) | ALL fields (including repodata patches) | None |
| **URL string** (`from_url()`) | name, version, build_string, channel, subdir, filename, md5/sha256 (if in URL hash) | build_number, license, timestamp, track_features, depends, constrains |
| **Conda lockfile** (trusted, sha256 present) | name, version, build, channel, platform, md5/sha256, url, **depends**, **constrains** | build_number, license, timestamp, track_features |
| **Conda lockfile** (untrusted, sha256 absent) | name, version, build, channel, platform, md5, url | build_number, license, timestamp, track_features, depends, constrains |
| **Mambajs lockfile** | name, version, build, channel, subdir, hash | build_number, license, timestamp, track_features, depends, constrains |
| **History entry** | *(never reaches `write_repodata_record()`)* | N/A |

The per-field nature of trust is critical. A lockfile with sha256 provides
trustworthy `depends` but still has stub values for `license` and `timestamp`.
No coarse-grained provenance enum can express this; per-field tracking is
required.

### Principle 2: The Merge Rule for `repodata_record.json`

When writing `repodata_record.json`, two data sources are merged:
1. **The `PackageInfo`** (from solver, URL, or lockfile)
2. **The `index.json`** (from inside the extracted tarball)

The merge rule depends on per-field trust:
- **Trusted fields** (not in `defaulted_keys`): `PackageInfo` value takes
  precedence. `index.json` only fills in keys not already present.
- **Stub fields** (listed in `defaulted_keys`): Erased before merge, so
  `index.json` provides the correct values.

This is implemented as:
1. `repodata_record = to_json(PackageInfo)` — full serialization
2. Erase fields listed in `defaulted_keys` (except `_initialized`)
3. `insert()` from `index.json` — only adds missing keys

This preserves channel patches (solver-derived packages have no stub fields)
while correctly backfilling URL-derived stubs from `index.json`.

### Principle 3: Normalization at the Write Boundary

Regardless of provenance, the final `repodata_record.json` should satisfy:
- `depends` and `constrains` are always present as JSON arrays (even if empty)
- `track_features` is omitted when empty (matching conda behavior)
- `md5` and `sha256` are always present (computed from tarball if needed)
- `size` is always present (computed from tarball if needed)

This normalization is applied in two separate code paths:
- `PackageFetcher::write_repodata_record()` in `package_fetcher.cpp`
- `construct()` in `micromamba/src/constructor.cpp`

### Principle 4: Trust Indicators for External Metadata

When metadata comes from external sources (lockfiles), its trustworthiness
must be assessed before deciding per-field trust:

- **sha256 present in lockfile entry**: Metadata is trustworthy. Dependencies
  and constrains provided by the lockfile should be trusted (removed from
  `defaulted_keys`), since they may reflect repodata patches.
- **sha256 absent**: Metadata may have been generated during the v2.1.1-v2.4.0
  bug period using corrupted `repodata_record.json`. Dependencies should be
  treated as untrusted (kept in `defaulted_keys`), allowing `index.json` to
  provide correct values.

### Principle 5: Cache Healing for Legacy Corruption

Caches written by v2.1.1-v2.4.0 may contain corrupted `repodata_record.json`
files. The corruption signature (`timestamp == 0 AND license == ""`) is used
to detect and invalidate these entries, triggering re-extraction. The false
positive risk is acceptable: no modern build system should produce packages
with both fields at their zero/empty defaults, and the only consequence of a
false positive is unnecessary re-extraction (not data corruption).

### Principle 6: Fail-Hard on Uninitialized Provenance

Every `PackageInfo` that reaches `write_repodata_record()` must have the
`_initialized` sentinel in `defaulted_keys`. This proves the object was
constructed through a proper code path (`from_url()`, `make_package_info()`,
or a lockfile parser) that established per-field trust. If missing, a
`std::logic_error` is thrown to catch programming bugs early.

---

## How the PR Implements These Principles

The PR uses the `defaulted_keys` field on `PackageInfo` as a **per-field trust
tracker**:

- `defaulted_keys = {"_initialized"}` — trust all fields (channel repodata)
- `defaulted_keys = {"_initialized", "field1", "field2", ...}` — listed fields
  are stubs to be replaced by `index.json`
- `defaulted_keys = {}` (empty) — error: not properly constructed

`defaulted_keys` is set at construction time and must survive the full journey
from creation to `write_repodata_record()`:

| Creation Path | Sets `defaulted_keys` | Reaches `write_repodata_record()` via |
|---|---|---|
| `from_url()` | URL stub fields listed | Direct → `MTransaction` → `PackageFetcher` |
| `make_package_info()` (solver) | `{"_initialized"}` only | Solver → `MTransaction` → `PackageFetcher` |
| Channel repodata JSON → solvable | `{"_initialized"}` set on solvable | `.solv` cache → solver → `PackageFetcher` |
| Conda lockfile parser | Copied from `from_url()` | `MTransaction` (lockfile) → `PackageFetcher` |
| Mambajs lockfile parser | Hardcoded stub list | `MTransaction` (lockfile) → `PackageFetcher` |
| `from_url()` via `channel_loader.cpp` | URL stub fields listed | Solver DB → solver → `PackageFetcher` |
| `read_history_url_entry()` | *(set but never consumed)* | Never reaches `write_repodata_record()` |

For the solver round-trip, `defaulted_keys` is serialized as a comma-separated
string in libsolv's `SOLVABLE_KEYWORDS` field and deserialized after solving.
Old `.solv` caches without this field get the fallback `{"_initialized"}`
(correct, since old caches came from channel repodata).

The constructor path (`micromamba/src/constructor.cpp`) is separate: it reads
cached channel repodata from `pkgs/cache/*.json` files (a third data source),
merges with `index.json`, and overlays URL-derived fields (`fn`, `url`,
`channel`). It never uses `defaulted_keys` because it keeps channel repodata
and URL data as separate JSON objects.

---

## Bugs and Gaps Identified

### Bug 1: Lockfile Dependencies Silently Discarded

**Severity: Medium.** The conda lockfile parser (`env_lockfile_conda.cpp`):
1. Parses sha256 from the lockfile (lines 48-51)
2. Calls `from_url()` → `defaulted_keys` includes `"depends"`, `"constrains"`
3. Copies `defaulted_keys` from `from_url()` result (line 79)
4. Populates `dependencies` from lockfile data (lines 82-89)
5. Populates `constrains` from lockfile data (lines 91-99)

After steps 4-5, `dependencies` and `constrains` have real values from the
lockfile, but `defaulted_keys` still lists them as stubs. When
`write_repodata_record()` runs, it erases them and replaces with `index.json`
values.

For trusted lockfiles (sha256 present), the fix should remove `"depends"` and
`"constrains"` from `defaulted_keys` after populating them. For untrusted
lockfiles (sha256 absent), keeping them in `defaulted_keys` is correct since
the dependency data may be corrupted.

**Tests added:** `test_env_lockfile.cpp` (parsing level) and
`test_package_fetcher.cpp` (merge level). Both currently fail.

### Issue 2: Dead Code in `history.cpp`

**Severity: Low.** `read_history_url_entry()` sets `defaulted_keys` but its
results only flow into `PackageDiff` (history display), never into
`PackageFetcher` or `write_repodata_record()`. The `defaulted_keys` set here
is dead code.

### Issue 3: `SOLVABLE_KEYWORDS` Repurposing

**Severity: Low risk.** The PR stores `defaulted_keys` in libsolv's
`SOLVABLE_KEYWORDS` field as a comma-separated string. This field is persisted
in `.solv` cache files. Concerns:
- Old mamba reading new caches: harmless (field is ignored)
- New mamba reading old caches: handled by fallback to `{"_initialized"}`
- Comma in key names: low risk (current keys don't contain commas)
- No validation on read: parses whatever is in the field

This is consistent with the existing pattern of repurposing `SOLVABLE_INSTALLSTATUS`
for `SolvableType`.

### Issue 4: Normalization Code Duplication

**Severity: Low.** The depends/constrains/track_features normalization logic
is copy-pasted between `package_fetcher.cpp` and `constructor.cpp`. These
should share a helper function.

### Issue 5: Fragile Per-Field Name Lists

**Severity: Low.** The `defaulted_keys` field name lists are hardcoded in 6
locations (7 counting the dead code in `history.cpp`). If a new field is added
to `PackageInfo`, all lists must be updated. This is inherent to the per-field
tracking approach and is the cost of its expressiveness.

---

## Why `defaulted_keys` Is the Right Mechanism

We initially explored replacing `defaulted_keys` with a simpler provenance
enum (`ChannelRepodata` / `UrlStub` / `Unknown`). This would centralize the
"what to trust" decision to `write_repodata_record()` instead of scattering
field-name lists across creation sites.

However, the provenance enum is insufficient because:

1. **Trust is per-field, not per-object.** A conda lockfile with sha256
   provides trustworthy `depends` but stub `license`. An enum can't express
   "trust depends but not license."
2. **The constructor has a separate merge path** with different data sources
   (cached channel repodata JSON files). The enum doesn't help there.
3. **The libsolv round-trip is still needed** for `from_url()` packages that
   enter the solver via `channel_loader.cpp`. Storing an enum is simpler than
   a comma-separated string, but the complexity savings are modest.
4. **The enum's overlay list** ("URL-known fields to preserve") becomes the
   new fragile maintenance point, just centralized.

The `defaulted_keys` mechanism is sound. The bug is not in the concept but in
the implementation: the conda lockfile parser doesn't update `defaulted_keys`
after populating fields with real data.

---

## Recommended Changes

1. **Fix the lockfile dependency bug** (Bug 1): In `env_lockfile_conda.cpp`,
   after populating dependencies/constrains from the lockfile, check if sha256
   is present. If so, remove `"depends"` and `"constrains"` from
   `defaulted_keys`. (~10 lines)

2. **Remove dead code** (Issue 2): Remove `defaulted_keys` assignment in
   `history.cpp`'s `read_history_url_entry()`. (~8 lines deleted)

3. **Extract normalization helper** (Issue 4): Move the shared
   depends/constrains/track_features normalization into a helper function
   used by both `package_fetcher.cpp` and `constructor.cpp`.

4. **Consider the fragile lists** (Issue 5): Optionally add a compile-time
   or test-time check that the `defaulted_keys` lists cover all expected
   fields. This is a nice-to-have, not a blocker.
