#!/usr/bin/env python3
"""Bundle the modular dashboard into a single self-contained HTML file.

Phases implemented:
* #237 (Phase 0): passthrough copy of ``index.html``.
* #238 (Phase 1): ``<!-- include: path -->`` directives.
* #239 (Phase 2): ``<link rel="stylesheet" href="...">`` inlining with
  recursive local CSS ``@import`` expansion.
* #240 (Phase 3): ``<script src="...">`` inlining with topological sort
  via ``// @depends-on: path`` and script/comment escaping.

Directive contract:

* ``<!-- include: <relative/path> -->`` is replaced by the raw byte contents of
  the referenced file, resolved relative to the directory of the file that
  contains the directive.
* Includes are resolved recursively. A cycle (A → B → A) is a fatal error.
* ``<link rel="stylesheet" href="<relative/path>">`` is replaced by a
  ``<style>`` block whose contents are the referenced stylesheet with all local
  ``@import`` statements recursively expanded.
* CSS ``@import`` targets are resolved relative to the stylesheet that contains
  the import. Cycles are fatal errors. External imports are preserved as-is.
* ``<script src="<relative/path>"></script>`` is replaced by a ``<script>``
  block containing the referenced file and all its dependencies (via
  ``// @depends-on: path``) in topological order.
* JS dependencies are resolved relative to the file that contains the directive.
  Cycles or missing files are fatal errors.
* Inlined JS has ``</script>`` escaped as ``<\/script>`` and ``<!--`` as ``<\!--``.
* A missing target file is a fatal error — we never emit a partial bundle.

Usage:
    inline_dashboard.py <input_index_html> <output_bundle_html>
"""
from __future__ import annotations

import re
import sys
from pathlib import Path
from urllib.parse import urlparse

_INCLUDE_RE = re.compile(rb"<!--\s*include:\s*(\S+?)\s*-->")

_STYLESHEET_LINK_RE = re.compile(
    rb"<link\s+rel=[\"']stylesheet[\"']\s+href=[\"']([^\"']+)[\"']\s*/?>"
)

_CSS_IMPORT_RE = re.compile(
    rb"@import\s+(?:url\(\s*)?[\"']?([^\"'\)\s;]+)[\"']?\s*\)?\s*;"
)

_SCRIPT_SRC_RE = re.compile(
    rb"<script\s+src=[\"']([^\"']+)[\"']\s*>\s*</script>"
)

_JS_DEPENDS_RE = re.compile(
    rb"//\s*@depends-on:\s*(\S+)"
)


def _die(message: str) -> None:
    print(f"error: {message}", file=sys.stderr)
    sys.exit(1)


def _is_external_url(raw: str) -> bool:
    parsed = urlparse(raw)
    return raw.startswith("//") or parsed.scheme not in ("", "file")


def _read_file(path: Path, kind: str) -> tuple[Path, bytes]:
    resolved = path.resolve()
    try:
        return resolved, resolved.read_bytes()
    except FileNotFoundError:
        _die(f"{kind} target not found: {resolved}")


def _format_cycle(kind: str, stack: tuple[Path, ...], repeated: Path) -> None:
    chain = " -> ".join(str(p) for p in stack + (repeated,))
    _die(f"cyclic {kind} detected: {chain}")


def _expand_css(path: Path, stack: tuple[Path, ...]) -> bytes:
    """Return ``path`` with local CSS imports recursively expanded."""
    resolved, data = _read_file(path, "stylesheet")
    if resolved in stack:
        _format_cycle("stylesheet import", stack, resolved)

    base_dir = resolved.parent
    css_stack = stack + (resolved,)

    def _replace_import(match: re.Match[bytes]) -> bytes:
        raw_target = match.group(1).decode("utf-8")
        if _is_external_url(raw_target):
            return match.group(0)
        return _expand_css(base_dir / raw_target, css_stack)

    return _CSS_IMPORT_RE.sub(_replace_import, data)


def _escape_js(data: bytes) -> bytes:
    """Escape </script> and <!-- in JS to prevent breaking the HTML parser."""
    # We use simple byte replacements.
    data = data.replace(b"</script>", b"<\\/script>")
    data = data.replace(b"<!--", b"<\\!--")
    return data


def _expand_js(path: Path) -> bytes:
    """Return concatenated JS files in topological order based on @depends-on."""
    visited: dict[Path, bytes] = {}
    order: list[Path] = []
    processing_stack: list[Path] = []

    def _visit(p: Path) -> None:
        resolved = p.resolve()
        if resolved in processing_stack:
            _format_cycle("JS dependency", tuple(processing_stack), resolved)
        if resolved in visited:
            return

        processing_stack.append(resolved)
        _, data = _read_file(resolved, "JS")
        
        base_dir = resolved.parent
        for match in _JS_DEPENDS_RE.finditer(data):
            dep_rel = match.group(1).decode("utf-8")
            _visit(base_dir / dep_rel)
        
        processing_stack.pop()
        visited[resolved] = data
        order.append(resolved)

    _visit(path)
    
    parts = []
    for p in order:
        parts.append(b"// --- " + str(p.name).encode("utf-8") + b" ---\n")
        parts.append(_escape_js(visited[p]).rstrip(b"\n") + b"\n")
    
    return b"".join(parts)


def _expand_html(path: Path, stack: tuple[Path, ...]) -> bytes:
    """Return ``path`` with HTML includes, stylesheet links, and scripts expanded."""
    resolved, data = _read_file(path, "include")
    if resolved in stack:
        _format_cycle("include", stack, resolved)

    base_dir = resolved.parent
    html_stack = stack + (resolved,)

    def _replace_include(match: re.Match[bytes]) -> bytes:
        rel = match.group(1).decode("utf-8")
        return _expand_html(base_dir / rel, html_stack)

    def _replace_stylesheet(match: re.Match[bytes]) -> bytes:
        href = match.group(1).decode("utf-8")
        if _is_external_url(href):
            return match.group(0)
        css = _expand_css(base_dir / href, stack=())
        return b"<style>\n" + css.rstrip(b"\n") + b"\n</style>"

    def _replace_script(match: re.Match[bytes]) -> bytes:
        src = match.group(1).decode("utf-8")
        if _is_external_url(src):
            return match.group(0)
        js = _expand_js(base_dir / src)
        return b"<script>\n" + js.rstrip(b"\n") + b"\n</script>"

    data = _INCLUDE_RE.sub(_replace_include, data)
    data = _STYLESHEET_LINK_RE.sub(_replace_stylesheet, data)
    data = _SCRIPT_SRC_RE.sub(_replace_script, data)
    return data


def main() -> None:
    if len(sys.argv) != 3:
        print(
            f"Usage: {sys.argv[0]} <input_index_html> <output_bundle_html>",
            file=sys.stderr,
        )
        sys.exit(1)

    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])

    if not src.is_file():
        _die(f"input file not found: {src}")

    expanded = _expand_html(src, stack=())

    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(expanded)


if __name__ == "__main__":
    main()
