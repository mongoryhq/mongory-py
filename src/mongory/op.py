"""Built-in Mongory query operator key constants (uppercase PEP 8 constants).

Example:
    from mongory import compile, op

    m = compile({
        "age": {op.GTE: 18, op.LT: 65},
        op.OR: [
            {"role": {op.IN: ["admin", "editor"]}},
            {"tags": {op.ELEM_MATCH: {op.EQ: "vip"}}},
        ],
    })
"""

from typing import Final

# Comparison operators
EQ: Final[str] = "$eq"
NE: Final[str] = "$ne"
GT: Final[str] = "$gt"
GTE: Final[str] = "$gte"
LT: Final[str] = "$lt"
LTE: Final[str] = "$lte"

# Inclusion operators
IN: Final[str] = "$in"
NIN: Final[str] = "$nin"

# Existence operators
EXISTS: Final[str] = "$exists"
PRESENT: Final[str] = "$present"

# Logical operators
AND: Final[str] = "$and"
OR: Final[str] = "$or"
NOR: Final[str] = "$nor"
NOT: Final[str] = "$not"

# Array operators
ELEM_MATCH: Final[str] = "$elemMatch"
EVERY: Final[str] = "$every"
SIZE: Final[str] = "$size"

# Regex operator and its modifier key
REGEX: Final[str] = "$regex"
OPTIONS: Final[str] = "$options"

__all__ = [
    "EQ",
    "NE",
    "GT",
    "GTE",
    "LT",
    "LTE",
    "IN",
    "NIN",
    "EXISTS",
    "PRESENT",
    "AND",
    "OR",
    "NOR",
    "NOT",
    "ELEM_MATCH",
    "EVERY",
    "SIZE",
    "REGEX",
    "OPTIONS",
]
