import hashlib
import http.server
import io
import json
import os
import subprocess
import tarfile
import threading
from functools import partial
from pathlib import Path

import pytest

from . import helpers


@pytest.fixture
def legacy_cache(tmp_path, monkeypatch, tmp_clean_env):
    channel = tmp_path / "channel"
    cache = tmp_path / "pkgs"
    cache.mkdir()
    monkeypatch.setenv("CONDA_PKGS_DIRS", str(cache))
    monkeypatch.setenv("MAMBA_ROOT_PREFIX", str(tmp_path / "root"))
    monkeypatch.setenv("MAMBA_ADD_PIP_AS_PYTHON_DEPENDENCY", "false")
    records = {}
    for name, depends in (("root-main", ["shared >=1"]), ("shared", [])):
        record = {
            "name": name,
            "version": "1.0",
            "build": "0",
            "build_number": 0,
            "subdir": "noarch",
            "noarch": "generic",
            "depends": depends,
            "constrains": [],
            "license": "MIT",
            "timestamp": 1577854800,
        }
        filename = f"{name}-1.0-0.tar.bz2"
        subdir = channel / "noarch"
        subdir.mkdir(parents=True, exist_ok=True)
        files = {
            "info/index.json": json.dumps(record).encode(),
            "info/files": f"share/{name}.txt\n".encode(),
            f"share/{name}.txt": b"cache metadata test\n",
        }
        with tarfile.open(subdir / filename, "w:bz2") as archive:
            for path, content in files.items():
                entry = tarfile.TarInfo(path)
                entry.size = len(content)
                archive.addfile(entry, io.BytesIO(content))
                target = cache / f"{name}-1.0-0" / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(content)
        data = (subdir / filename).read_bytes()
        record.update(
            md5=hashlib.md5(data).hexdigest(),
            sha256=hashlib.sha256(data).hexdigest(),
            size=len(data),
        )
        records[filename] = record
        cached = {
            **record,
            "fn": filename,
            "url": (subdir / filename).as_uri(),
            "channel": channel.as_uri(),
        }
        # The files and digests remain correct, as with a 2.3.2 explicit install.
        # Only metadata is damaged. The archive cannot supply channel patches.
        if name == "root-main":
            cached.update(depends=[], constrains=[], timestamp=0, license="")
        (cache / f"{name}-1.0-0" / "info" / "repodata_record.json").write_text(json.dumps(cached))
        (cache / filename).write_bytes(data)

    def publish(depends, constrains):
        records["root-main-1.0-0.tar.bz2"].update(depends=depends, constrains=constrains)
        for subdir in ("linux-64", "noarch"):
            directory = channel / subdir
            directory.mkdir(exist_ok=True)
            repodata = {
                "info": {"subdir": subdir},
                "packages": records if subdir == "noarch" else {},
                "packages.conda": {},
            }
            (directory / "repodata.json").write_text(json.dumps(repodata))
        return records["root-main-1.0-0.tar.bz2"]

    return channel, cache, publish


@pytest.mark.parametrize("offline", [False, True], ids=["online", "offline"])
@pytest.mark.parametrize("depends", [["shared >=1,<2"], []], ids=["patched-edge", "patched-empty"])
def test_reused_cache_preserves_channel_metadata(legacy_cache, tmp_path, offline, depends):
    channel, cache, publish = legacy_cache
    expected = publish(depends, ["optional <2"])
    prefix = tmp_path / "env"
    args = [
        helpers.get_umamba(),
        "create",
        "--no-rc",
        "--yes",
        "--json",
        "--platform",
        "linux-64",
        "--override-channels",
        "-c",
        channel.as_uri(),
        "-p",
        str(prefix),
        "root-main",
    ]
    if offline:
        args.append("--offline")
    proc = subprocess.run(args, text=True, capture_output=True)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    result = json.loads(proc.stdout)
    selected = {record["name"]: record for record in result["actions"]["LINK"]}
    assert set(selected) == ({"root-main", "shared"} if depends else {"root-main"})
    installed = json.loads((prefix / "conda-meta" / "root-main-1.0-0.json").read_text())
    for field in ("depends", "constrains", "license", "timestamp"):
        assert selected["root-main"][field] == expected[field]
        assert installed[field] == expected[field]
    assert (prefix / "share" / "shared.txt").exists() == bool(depends)


