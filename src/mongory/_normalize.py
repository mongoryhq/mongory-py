"""Condition normalizer and compile-scoped context for mongory-py.

Handles:
- Top-level condition validation (`None` -> `{}`, `Mapping` -> normalized `dict`).
- Unescaped dot-path splitting (`"a.b"` -> `{"a": {"b": ...}}`) and `\\.` unescaping.
- Leaf collision preservation via `"$and"` (aligned with Go, JS, Ruby, and C spec).
- Cycle detection and `max_depth` / `max_nodes` safety limits.
- Compile-scoped type converters and custom operators.
"""

from __future__ import annotations

import contextlib
import dataclasses
from collections.abc import Callable, Iterator, Mapping, Sequence
from typing import Any

DEFAULT_MAX_DEPTH = 64
DEFAULT_MAX_NODES = 4096

OperatorPredicate = Callable[[Any, bool], bool]
OperatorCompiler = Callable[[Any], OperatorPredicate]

# Operators whose operand is a data literal (compared against records), not a nested condition.
_LITERAL_OPERATORS = frozenset({"$eq", "$ne", "$in", "$nin"})


def _split_dotted_key(key: str) -> list[str]:
    if key == "":
        return [""]
    segments: list[str] = []
    buf: list[str] = []
    i = 0
    n = len(key)
    while i < n:
        ch = key[i]
        if ch == "\\" and i + 1 < n and key[i + 1] == ".":
            buf.append(".")
            i += 2
        elif ch == ".":
            segments.append("".join(buf))
            buf = []
            i += 1
        else:
            buf.append(ch)
            i += 1
    segments.append("".join(buf))
    while len(segments) > 1 and segments[-1] == "":
        segments.pop()
    return segments


def _deep_merge_into(target: dict[str, Any], source: dict[str, Any]) -> dict[str, Any]:
    and_clauses: list[dict[str, Any]] | None = None
    for key, val in source.items():
        if key not in target:
            target[key] = val
        elif isinstance(target[key], dict) and isinstance(val, dict):
            target[key] = _deep_merge_into(dict(target[key]), val)
        else:
            if and_clauses is None:
                and_clauses = []
            prev = target.pop(key)
            and_clauses.append({key: prev})
            and_clauses.append({key: val})

    if and_clauses is not None:
        existing_and = target.get("$and")
        if isinstance(existing_and, list):
            target["$and"] = [*existing_and, *and_clauses]
        elif "$and" in target:
            prev_and = target.pop("$and")
            target["$and"] = [{"$and": prev_and}, *and_clauses]
        else:
            target["$and"] = and_clauses

    return target


