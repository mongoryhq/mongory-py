"""Randomized fuzzing and match == trace_tree invariant tests for mongory-py."""

from __future__ import annotations

import os
import random
import sys
import unittest
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
if list((ROOT / "src" / "mongory").glob("_mongory_ext*")):
    sys.path.insert(0, str(ROOT / "src"))

import mongory  # noqa: E402

KEYS = ["a", "b", "c", "tags", "0", "-1", "01", "-0", "", "a.b", "a\\.b", "tags.0", "tags.-1"]
SCALARS: list[Any] = [None, True, False, 0, 1, -1, 1.5, "", "a", "b", "0", "1"]


def _gen_val(rng: random.Random, depth: int = 0) -> Any:
    if depth >= 2 or rng.random() < 0.6:
        return rng.choice(SCALARS)
    if rng.random() < 0.5:
        return [_gen_val(rng, depth + 1) for _ in range(rng.randint(0, 3))]
    return {rng.choice(["a", "b", "0", "-1", ""]): _gen_val(rng, depth + 1) for _ in range(rng.randint(0, 3))}


def _gen_cond(rng: random.Random, depth: int = 0) -> dict[str, Any]:
    if depth >= 2:
        return {rng.choice(["a", "b", "0"]): rng.choice(SCALARS)}
    out: dict[str, Any] = {}
    for _ in range(rng.randint(0, 3)):
        k = rng.choice(KEYS)
        op = rng.choice(["$eq", "$ne", "$gt", "$gte", "$lt", "$lte", "$in", "$nin", "$exists", "$present", "$size", "literal"])
        if op == "literal":
            out[k] = _gen_val(rng, depth + 1)
        elif op in ("$in", "$nin"):
            out[k] = {op: [_gen_val(rng, depth + 1) for _ in range(rng.randint(0, 3))]}
        elif op in ("$exists", "$present"):
            out[k] = {op: rng.choice([True, False])}
        elif op == "$size":
            out[k] = {"$size": rng.choice([0, 1, 2])}
        else:
            out[k] = {op: rng.choice(SCALARS)}
    return out


class TestMongoryFuzz(unittest.TestCase):
    def test_randomized_match_and_trace_invariants(self) -> None:
        iterations = int(os.environ.get("FUZZ_ITERATIONS", "500"))
        rng = random.Random(20261121)

        for i in range(iterations):
            cond = _gen_cond(rng)
            rec = _gen_val(rng)
            matcher = mongory.compile(cond)
            matched = matcher.match(rec)
            traced_ok, trace_tree = matcher.trace_tree(rec)
            self.assertIs(
                traced_ok,
                matched,
                f"Iteration #{i}: match={matched} != trace_tree={traced_ok} for cond={cond!r} rec={rec!r}",
            )
            self.assertEqual(trace_tree["result"] == "pass", matched)


if __name__ == "__main__":
    unittest.main()
