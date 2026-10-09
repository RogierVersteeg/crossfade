"""Pin the pioarduino core inside the platform's own Python env before a hybrid build.

Envs with custom_sdkconfig (default, sticky) run a nested `pio run` from
~/.platformio/penv. pioarduino's platform 55.03.311 bootstraps that penv with an
open-ended "pioarduino>=6.1.19", which now resolves to 6.2.0. That core wants
tool-scons 4.11.1 while the platform pins 4.8.1; the two keep reinstalling over
each other and the running SCons loses its package mid-build
("No module named 'SCons.Tool.FortranCommon'").
See https://github.com/pioarduino/platform-espressif32/issues/529

Upstream CrossPoint applies this pin as a GitHub Actions step. CrossFade does it
here, as a pre: extra_script, so it also works without touching the workflow
files. Only active on GitHub Actions (GITHUB_ACTIONS set) or when
CROSSPOINT_PIN_PENV_CORE=1, and only for envs that actually run the nested build.
"""

import os
import subprocess
import sys
from pathlib import Path

Import("env")  # noqa: F821 -- provided by PlatformIO

PINNED_CORE = "6.1.19"


def _current_version(python_exe):
    try:
        out = subprocess.run(
            [str(python_exe), "-c", "import platformio; print(platformio.__version__)"],
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    return out.stdout.strip() if out.returncode == 0 else None


def _install(python_exe, penv_dir):
    spec = f"pioarduino=={PINNED_CORE}"
    is_windows = sys.platform.startswith("win")
    bin_dir = penv_dir / ("Scripts" if is_windows else "bin")
    candidates = [
        [str(bin_dir / ("uv.exe" if is_windows else "uv")), "pip", "install", "--quiet", f"--python={python_exe}", spec],
        ["uv", "pip", "install", "--quiet", f"--python={python_exe}", spec],
        [str(python_exe), "-m", "pip", "install", "--quiet", spec],
    ]
    for cmd in candidates:
        if not Path(cmd[0]).is_absolute() or Path(cmd[0]).exists():
            try:
                subprocess.run(cmd, check=True, timeout=600)
                return True
            except (OSError, subprocess.SubprocessError) as exc:
                print(f"pin_penv_core: {cmd[0]} failed ({exc}), trying next installer")
    return False


def main():
    if not (os.getenv("GITHUB_ACTIONS") or os.getenv("CROSSPOINT_PIN_PENV_CORE")):
        return
    if not env.GetProjectOption("custom_sdkconfig", "").strip():
        return  # no nested pio run for this env
    core_dir = Path(env.subst("$PROJECT_CORE_DIR"))
    penv_dir = core_dir / "penv"
    is_windows = sys.platform.startswith("win")
    python_exe = penv_dir / ("Scripts/python.exe" if is_windows else "bin/python")
    if not python_exe.exists():
        print(f"pin_penv_core: no platform penv at {penv_dir}, nothing to pin")
        return
    current = _current_version(python_exe)
    if current == PINNED_CORE:
        print(f"pin_penv_core: penv core already {PINNED_CORE}")
        return
    print(f"pin_penv_core: penv core is {current or 'unknown'}, pinning to {PINNED_CORE}")
    if not _install(python_exe, penv_dir):
        raise RuntimeError("pin_penv_core: could not pin the pioarduino core inside the platform penv")
    print(f"pin_penv_core: penv core now {_current_version(python_exe)}")


main()