class CompileContext:
    """Holds compile-scoped converters, custom operators, and safety limits."""

    def __init__(
        self,
        *,
        converters: Mapping[type, Callable[[Any], Any]] | None = None,
        operators: Mapping[str, OperatorCompiler] | None = None,
        max_depth: int = DEFAULT_MAX_DEPTH,
        max_nodes: int = DEFAULT_MAX_NODES,
        invalid_option_error: type[Exception],
        invalid_condition_error: type[Exception],
        limit_exceeded_error: type[Exception],
    ) -> None:
        if not isinstance(max_depth, int) or isinstance(max_depth, bool) or max_depth < 1:
            raise invalid_option_error("max_depth must be a positive integer")
        if not isinstance(max_nodes, int) or isinstance(max_nodes, bool) or max_nodes < 1:
            raise invalid_option_error("max_nodes must be a positive integer")

        self.converters: dict[type, Callable[[Any], Any]] = {}
        if converters is not None:
            if not isinstance(converters, Mapping):
                raise invalid_option_error("converters must be a mapping of {type: callable}")
            for typ, fn in converters.items():
                if not isinstance(typ, type) or not callable(fn):
                    raise invalid_option_error("converters entries must be (type, callable)")
                self.converters[typ] = fn

        self.operators: dict[str, OperatorCompiler] = {}
        if operators is not None:
            if not isinstance(operators, Mapping):
                raise invalid_option_error("operators must be a mapping of {'$op': callable}")
            for name, compiler in operators.items():
                if not isinstance(name, str) or not name.startswith("$") or len(name) < 2 or not callable(compiler):
                    raise invalid_option_error(f"Invalid custom operator registration: {name!r}")
                self.operators[name] = compiler

        self.max_depth = max_depth
        self.max_nodes = max_nodes
        self._invalid_condition_error = invalid_condition_error
        self._limit_exceeded_error = limit_exceeded_error

    def has_operator(self, key: str) -> bool:
        return key in self.operators

    def build_operator(self, key: str, raw_arg: Any) -> tuple[str, OperatorPredicate]:
        compiler = self.operators[key]
        pred = compiler(raw_arg)
        if not callable(pred):
            raise self._invalid_condition_error(f"Custom operator {key} compiler must return a callable")
        display_name = key[1:2].upper() + key[2:]
        return (display_name, pred)

    def convert_data(self, value: Any) -> Any:
        for typ, fn in self.converters.items():
            if isinstance(value, typ):
                return fn(value)
        if dataclasses.is_dataclass(value) and not isinstance(value, type):
            return {f.name: getattr(value, f.name) for f in dataclasses.fields(value)}
        if isinstance(value, Mapping) and not isinstance(value, dict):
            return dict(value)
        if isinstance(value, Sequence) and not isinstance(value, (list, tuple, str, bytes, bytearray)):
            return list(value)
        return value

    def normalize_root(self, condition: Any) -> dict[str, Any]:
        if condition is None:
            return {}
        if not isinstance(condition, Mapping):
            raise self._invalid_condition_error("Top-level condition must be a mapping (dict) or None")
        nodes = [0]
        seen: set[int] = set()
        return self._normalize_mapping(condition, depth=1, seen=seen, nodes=nodes)

    def _bump_node(self, nodes: list[int]) -> None:
        nodes[0] += 1
        if nodes[0] > self.max_nodes:
            raise self._limit_exceeded_error(f"Condition exceeds max_nodes limit ({self.max_nodes})")

    def _normalize_mapping(
        self,
        mapping: Mapping[Any, Any],
        *,
        depth: int,
        seen: set[int],
        nodes: list[int],
    ) -> dict[str, Any]:
        if depth > self.max_depth:
            raise self._limit_exceeded_error(f"Condition exceeds max_depth limit ({self.max_depth})")
        obj_id = id(mapping)
        if obj_id in seen:
            raise self._invalid_condition_error("Circular reference detected in condition")
        seen.add(obj_id)
        try:
            result: dict[str, Any] = {}
            for raw_key, raw_val in mapping.items():
                if not isinstance(raw_key, str):
                    raise self._invalid_condition_error(f"Condition key must be a str, got {type(raw_key).__name__}")
                self._bump_node(nodes)
                segments = _split_dotted_key(raw_key)
                extra_depth = len(segments) - 1
                if depth + extra_depth > self.max_depth:
                    raise self._limit_exceeded_error(f"Condition exceeds max_depth limit ({self.max_depth})")
                leaf_key = segments[-1]
                norm_val = self._normalize_value(
                    raw_val,
                    parent_key=leaf_key,
                    depth=depth + extra_depth,
                    seen=seen,
                    nodes=nodes,
                )
                nested: Any = norm_val
                for seg in reversed(segments[1:]):
                    nested = {seg: nested}
                _deep_merge_into(result, {segments[0]: nested})
            return result
        finally:
            seen.remove(obj_id)

    def _normalize_value(
        self,
        value: Any,
        *,
        parent_key: str,
        depth: int,
        seen: set[int],
        nodes: list[int],
    ) -> Any:
        # Preserve raw argument for custom operators unless a converter applies
        if parent_key in self.operators:
            return self.convert_data(value)

        if parent_key in _LITERAL_OPERATORS:
            return self._copy_literal(value, depth=depth, seen=seen)

        value = self._apply_converter(value)

        if isinstance(value, Mapping):
            return self._normalize_mapping(value, depth=depth + 1, seen=seen, nodes=nodes)

        if isinstance(value, (list, tuple)):
            with self._enter_container(value, depth=depth + 1, seen=seen):
                out: list[Any] = []
                for item in value:
                    self._bump_node(nodes)
                    out.append(self._normalize_value(item, parent_key="", depth=depth + 1, seen=seen, nodes=nodes))
                return out

        return value

    def _apply_converter(self, value: Any) -> Any:
        for typ, fn in self.converters.items():
            if isinstance(value, typ):
                return fn(value)
        return value

    @contextlib.contextmanager
    def _enter_container(self, container: Any, *, depth: int, seen: set[int]) -> Iterator[None]:
        obj_id = id(container)
        if obj_id in seen:
            raise self._invalid_condition_error("Circular reference detected in condition")
        if depth > self.max_depth:
            raise self._limit_exceeded_error(f"Condition exceeds max_depth limit ({self.max_depth})")
        seen.add(obj_id)
        try:
            yield
        finally:
            seen.remove(obj_id)

    def _copy_literal(self, value: Any, *, depth: int, seen: set[int]) -> Any:
        """Copy a data operand verbatim: no dotted-key splitting, no `$and` merging."""
        value = self._apply_converter(value)
        if isinstance(value, Mapping):
            with self._enter_container(value, depth=depth + 1, seen=seen):
                result: dict[str, Any] = {}
                for key, item in value.items():
                    if not isinstance(key, str):
                        raise self._invalid_condition_error(f"Condition key must be a str, got {type(key).__name__}")
                    result[key] = self._copy_literal(item, depth=depth + 1, seen=seen)
                return result
        if isinstance(value, (list, tuple)):
            with self._enter_container(value, depth=depth + 1, seen=seen):
                return [self._copy_literal(item, depth=depth + 1, seen=seen) for item in value]
        return value
