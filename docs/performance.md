# Performance & Benchmarks

## Reproducible Benchmarks

Run the benchmark script in `mongory-py`:

```bash
python3 scripts/bench.py
```

To include the peer Python library comparison (`mongoquery`), install `mongoquery` in an isolated path (keeping `mongory` zero runtime dependencies):

```bash
python3 -m pip install --target /tmp/py-bench-deps mongoquery
PYTHONPATH=/tmp/py-bench-deps python3 scripts/bench.py
```

Each case compares:
- **`plain_fast`**: Hand-written Python assuming a static schema (no implicit array matching).
- **`plain_safe`**: Semantics-equivalent defensive pure Python performing missing-key checks, strict `bool != int` checks, and Mongo-style implicit array matching.
- **`mongory(C)`**: Compiled `mongory.Matcher` backed by `mongory-core` (C99).
- **`mongoquery`** (`1.4.3`): Peer pure-Python MongoDB in-memory query matcher on PyPI.

### Monomorphic Dataset (Apple M3, macOS arm64, Python `3.14.5`, 10,000 records x 20 rounds)

| Case | `plain_fast` | `plain_safe` | `mongory(C)` | `mongoquery` | `mongory` vs `plain_safe` | `mongory` vs `mongoquery` |
|---|---:|---:|---:|---:|---:|---:|
| `simple` | `139.9 ns` | `242.5 ns` | `86.8 ns` | `1,543.1 ns` | **2.79x faster** | **17.78x faster** |
| `or` | `157.2 ns` | `303.1 ns` | `123.3 ns` | `2,722.4 ns` | **2.46x faster** | **22.07x faster** |
| `nested_and` | `136.0 ns` | `353.9 ns` | `135.4 ns` | `2,265.4 ns` | **2.61x faster** | **16.73x faster** |
| `in_array_field` | `238.5 ns` | `303.8 ns` | `140.8 ns` | `2,213.3 ns` | **2.16x faster** | **15.72x faster** |
| `large_in_500` | `80.3 ns` | `215.4 ns` | `111.6 ns` | `258,170.1 ns` | **1.93x faster** | **2,314x faster** |
| `regex` | `150.3 ns` | `307.1 ns` | `244.8 ns` | `2,241.6 ns` | **1.25x faster** | **9.16x faster** |
| `regex_short_circuit` | `171.1 ns` | `374.9 ns` | `168.1 ns` | `2,344.7 ns` | **2.23x faster** | **13.95x faster** |

### Polymorphic Dataset (10,000 heterogeneous records x 20 rounds: missing keys, mixed scalar/list/str fields)

| Case | `plain_fast` | `plain_safe` | `mongory(C)` | `mongoquery` | `mongory` vs `plain_safe` | `mongory` vs `mongoquery` |
|---|---:|---:|---:|---:|---:|---:|
| `simple` | `146.2 ns` | `303.1 ns` | `107.6 ns` | `1,757.0 ns` | **2.82x faster** | **16.32x faster** |
| `or` | `158.7 ns` | `397.0 ns` | `145.6 ns` | `3,504.6 ns` | **2.73x faster** | **24.06x faster** |
| `nested_and` | `132.2 ns` | `290.0 ns` | `123.5 ns` | `2,167.0 ns` | **2.35x faster** | **17.55x faster** |
| `in_array_field` | `227.1 ns` | `309.2 ns` | `142.7 ns` | `2,305.2 ns` | **2.17x faster** | **16.15x faster** |
| `large_in_500` | `84.0 ns` | `229.1 ns` | `116.5 ns` | `255,214.6 ns` | **1.97x faster** | **2,191x faster** |
| `regex` | `148.2 ns` | `319.5 ns` | `247.6 ns` | `2,281.1 ns` | **1.29x faster** | **9.21x faster** |
| `regex_short_circuit` | `170.9 ns` | `372.5 ns` | `135.9 ns` | `1,837.0 ns` | **2.74x faster** | **13.51x faster** |

### Key Takeaways

- **vs Peer PyPI Library (`mongoquery`)**: `mongory` is **9.2x-24.1x faster** on standard queries, and **2,191x-2,314x faster** on `large_in_500` thanks to `mongory-core`'s compiled O(1) hash lookup.
- **vs Hand-Written Python (`plain_fast`)**: `mongory` stays roughly **on par to ~1.7x faster** on structural/numeric filters (`regex` alone is slower because the C adapter delegates back to Python's `re` engine), while providing reusable JSON rules and `explain`/`trace` debugging.
- **vs Semantics-Equivalent Pure Python (`plain_safe`)**: `mongory` is **1.25x-2.82x faster** across every case.

> For cross-language benchmarks (`mongory-go`, `mongory-js`, `mongory-rb`), see [Mongory Central](https://mongoryhq.github.io/mongory-central/#benchmarks).
