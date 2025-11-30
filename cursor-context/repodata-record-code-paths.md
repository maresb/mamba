# `repodata_record.json` write paths

## How the writers were identified
- `rg "repodata_record.json"` across the repository shows only two places that open the file for output: `libmamba/src/core/package_fetcher.cpp` and `micromamba/src/constructor.cpp`. Every other reference reads or validates the file, so these two sites fully describe how the file can be created or rewritten.

## Path 1 – Package extraction via `PackageFetcher`

### Where the write happens
```431:466:libmamba/src/core/package_fetcher.cpp
void PackageFetcher::write_repodata_record(const fs::u8path& base_path) const
{
    const fs::u8path repodata_record_path = base_path / "info" / "repodata_record.json";
    const fs::u8path index_path = base_path / "info" / "index.json";

    nlohmann::json index;
    std::ifstream index_file = open_ifstream(index_path);
    index_file >> index;

    nlohmann::json repodata_record = m_package_info;

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

    repodata_record.insert(index.cbegin(), index.cend());

    if (repodata_record.find("size") == repodata_record.end() || repodata_record["size"] == 0)
    {
        repodata_record["size"] = fs::file_size(m_tarball_path);
    }

    std::ofstream repodata_record_file(repodata_record_path.std_path());
    repodata_record_file << repodata_record.dump(4);
}
```

### Reverse-engineered call chain & gating conditions
- `PackageFetcher::extract` always calls `write_repodata_record` immediately after the archive is extracted. It first deletes any existing extraction directory, guaranteeing the file is regenerated on each extraction and never incrementally updated.
```277:311:libmamba/src/core/package_fetcher.cpp
bool PackageFetcher::extract(const ExtractOptions& options, progress_callback_t* cb)
{
    std::lock_guard<counting_semaphore> lock(PackageFetcherSemaphore::semaphore);
    const fs::u8path extract_path = get_extract_path(filename(), m_cache_path);
    clear_extract_path(extract_path);
    extract_impl(m_tarball_path, extract_path, options);
    write_repodata_record(extract_path);
    update_urls_txt();
    ...
}
```
- `PackageFetcher` instances are only asked to extract packages that lack a ready-to-use extracted cache. The constructor inspects `MultiPackageCache` and sets `m_needs_extract`/`m_needs_download` accordingly. If an extracted directory already exists, no extraction (and thus no write) takes place.
```87:120:libmamba/src/core/package_fetcher.cpp
PackageFetcher::PackageFetcher(const specs::PackageInfo& pkg_info, MultiPackageCache& caches)
    : m_package_info(pkg_info)
{
    const fs::u8path extracted_cache = caches.get_extracted_dir_path(m_package_info);
    if (extracted_cache.empty())
    {
        const fs::u8path tarball_cache = caches.get_tarball_path(m_package_info);
        auto& cache = caches.first_writable_cache(true);
        m_cache_path = cache.path();
        if (!tarball_cache.empty())
        {
            m_tarball_path = tarball_cache / filename();
            m_needs_extract = true;
        }
        else
        {
            m_tarball_path = m_cache_path / filename();
            m_needs_extract = true;
            m_needs_download = true;
        }
    }
    else
    {
        LOG_DEBUG << "Using cached '" << name() << "'";
    }
}
```
- `MTransaction::fetch_extract_packages` partitions the fetchers so that only those with `needs_extract()==true` schedule `PackageExtractTask::run`, which in turn calls `PackageFetcher::extract`. Any package with a valid extracted cache skips this entire chain and never rewrites its `repodata_record.json`.
```760:829:libmamba/src/core/transaction.cpp
FetcherList fetchers = build_fetchers(...);
auto download_end = std::partition(fetchers.begin(), fetchers.end(), [](const auto& f) { return f.needs_download(); });
auto extract_end = std::partition(download_end, fetchers.end(), [](const auto& f) { return f.needs_extract(); });
ExtractTaskList extract_tasks = build_extract_tasks(..., fetchers, extract_size);
... // download requests scheduled
for (auto& task : extract_trackers)
{
    task.wait(); // blocks until PackageFetcher::extract finishes
}
```
- `MTransaction::execute` is the only production caller of `fetch_extract_packages`. It aborts early when `--dry-run` is set; otherwise it always downloads and extracts packages before either linking them or, in `--download-only` mode, returning immediately after extraction. This means `repodata_record.json` will be written for every package extracted during any `install`, `update`, `create`, or lockfile-apply transaction that is not a dry run.
```362:396:libmamba/src/core/transaction.cpp
if (ctx.dry_run)
{
    return true; // no extraction, no writes
}
...
fetch_extract_packages(ctx, channel_context);
if (ctx.download_only)
{
    return true; // extraction already happened, so repodata_record.json was written
}
```
- `build_fetchers` iterates over `solution.packages_to_install()`, so every install action produced by the solver (or supplied explicitly from a lock file) creates a `PackageFetcher`. That includes CLI entry points such as `mamba install`, `mamba update`, `mamba create`, `micromamba install`, etc., as well as any other caller that drives `MTransaction::execute` through the C++ API.
```551:635:libmamba/src/core/transaction.cpp
for (const auto& pkg : solution.packages_to_install())
{
    ...
    fetchers.emplace_back(pkg, multi_cache);
}
```

