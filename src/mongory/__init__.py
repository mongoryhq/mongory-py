"""Mongory (Python): C99-backed in-memory Mongo-style rule engine & query matcher.

Powered by `mongory-core` (C99) via the `_mongory_ext` CPython C-API extension,
and cross-checked against `mongory-spec`.
"""

from __future__ import annotations

from collections.abc import Callable, Iterable, Iterator, Mapping
from typing import Any, TypeVar

from . import op, operator
from ._mongory_ext import CMatcher as _CMatcher
from ._mongory_ext import InvalidConditionError, MongoryError
from ._normalize import (
    DEFAULT_MAX_DEPTH,
    DEFAULT_MAX_NODES,
    CompileContext,
    OperatorCompiler,
    OperatorPredicate,
)
from ._tree import render_explain_tree, render_trace_tree

__version__ = "0.2.1"

T = TypeVar("T")


def _copy_tree(value: Any) -> Any:
    if isinstance(value, dict):
        return {k: _copy_tree(v) for k, v in value.items()}
    if isinstance(value, list):
        return [_copy_tree(v) for v in value]
    return value


class InvalidOptionError(MongoryError, ValueError):
    """Raised when compile-time configuration options (converters, operators, limits) are invalid."""


class LimitExceededError(InvalidConditionError):
    """Raised when a query condition exceeds `max_depth` or `max_nodes`."""


class Matcher:
    """Immutable compiled query matcher backed by `mongory-core` (C99)."""

    __slots__ = ("_c_matcher", "_context")

    def __init__(
        self,
        condition: Mapping[str, Any] | None = None,
        *,
        converters: Mapping[type, Callable[[Any], Any]] | None = None,
        operators: Mapping[str, OperatorCompiler] | None = None,
        max_depth: int = DEFAULT_MAX_DEPTH,
        max_nodes: int = DEFAULT_MAX_NODES,
    ) -> None:
        ctx = CompileContext(
            converters=converters,
            operators=operators,
            max_depth=max_depth,
            max_nodes=max_nodes,
            invalid_option_error=InvalidOptionError,
            invalid_condition_error=InvalidConditionError,
            limit_exceeded_error=LimitExceededError,
        )
        normalized = ctx.normalize_root(condition)
        self._context = ctx
        self._c_matcher = _CMatcher(normalized, ctx)

    @property
    def condition(self) -> dict[str, Any]:
        """Normalized condition dictionary (a fresh copy; mutating it does not affect the matcher)."""
        return _copy_tree(self._c_matcher.condition)

    def match(self, record: Any) -> bool:
        """Return True if `record` satisfies the compiled query condition."""
        return bool(self._c_matcher.match(record))

    def __call__(self, record: Any) -> bool:
        """Callable alias for `match(record)` (usable directly with built-in `filter(matcher, records)`)."""
        return bool(self._c_matcher.match(record))

    def filter(self, records: Iterable[T], *, limit: int | None = None) -> Iterator[T]:
        """Lazily yield matching records from `records` (up to `limit` when specified)."""
        if limit is not None and (not isinstance(limit, int) or isinstance(limit, bool)):
            raise InvalidOptionError("limit must be an integer or None")
        return self._filter(records, limit)

    def _filter(self, records: Iterable[T], limit: int | None) -> Iterator[T]:
        if limit is not None and limit <= 0:
            return
        yielded = 0
        c_match = self._c_matcher.match
        for item in records:
            if c_match(item):
                yield item
                yielded += 1
                if limit is not None and yielded >= limit:
                    return

    def filter_list(self, records: Iterable[T], *, limit: int | None = None) -> list[T]:
        """Return a list of matching records from `records` (up to `limit` when specified)."""
        return list(self.filter(records, limit=limit))

    def find_one(self, records: Iterable[T]) -> T | None:
        """Return the first matching record from `records`, or `None` if none match."""
        c_match = self._c_matcher.match
        for item in records:
            if c_match(item):
                return item
        return None

    def any(self, records: Iterable[Any]) -> bool:
        """Return True if at least one record in `records` matches."""
        c_match = self._c_matcher.match
        for item in records:
            if c_match(item):
                return True
        return False

    def count(self, records: Iterable[Any]) -> int:
        """Return the number of matching records in `records`."""
        total = 0
        c_match = self._c_matcher.match
        for item in records:
            if c_match(item):
                total += 1
        return total

    def explain_tree(self) -> dict[str, Any]:
        """Return the structured cost-sorted execution plan tree."""
        return self._c_matcher.explain_tree()

    def explain(self) -> str:
        """Return a human-readable execution plan string."""
        return render_explain_tree(self._c_matcher.explain_tree())

    def trace_tree(self, record: Any) -> tuple[bool, dict[str, Any]]:
        """Evaluate `record` and return `(matched, trace_tree_dict)` with PASS/FAIL/SKIP nodes."""
        tree = self._c_matcher.trace_tree(record)
        matched = tree.get("result") == "pass"
        return (matched, tree)

    def trace(self, record: Any) -> tuple[bool, str]:
        """Evaluate `record` and return `(matched, human_readable_trace_str)`."""
        matched, tree = self.trace_tree(record)
        return (matched, render_trace_tree(tree))


def compile(
    condition: Mapping[str, Any] | None = None,
    *,
    converters: Mapping[type, Callable[[Any], Any]] | None = None,
    operators: Mapping[str, OperatorCompiler] | None = None,
    max_depth: int = DEFAULT_MAX_DEPTH,
    max_nodes: int = DEFAULT_MAX_NODES,
) -> Matcher:
    """Compile a MongoDB-style query condition into a reusable `Matcher`."""
    return Matcher(
        condition,
        converters=converters,
        operators=operators,
        max_depth=max_depth,
        max_nodes=max_nodes,
    )


def match(
    condition: Mapping[str, Any] | None,
    record: Any,
    *,
    converters: Mapping[type, Callable[[Any], Any]] | None = None,
    operators: Mapping[str, OperatorCompiler] | None = None,
    max_depth: int = DEFAULT_MAX_DEPTH,
    max_nodes: int = DEFAULT_MAX_NODES,
) -> bool:
    """One-shot convenience helper: compile `condition` and evaluate `record`."""
    return compile(
        condition,
        converters=converters,
        operators=operators,
        max_depth=max_depth,
        max_nodes=max_nodes,
    ).match(record)


__all__ = [
    "__version__",
    "DEFAULT_MAX_DEPTH",
    "DEFAULT_MAX_NODES",
    "MongoryError",
    "InvalidConditionError",
    "InvalidOptionError",
    "LimitExceededError",
    "OperatorPredicate",
    "OperatorCompiler",
    "Matcher",
    "compile",
    "match",
    "op",
    "operator",
]
