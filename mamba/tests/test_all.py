import json
import os
import platform
import subprocess
import uuid
from distutils.version import StrictVersion

import pytest
from utils import (
    Environment,
    add_glibc_virtual_package,
    copy_channels_osx,
    platform_shells,
    run_mamba_conda,
)


def test_install():
    add_glibc_virtual_package()
    copy_channels_osx()

    channels = [
        (".", "mamba", "tests", "channel_b"),
        (".", "mamba", "tests", "channel_a"),
    ]
    package = "a"
    run_mamba_conda(channels, package)

    package = "b"
    run_mamba_conda(channels, package)

    channels = channels[::-1]
    run_mamba_conda(channels, package)


@pytest.mark.parametrize("shell_type", platform_shells())
def test_update(shell_type):
    # check updating a package when a newer version
    with Environment(shell_type) as env:
        # first install an older version
        version = "1.25.11"
        env.mamba(f"install -q -y urllib3={version}")
        out = env.execute('python -c "import urllib3; print(urllib3.__version__)"')

        # check that the installed version is the old one
        assert out[-1] == version

        # then update package
        env.mamba("update -q -y urllib3")
        out = env.execute('python -c "import urllib3; print(urllib3.__version__)"')
        # check that the installed version is newer
        assert StrictVersion(out[-1]) > StrictVersion(version)


@pytest.mark.parametrize("shell_type", platform_shells())
def test_env_update(shell_type, tmpdir):
    # check updating a package when a newer version
    with Environment(shell_type) as env:
        # first install an older version
        version = "1.25.11"
        config_a = tmpdir / "a.yml"
        config_a.write(
            f"""
            dependencies:
             - python
             - urllib3={version}
            """
        )
        env.mamba(f"env update -q -f {config_a}")
        out = env.execute('python -c "import urllib3; print(urllib3.__version__)"')

        # check that the installed version is the old one
        assert out[-1] == version

        # then release the pin
        config_b = tmpdir / "b.yml"
        config_b.write(
            """
            dependencies:
             - urllib3
            """
        )
        env.mamba(f"env update -q -f {config_b}")
        out = env.execute('python -c "import urllib3; print(urllib3.__version__)"')
        # check that the installed version is newer
        assert StrictVersion(out[-1]) > StrictVersion(version)


@pytest.mark.parametrize("shell_type", platform_shells())
def test_track_features(shell_type):
    with Environment(shell_type) as env:
        # should install CPython since PyPy has track features
        version = "3.7.9"
        env.mamba(
            f'install -q -y "python={version}" --strict-channel-priority -c conda-forge'
        )
        out = env.execute('python -c "import sys; print(sys.version)"')

        if platform.system() == "Windows":
            assert out[-1].startswith(version)
            assert "[MSC v." in out[-1]
        elif platform.system() == "Linux":
            assert out[-2].startswith(version)
            assert out[-1].startswith("[GCC")
        else:
            assert out[-2].startswith(version)
            assert out[-1].startswith("[Clang")

        if platform.system() == "Linux":
            # now force PyPy install
            env.mamba(
                f'install -q -y "python={version}=*pypy" --strict-channel-priority -c conda-forge'
            )
            out = env.execute('python -c "import sys; print(sys.version)"')
            assert out[-2].startswith(version)
            assert out[-1].startswith("[PyPy")


@pytest.mark.parametrize("experimental", [True, False])
@pytest.mark.parametrize("use_json", [True, False])
def test_create_dry_run(experimental, use_json, tmpdir):
    env_dir = tmpdir / str(uuid.uuid1())

    mamba_cmd = "mamba create --dry-run -y"
    if experimental:
        mamba_cmd += " --mamba-experimental"
    if use_json:
        mamba_cmd += " --json"
    mamba_cmd += f" -p {env_dir} python=3.8"

    output = subprocess.check_output(mamba_cmd, shell=True).decode()

    if use_json:
        # assert that the output is parseable parseable json
        json.loads(output)
    elif not experimental:
        # Assert the non-JSON output is in the terminal.
        assert "Total download" in output

    if not experimental:
        # dry-run, shouldn't create an environment
        assert not env_dir.check()


def test_create_files(tmpdir):
    """Check that multiple --file arguments are respected."""
    (tmpdir / "1.txt").write(b"a")
    (tmpdir / "2.txt").write(b"b")
    output = subprocess.check_output(
        [
            "mamba",
            "create",
            "-p",
            str(tmpdir / "env"),
            "--json",
            "--override-channels",
            "--strict-channel-priority",
            "--dry-run",
            "-c",
            "./mamba/tests/channel_b",
            "-c",
            "./mamba/tests/channel_a",
            "--file",
            str(tmpdir / "1.txt"),
            "--file",
            str(tmpdir / "2.txt"),
        ]
    )
    output = json.loads(output)
    names = {x["name"] for x in output["actions"]["FETCH"]}
    assert names == {"a", "b"}


def test_unicode(tmpdir):
    uc = "320 áγђß家固êôōçñ한"
    output = subprocess.check_output(
        ["mamba", "create", "-p", str(tmpdir / uc), "--json", "xtensor"]
    )
    output = json.loads(output)
    assert output["prefix"] == str(tmpdir / uc)

    import libmambapy

    pd = libmambapy.PrefixData(str(tmpdir / uc))
    pd.load()
    assert len(pd.package_records) > 1
    assert "xtl" in pd.package_records
    assert "xtensor" in pd.package_records
