# Old Conda Forge Packages can look like their repodata is corrupted

Consider the `libmo_unpack==2.0.3` package for `linux-64` on conda-forge.
It has the following repodata record from conda-forge:

```json
{
  "build": "0",
  "build_number": 0,
  "depends": [],
  "license": null,
  "md5": "c057f91c609b7ae6bc96c85f9fee5d37",
  "name": "libmo_unpack",
  "sha256": "a70fe5290550a350bb6fb9069a55ef69a1c286a459f00196ace022b680998d88",
  "size": 108643,
  "subdir": "linux-64",
  "version": "2.0.3"
}
```

Note that `license` is `null`, `timestamp` is absent, `sha256` is set, and `size` is positive.

The corresponding `index.json` record is:

```json
{
  "arch": "x86_64",
  "build": "0",
  "build_number": 0,
  "depends": [],
  "license": null,
  "name": "libmo_unpack",
  "platform": "linux",
  "subdir": "linux-64",
  "version": "2.0.3"
}
```

This is a fairly extreme edge case, but it does represent something that we'd ideally like to be able to cover, although practically speaking this may be impossible.
It's not even that important because such an ancient package is very unlikely to arise in practice.
Nevertheless, we note it here as a possibility.

Note that with the latest patches, if we run

```bash
mamba create -y -n delme-test libmo_unpack==2.0.3
```

then it will currently fail with:

```text
Linking libmo_unpack-2.0.3-0
info     libmamba Detected corrupted metadata in cache (v2.1.1-v2.4.0 bug, issue #4095), will re-extract: /home/mares/micromamba/pkgs/libmo_unpack-2.0.3-0
error    libmamba Cannot find a valid extracted directory cache for 'libmo_unpack-2.0.3-0.tar.bz2'
critical libmamba Package cache error.
```

This is all partially explained in `mixed-spec-analysis.md`.
