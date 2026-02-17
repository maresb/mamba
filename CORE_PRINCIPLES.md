# Core Principles for Metadata Integrity in `repodata_record.json`

*Context: [GitHub issue #4095](https://github.com/mamba-org/mamba/issues/4095)*

## Background

When a conda package is installed, a `repodata_record.json` file is written
alongside the extracted package contents. This file is the authoritative local
record of the package's metadata and is consumed by downstream tools (conda-lock,
`mamba list`, environment cloning, etc.).

The metadata in `repodata_record.json` is assembled from multiple sources, each
with different levels of reliability. Getting this merge wrong causes metadata
corruption that silently propagates to downstream consumers.

Currently (as of main), the merge in `PackageFetcher::write_repodata_record()`
works as follows:
1. Serialize the `PackageInfo` object to JSON (the "repodata" side)
2. Unconditionally erase empty `depends`/`constrains` arrays
3. `insert()` from `index.json` (only adds keys not already present)

This logic has two problems:
- For **URL-derived packages**, the `PackageInfo` contains stub/zero values for
  fields that could not be parsed from the URL (e.g., `license`, `timestamp`,
  `build_number`). These stubs take precedence over correct values in `index.json`.
- For **solver-derived packages**, the unconditional erasure of empty
  `depends`/`constrains` silently undoes channel repodata patches that
  intentionally set these to `[]`.

The constructor path (`micromamba/src/constructor.cpp`) has a separate merge
implementation that is mostly correct: it looks up cached channel repodata from
`pkgs/cache/*.json` files, uses that as the base, and backfills from `index.json`.
When no cached repodata exists, it falls back to `index.json` directly.

The following principles define the rules for correct behavior.

---

## Principle 1: Per-Field Trust Depends on Origin

A `PackageInfo` object aggregates metadata from a single origin, but not all
fields are equally reliable. The trustworthiness of each field depends on how
the `PackageInfo` was created:

**Channel repodata (via solver):** All fields are authoritative. The solver
received this data from channel `repodata.json`, which may include repodata
patches. Every field should be trusted, including intentionally empty arrays.

**URL string (`PackageInfo::from_url()`):** Only fields derivable from the URL
are real: `name`, `version`, `build_string`, `channel`, `subdir`, `filename`,
and optionally `md5`/`sha256` (from the URL hash fragment). All other fields
(`build_number`, `license`, `timestamp`, `track_features`, `depends`,
`constrains`) use struct defaults (0, `""`, `[]`) and are stubs.

**Conda lockfile (conda-lock YAML format):** The lockfile provides `name`,
`version`, `build_string`, `channel`, `platform`, `url`, `md5`, and optionally
`sha256`. It also provides per-package `dependencies` and `constrains` maps.
Other fields (`build_number`, `license`, `timestamp`, `track_features`) are not
in the lockfile and use struct defaults.

**Mambajs lockfile (JSON format):** Provides `name`, `version`, `build`,
`channel`, `subdir`, and a hash. Does not provide `dependencies`, `constrains`,
or other detailed metadata.

**Installed package records (`from_json()`):** Deserialized from existing
`repodata_record.json` files for display/query. These objects are used for
`mamba list`, dependency computation, and as input to the solver's installed
repository. They are never passed to `write_repodata_record()`.

## Principle 2: The Merge Must Distinguish Stub Fields from Authoritative Fields

When writing `repodata_record.json`, two data sources are merged:
1. The `PackageInfo` — may contain a mix of authoritative and stub fields
2. The `index.json` — the metadata baked into the tarball by the package builder

The merge rule must be:
- **Authoritative fields** from the `PackageInfo` take precedence over
  `index.json`. This preserves channel repodata patches.
- **Stub fields** from the `PackageInfo` must yield to `index.json`, which has
  the correct values from the package builder.

Erasing fields based solely on their *value* (e.g., "erase if empty array") is
wrong because it conflates intentionally empty values (from repodata patches)
with stub defaults (from URL parsing). The merge must be based on *origin
knowledge*, not value inspection.

## Principle 3: Trust Indicators for External Metadata Sources

When metadata comes from external sources such as lockfiles, the
trustworthiness of specific fields must be assessed:

