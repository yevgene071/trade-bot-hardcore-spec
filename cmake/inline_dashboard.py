#!/usr/bin/env python3
"""Bundle the modular dashboard into a single HTML file.

Phases implemented:
* #237 (Phase 0): passthrough copy of ``index.html``.
* #238 (Phase 1): ``<!-- include: path -->`` directives.

Future phases (#239..#245) will extend this script to inline CSS chunks
and JS modules referenced from ``index.html`` so the emitted bundle
remains a single self-contained file — preserving the current
``embed_html.py`` mechanism that compiles the dashboard into the C++
binary.

Directive contract (Phase 1):

* ``<!-- include: <relative/path> -->`` is replaced by the raw byte
  contents of the referenced file, resolved relative to the directory
  of the file that contains the directive (i.e. includes inside
  fragments resolve relative to the fragment, not the root index).
* No bytes are added or stripped around the replacement — the directive
  must occupy exactly the byte range that should be substituted.
  Fragments MUST NOT be wrapped with extra leading/trailing newlines.
* Includes are resolved recursively. A cycle (A → B → A) is a fatal
  error.
* A missing target file is a fatal error — we never emit a partial
  bundle.

Usage:
    inline_dashboard.py <input_index_html> <output_bundle_html>
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

# HTML comment form of the include directive. Paths are a run of
# non-whitespace characters; surrounding whitespace inside the comment
# is tolerated. Multiple directives on the same line are handled
# correctly by ``re.sub`` — each match is replaced independently.
_INCLUDE_RE = re.compile(rb"<!--\s*include:\s*(\S+?)\s*-->")


def _expand(path: Path, stack: tuple[Path, ...]) -> bytes:
    """Return the byte contents of ``path`` with include directives expanded.

    ``stack`` is the chain of files currently being expanded; used to
    detect cycles.
    """
    resolved = path.resolve()
    if resolved in stack:
        chain = " -> ".join(str(p) for p in stack + (resolved,))
        print(f"error: cyclic include detected: {chain}", file=sys.stderr)
        sys.exit(1)

    try:
        data = resolved.read_bytes()
    except FileNotFoundError:
        print(f"error: include target not found: {resolved}", file=sys.stderr)
        sys.exit(1)

    base_dir = resolved.parent

    def _replace(match: re.Match[bytes]) -> bytes:
        # Directive paths are ASCII-only file paths, decoding as UTF-8 is
        # safe and lets us use pathlib comfortably.
        rel = match.group(1).decode("utf-8")
        target = (base_dir / rel)
        return _expand(target, stack + (resolved,))

    return _INCLUDE_RE.sub(_replace, data)


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
        print(f"error: input file not found: {src}", file=sys.stderr)
        sys.exit(1)

    expanded = _expand(src, stack=())

    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(expanded)


if __name__ == "__main__":
    main()
