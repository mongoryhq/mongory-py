# mongory-py

**C99-accelerated Python in-memory Mongo-style rule engine & query matcher** with built-in **Explain & Trace** debugging — powered by [`mongory-core`](https://github.com/mongoryhq/mongory-core) and verified against [`mongory-spec`](https://github.com/mongoryhq/mongory-spec).

**[Try the Interactive Playground & Visual Explain/Trace Debugger ↗](https://mongoryhq.github.io/mongory-central/)**

[![Mongory Playground & Visual Trace](https://mongoryhq.github.io/mongory-central/playground-trace.png)](https://mongoryhq.github.io/mongory-central/)

> **Zero database required:** Compile a PyMongo-style `dict` filter once into a reusable `mongory.Matcher` backed by `mongory-core`'s C99 arena engine and evaluate Python `dict`s, `dataclass`es, or custom objects in memory.  
> **Ecosystem:** [Interactive Playground](https://mongoryhq.github.io/mongory-central/) · [`mongory-go` (Go)](https://github.com/mongoryhq/mongory-go) · [`mongory-js` (TypeScript/JS)](https://github.com/mongoryhq/mongory-js) · [`mongory-rb` (Ruby)](https://github.com/mongoryhq/mongory-rb) · [`mongory-core` (C99)](https://github.com/mongoryhq/mongory-core) · [`mongory-spec`](https://github.com/mongoryhq/mongory-spec)

## Features

- **Native C99 Engine (`mongory-core`)**: Shallow `PyObject*` wrapping with arena memory pools and zero runtime Python dependencies.
- **PyMongo & `re.compile` Ergonomics**: Pass raw `dict` conditions (including compiled `re.Pattern` objects), use `mongory.compile(cond)` or one-shot `mongory.match(cond, record)`, and pass `Matcher` directly to Python's built-in `filter(matcher, records)`.
- **Operator Constants (`mongory.op` / `mongory.operator`)**: Optional uppercase constants (`op.GTE`, `op.IN`, `op.OR`, `op.ELEM_MATCH`, etc.) for typo-free conditions with zero overhead.
- **Built-in `explain()` / `trace()` & `explain_tree()` / `trace_tree()`**: Inspect priority-ordered execution plans and per-record `PASS` / `FAIL` / `SKIP` evaluation trees.
- **Compile-Scoped Extensions & Safety Limits**: Custom `converters`, custom `operators`, `max_depth` (default `64`), `max_nodes` (default `4096`), and full PEP 561 `.pyi` type stubs.

## Installation

```bash
pip install mongory
```

Requires Python 3.10 or later.

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
    mongory.and_(
        {"active": True},
        mongory.or_(
            {"tags": "py"},
            {"name": re.compile(r"^b", re.IGNORECASE)},
        ),
    )
)

matched = list(matcher.filter(users))        # [Ann, Bob]
top1 = list(matcher.filter(users, limit=1))  # [Ann]
first = matcher.find_one(users)              # Ann
total = matcher.count(users)                 # 2

# Matcher also implements __call__:
via_builtin = list(filter(matcher, users))   # [Ann, Bob]
```

## Debugging with `explain` & `trace`

```python
matcher = mongory.compile({
    "name": {op.REGEX: "^A"},
    "age": {op.GTE: 18},
    "active": True,
})

print(matcher.explain())

matched, trace_str = matcher.trace({"name": "Bob", "age": 15, "active": False})
print(matched)  # False
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

## Development & Conformance Tests

```bash
python3 scripts/build_ext.py
MONGORY_SPEC_DIR=../mongory-spec python3 -m unittest discover -s tests -v
```

## License

MIT
