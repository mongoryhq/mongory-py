"""Human-readable explain() and trace() tree formatters."""

from __future__ import annotations

import json
from typing import Any


def _format_val(val: Any) -> str:
    try:
        return json.dumps(val, ensure_ascii=False)
    except TypeError:
        return repr(val)


def _format_node_label(node: dict[str, Any]) -> str:
    name = node.get("name", "")
    if name == "Field" and "field" in node:
        return f"Field({_format_val(node['field'])})"
    if name in ("And", "Or", "Not", "ElemMatch", "Every", "ArrayRecord"):
        return name
    cond = node.get("condition")
    return f"{name}({_format_val(cond)})"


def render_explain_tree(root: dict[str, Any]) -> str:
    lines: list[str] = [_format_node_label(root)]

    def walk(children: list[dict[str, Any]], prefix: str) -> None:
        total = len(children)
        for idx, child in enumerate(children):
            is_last = idx == total - 1
            branch = "└── " if is_last else "├── "
            lines.append(f"{prefix}{branch}{_format_node_label(child)}")
            sub = child.get("children") or []
            if sub:
                walk(sub, prefix + ("    " if is_last else "│   "))

    walk(root.get("children") or [], "")
    return "\n".join(lines)


def render_trace_tree(root: dict[str, Any]) -> str:
    def fmt_line(node: dict[str, Any]) -> str:
        res = str(node.get("result", "skip")).upper()
        label = _format_node_label(node)
        if "record" in node and res != "SKIP":
            return f"[{res}] {label} | value={_format_val(node['record'])}"
        return f"[{res}] {label}"

    lines: list[str] = [fmt_line(root)]

    def walk(children: list[dict[str, Any]], prefix: str) -> None:
        total = len(children)
        for idx, child in enumerate(children):
            is_last = idx == total - 1
            branch = "└── " if is_last else "├── "
            lines.append(f"{prefix}{branch}{fmt_line(child)}")
            sub = child.get("children") or []
            if sub:
                walk(sub, prefix + ("    " if is_last else "│   "))

    walk(root.get("children") or [], "")
    return "\n".join(lines)
