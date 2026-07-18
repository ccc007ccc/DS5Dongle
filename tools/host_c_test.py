"""Compile and run small dependency-free C tests on Linux or Windows/WSL."""

from __future__ import annotations

import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def _wsl_path(path: Path) -> str:
    resolved = path.resolve()
    drive, tail = os.path.splitdrive(str(resolved))
    if not drive:
        return str(resolved).replace("\\", "/")
    return f"/mnt/{drive[0].lower()}{tail.replace(chr(92), '/')}"


def run_host_c_test(
    name: str,
    sources: list[Path],
    include_dirs: list[Path],
) -> int:
    common = ["-std=c11", "-Wall", "-Wextra", "-Werror"]

    if os.name == "nt":
        output = f"/tmp/{name}"
        args = [*common]
        for include_dir in include_dirs:
            args.extend(("-I", _wsl_path(include_dir)))
        args.extend(_wsl_path(source) for source in sources)
        args.extend(("-o", output))
        compile_command = " ".join(shlex.quote(arg) for arg in ("cc", *args))
        command = (
            "set -eu; "
            f"trap 'rm -f {shlex.quote(output)}' EXIT; "
            f"{compile_command}; {shlex.quote(output)}"
        )
        return subprocess.run(["wsl", "sh", "-lc", command]).returncode

    with tempfile.TemporaryDirectory(prefix=f"{name}-") as directory:
        output = Path(directory) / name
        command = [os.environ.get("CC", "cc"), *common]
        for include_dir in include_dirs:
            command.extend(("-I", str(include_dir.resolve())))
        command.extend(str(source.resolve()) for source in sources)
        command.extend(("-o", str(output)))
        compiled = subprocess.run(command)
        if compiled.returncode != 0:
            return compiled.returncode
        return subprocess.run([str(output)]).returncode
