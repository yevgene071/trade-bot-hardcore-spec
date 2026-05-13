#!/usr/bin/env python3
"""Bundle the modular dashboard into a single self-contained HTML file.

Phases implemented:
* #237 (Phase 0): passthrough copy of ``index.html``.
* #238 (Phase 1): ``<!-- include: path -->`` directives.
* #239 (Phase 2): ``<link rel="stylesheet" href="...">`` inlining with
  recursive local CSS ``@import`` expansion.

Directive contract:

* ``<!-- include: <relative/path> -->`` is replaced by the raw byte contents of
  the referenced file, resolved relative to the directory of the file that
  contains the directive (i.e. includes inside fragments resolve relative to the
  fragment, not the root index).
* Includes are resolved recursively. A cycle (A → B → A) is a fatal error.
* ``<link rel="stylesheet" href="<relative/path>">`` is replaced by a
  ``<style>`` block whose contents are the referenced stylesheet with all local
  ``@import`` statements recursively expanded.
* CSS ``@import`` targets are resolved relative to the stylesheet that contains
  the import. Cycles are fatal errors. External imports (``http:``, ``https:``,
  protocol-relative URLs, ``data:``, etc.) are preserved as-is.
* A missing target file is a fatal error — we never emit a partial bundle.

Usage:
    inline_dashboard.py <input_index_html> <output_bundle_html>
"""
from __future__ import annotations

import re
import sys
from pathlib import Path
from urllib.parse import urlparse

# HTML comment form of the include directive. Paths are a run of non-whitespace
# characters; surrounding whitespace inside the comment is tolerated. Multiple
# directives on the same line are handled correctly by ``re.sub`` — each match is
# replaced independently.
_INCLUDE_RE = re.compile(rb"<!--\s*include:\s*(\S+?)\s*-->")

# Phase 2 stylesheet links are intentionally simple and local. Keep the matcher
# strict so unrelated links (preconnect, icons, alternate stylesheets) are not
# rewritten accidentally.
_STYLESHEET_LINK_RE = re.compile(
    rb"<link\s+rel=[\"']stylesheet[\"']\s+href=[\"']([^\"']+)[\"']\s*/?>"
)

# Accept all common CSS forms used by hand-authored stylesheets:
#   @import "file.css";
#   @import 'file.css';
#   @import url("file.css");
#   @import url(file.css);
_CSS_IMPORT_RE = re.compile(
    rb"@import\s+(?:url\(\s*)?[\"']?([^\"'\)\s;]+)[\"']?\s*\)?\s*;"
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


def _expand_html(path: Path, stack: tuple[Path, ...]) -> bytes:
    """Return ``path`` with HTML includes and stylesheet links expanded."""
    resolved, data = _read_file(path, "include")
    if resolved in stack:
        _format_cycle("include", stack, resolved)

    base_dir = resolved.parent
    html_stack = stack + (resolved,)

    def _replace_include(match: re.Match[bytes]) -> bytes:
        # Directive paths are ASCII-only file paths, decoding as UTF-8 is safe
        # and lets us use pathlib comfortably.
        rel = match.group(1).decode("utf-8")
        return _expand_html(base_dir / rel, html_stack)

    def _replace_stylesheet(match: re.Match[bytes]) -> bytes:
        href = match.group(1).decode("utf-8")
        if _is_external_url(href):
            return match.group(0)
        css = _expand_css(base_dir / href, stack=())
        return b"<style>\n" + css.rstrip(b"\n") + b"\n</style>"

    data = _INCLUDE_RE.sub(_replace_include, data)
    return _STYLESHEET_LINK_RE.sub(_replace_stylesheet, data)


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