def test_offline_query_prefers_channel_over_extracted_record(legacy_cache):
    channel, _, publish = legacy_cache
    expected = publish(["shared >=1,<2"], ["optional <2"])
    result = helpers.umamba_repoquery(
        "search",
        "root-main",
        "--offline",
        "--json",
        "--platform",
        "linux-64",
        "--override-channels",
        "-c",
        channel.as_uri(),
    )
    records = result["result"]["pkgs"]
    assert len(records) == 1
    assert records[0]["depends"] == expected["depends"]
    assert records[0]["constrains"] == expected["constrains"]


def test_offline_install_uses_cached_http_channel_metadata(legacy_cache, tmp_path):
    channel, cache, publish = legacy_cache
    expected = publish(["shared >=1,<2"], ["optional <2"])
    handler = partial(http.server.SimpleHTTPRequestHandler, directory=str(channel))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    url = f"http://127.0.0.1:{server.server_port}"
    thread = threading.Thread(target=server.serve_forever)
    thread.start()
    try:
        for path in cache.glob("*/info/repodata_record.json"):
            record = json.loads(path.read_text())
            record.update(channel=url, url=f"{url}/noarch/{record['fn']}")
            path.write_text(json.dumps(record))
        helpers.umamba_repoquery(
            "search",
            "root-main",
            "--json",
            "--platform",
            "linux-64",
            "--override-channels",
            "-c",
            url,
        )
    finally:
        server.shutdown()
        server.server_close()
        thread.join()

    # With the server stopped, only cached channel repodata and package files remain.
    prefix = tmp_path / "http-offline"
    result = helpers.create(
        "--json",
        "--offline",
        "--platform",
        "linux-64",
        "--override-channels",
        "-c",
        url,
        "-p",
        prefix,
        "root-main",
        default_channel=False,
        no_dry_run=True,
    )
    assert {pkg["name"] for pkg in result["actions"]["LINK"]} == {"root-main", "shared"}
    installed = json.loads((prefix / "conda-meta/root-main-1.0-0.json").read_text())
    assert installed["depends"] == expected["depends"]
    assert installed["constrains"] == expected["constrains"]


@pytest.mark.parametrize("milliseconds", [False, True])
def test_healthy_flat_cache_is_reused(legacy_cache, tmp_path, milliseconds):
    channel, cache, publish = legacy_cache
    expected = publish(["shared >=1"], [])
    path = cache / "root-main-1.0-0/info/repodata_record.json"
    record = json.loads(path.read_text())
    record.update(expected)
    if milliseconds:
        record["timestamp"] *= 1000
    path.write_text(json.dumps(record))
    prefix = tmp_path / "healthy"
    result = helpers.create(
        "--json",
        "--platform",
        "linux-64",
        "--override-channels",
        "-c",
        channel.as_uri(),
        "-p",
        prefix,
        "root-main",
        default_channel=False,
        no_dry_run=True,
    )
    assert not result["actions"].get("FETCH")
    installed = json.loads((prefix / "conda-meta/root-main-1.0-0.json").read_text())
    assert Path(installed["link"]["source"]) == path.parent.parent
    assert installed["depends"] == expected["depends"]
    assert json.loads(path.read_text()) == record


def test_explicit_cached_install_preserves_nondefaulted_metadata(legacy_cache, tmp_path):
    channel, cache, publish = legacy_cache
    expected = publish([], ["optional <2"])
    path = cache / "root-main-1.0-0/info/repodata_record.json"
    record = json.loads(path.read_text())
    record.update(expected)
    path.write_text(json.dumps(record))
    explicit = tmp_path / "explicit.txt"
    explicit.write_text("@EXPLICIT\n" + record["url"] + "#" + record["md5"] + "\n")
    prefix = tmp_path / "explicit-env"
    result = helpers.create(
        "--json",
        "--platform",
        "linux-64",
        "-p",
        prefix,
        "--file",
        explicit,
        default_channel=False,
        no_dry_run=True,
    )
    assert not result["actions"].get("FETCH")
    installed = json.loads((prefix / "conda-meta/root-main-1.0-0.json").read_text())
    assert Path(installed["link"]["source"]) == path.parent.parent
    assert installed["depends"] == []
    assert installed["constrains"] == ["optional <2"]


