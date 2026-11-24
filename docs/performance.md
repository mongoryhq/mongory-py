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
| `simple` | `137.0 ns` | `245.2 ns` | `88.2 ns` | `1,579.8 ns` | **2.78x faster** | **17.92x faster** |
| `or` | `157.0 ns` | `293.2 ns` | `120.1 ns` | `2,642.7 ns` | **2.44x faster** | **22.00x faster** |
| `nested_and` | `135.7 ns` | `358.5 ns` | `127.3 ns` | `2,204.1 ns` | **2.82x faster** | **17.31x faster** |
| `in_array_field` | `227.9 ns` | `295.9 ns` | `129.8 ns` | `2,162.5 ns` | **2.28x faster** | **16.66x faster** |
| `regex` | `141.6 ns` | `299.4 ns` | `218.1 ns` | `2,027.1 ns` | **1.37x faster** | **9.30x faster** |
| `regex_short_circuit` | `158.7 ns` | `345.9 ns` | `153.7 ns` | `2,110.5 ns` | **2.25x faster** | **13.73x faster** |

### Polymorphic Dataset (10,000 heterogeneous records x 20 rounds: missing keys, mixed scalar/list/str fields)

| Case | `plain_fast` | `plain_safe` | `mongory(C)` | `mongoquery` | `mongory` vs `plain_safe` | `mongory` vs `mongoquery` |
|---|---:|---:|---:|---:|---:|---:|
| `simple` | `143.2 ns` | `285.7 ns` | `100.7 ns` | `1,637.9 ns` | **2.84x faster** | **16.26x faster** |
| `or` | `149.3 ns` | `361.9 ns` | `132.4 ns` | `3,421.0 ns` | **2.73x faster** | **25.84x faster** |
| `nested_and` | `127.4 ns` | `270.7 ns` | `117.2 ns` | `2,070.2 ns` | **2.31x faster** | **17.66x faster** |
| `in_array_field` | `219.0 ns` | `298.4 ns` | `134.2 ns` | `2,175.3 ns` | **2.22x faster** | **16.21x faster** |
| `regex` | `145.9 ns` | `290.9 ns` | `226.3 ns` | `2,074.5 ns` | **1.29x faster** | **9.17x faster** |
| `regex_short_circuit` | `159.6 ns` | `343.1 ns` | `122.0 ns` | `1,716.1 ns` | **2.81x faster** | **14.07x faster** |

### Key Takeaways

- **vs Peer PyPI Library (`mongoquery`)**: `mongory` is **9.2x–25.8x faster** across all monomorphic and polymorphic cases.
- **vs Hand-Written Python (`plain_fast`)**: `mongory` stays roughly **on par to ~1.7x faster** on structural/numeric filters (`regex` alone is slower because the C adapter delegates back to Python's `re` engine), while providing reusable JSON rules and `explain`/`trace` debugging.
- **vs Semantics-Equivalent Pure Python (`plain_safe`)**: `mongory` is **1.29x–2.84x faster** across every case.

> For cross-language benchmarks (`mongory-go`, `mongory-js`, `mongory-rb`), see [Mongory Central](https://mongoryhq.github.io/mongory-central/#benchmarks).
