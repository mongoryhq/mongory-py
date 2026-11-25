# mongory-py

[![PyPI](https://img.shields.io/pypi/v/mongory)](https://pypi.org/project/mongory/)
[![API Docs](https://img.shields.io/badge/API%20Docs-pdoc-blue)](https://mongoryhq.github.io/mongory-py/)
[![CI](https://github.com/mongoryhq/mongory-py/actions/workflows/ci.yml/badge.svg)](https://github.com/mongoryhq/mongory-py/actions/workflows/ci.yml)
[![mongory-spec](https://github.com/mongoryhq/mongory-py/actions/workflows/spec-conformance.yml/badge.svg)](https://github.com/mongoryhq/mongory-py/actions/workflows/spec-conformance.yml)
![Python](https://img.shields.io/badge/Python-3.10%2B-3776AB)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

**Zero-dependency, C99-backed in-memory MongoDB-style query & rule engine for Python** — powered by [`mongory-core`](https://github.com/mongoryhq/mongory-core) with built-in `explain` / `trace` debugging and cross-language parity via [`mongory-spec`](https://github.com/mongoryhq/mongory-spec).

**[Try the Interactive Playground & Visual Explain/Trace Debugger ↗](https://mongoryhq.github.io/mongory-central/)**

[![Mongory Playground & Visual Trace](https://mongoryhq.github.io/mongory-central/playground-trace.png)](https://mongoryhq.github.io/mongory-central/)

> **Ecosystem:** [Playground](https://mongoryhq.github.io/mongory-central/) · [`mongory-go` (Go)](https://github.com/mongoryhq/mongory-go) · [`mongory-js` (TypeScript/JS)](https://github.com/mongoryhq/mongory-js) · [`mongory-rb` (Ruby)](https://github.com/mongoryhq/mongory-rb) · [`mongory-core` (C99)](https://github.com/mongoryhq/mongory-core) · [`mongory-spec`](https://github.com/mongoryhq/mongory-spec)

## Installation

```bash
pip install mongory
```

Requires Python `3.10+`. Ships precompiled wheels for Linux (`x86_64`/`aarch64`, glibc & musl), macOS (Intel & Apple Silicon), and Windows AMD64 with zero runtime Python dependencies and full PEP 561 `.pyi` type stubs.

## Quick Start

```python
import re
import mongory
from mongory import op

users = [
    {"name": "Ann", "age": 30, "tags": ["py", "go"], "active": True},
    {"name": "Bob", "age": 15, "tags": ["ruby"], "active": True},
    {"name": "Cid", "age": 42, "tags": ["py", "c"], "active": False},
]

# 1. One-shot matching
is_adult = mongory.match({"age": {op.GTE: 18}}, users[0])  # True

# 2. Compile once, reuse across collections / streams
matcher = mongory.compile(
    {
        "active": True,
        op.OR: [
            {"tags": "py"},
            {"name": re.compile(r"^b", re.IGNORECASE)},
        ],
    }
)

matched = list(matcher.filter(users))        # [Ann, Bob]
top1 = list(matcher.filter(users, limit=1))  # [Ann]
first = matcher.find_one(users)              # Ann
total = matcher.count(users)                 # 2

# Matcher also implements __call__:
via_builtin = list(filter(matcher, users))   # [Ann, Bob]
```

## Why Mongory? (and When Not to Use It)

- **Use Mongory when**: filter/routing rules come from JSON/DB/config/API, need identical semantics across Python + TS/Go/Ruby services, or need `explain()` / `trace()` to show why a record failed.
- **Don't use Mongory when**: your filter is a simple static list comprehension hardcoded in Python, or when you need a full aggregation pipeline (`$group`, `$lookup`, `$project`).
- **Python ergonomics built in**: immune to Python's `bool <: int` trap (`True != 1`, `False != 0`), supports `re.compile` patterns, optional `mongory.op` constants, and `dict` / `dataclass` / custom object inputs via shallow `PyObject*` adapters.

## Benchmark Snapshot

Measured on Apple M3 (macOS arm64, Python `3.14.5`, `10,000` records x 20 rounds; full monomorphic & polymorphic tables in [docs/performance.md](docs/performance.md)):

| Case (Monomorphic) | `plain_fast` | `plain_safe` | `mongory` (C99) | `mongoquery` (`1.4.3`) | `mongory` vs `mongoquery` |
|---|---:|---:|---:|---:|---:|
| `simple` (`age >= 18`) | `139.9 ns` | `242.5 ns` | `86.8 ns` | `1,543.1 ns` | **17.8x faster** |
| `or` (`age >= 18` or `status == "active"`) | `157.2 ns` | `303.1 ns` | `123.3 ns` | `2,722.4 ns` | **22.1x faster** |
| `in_array_field` (`tags $in ["go", "rust"]`) | `238.5 ns` | `303.8 ns` | `140.8 ns` | `2,213.3 ns` | **15.7x faster** |
| `large_in_500` (`name $in [500 items]`) | `80.3 ns` | `215.4 ns` | `111.6 ns` | `258,170.1 ns` | **2,314x faster** |

## Debugging with `explain` & `trace`

Sibling conditions are automatically cost-sorted so cheap `$eq`/`$exists`/`$in` checks short-circuit before `$regex` or array scans:

```python
matcher = mongory.compile({
    "name": {op.REGEX: "^A"},
    "age": {op.GTE: 18},
    "active": True,
})

print(matcher.explain())

matched, trace_str = matcher.trace({"name": "Bob", "age": 15, "active": False})
print(matched)    # False
print(trace_str)

# Or retrieve structured dicts for logging / UIs:
plan_tree = matcher.explain_tree()
matched, trace_tree = matcher.trace_tree({"name": "Bob", "age": 15, "active": False})
```

## Compile-Scoped Custom Operators & Converters

```python
import mongory
from mongory import op


class Money:
    def __init__(self, currency: str, amount: int) -> None:
        self.currency = currency
        self.amount = amount


def compile_money_gte(target: Money):
    if not isinstance(target, Money):
        raise ValueError("$moneyGte requires Money")
    return lambda val, exists: (
        exists
        and isinstance(val, Money)
        and val.currency == target.currency
        and val.amount >= target.amount
    )


matcher = mongory.compile(
    {"price": {"$moneyGte": Money("USD", 100)}, "tags": {op.IN: ["vip"]}},
    converters={set: sorted},
    operators={"$moneyGte": compile_money_gte},
)

print(matcher.match({"price": Money("USD", 150), "tags": {"vip", "new"}}))  # True
```

## Development, Conformance & Benchmarks

```bash
python3 scripts/build_ext.py
MONGORY_SPEC_DIR=../mongory-spec python3 -m unittest discover -s tests -v
python3 scripts/bench.py
```

See [docs/performance.md](docs/performance.md) for full benchmark tables and reproduction instructions (including `mongoquery`).

## Security & Untrusted Input

- **Engine protections**: Pure in-memory C99 evaluation (no SQL, no `$where`/`eval`), arena memory pool limits, and configurable `max_depth` (default `64`) / `max_nodes` (default `4096`) compile budgets.
- **Application responsibility**: When accepting query conditions from untrusted callers, validate scalar types (to prevent operator injection such as `{"$ne": None}`), allowlist queryable fields, and restrict untrusted `$regex` patterns (Python `re` uses backtracking).

## License

MIT