@pytest.mark.skipif(os.name == "nt", reason="POSIX cache permissions")
def test_repair_uses_writable_cache_without_changing_readonly_source(
    legacy_cache, tmp_path, monkeypatch
):
    channel, cache, publish = legacy_cache
    expected = publish(["shared >=1,<2"], ["optional <2"])
    original = cache / "root-main-1.0-0/info/repodata_record.json"
    original_bytes = original.read_bytes()
    writable = tmp_path / "writable-pkgs"
    writable.mkdir()
    monkeypatch.setenv("CONDA_PKGS_DIRS", f"{cache},{writable}")
    paths = [cache, *cache.rglob("*")]
    for path in paths:
        path.chmod(0o555 if path.is_dir() else 0o444)
    try:
        prefix = tmp_path / "readonly-source-env"
        helpers.create(
            "--json",
            "--platform",
            "linux-64",
            "--override-channels",
            "-c",
            channel.as_uri(),
            "-p",
            prefix,
            "root-main",
            default_channel=False,
            no_dry_run=True,
        )
        installed = json.loads((prefix / "conda-meta/root-main-1.0-0.json").read_text())
        assert installed["depends"] == expected["depends"]
        assert installed["constrains"] == expected["constrains"]
        assert original.read_bytes() == original_bytes
        repaired = list(writable.rglob("root-main-1.0-0/info/repodata_record.json"))
        assert len(repaired) == 1
        assert json.loads(repaired[0].read_text())["depends"] == expected["depends"]
    finally:
        for path in paths:
            path.chmod(0o755 if path.is_dir() else 0o644)


def test_channel_can_validate_legitimate_empty_license_and_timestamp(legacy_cache, tmp_path):
    channel, _, publish = legacy_cache
    expected = publish([], [])
    expected.update(timestamp=0, license="")
    publish([], [])
    prefix = tmp_path / "legitimate-empty-fields"
    result = helpers.create(
        "--json",
        "--offline",
        "--platform",
        "linux-64",
        "--override-channels",
        "-c",
        channel.as_uri(),
        "-p",
        prefix,
        "root-main",
        default_channel=False,
        no_dry_run=True,
    )
    assert not result["actions"].get("FETCH")
    installed = json.loads((prefix / "conda-meta/root-main-1.0-0.json").read_text())
    assert installed["depends"] == []
    assert installed["timestamp"] == 0
    assert installed["license"] == ""


def test_healthy_cache_only_offline_fallback_remains_available(legacy_cache, tmp_path):
    _, cache, publish = legacy_cache
    record = publish([], [])
    path = cache / "root-main-1.0-0/info/repodata_record.json"
    cached = json.loads(path.read_text())
    cached.update(record)
    path.write_text(json.dumps(cached))
    empty_channel = tmp_path / "empty-channel"
    for subdir in ("linux-64", "noarch"):
        directory = empty_channel / subdir
        directory.mkdir(parents=True)
        (directory / "repodata.json").write_text(json.dumps({"packages": {}, "packages.conda": {}}))
    result = helpers.create(
        "--json",
        "--offline",
        "--platform",
        "linux-64",
        "--override-channels",
        "-c",
        empty_channel.as_uri(),
        "-p",
        tmp_path / "healthy-offline",
        "root-main",
        default_channel=False,
        no_dry_run=True,
    )
    assert {pkg["name"] for pkg in result["actions"]["LINK"]} == {"root-main"}
    assert not result["actions"].get("FETCH")


