# All Paths Writing `repodata_record.json`

## TL;DR

- Only **two code locations** emit `info/repodata_record.json`: `PackageFetcher::write_repodata_record()` during normal transactions and the `construct()` helper used by `micromamba constructor --extract-conda-pkgs`.
- Both paths merge solver/download-time metadata with `info/index.json`, but they differ in their base JSON (PackageInfo vs cached repodata).
- Any malformed `repodata_record.json` must originate from one of these two writers; every other component only reads or validates the file.

### Quick Reference

#### PackageFetcher write

- **Source file**: `libmamba/src/core/package_fetcher.cpp` (`write_repodata_record`)
- **Direct caller**: `PackageFetcher::extract()`
- **Typical entry points**: `mamba`/`micromamba` install/update/create/remove, lockfile or explicit
  transactions, any library user running `MTransaction::execute()`
- **Notes**: Runs for every package that is extracted (including `--download-only`).

#### Constructor write

- **Source file**: `micromamba/src/constructor.cpp` (`construct`, `extract_conda_pkgs` branch)
- **Direct caller**: CLI callback from `set_constructor_command()`
- **Typical entry points**: `micromamba constructor --extract-conda-pkgs -p <prefix>`
- **Notes**: Iterates `prefix/pkgs/urls`; may reuse cached repodata entries.

---

## Path 1 – `PackageFetcher::write_repodata_record()`

### Location & Core Logic

- `libmamba/src/core/package_fetcher.cpp`, `void PackageFetcher::write_repodata_record(const fs::u8path& base_path) const`.
- Steps:
  1. Load `base_path/info/index.json`.
  2. Serialize `m_package_info` into JSON (`repodata_record`).
  3. Remove empty `depends`/`constrains` arrays that originate from explicit URL specs.
  4. `repodata_record.insert(index.cbegin(), index.cend())` (adds missing keys, never overwrites).
  5. Ensure `size` is populated (backfill from `m_tarball_path` when absent or zero).
  6. Dump JSON with 4-space indent to `base_path/info/repodata_record.json`.

### Call Chain & Trigger Conditions

```text
PackageFetcher::extract()
  └─ write_repodata_record()

PackageExtractTask::run[(downloaded_size?)]
  └─ PackageFetcher::extract()

MTransaction::fetch_extract_packages()
  ├─ build_fetchers()
  ├─ partition by needs_download / needs_extract
  ├─ schedule downloads + extractions
  └─ wait for PackageExtractTask futures

MTransaction::execute()
  └─ fetch_extract_packages()   (skipped only when ctx.dry_run)
```

The write occurs when **all** of the following are true:

1. `PackageFetcher` decided `m_needs_extract == true` (determined in its constructor by consulting `MultiPackageCache`).
2. Extraction succeeded (`extract_impl` finished without throwing).
3. Not in `ctx.dry_run` mode (because `fetch_extract_packages` is skipped).

Additional behavior:

- `ctx.download_only` still extracts and writes before returning.
- `clear_extract_path()` guarantees the extracted directory (and the file) is recreated from scratch each time; no incremental updates.
- `PackageFetcherSemaphore` serializes extraction to avoid races on shared cache directories.

### Entry Points That Reach This Path

- `mamba install/update/create/remove`
- `micromamba install/update/create/remove`
- Transactions generated from:
  - Solver outputs (`solution.packages_to_install()`).
  - Explicit files (`--file explicit.txt` or `micromamba install <url>`), via `create_explicit_transaction_from_urls()`.
  - Lockfiles (`micromamba install -f lockfile.yml` / Conda-lock), via `create_explicit_transaction_from_lockfile()`.
- Any direct embedding that constructs `MTransaction` and calls `.execute()`.
- Unit tests such as `libmamba/tests/src/core/test_package_fetcher.cpp`.

### `PackageInfo` Provenance & Metadata Quality

`m_package_info` supplies the initial JSON. Its accuracy depends on how it was created:

1. **Solver-based installs** (`solver::libsolv::make_package_info`) → full metadata from channel repodata (including patches).
2. **Explicit URLs** (`specs::PackageInfo::from_url`) → many stub defaults (timestamp `0`, license `""`, build_number `0`, empty depends/constrains, etc.).
3. **Lockfiles** (e.g., `env_lockfile_conda.cpp`) → hybrid: start from URL parsing then overlay lockfile fields such as dependencies and hashes.

Because `nlohmann::json::insert` never overwrites existing keys, stub fields emitted by `from_url()`
persist unless they were erased beforehand. Current fixes remove empty `depends`/`constrains`, and
the `defaulted_keys` mechanism tracks other stub fields so they can be erased before merge, allowing
`index.json` (which already reflects repodata patches) to repopulate them with correct values.

### Data Inputs

| Field                                    | Primary source                                               | Fallback / notes                            |
| ---------------------------------------- | ------------------------------------------------------------ | ------------------------------------------- |
| `name`, `version`, `build`, hashes, etc. | `m_package_info`                                             | Derived from solver or URL/lockfile parsing |
| `depends`, `constrains`                  | `index.json` unless `m_package_info` populated them          | Empty arrays removed so patched values win  |
| `track_features`, `timestamp`, `license` | `index.json` when key was erased; otherwise `m_package_info` | Requires erasing defaulted keys first       |
| `size`                                   | `index.json` or `m_package_info`                             | Backfilled via `fs::file_size` when missing |

### Observability & Healing

- `PackageCacheData::has_valid_extracted_dir()` (`libmamba/src/core/package_cache.cpp`) reads the
  file to validate cached extractions (size, hashes, URL/channel). When validation fails, the cache
  entry is deleted, forcing re-extraction and rewrites.
