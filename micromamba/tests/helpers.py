import errno
import json
import os
import platform
import random
import shutil
import string
import subprocess
import sys
from enum import Enum
from pathlib import Path

import pytest
import yaml


class DryRun(Enum):
    OFF = "OFF"
    DRY = "DRY"
    ULTRA_DRY = "ULTRA_DRY"


use_offline = False
channel = ["-c", "conda-forge"]
dry_run_tests = DryRun(
    os.environ["MAMBA_DRY_RUN_TESTS"]
    if ("MAMBA_DRY_RUN_TESTS" in os.environ)
    else "OFF"
)

MAMBA_NO_PREFIX_CHECK = 1 << 0
MAMBA_ALLOW_EXISTING_PREFIX = 1 << 1
MAMBA_ALLOW_MISSING_PREFIX = 1 << 2
MAMBA_ALLOW_NOT_ENV_PREFIX = 1 << 3
MAMBA_EXPECT_EXISTING_PREFIX = 1 << 4

MAMBA_NOT_ALLOW_EXISTING_PREFIX = 0
MAMBA_NOT_ALLOW_MISSING_PREFIX = 0
MAMBA_NOT_ALLOW_NOT_ENV_PREFIX = 0
MAMBA_NOT_EXPECT_EXISTING_PREFIX = 0

if platform.system() == "Windows":
    xtensor_hpp = "Library/include/xtensor/xtensor.hpp"
    xsimd_hpp = "Library/include/xsimd/xsimd.hpp"
else:
    xtensor_hpp = "include/xtensor/xtensor.hpp"
    xsimd_hpp = "include/xsimd/xsimd.hpp"


def get_umamba(cwd=os.getcwd()):
    if os.getenv("TEST_MAMBA_EXE"):
        umamba = os.getenv("TEST_MAMBA_EXE")
    else:
        if platform.system() == "Windows":
            umamba_bin = "micromamba.exe"
        else:
            umamba_bin = "micromamba"
        umamba = os.path.join(cwd, "build", "micromamba", umamba_bin)
    if not Path(umamba).exists():
        print("MICROMAMBA NOT FOUND!")
    return umamba


def random_string(N=10):
    return "".join(random.choices(string.ascii_uppercase + string.digits, k=N))


def shell(*args, cwd=os.getcwd()):
    umamba = get_umamba(cwd=cwd)
    cmd = [umamba, "shell"] + [arg for arg in args if arg]

    if "--print-config-only" in args:
        cmd += ["--debug"]

    res = subprocess.check_output(cmd)
    if "--json" in args:
        try:
            j = json.loads(res)
            return j
        except json.decoder.JSONDecodeError as e:
            print(f"Error when loading JSON output from {res}")
            raise (e)
    if "--print-config-only" in args:
        return yaml.load(res, Loader=yaml.FullLoader)
    return res.decode()


def info(*args):
    umamba = get_umamba()
    cmd = [umamba, "info"] + [arg for arg in args if arg]
    res = subprocess.check_output(cmd)
    if "--json" in args:
        try:
            j = json.loads(res)
            return j
        except json.decoder.JSONDecodeError as e:
            print(f"Error when loading JSON output from {res}")
            raise (e)
    return res.decode()


def install(*args, default_channel=True, no_rc=True, no_dry_run=False):
    umamba = get_umamba()
    cmd = [umamba, "install", "-y"] + [arg for arg in args if arg]

    if "--print-config-only" in args:
        cmd += ["--debug"]
    if default_channel:
        cmd += channel
    if no_rc:
        cmd += ["--no-rc"]
    if use_offline:
        cmd += ["--offline"]
    if (dry_run_tests == DryRun.DRY) and "--dry-run" not in args and not no_dry_run:
        cmd += ["--dry-run"]
    cmd += ["--log-level=info"]

    print(f"Running command {' '.join(cmd)}", file=sys.stderr)
    res = subprocess.check_output(cmd, stderr=sys.stderr)

    if "--json" in args:
        try:
            j = json.loads(res)
            return j
        except:
            print(res.decode())
            return
    if "--print-config-only" in args:
        return yaml.load(res, Loader=yaml.FullLoader)
    return res.decode()


