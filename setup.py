from __future__ import annotations

import glob
import sys

from setuptools import Extension, find_packages, setup

core_sources = sorted(
    glob.glob("ext/mongory_ext/mongory-core/src/foundations/*.c")
    + glob.glob("ext/mongory_ext/mongory-core/src/matchers/*.c")
)

ext_modules = [
    Extension(
        name="mongory._mongory_ext",
        sources=["ext/mongory_ext/mongory_ext.c", *core_sources],
        include_dirs=[
            "ext/mongory_ext/mongory-core/include",
            "ext/mongory_ext/mongory-core/src/foundations",
            "ext/mongory_ext/mongory-core/src/matchers",
        ],
        extra_compile_args=["/O2", "/std:c11"] if sys.platform == "win32" else ["-std=c99", "-O3"],
    )
]

setup(
    package_dir={"": "src"},
    packages=find_packages(where="src"),
    package_data={"mongory": ["py.typed", "*.pyi"]},
    ext_modules=ext_modules,
)