@pytest.fixture(params=["2.3.2", "2.3.3", "futures-compat"])
def recovery_cache(request, tmp_path, monkeypatch, tmp_clean_env):
    """No channel record exists: recovery must come from the retained package."""
    cache = tmp_path / "pkgs"
    cache.mkdir()
    monkeypatch.setenv("CONDA_PKGS_DIRS", str(cache))
    monkeypatch.setenv("MAMBA_ROOT_PREFIX", str(tmp_path / "root"))
    monkeypatch.setenv("MAMBA_ADD_PIP_AS_PYTHON_DEPENDENCY", "false")
    channel = tmp_path / "empty-channel"
    for subdir in ("linux-64", "noarch"):
        (channel / subdir).mkdir(parents=True)
        (channel / subdir / "repodata.json").write_text('{"packages": {}}')

    records = {}

    def add(name, version="1.0", depends=(), build="0", build_number=0, artifact=None):
        filename = f"{name}-{version}-{build}.tar.bz2"
        archive = cache / filename
        if artifact is not None:
            archive.write_bytes(artifact.read_bytes())
        else:
            index = dict(
                name=name,
                version=version,
                build=build,
                build_number=build_number,
                subdir="noarch",
                depends=list(depends),
                constrains=[],
                license="MIT",
                timestamp=1750000000000,
            )
            payload = {
                "info/index.json": json.dumps(index).encode(),
                "info/files": f"share/{name}.txt\n".encode(),
                f"share/{name}.txt": name.encode(),
            }
            with tarfile.open(archive, "w:bz2") as output:
                for path, data in payload.items():
                    entry = tarfile.TarInfo(path)
                    entry.size = len(data)
                    output.addfile(entry, io.BytesIO(data))
        extracted = cache / filename.removesuffix(".tar.bz2")
        with tarfile.open(archive) as source:
            # Only known, tiny test artifacts with regular files/directories.
            for entry in source.getmembers():
                target = extracted / entry.name
                if entry.isdir():
                    target.mkdir(parents=True, exist_ok=True)
                else:
                    assert entry.isfile()
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(source.extractfile(entry).read())
        index = json.loads((extracted / "info/index.json").read_text())
        data = archive.read_bytes()
        record = dict(
            index,
            fn=filename,
            url=(channel / "noarch" / filename).as_uri(),
            channel=channel.as_uri(),
            md5=hashlib.md5(data).hexdigest(),
            sha256=hashlib.sha256(data).hexdigest(),
            size=len(data),
        )
        records[name] = record
        (extracted / "info/repodata_record.json").write_text(json.dumps(record))
        return extracted

    if request.param == "futures-compat":
        # Published 2016 artifact, MD5 e3d52c397a27973bc694e31f41768154:
        # https://repo.anaconda.com/pkgs/free/noarch/futures-compat-1.0-py3_0.tar.bz2
        artifact = Path(__file__).parent / "data/futures-compat-1.0-py3_0.tar.bz2"
        root = add("futures-compat", build="py3_0", artifact=artifact)
        add("python", version="3.14.0")
        name = "futures-compat"
    else:
        name = "root-main"
        root = add(name, depends=["shared >=1"], build="7", build_number=7)
        add("shared")
        corrupted = dict(records[name], timestamp=0, license="", build_number=0)
        if request.param == "2.3.2":
            corrupted.update(depends=[], constrains=[])
        (root / "info/repodata_record.json").write_text(json.dumps(corrupted))
    explicit = tmp_path / "explicit.txt"
    explicit.write_text(
        "@EXPLICIT\n" + "".join(f"{r['url']}#{r['md5']}\n" for r in records.values())
    )
    return dict(
        cache=cache,
        channel=channel,
        root=root,
        name=name,
        records=records,
        explicit=explicit,
        case=request.param,
    )


def run_recovery(case, prefix, route):
    args = [
        helpers.get_umamba(),
        "create",
        "--no-rc",
        "--offline",
        "--yes",
        "--json",
        "--platform",
        "linux-64",
        "-p",
        str(prefix),
    ]
    if route == "explicit":
        args += ["--file", str(case["explicit"])]
    else:
        args += ["--override-channels", "-c", case["channel"].as_uri(), case["name"]]
    return subprocess.run(args, text=True, capture_output=True)


