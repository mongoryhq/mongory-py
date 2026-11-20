#!/usr/bin/env python3
"""Zero-dependency CPython extension builder using stdlib `sysconfig` + system C compiler.

Compiles `ext/mongory_ext/mongory_ext.c` and all `mongory-core` C99 sources into
`src/mongory/_mongory_ext<EXT_SUFFIX>` for the current Python interpreter.
"""

from __future__ import annotations

import glob
import os
import platform
import shlex
import subprocess
import sys
import sysconfig
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXT_DIR = ROOT / "ext" / "mongory_ext"
CORE_DIR = EXT_DIR / "mongory-core"
OUT_DIR = ROOT / "src" / "mongory"


def build() -> Path:
    include_dir = sysconfig.get_paths()["include"]
    ext_suffix = sysconfig.get_config_var("EXT_SUFFIX") or ".so"
    out_file = OUT_DIR / f"_mongory_ext{ext_suffix}"

    sources = [
        str(EXT_DIR / "mongory_ext.c"),
        *sorted(glob.glob(str(CORE_DIR / "src" / "foundations" / "*.c"))),
        *sorted(glob.glob(str(CORE_DIR / "src" / "matchers" / "*.c"))),
    ]

    cc_var = sysconfig.get_config_var("CC") or os.environ.get("CC") or "cc"
    cc_cmd = shlex.split(cc_var)

    cflags = [
        "-std=c99",
        "-O3",
        "-fPIC",
        f"-I{include_dir}",
        f"-I{CORE_DIR / 'include'}",
        f"-I{CORE_DIR / 'src' / 'foundations'}",
        f"-I{CORE_DIR / 'src' / 'matchers'}",
    ]

    system = platform.system()
    if system == "Darwin":
        ldflags = ["-bundle", "-undefined", "dynamic_lookup"]
    else:
        ldflags = ["-shared"]

    cmd = [*cc_cmd, *cflags, *sources, *ldflags, "-o", str(out_file)]
    print("Building C extension:", " ".join(shlex.quote(c) for c in cmd))
    subprocess.run(cmd, check=True)
    print(f"Built {out_file} ({Python_version()})")
    return out_file


def Python_version() -> str:
    return f"Python {sys.version.split()[0]}"


if __name__ == "__main__":
    build()
