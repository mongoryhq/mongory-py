"""Unit tests for mongory-py Pythonic API, operator constants, extensions, and limits."""

from __future__ import annotations

import dataclasses
import re
import sys
import unittest
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
if list((ROOT / "src" / "mongory").glob("_mongory_ext*")):
    sys.path.insert(0, str(ROOT / "src"))

import mongory  # noqa: E402
from mongory import op  # noqa: E402
import mongory.operator as op_alias  # noqa: E402


@dataclasses.dataclass
class UserRecord:
    name: str
    age: int
    tags: list[str]
    active: bool = True


class Money:
    def __init__(self, currency: str, amount: int) -> None:
        self.currency = currency
        self.amount = amount


class TestMongoryPythonAPI(unittest.TestCase):
    def test_operator_constants_and_alias(self) -> None:
        self.assertEqual(op.EQ, "$eq")
        self.assertEqual(op.NE, "$ne")
        self.assertEqual(op.GT, "$gt")
        self.assertEqual(op.GTE, "$gte")
        self.assertEqual(op.LT, "$lt")
        self.assertEqual(op.LTE, "$lte")
        self.assertEqual(op.IN, "$in")
        self.assertEqual(op.NIN, "$nin")
        self.assertEqual(op.EXISTS, "$exists")
        self.assertEqual(op.PRESENT, "$present")
        self.assertEqual(op.AND, "$and")
        self.assertEqual(op.OR, "$or")
        self.assertEqual(op.NOT, "$not")
        self.assertEqual(op.ELEM_MATCH, "$elemMatch")
        self.assertEqual(op.EVERY, "$every")
        self.assertEqual(op.SIZE, "$size")
        self.assertEqual(op.REGEX, "$regex")
        self.assertEqual(op.OPTIONS, "$options")
        self.assertEqual(op_alias.ELEM_MATCH, "$elemMatch")
        self.assertEqual(op_alias.IN, "$in")

        m = mongory.compile(
            {
                "age": {op.GTE: 18, op.LT: 65},
                op.OR: [
                    {"role": {op.IN: ["admin", "editor"]}},
                    {"tags": {op.ELEM_MATCH: {op.EQ: "vip"}}},
                ],
            }
        )
        self.assertTrue(m.match({"age": 30, "role": "admin"}))
        self.assertTrue(m.match({"age": 22, "role": "guest", "tags": ["vip"]}))
        self.assertFalse(m.match({"age": 16, "role": "admin"}))

    def test_collection_helpers_and_callable(self) -> None:
        users = [
            {"name": "Ann", "age": 30, "tags": ("py", "c"), "active": True},
            {"name": "Bob", "age": 15, "tags": ("ruby",), "active": True},
            {"name": "Cid", "age": 42, "tags": ("py", "go"), "active": False},
            {"name": "Dan", "age": 25, "tags": ("py",), "active": True},
        ]
        m = mongory.compile(
            {
                op.AND: [
                    {"active": True},
                    {op.OR: [{"tags": "py"}, {"age": {op.LT: 18}}]},
                ]
            }
        )

        # __call__ works directly with built-in filter()
        self.assertEqual([u["name"] for u in filter(m, users)], ["Ann", "Bob", "Dan"])

        # Lazy iterator filter() + limit
        it = m.filter(users)
        self.assertEqual(next(it)["name"], "Ann")
        self.assertEqual([u["name"] for u in m.filter(users, limit=2)], ["Ann", "Bob"])
        self.assertEqual(m.filter_list(users, limit=0), [])
        self.assertEqual(m.find_one(users), users[0])
        self.assertIsNone(mongory.compile({"age": {op.GT: 100}}).find_one(users))
        self.assertTrue(m.any(users))
        self.assertFalse(mongory.compile({op.OR: []}).any(users))
        self.assertEqual(m.count(users), 3)
        self.assertTrue(mongory.match(None, {"anything": 1}))

    def test_compiled_re_pattern_and_dataclass_records(self) -> None:
        m = mongory.compile(
            {
                "name": re.compile(r"^a", re.IGNORECASE),
                "age": {op.GTE: 18},
                "tags": "py",
            }
        )
        self.assertTrue(m.match(UserRecord(name="Alice", age=28, tags=["py", "c"])))
        self.assertFalse(m.match(UserRecord(name="Bob", age=28, tags=["py"])))

    def test_explain_and_trace(self) -> None:
        m = mongory.compile(
            {
                "name": {op.REGEX: "^A"},
                "age": {op.GTE: 18},
                "active": True,
            }
        )
        plan_text = m.explain()
        self.assertIn("Field", plan_text)
        plan_tree = m.explain_tree()
        self.assertEqual(plan_tree["name"], "And")
        self.assertEqual(len(plan_tree["children"]), 3)

        matched, trace_text = m.trace({"name": "Bob", "age": 15, "active": False})
        self.assertFalse(matched)
        self.assertIn("[FAIL]", trace_text)
        self.assertIn("[SKIP]", trace_text)

        matched2, trace_tree = m.trace_tree({"name": "Alice", "age": 25, "active": True})
        self.assertTrue(matched2)
        self.assertEqual(trace_tree["result"], "pass")

    def test_compile_scoped_converters_and_custom_operators(self) -> None:
        def compile_money_gte(target: Any):
            if not isinstance(target, Money):
                raise ValueError("$moneyGte requires a Money instance")

            def predicate(val: Any, exists: bool) -> bool:
                return (
                    exists
                    and isinstance(val, Money)
                    and val.currency == target.currency
                    and val.amount >= target.amount
                )

            return predicate

        m = mongory.compile(
            {
                "price": {"$moneyGte": Money("USD", 100)},
                "tags": {op.IN: ["sale"]},
            },
            converters={set: sorted},
            operators={"$moneyGte": compile_money_gte},
        )
        self.assertTrue(m.match({"price": Money("USD", 150), "tags": {"sale", "new"}}))
        self.assertFalse(m.match({"price": Money("USD", 80), "tags": {"sale"}}))
        self.assertFalse(m.match({"price": Money("EUR", 200), "tags": {"sale"}}))

        # Without the custom operator registered, $moneyGte is rejected
        with self.assertRaises(mongory.InvalidConditionError):
            mongory.compile({"price": {"$moneyGte": Money("USD", 100)}})

    def test_limits_cycles_and_error_hierarchy(self) -> None:
        self.assertTrue(issubclass(mongory.InvalidConditionError, mongory.MongoryError))
        self.assertTrue(issubclass(mongory.InvalidConditionError, ValueError))
        self.assertTrue(issubclass(mongory.LimitExceededError, mongory.InvalidConditionError))
        self.assertTrue(issubclass(mongory.InvalidOptionError, mongory.MongoryError))
        self.assertTrue(issubclass(mongory.InvalidOptionError, ValueError))

        with self.assertRaises(mongory.InvalidOptionError):
            mongory.compile({}, max_depth=0)

        with self.assertRaises(mongory.LimitExceededError):
            mongory.compile({"a.b.c.d": 1}, max_depth=2)

        with self.assertRaises(mongory.LimitExceededError):
            mongory.compile({"a": 1, "b": 2, "c": 3}, max_nodes=2)

        # Single large $in counts as 1 node
        m = mongory.compile({"a": {op.IN: list(range(100))}}, max_nodes=5)
        self.assertTrue(m.match({"a": 42}))

        # Circular condition is rejected
        cyclic: dict[str, Any] = {}
        cyclic["self"] = cyclic
        with self.assertRaises(mongory.InvalidConditionError):
            mongory.compile(cyclic)

    def test_python_bool_vs_int_isolation(self) -> None:
        # In Python, `True == 1`, `False == 0`, and `isinstance(True, int)` are True.
        # Mongory must strictly distinguish bool and int across all operators and converters.
        self.assertFalse(mongory.match({"a": True}, {"a": 1}))
        self.assertFalse(mongory.match({"a": False}, {"a": 0}))
        self.assertFalse(mongory.match({"a": 1}, {"a": True}))
        self.assertFalse(mongory.match({"a": 0}, {"a": False}))
        self.assertTrue(mongory.match({"a": {op.NE: True}}, {"a": 1}))
        self.assertTrue(mongory.match({"a": {op.NE: 1}}, {"a": True}))
        self.assertFalse(mongory.match({"a": {op.IN: [1, 0]}}, {"a": True}))
        self.assertFalse(mongory.match({"a": {op.IN: [True, False]}}, {"a": 1}))
        self.assertFalse(mongory.match({"a": {op.GTE: 0}}, {"a": True}))
        self.assertFalse(mongory.match({"a": {op.LTE: 1}}, {"a": False}))

        # Registering an `int` converter must NOT convert `bool` values
        class BoxedInt:
            def __init__(self, v: int) -> None:
                self.v = v

        m = mongory.compile(
            {"flag": True, "count": 50},
            converters={
                int: lambda x: x + 1,
                BoxedInt: lambda b: b.v,
            },
        )
        self.assertTrue(m.match({"flag": True, "count": BoxedInt(51)}))
        self.assertFalse(m.match({"flag": 2, "count": BoxedInt(51)}))

    def test_literal_operands_are_not_normalized(self) -> None:
        cond = {"x": {"$eq": {"a.b": 1}}}
        self.assertEqual(mongory.compile(cond).condition, cond)
        self.assertTrue(mongory.match(cond, {"x": {"a.b": 1}}))
        self.assertFalse(mongory.match(cond, {"x": {"a": {"b": 1}}}))
        self.assertTrue(mongory.match({"x": {"$in": [{"a.b": 1}]}}, {"x": {"a.b": 1}}))
        self.assertTrue(mongory.match({"x": {"$nin": [{"a.b": 1}]}}, {"x": {"a": {"b": 1}}}))

    def test_callback_exceptions_propagate(self) -> None:
        class Boom(Exception):
            pass

        def raising_pred(value: Any, exists: bool) -> bool:
            raise Boom("pred")

        m = mongory.compile({"x": {"$boom": 1}}, operators={"$boom": lambda arg: raising_pred})
        with self.assertRaises(Boom):
            m.match({"x": 1})
        with self.assertRaises(Boom):
            m.trace_tree({"x": 1})

        def interrupting_pred(value: Any, exists: bool) -> bool:
            raise KeyboardInterrupt

        m = mongory.compile({"x": {"$stop": 1}}, operators={"$stop": lambda arg: interrupting_pred})
        with self.assertRaises(KeyboardInterrupt):
            m.match({"x": 1})

        def bad_converter(value: Any) -> Any:
            raise Boom("converter")

        m = mongory.compile({"x": 1}, converters={Money: bad_converter})
        with self.assertRaises(Boom):
            m.match({"x": Money("USD", 1)})

        def bad_compiler(arg: Any) -> Any:
            raise Boom("compiler")

        with self.assertRaises(mongory.InvalidConditionError) as ctx:
            mongory.compile({"x": {"$bad": 1}}, operators={"$bad": bad_compiler})
        self.assertIsInstance(ctx.exception.__cause__, Boom)

        with self.assertRaises(mongory.InvalidConditionError) as ctx:
            mongory.match({"s": {"$regex": "("}}, {"s": "x"})
        self.assertIsInstance(ctx.exception.__cause__, re.error)

    def test_matcher_cycles_are_collected(self) -> None:
        import gc
        import weakref

        class Holder:
            matcher: Any = None

        holder = Holder()
        m = mongory.compile({"x": {"$ref": 1}}, operators={"$ref": lambda arg: (lambda v, e: holder is not None)})
        holder.matcher = m
        self.assertTrue(m.match({"x": 1}))
        ref = weakref.ref(holder)
        del holder, m
        gc.collect()
        self.assertIsNone(ref())

    def test_concurrent_matches_with_python_callbacks(self) -> None:
        import threading

        def slow_pred(value: Any, exists: bool) -> bool:
            sum(range(2000))  # give the interpreter a chance to switch threads
            return value == 1

        m = mongory.compile(
            {"x": {"$slow": 1}, "tags": "a"},
            operators={"$slow": lambda arg: slow_pred},
        )
        errors: list[BaseException] = []

        def worker() -> None:
            try:
                for _ in range(300):
                    assert m.match({"x": 1, "tags": ["b", "a"]})
                    assert not m.match({"x": 2, "tags": ["a"]})
                    assert m.trace_tree({"x": 1, "tags": ["a"]})[0]
            except BaseException as exc:  # noqa: BLE001
                errors.append(exc)

        threads = [threading.Thread(target=worker) for _ in range(4)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=30)
        self.assertFalse(any(t.is_alive() for t in threads), "deadlock")
        self.assertEqual(errors, [])

    def test_recursive_reentry_preserves_scratch_pool(self) -> None:
        nested_results: list[bool] = []

        class ReentrantScore:
            def __init__(self, value: int) -> None:
                self.value = value

        m_box: list[mongory.Matcher] = []

        def convert_score(obj: ReentrantScore) -> int:
            nested_results.append(m_box[0].match({"score": 1, "tag": "no"}))
            nested_results.append(m_box[0].match({"score": 9, "tag": "ok"}))
            return obj.value

        m = mongory.compile({"score": {"$gt": 5}, "tag": "ok"}, converters={ReentrantScore: convert_score})
        m_box.append(m)

        for _ in range(3):
            nested_results.clear()
            self.assertTrue(m.match({"score": ReentrantScore(10), "tag": "ok"}))
            self.assertEqual(nested_results, [False, True])
        self.assertTrue(m.match({"score": 7, "tag": "ok"}))

    def test_lossy_scalars_never_compare_equal(self) -> None:
        self.assertFalse(mongory.match({"s": "a\x00b"}, {"s": "a"}))
        self.assertFalse(mongory.match({"n": 2**63}, {"n": 2**63 + 1}))
        self.assertTrue(mongory.match({"n": 2**64}, {"n": 2**64}))
        self.assertTrue(mongory.match({"n": {"$gt": 2**63}}, {"n": 2**70}))

    def test_filter_validates_limit_eagerly_and_condition_is_a_copy(self) -> None:
        m = mongory.compile({"a": {"b": 1}})
        with self.assertRaises(mongory.InvalidOptionError):
            m.filter([], limit="1")  # type: ignore[arg-type]
        m.condition["a"]["b"] = 2
        self.assertEqual(m.condition, {"a": {"b": 1}})


if __name__ == "__main__":
    unittest.main()
