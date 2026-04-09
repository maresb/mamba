#!/usr/bin/env python3
"""
Reproduce and report on GitHub issue #4095.

Tests that explicit URL installs produce correct repodata_record.json
by comparing against the package's own index.json (ground truth).

Usage:
    python reproduce_4095.py [path/to/mamba]

If no path is given, uses "mamba" from PATH.

Tested on: 2.1.0, 2.1.1, 2.3.3, 2.5.0, 2.6.0.rc0, and #4110 branch.
See: https://github.com/mamba-org/mamba/issues/4095
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path


PACKAGE_URL = "https://conda.anaconda.org/conda-forge/noarch/pyyaml-6.0.3-pyh7db6752_0.conda"
PACKAGE_FILENAME = "pyyaml-6.0.3-pyh7db6752_0.conda"

# Fields that should come from index.json for explicit installs
# (the whole point of issue #4095).
EXPECTED_FROM_INDEX = {
    "license": "MIT",
    "timestamp": 1758891992558,
    "track_features": "pyyaml_no_compile",
    "subdir": "noarch",
    "noarch": "python",
    "build_number": 0,
    "depends": ["python >=3.10.*", "yaml"],
}

# Fields that should always be present (even if not in index.json)
EXPECTED_PRESENT = ["constrains", "md5", "sha256"]


def find_repodata_record(search_root: Path) -> Path | None:
    for p in search_root.rglob("repodata_record.json"):
        if "pyyaml" in str(p):
            return p
    return None


def run(mamba: str) -> int:
    with tempfile.TemporaryDirectory(prefix="repro_4095_") as tmpdir:
        tmp = Path(tmpdir)
        env_dir = tmp / "env"
        pkg_dir = tmp / "pkgs"
        cache_dir = tmp / "cache"
        pkg_dir.mkdir()
        cache_dir.mkdir()
        pkg_path = pkg_dir / PACKAGE_FILENAME

        print(f"mamba binary:  {mamba}")
        version = subprocess.check_output([mamba, "--version"], text=True).strip()
        print(f"mamba version: {version}")
        print()

        print(f"Downloading {PACKAGE_FILENAME}...")
        urllib.request.urlretrieve(PACKAGE_URL, pkg_path)

        # Create empty environment, using isolated package cache
        env = {
            **dict(__import__("os").environ),
            "CONDA_PKGS_DIRS": str(cache_dir),
        }
        subprocess.check_output(
            [mamba, "create", "-y", "-p", str(env_dir), "--no-rc", "--override-channels"],
            stderr=subprocess.STDOUT,
            env=env,
        )

        # Explicit install from file:// URL (the buggy code path)
        spec_file = tmp / "explicit.txt"
        spec_file.write_text(f"@EXPLICIT\nfile://{pkg_path}\n")

        print("Installing via explicit file:// URL...")
        subprocess.check_output(
            [
                mamba,
                "install",
                "-y",
                "-p",
                str(env_dir),
                "--no-rc",
                "--override-channels",
                "--file",
                str(spec_file),
            ],
            stderr=subprocess.STDOUT,
            env=env,
        )
        print()

        rr_path = find_repodata_record(cache_dir)
        if rr_path is None:
            print("ERROR: Could not find repodata_record.json")
            print(f"Searched under: {cache_dir}")
            return 1

        print(f"Found: {rr_path}")
        rr = json.loads(rr_path.read_text())

        index_path = rr_path.parent / "index.json"
        if index_path.exists():
            print(f"Index: {index_path}")
        else:
            print("WARNING: index.json not found alongside repodata_record")

        print()
        print("=" * 60)
        print("RESULTS")
        print("=" * 60)
        print()

        failures = 0

        for field, expected in EXPECTED_FROM_INDEX.items():
            actual = rr.get(field)
            if actual == expected:
                print(f"  PASS  {field}: {json.dumps(actual)}")
            else:
                print(f"  FAIL  {field}: {json.dumps(actual)}  (expected {json.dumps(expected)})")
                failures += 1

        for field in EXPECTED_PRESENT:
            if field in rr and rr[field] not in (None, ""):
                print(f"  PASS  {field}: present")
            else:
                print(
                    f"  FAIL  {field}: "
                    f"{json.dumps(rr.get(field, '<absent>'))}"
                    f"  (expected non-empty value)"
                )
                failures += 1

        print()
        if failures == 0:
            print(f"All checks passed. Issue #4095 is fixed in {version}.")
        else:
            print(f"{failures} check(s) FAILED. Issue #4095 is present in {version}.")

        print()
        print("Full repodata_record.json:")
        print(json.dumps(rr, indent=2))

        return 1 if failures else 0


def main() -> int:
    if len(sys.argv) > 1:
        mamba = sys.argv[1]
    else:
        mamba = shutil.which("mamba") or shutil.which("micromamba")
        if mamba is None:
            print("ERROR: mamba/micromamba not found in PATH.")
            print("Usage: python reproduce_4095.py [path/to/mamba]")
            return 1

    return run(mamba)


if __name__ == "__main__":
    raise SystemExit(main())