### Data sources and mutation rules
- The JSON starts as a serialization of `m_package_info`, so it carries solver-time metadata such as `channel`, `fn`, `url`, hashes, platform, dependency metadata, etc.
- Before merging with `info/index.json`, the function erases empty `depends`/`constrains` arrays so that patched values from `index.json` (or repodata patches) are not overwritten by stub values created from explicit URLs.
- `insert(index.begin(), index.end())` adds any keys that are present in the package's `info/index.json` but absent from the current record. Because `insert` does not overwrite existing keys, solver metadata continues to win unless the field was removed first.
- Finally, the function ensures `size` is non-zero, backfilling it from the tarball on disk when necessary, and dumps the JSON with 4-space indentation.

### When this path runs
- Any `mamba`/`micromamba` transaction that is actually executed (not dry-run) and requires extraction of a package into a cache.
- `--download-only` transactions (the write happens before the command returns).
- Direct library/API usage that instantiates `PackageFetcher` and calls `extract` (e.g., tests, custom tooling embedding libmamba).
- Re-extraction scenarios triggered by cache invalidation (`clear_extract_path` removes stale directories before writing), such as after `mamba clean --packages` or whenever `MultiPackageCache` decides the extracted dir is invalid.

## Path 2 – `micromamba constructor --extract-conda-pkgs`

### Where the write happens
```83:185:micromamba/src/constructor.cpp
if (extract_conda_pkgs)
{
    ...
    for (const auto& raw_url : read_lines(urls_file))
    {
        auto pkg_info = specs::PackageInfo::from_url(raw_url).value();
        fs::u8path base_path = extract(entry, ExtractOptions::from_context(config.context()));
        fs::u8path repodata_record_path = base_path / "info" / "repodata_record.json";
        ...
        if (!repodata_record.is_null())
        {
            repodata_record.insert(index.cbegin(), index.cend());
        }
        else
        {
            repodata_record = index;
            repodata_record["size"] = fs::file_size(entry);
            if (!pkg_info.md5.empty())
            {
                repodata_record["md5"] = pkg_info.md5;
            }
            if (!pkg_info.sha256.empty())
            {
                repodata_record["sha256"] = pkg_info.sha256;
            }
        }
        repodata_record["fn"] = pkg_info.filename;
        repodata_record["url"] = pkg_info.package_url;
        repodata_record["channel"] = pkg_info.channel;
        if (repodata_record.find("size") == repodata_record.end() || repodata_record["size"] == 0)
        {
            repodata_record["size"] = fs::file_size(entry);
        }
        std::ofstream repodata_record_of{ repodata_record_path.std_path() };
        repodata_record_of << repodata_record.dump(4);
    }
}
```

### Reverse-engineered call chain & gating conditions
- The CLI wire-up uses `set_constructor_command`, so the only way to enter this code path is to run `micromamba constructor` and pass `--extract-conda-pkgs`. If the flag is omitted, the entire block (and thus the write) is skipped.
```52:67:micromamba/src/constructor.cpp
subcom->callback([
    &config]
{
    auto& extract_conda_pkgs = config.at("constructor_extract_conda_pkgs").compute().value<bool>();
    construct(config, prefix, extract_conda_pkgs, extract_tarball);
});
```
- Inside `construct`, the `extract_conda_pkgs` branch iterates over `<prefix>/pkgs/urls`, extracting every tarball listed there (these URLs typically come from a prior download-only install step). Each extraction returns the package directory path, and the function immediately synthesizes `info/repodata_record.json` in that directory.
- The block is independent of the `--extract-tarball` option; the latter only unpacks a single tarball into the prefix and never touches `repodata_record.json`.

### Data sources and mutation rules
- The code tries to load the corresponding channel repodata cache under `<prefix>/pkgs/cache/<cache_name>.json`. When present, it uses the cached entry (which already reflects repodata patches) and merges any missing keys from the package’s `info/index.json`.
- If the cache entry is missing, it falls back to `info/index.json` and explicitly backfills `size`, `md5`, and `sha256` from the on-disk tarball plus `PackageInfo` metadata.
- Regardless of the source, it overwrites `fn`, `url`, and `channel` with values derived from the parsed URL to ensure the record accurately reflects where the package was downloaded from and under which channel name it should be attributed.
- As with the libmamba path, the JSON is dumped with 4-space indentation.

### When this path runs
- Running `micromamba constructor --extract-conda-pkgs -p <prefix>` after populating `<prefix>/pkgs/urls` (usually by running `micromamba install --download-only` or by providing your own list of URLs) triggers the write once per package extracted into `<prefix>/pkgs`.
- The write does **not** occur when only `--extract-tarball` is used; that flag exercises a different branch that never touches `repodata_record.json`.

## Exhaustiveness and non-scenarios
- There are no other `std::ofstream` writes to `info/repodata_record.json` in the repository. Other components (e.g., `PackageCache`, `PrefixData`, `link.cpp`) only read or validate the file. Therefore, any `repodata_record.json` found in an extracted package must have been created either by `PackageFetcher::write_repodata_record` during a package extraction or by the `micromamba constructor --extract-conda-pkgs` workflow described above.
- Consequences: diagnosing a malformed `repodata_record.json` can be reduced to determining which of the two entry points ran (regular install/update/create vs. constructor extraction) and which inputs they consumed (`PackageInfo` metadata, `info/index.json`, and optionally cached channel repodata).