def create(*args, default_channel=True, no_rc=True, no_dry_run=False, always_yes=True):
    umamba = get_umamba()
    cmd = [umamba, "create"] + [arg for arg in args if arg]

    if "--print-config-only" in args:
        cmd += ["--debug"]
    if always_yes:
        cmd += ["-y"]
    if default_channel:
        cmd += channel
    if no_rc:
        cmd += ["--no-rc"]
    if use_offline:
        cmd += ["--offline"]
    if (dry_run_tests == DryRun.DRY) and "--dry-run" not in args and not no_dry_run:
        cmd += ["--dry-run"]

    try:
        res = subprocess.check_output(cmd)
        if "--json" in args:
            j = json.loads(res)
            return j
        if "--print-config-only" in args:
            return yaml.load(res, Loader=yaml.FullLoader)
        return res.decode()
    except subprocess.CalledProcessError as e:
        print(f"Error when executing '{' '.join(cmd)}'")
        raise (e)


def remove(*args, no_dry_run=False):
    umamba = get_umamba()
    cmd = [umamba, "remove", "-y"] + [arg for arg in args if arg]

    if "--print-config-only" in args:
        cmd += ["--debug"]
    if (dry_run_tests == DryRun.DRY) and "--dry-run" not in args and not no_dry_run:
        cmd += ["--dry-run"]

    try:
        res = subprocess.check_output(cmd)
        if "--json" in args:
            j = json.loads(res)
            return j
        if "--print-config-only" in args:
            return yaml.load(res, Loader=yaml.FullLoader)
        return res.decode()
    except subprocess.CalledProcessError as e:
        print(f"Error when executing '{' '.join(cmd)}'")
        raise (e)


def update(*args, default_channel=True, no_rc=True, no_dry_run=False):
    umamba = get_umamba()
    cmd = [umamba, "update", "-y"] + [arg for arg in args if arg]
    if use_offline:
        cmd += ["--offline"]
    if no_rc:
        cmd += ["--no-rc"]
    if default_channel:
        cmd += channel
    if (dry_run_tests == DryRun.DRY) and "--dry-run" not in args and not no_dry_run:
        cmd += ["--dry-run"]

    try:
        res = subprocess.check_output(cmd)
        if "--json" in args:
            try:
                j = json.loads(res)
                return j
            except json.decoder.JSONDecodeError as e:
                print(f"Error when loading JSON output from {res}")
                raise (e)
        print(f"Error when executing '{' '.join(cmd)}'")
        raise

        return res.decode()
    except subprocess.CalledProcessError as e:
        print(f"Error when executing '{' '.join(cmd)}'")
        raise (e)


def run_env(*args, f=None):
    umamba = get_umamba()
    cmd = [umamba, "env"] + [arg for arg in args if arg]

    res = subprocess.check_output(cmd)

    if "--json" in args:
        j = json.loads(res)
        return j

    return res.decode()


def umamba_list(*args):
    umamba = get_umamba()

    cmd = [umamba, "list"] + [arg for arg in args if arg]
    res = subprocess.check_output(cmd)

    if "--json" in args:
        j = json.loads(res)
        return j

    return res.decode()


def get_concrete_pkg(t, needle):
    pkgs = t["actions"]["LINK"]
    for p in pkgs:
        if p["name"] == needle:
            return f"{p['name']}-{p['version']}-{p['build_string']}"
    raise RuntimeError("Package not found in transaction")


def get_env(n, f=None):
    root_prefix = os.getenv("MAMBA_ROOT_PREFIX")
    if f:
        return Path(os.path.join(root_prefix, "envs", n, f))
    else:
        return Path(os.path.join(root_prefix, "envs", n))


def get_pkg(n, f=None, root_prefix=None):
    if not root_prefix:
        root_prefix = os.getenv("MAMBA_ROOT_PREFIX")
    if f:
        return Path(os.path.join(root_prefix, "pkgs", n, f))
    else:
        return Path(os.path.join(root_prefix, "pkgs", n))


def get_tarball(n):
    root_prefix = os.getenv("MAMBA_ROOT_PREFIX")
    return Path(os.path.join(root_prefix, "pkgs", n + ".tar.bz2"))


def get_concrete_pkg_info(env, pkg_name):
    with open(os.path.join(env, "conda-meta", pkg_name + ".json")) as fi:
        return json.load(fi)