@pytest.mark.parametrize("route", ["solve", "explicit"])
@pytest.mark.parametrize("source", ["index-only", "archive-only"])
def test_recover_legacy_metadata(recovery_cache, tmp_path, route, source):
    case = recovery_cache
    original = case["root"] / "info/repodata_record.json"
    original_bytes = original.read_bytes()
    if source == "index-only":
        for archive in case["cache"].glob("*.tar.bz2"):
            archive.unlink()
    else:
        (case["root"] / "info/index.json").write_text("not JSON")
    prefix = tmp_path / "installed"
    proc = run_recovery(case, prefix, route)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    installed = [json.loads(p.read_text()) for p in (prefix / "conda-meta").glob("*.json")]
    assert {r["name"] for r in installed} == set(case["records"])
    root = next(r for r in installed if r["name"] == case["name"])
    expected = case["records"][case["name"]]
    for key, empty in [
        ("depends", []),
        ("constrains", []),
        ("build_number", 0),
        ("timestamp", 0),
        ("license", ""),
    ]:
        assert root.get(key, empty) == expected.get(key, empty)
    assert original.read_bytes() == original_bytes
    stored = json.loads((Path(root["link"]["source"]) / "info/repodata_record.json").read_text())
    assert stored["depends"] == expected["depends"]
    if case["case"] != "futures-compat":
        assert "Recovered cached metadata" in proc.stdout + proc.stderr
        assert Path(root["link"]["source"]) != case["root"]
    elif source == "index-only":
        assert "Recovered cached metadata" not in proc.stdout + proc.stderr
        assert Path(root["link"]["source"]) == case["root"]
    # Reuse must not repeatedly replace a corrected copy or change requirements.
    stored_path = Path(root["link"]["source"]) / "info/repodata_record.json"
    last_write = stored_path.stat().st_mtime_ns
    again = run_recovery(case, tmp_path / "installed-again", route)
    assert again.returncode == 0, again.stdout + again.stderr
    assert stored_path.stat().st_mtime_ns == last_write
    assert "Recovered cached metadata" not in again.stdout + again.stderr


@pytest.mark.parametrize("route", ["solve", "explicit"])
def test_recovery_requires_usable_local_source(recovery_cache, tmp_path, route):
    case = recovery_cache
    (case["root"] / "info/index.json").write_text("not JSON")
    (case["cache"] / case["records"][case["name"]]["fn"]).unlink()
    original = (case["root"] / "info/repodata_record.json").read_bytes()
    proc = run_recovery(case, tmp_path / "missing-source", route)
    assert proc.returncode != 0
    assert "extracted index.json or retained archive" in proc.stdout + proc.stderr
    assert (case["root"] / "info/repodata_record.json").read_bytes() == original


@pytest.mark.skipif(os.name == "nt", reason="POSIX cache permissions")
@pytest.mark.parametrize("route", ["solve", "explicit"])
def test_recovery_requires_writable_cache(recovery_cache, tmp_path, monkeypatch, route):
    case = recovery_cache
    (case["cache"] / "urls.txt").touch()
    paths = [case["cache"], *case["cache"].rglob("*")]
    for path in paths:
        path.chmod(0o555 if path.is_dir() else 0o444)
    try:
        if case["case"] == "futures-compat":
            # A harmless trigger match needs no writes when its index agrees.
            proc = run_recovery(case, tmp_path / "harmless-readonly", route)
            assert proc.returncode == 0, proc.stdout + proc.stderr
            assert "Recovered cached metadata" not in proc.stdout + proc.stderr
            # Force recovery from the archive to exercise the writable-cache check.
            info = case["root"] / "info"
            info.chmod(0o755)
            (info / "index.json").unlink()
            info.chmod(0o555)
        proc = run_recovery(case, tmp_path / "no-writable-cache", route)
        assert proc.returncode != 0
        assert "no package cache is writable" in proc.stdout + proc.stderr
        assert "CONDA_PKGS_DIRS" in proc.stdout + proc.stderr
        writable = tmp_path / "writable"
        monkeypatch.setenv("CONDA_PKGS_DIRS", f"{case['cache']},{writable}")
        proc = run_recovery(case, tmp_path / "writable-cache", route)
        assert proc.returncode == 0, proc.stdout + proc.stderr
        assert list(writable.rglob("repodata_record.json"))
    finally:
        for path in paths:
            if path.exists():
                path.chmod(0o755 if path.is_dir() else 0o644)
