# PlatformIO pre-build script: sets BRIDGE_VERSION from `git describe`.
import subprocess

Import("env")  # noqa: F821  (provided by PlatformIO)

try:
    version = subprocess.run(
        ["git", "describe", "--tags", "--always", "--dirty"],
        capture_output=True, text=True, check=True, cwd=env["PROJECT_DIR"],  # noqa: F821
    ).stdout.strip()
except (OSError, subprocess.CalledProcessError):
    version = "unknown"

env.Append(CPPDEFINES=[("BRIDGE_VERSION", env.StringifyMacro(version))])  # noqa: F821
