#!/usr/bin/env python3
"""Benchmark suite for mongory-py (C99 mongory-core extension vs pure Python)."""

from __future__ import annotations

import platform
import re
import sys
import time
from pathlib import Path
from typing import Any, Callable

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "src"))

import mongory  # noqa: E402

N = 10_000
ROUNDS = 20

MONO_RECORDS: list[dict[str, Any]] = [
    {
        "id": i,
        "name": f"user{'abcdef'[i % 6]}" if i % 5 == 0 else f"other_{i}",
        "age": 10 + (i % 50),
        "status": "active" if i % 3 == 0 else "inactive",
        "tags": ["go", "ts"] if i % 4 == 0 else ["ruby", "c"],
        "profile": {"city": "taipei" if i % 2 == 0 else "tokyo"},
    }
    for i in range(N)
]

POLY_RECORDS: list[dict[str, Any]] = []
for i in range(N):
    variant = i % 6
    age: Any = (
        [10 + (i % 50), 25]
        if variant == 4
        else str(10 + (i % 50))
        if variant == 5
        else 10 + (i % 50)
    )
    status = "active" if i % 3 == 0 else "inactive"
    name = f"user{'abcdef'[i % 6]}" if i % 5 == 0 else f"other_{i}"
    tags: Any = "go" if variant == 3 else (["go", "ts"] if i % 4 == 0 else ["ruby", "c"])
    profile: Any = (
        None
        if variant == 2
        else {"city": "taipei" if i % 2 == 0 else "tokyo", "zip": 100 + (i % 900)}
    )
    if variant == 0:
        POLY_RECORDS.append({"id": i, "name": name, "age": age, "status": status, "tags": tags, "profile": profile})
    elif variant == 1:
        POLY_RECORDS.append({"status": status, "age": age, "name": name, "tags": tags, "profile": profile, "extraA": i})
    elif variant == 2:
        POLY_RECORDS.append({"name": name, "status": status, "tags": tags, "extraB": True})
    elif variant == 3:
        POLY_RECORDS.append({"id": i, "tags": tags, "profile": profile, "age": age, "name": name, "extraC": "x"})
    elif variant == 4:
        POLY_RECORDS.append({"profile": profile, "status": status, "age": age, "name": name, "tags": tags, "extraD": i % 2})
    else:
        POLY_RECORDS.append({"age": age, "name": name, "status": status, "profile": profile, "tags": tags, "extraE": [1, 2]})

_MISSING = object()


def _is_num(v: Any) -> bool:
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def _get_prop(obj: Any, key: str) -> Any:
    if not isinstance(obj, dict):
        return _MISSING
    return obj.get(key, _MISSING)


def _match_maybe_array(val: Any, pred: Callable[[Any], bool]) -> bool:
    if val is _MISSING:
        return False
    if pred(val):
        return True
    if isinstance(val, (list, tuple)):
        for item in val:
            if pred(item):
                return True
    return False


def _match_dot_path(obj: Any, k1: str, k2: str, pred: Callable[[Any], bool]) -> bool:
    first = _get_prop(obj, k1)
    if first is _MISSING:
        return False
    if isinstance(first, (list, tuple)):
        for elem in first:
            if _match_maybe_array(_get_prop(elem, k2), pred):
                return True
        return False
    return _match_maybe_array(_get_prop(first, k2), pred)


RE_USER = re.compile(r"^user[a-f]$")