def read_windows_registry(target_path):  # pragma: no cover
    import winreg

    # HKEY_LOCAL_MACHINE\Software\Microsoft\Command Processor\AutoRun
    # HKEY_CURRENT_USER\Software\Microsoft\Command Processor\AutoRun
    # returns value_value, value_type  -or-  None, None if target does not exist
    main_key, the_rest = target_path.split("\\", 1)
    subkey_str, value_name = the_rest.rsplit("\\", 1)
    main_key = getattr(winreg, main_key)

    try:
        key = winreg.OpenKey(main_key, subkey_str, 0, winreg.KEY_READ)
    except EnvironmentError as e:
        if e.errno != errno.ENOENT:
            raise
        return None, None

    try:
        value_tuple = winreg.QueryValueEx(key, value_name)
        value_value = value_tuple[0]
        if isinstance(value_value, str):
            value_value = value_value.strip()
        value_type = value_tuple[1]
        return value_value, value_type
    except Exception:
        # [WinError 2] The system cannot find the file specified
        winreg.CloseKey(key)
        return None, None
    finally:
        winreg.CloseKey(key)


def write_windows_registry(target_path, value_value, value_type):  # pragma: no cover
    import winreg

    main_key, the_rest = target_path.split("\\", 1)
    subkey_str, value_name = the_rest.rsplit("\\", 1)
    main_key = getattr(winreg, main_key)
    try:
        key = winreg.OpenKey(main_key, subkey_str, 0, winreg.KEY_WRITE)
    except EnvironmentError as e:
        if e.errno != errno.ENOENT:
            raise
        key = winreg.CreateKey(main_key, subkey_str)
    try:
        winreg.SetValueEx(key, value_name, 0, value_type, value_value)
    finally:
        winreg.CloseKey(key)


@pytest.fixture(scope="session")
def cache_warming():
    cache = Path(os.path.expanduser(os.path.join("~", "cache" + random_string())))
    os.makedirs(cache)

    os.environ["CONDA_PKGS_DIRS"] = str(cache)
    tmp_prefix = os.path.expanduser(os.path.join("~", "tmpprefix" + random_string()))

    res = create("-p", tmp_prefix, "xtensor", "--json", no_dry_run=True)
    pkg_name = get_concrete_pkg(res, "xtensor")

    yield cache, pkg_name

    if "CONDA_PKGS_DIRS" in os.environ:
        os.environ.pop("CONDA_PKGS_DIRS")
    rmtree(cache)
    rmtree(tmp_prefix)


@pytest.fixture(scope="session")
def existing_cache(cache_warming):
    yield cache_warming[0]


@pytest.fixture(scope="session")
def repodata_files(existing_cache):
    yield [f for f in existing_cache.iterdir() if f.is_file() and f.suffix == ".json"]


@pytest.fixture(scope="session")
def test_pkg(cache_warming):
    yield cache_warming[1]


@pytest.fixture
def first_cache_is_writable():
    return True


def link_dir(new_dir, existing_dir, prefixes=None):

    for i in existing_dir.iterdir():
        if i.is_dir():
            subdir = new_dir / i.name
            os.makedirs(subdir, exist_ok=True)
            link_dir(subdir, i)
        elif i.is_symlink():
            linkto = os.readlink(i)
            os.symlink(linkto, new_dir / i.name)
        elif i.is_file():
            os.makedirs(new_dir, exist_ok=True)
            name = i.name
            os.link(i, new_dir / name)


def recursive_chmod(path: Path, permission, is_root=True):
    p = Path(path)

    if not p.is_symlink():
        os.chmod(p, permission)

    if p.is_dir():
        for i in p.iterdir():
            recursive_chmod(i, permission, is_root=False)


def rmtree(path: Path):
    p = Path(path)
    recursive_chmod(p, 0o700)

    def handleError(func, path, exc_info):
        recursive_chmod(path, 0o700)
        func(path)

    if p.is_dir():
        shutil.rmtree(p, onerror=handleError)
    else:
        os.remove(p)


def get_fake_activate(prefix):
    prefix = Path(prefix)
    env = os.environ.copy()
    curpath = env["PATH"]
    curpath = curpath.split(os.pathsep)
    if platform.system() == "Windows":
        addpath = [
            prefix,
            prefix / "Library" / "mingw-w64" / "bin",
            prefix / "Library" / "usr" / "bin",
            prefix / "Library" / "bin",
            prefix / "Scripts",
            prefix / "bin",
        ]
    else:
        addpath = [prefix / "bin"]
    env["PATH"] = os.pathsep.join([str(x) for x in addpath + curpath])
    env["CONDA_PREFIX"] = str(prefix)
    return env
