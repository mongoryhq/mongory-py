"""Cross-language mongory-spec conformance test suite for mongory-py."""

from __future__ import annotations

import glob
import json
import os
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "src"))

import mongory  # noqa: E402


def _resolve_spec_dir() -> Path:
    env_dir = os.environ.get("MONGORY_SPEC_DIR")
    if env_dir:
        return Path(env_dir).resolve()
    return (ROOT.parent / "mongory-spec").resolve()


class TestMongorySpecConformance(unittest.TestCase):
    def test_all_spec_fixtures(self) -> None:
        spec_dir = _resolve_spec_dir()
        fixtures_dir = spec_dir / "fixtures"
        self.assertTrue(
            fixtures_dir.is_dir(),
            f"mongory-spec fixtures directory not found at {fixtures_dir}",
        )

        fixture_files = sorted(glob.glob(str(fixtures_dir / "**" / "*.json"), recursive=True))
        self.assertGreater(len(fixture_files), 0, "Expected non-empty mongory-spec fixtures")

        total_suites = 0
        total_records = 0

        for filepath in fixture_files:
            rel_path = os.path.relpath(filepath, fixtures_dir)
            with open(filepath, "r", encoding="utf-8") as f:
                suites = json.load(f)

            for suite in suites:
                total_suites += 1
                desc = suite["description"]
                cond = suite["condition"]
                label = f"{rel_path} :: {desc}"

                if suite.get("error"):
                    with self.subTest(suite=label):
                        with self.assertRaises(mongory.InvalidConditionError, msg=label):
                            mongory.compile(cond)
                else:
                    matcher = mongory.compile(cond)
                    plan_tree = matcher.explain_tree()
                    self.assertIsInstance(plan_tree, dict, label)

                    for idx, rec in enumerate(suite["records"]):
                        total_records += 1
                        data = rec["data"]
                        expected = rec["expected"]
                        rec_label = f"{label} [record #{idx}]"
                        with self.subTest(record=rec_label):
                            got = matcher.match(data)
                            self.assertIs(
                                got,
                                expected,
                                f"{rec_label}: cond={cond!r} data={data!r} expected={expected} got={got}",
                            )
                            traced_ok, trace_tree = matcher.trace_tree(data)
                            self.assertIs(
                                traced_ok,
                                expected,
                                f"{rec_label}: trace_tree disagreed with match()",
                            )
                            self.assertIn(trace_tree.get("result"), ("pass", "fail"))

        self.assertGreaterEqual(total_suites, 100)
        self.assertGreaterEqual(total_records, 400)


if __name__ == "__main__":
    unittest.main()