CASES: list[tuple[str, dict[str, Any], Callable[[dict[str, Any]], bool], Callable[[dict[str, Any]], bool]]] = [
    (
        "simple",
        {"age": {"$gte": 18}},
        lambda r: _is_num(r.get("age")) and r["age"] >= 18,
        lambda r: _match_maybe_array(_get_prop(r, "age"), lambda v: _is_num(v) and v >= 18),
    ),
    (
        "or",
        {"$or": [{"age": {"$gte": 18}}, {"status": "active"}]},
        lambda r: (_is_num(r.get("age")) and r["age"] >= 18) or r.get("status") == "active",
        lambda r: (
            _match_maybe_array(_get_prop(r, "age"), lambda v: _is_num(v) and v >= 18)
            or _match_maybe_array(_get_prop(r, "status"), lambda v: v == "active")
        ),
    ),
    (
        "nested_and",
        {"profile.city": "taipei", "status": "active", "age": {"$lt": 40}},
        lambda r: (
            isinstance(r.get("profile"), dict)
            and r["profile"].get("city") == "taipei"
            and r.get("status") == "active"
            and _is_num(r.get("age"))
            and r["age"] < 40
        ),
        lambda r: (
            _match_maybe_array(_get_prop(r, "status"), lambda v: v == "active")
            and _match_maybe_array(_get_prop(r, "age"), lambda v: _is_num(v) and v < 40)
            and _match_dot_path(r, "profile", "city", lambda v: v == "taipei")
        ),
    ),
    (
        "in_array_field",
        {"tags": {"$in": ["go", "rust"]}},
        lambda r: isinstance(r.get("tags"), list) and any(t in ("go", "rust") for t in r["tags"]),
        lambda r: _match_maybe_array(_get_prop(r, "tags"), lambda v: v in ("go", "rust")),
    ),
    (
        "regex",
        {"name": {"$regex": "^user[a-f]$"}},
        lambda r: isinstance(r.get("name"), str) and RE_USER.search(r["name"]) is not None,
        lambda r: _match_maybe_array(
            _get_prop(r, "name"), lambda v: isinstance(v, str) and RE_USER.search(v) is not None
        ),
    ),
    (
        "regex_short_circuit",
        {"status": "active", "age": {"$gte": 18}, "name": {"$regex": "^user[a-f]$"}},
        lambda r: (
            isinstance(r.get("name"), str)
            and RE_USER.search(r["name"]) is not None
            and r.get("status") == "active"
            and _is_num(r.get("age"))
            and r["age"] >= 18
        ),
        lambda r: (
            _match_maybe_array(
                _get_prop(r, "name"), lambda v: isinstance(v, str) and RE_USER.search(v) is not None
            )
            and _match_maybe_array(_get_prop(r, "status"), lambda v: v == "active")
            and _match_maybe_array(_get_prop(r, "age"), lambda v: _is_num(v) and v >= 18)
        ),
    ),
]


def _measure(fn: Callable[[], int]) -> tuple[float, int]:
    for _ in range(3):
        fn()
    t0 = time.perf_counter_ns()
    matched = 0
    for _ in range(ROUNDS):
        matched = fn()
    elapsed_ns = time.perf_counter_ns() - t0
    return (elapsed_ns / (ROUNDS * N), matched)


def _count_py(records: list[dict[str, Any]], pred: Callable[[dict[str, Any]], bool]) -> int:
    c = 0
    for r in records:
        if pred(r):
            c += 1
    return c


def run_suite(label: str, records: list[dict[str, Any]]) -> None:
    print(f"\n=== {label} ({N} records x {ROUNDS} rounds) ===")
    print(f"{'Query':<20} {'plain_fast':>12} {'plain_safe':>12} {'mongory(C)':>12} {'mongory/safe':>13} {'matched (safe/mg)':>20}")
    print("-" * 95)
    for name, cond, fast_fn, safe_fn in CASES:
        matcher = mongory.compile(cond)
        fast_ns, _ = _measure(lambda: _count_py(records, fast_fn))
        safe_ns, safe_m = _measure(lambda: _count_py(records, safe_fn))
        mg_ns, mg_m = _measure(lambda: matcher.count(records))
        ratio = mg_ns / safe_ns
        print(
            f"{name:<20} {fast_ns:>9.1f} ns {safe_ns:>9.1f} ns {mg_ns:>9.1f} ns {ratio:>12.2f}x {f'{safe_m}/{mg_m}':>20}"
        )


if __name__ == "__main__":
    print(f"Runtime: Python {platform.python_version()} ({platform.system()}/{platform.machine()})")
    run_suite("Monomorphic records", MONO_RECORDS)
    run_suite("Polymorphic records", POLY_RECORDS)