- `LinkPackage::execute()` (`libmamba/src/core/link.cpp`) reads the record when linking.
- Offline repo builders (`channel_loader.cpp::create_repo_from_pkgs_dir`) load existing records to
  synthesize local channels.

---

## Path 2 – `micromamba constructor --extract-conda-pkgs`

### Location & Core Logic

- `micromamba/src/constructor.cpp`, inside `construct(Configuration&, const fs::u8path&, bool extract_conda_pkgs, bool extract_tarball)`.
- When `extract_conda_pkgs` is true:
  1. Read `<prefix>/pkgs/urls` containing package URLs.
  2. For each entry, parse into `specs::PackageInfo` via `from_url()`.
  3. Extract the tarball into the package cache (`extract()` returns `base_path`).
  4. Attempt to find the package in cached repodata JSON (`<prefix>/pkgs/cache/<cache_name>.json`). If found, use that object as `repodata_record`.
  5. Read `base_path/info/index.json`.
  6. If cached repodata exists, merge any missing keys from `index.json`; otherwise use `index.json` as the base and backfill `size`, `md5`, `sha256` from the tarball / URL metadata.
  7. Overwrite `fn`, `url`, `channel` with values derived from the parsed URL to reflect the actual download source.
  8. Ensure `size` is non-zero (fall back to `fs::file_size(entry)`).
  9. Write `repodata_record.json` with 4-space indentation.

### Call Chain & Trigger Conditions

```text
micromamba constructor --extract-conda-pkgs -p <prefix>
  └─ set_constructor_command(cfg)
        └─ construct(cfg, prefix, extract_conda_pkgs=true, extract_tarball)
             └─ for raw_url in <prefix>/pkgs/urls:
                   extract(entry)
                   synthesize repodata_record.json
```

Write occurs per package when:

1. `--extract-conda-pkgs` flag is provided.
2. `<prefix>/pkgs/urls` exists and lists packages (typically created by `micromamba install --download-only` or by custom tooling).
3. Extraction of each tarball succeeds.
4. Either cached repodata contains the entry _or_ `info/index.json` is present (the tarball always contains it).

### Data Inputs & Priorities

| Scenario            | Base JSON                                    | Secondary merge                              | Always overwritten                           |
| ------------------- | -------------------------------------------- | -------------------------------------------- | -------------------------------------------- |
| Cached repodata hit | Cached package entry (already patch-applied) | `index.json` inserts missing keys            | `fn`, `url`, `channel`; `size` fixed if zero |
| No cached entry     | `index.json`                                 | Tarball metadata for `size`, `md5`, `sha256` | Same as above                                |

Unlike Path 1, this branch never serializes `PackageInfo` wholesale; it only uses `PackageInfo` for URL-derived fields and checksum hints.

### Usage Notes

- Independent of `--extract-tarball`; that flag handles single-tarball extraction and never touches `repodata_record.json`.
- Primarily consumed by conda-constructor workflows or bespoke automation that pre-populates `pkgs/urls`.
- Single-threaded: no semaphore needed.

---

## Reads, Non-Writes, and Cache Healing

- `PackageCacheData::has_valid_extracted_dir()` ensures cached records match expectations; on failure it deletes the dir, forcing re-extraction (and therefore a fresh write via Path 1).
- `LinkPackage::execute()` reads the record to gather metadata during linking.
- `channel_loader.cpp::create_repo_from_pkgs_dir()` consumes existing records to build offline repositories.
- No other file writes `info/repodata_record.json`. All references outside the two paths above merely read or validate it.

---

## Condition Matrix

| Scenario                                                  | Write? | Reason                                                               |
| --------------------------------------------------------- | ------ | -------------------------------------------------------------------- |
| `mamba install foo` (normal execution)                    | Yes    | PackageFetcher extracts and writes for every package lacking cache.  |
| `mamba install foo --download-only`                       | Yes    | Downloads, validates, extracts, writes, then returns before linking. |
| `mamba install foo --dry-run`                             | No     | `MTransaction::execute()` exits before `fetch_extract_packages()`.   |
| `mamba clean --packages` followed by a new install        | Yes    | Cache removal forces re-extraction, re-triggering Path 1.            |
| `micromamba constructor --extract-conda-pkgs -p <prefix>` | Yes    | Path 2 runs once per URL in `<prefix>/pkgs/urls`.                    |
| `micromamba constructor --extract-tarball` only           | No     | That branch never calls the repodata writer.                         |

---

## Known Pitfalls & Fixes

- **Stub metadata from URL installs:** `PackageInfo::from_url()` emits placeholder values.
  Unless those keys are erased before merging, `repodata_record.json` inherits incorrect defaults
  (e.g., `timestamp: 0`). Recent fixes rely on `defaulted_keys` bookkeeping to remove placeholders
  so `index.json` (or repodata patches) supply accurate values.
- **Cache corruption detection:** Comparing stored `size`/hash/URL during
  `has_valid_extracted_dir()` helps detect stale or manually edited caches; failures automatically
  trigger a clean re-extraction that regenerates the record via Path 1.

---

## Testing Hooks

- `libmamba/tests/src/core/test_package_fetcher.cpp` directly exercises `PackageFetcher::extract()` and `write_repodata_record()` to validate merge and file output logic.
- Integration tests that run full transactions implicitly cover the Path 1 writer; constructor-specific tests cover Path 2.

---

## Takeaways

1. There are exactly two writers; diagnosing issues reduces to determining which path ran and what inputs it saw (`PackageInfo`, `index.json`, optional cached repodata).
2. `nlohmann::json::insert` semantics are key: removing placeholder keys before merging is the only way to let `index.json` values win.
3. Cache validation and offline tooling rely on accurate `repodata_record.json`, so ensuring both paths harmonize their inputs is essential for reproducible installs and constructor workflows.