- **sha256 present in lockfile entry:** The lockfile metadata is trustworthy.
  The `dependencies` and `constrains` fields provided by the lockfile should be
  treated as authoritative (not stubs), since they may reflect repodata patches
  applied during the solve that generated the lockfile.

- **sha256 absent from lockfile entry:** The lockfile may have been generated
  during a period when micromamba produced corrupted `repodata_record.json`
  files (v2.1.1-v2.4.0). In this case, `dependencies` and `constrains` from
  the lockfile should be treated as potentially unreliable stubs, allowing
  `index.json` to provide correct values.

## Principle 4: Every Code Path to `repodata_record.json` Must Declare Field Trust

There are multiple code paths that create `PackageInfo` objects which
eventually reach the `repodata_record.json` write logic:

- **Explicit URL installs:** `PackageInfo::from_url()` → `MTransaction` →
  `PackageFetcher` → `write_repodata_record()`
- **Solver-based installs:** Channel repodata → libsolv → `make_package_info()`
  → `MTransaction` → `PackageFetcher` → `write_repodata_record()`
- **Lockfile installs (conda format):** Lockfile parser → `MTransaction` →
  `PackageFetcher` → `write_repodata_record()`
- **Lockfile installs (mambajs format):** Lockfile parser → `MTransaction` →
  `PackageFetcher` → `write_repodata_record()`
- **Package-channel URLs:** `from_url()` → solver database →
  `make_package_info()` → `MTransaction` → `PackageFetcher` →
  `write_repodata_record()`
- **Constructor path:** URL → cached channel repodata JSON + `index.json`
  → direct JSON merge (separate from `write_repodata_record()`)

Each of these paths must establish which fields are authoritative and which are
stubs. If a code path reaches `write_repodata_record()` without having
established field trust, the result is undefined and should be treated as a
programming error.

Note: Some code paths create `PackageInfo` objects that never reach
`write_repodata_record()` (e.g., `read_history_url_entry()` for history
display, `from_json()` for installed package queries). These do not need
field trust annotations.

## Principle 5: Field Trust Must Survive the Solver Round-Trip

When a `PackageInfo` enters the solver (e.g., URL-derived packages added via
`add_repo_from_packages()` in `channel_loader.cpp`), the field trust
information must be preserved through the libsolv round-trip:
`PackageInfo` → solvable → solver → `make_package_info()` → `PackageInfo`.

Without this, URL-derived packages that pass through the solver lose their
trust annotations, and `write_repodata_record()` cannot distinguish their
stub fields from authoritative solver-derived fields.

## Principle 6: Normalization at the Write Boundary

Regardless of origin, the final `repodata_record.json` should satisfy
consistency invariants matching conda behavior:

- `depends` and `constrains` are always present as JSON arrays (even if empty).
  Some packages (e.g., `nlohmann_json-abi`) don't have `depends` in their
  `index.json`, so this must be enforced at write time.
- `track_features` is omitted when empty (reduces JSON noise, matches conda).
- `md5` and `sha256` are always present. If not available from the
  `PackageInfo` or `index.json`, they should be computed from the tarball.
- `size` is always present. If zero or missing, use the tarball file size.

This normalization must be applied in all code paths that write
`repodata_record.json` (currently `PackageFetcher::write_repodata_record()`
and the constructor path in `constructor.cpp`).

## Principle 7: Healing Legacy Cache Corruption

Caches written by versions v2.1.1-v2.4.0 may contain `repodata_record.json`
files with corrupted metadata (zeroed `timestamp`, empty `license`, zeroed
`build_number`, empty `track_features`). These corrupted caches persist after
upgrading and silently provide bad metadata.

The corruption signature `timestamp == 0 AND license == ""` should be used to
detect potentially corrupted cache entries. When detected, the cache entry
should be invalidated to trigger re-extraction from the tarball, allowing
`write_repodata_record()` to produce correct metadata using `index.json`.

The false positive risk (a legitimate package with both `timestamp == 0` and
`license == ""`) is acceptable: no modern build system should produce packages
with these values, and the only consequence is unnecessary re-extraction, not
data corruption.
