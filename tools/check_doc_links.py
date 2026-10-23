"""Checks the relative links of Markdown files: every link or image must point to an existing file, and every
`#anchor` must match a heading of its target (or of the same file).

Usage:
  py -3 tools/check_doc_links.py                 every Markdown file tracked by Git in this repository
  py -3 tools/check_doc_links.py FILE.md ...     only these files
  py -3 tools/check_doc_links.py --root DIR      the tracked Markdown files of another checkout

In a Git checkout, the target must also be tracked by Git (or be a folder holding tracked files): a file that is
only on disk, or that .gitignore excludes, is a broken link once the repository is published.

Exit code: 0 if every link is valid, 1 if a link is broken (each one is listed as file:line: target: reason),
2 on a usage error.

What is checked: inline links and images `[text](target)` / `![alt](target)`, reference definitions `[id]: target`,
and the `href="..."` / `src="..."` attributes of HTML tags. Links with a scheme (https:, mailto:, ...) are skipped, as
is everything inside fenced code blocks and inline code spans. Anchors follow GitHub's heading ids: lower case,
punctuation removed except `-` and `_`, spaces turned into `-`, and `-1`, `-2`... added to repeated headings.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path
from urllib.parse import unquote

INLINE = re.compile(r"!?\[(?:[^\[\]]|\[[^\[\]]*\])*\]\(\s*<?([^)\s>]+)>?(?:\s+\"[^\"]*\")?\s*\)")
REFDEF = re.compile(r"^\s{0,3}\[[^\]]+\]:\s*<?(\S+?)>?(?:\s+.*)?$")
HTML_ATTR = re.compile(r"<[a-zA-Z][^>]*?\s(?:href|src)\s*=\s*\"([^\"]+)\"")
HEADING = re.compile(r"^\s{0,3}(#{1,6})\s+(.*?)\s*#*\s*$")
SCHEME = re.compile(r"^[a-zA-Z][a-zA-Z0-9+.-]*:")
FENCE = re.compile(r"^\s{0,3}(```|~~~)")
CODE_SPAN = re.compile(r"(`+)(.+?)\1")


def strip_code(text: str) -> list[tuple[int, str]]:
    """Lines of `text` (1-based numbers) outside fenced code blocks, with inline code spans blanked out."""
    out: list[tuple[int, str]] = []
    fence = None
    for n, line in enumerate(text.splitlines(), 1):
        m = FENCE.match(line)
        if m:
            if fence is None:
                fence = m.group(1)
            elif m.group(1) == fence:
                fence = None
            continue
        if fence is None:
            out.append((n, CODE_SPAN.sub(lambda c: " " * len(c.group(0)), line)))
    return out


def slug(heading: str) -> str:
    """GitHub's id for a heading text (inline Markdown removed first)."""
    t = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", heading)  # links and images: keep the text
    t = re.sub(r"<[^>]+>", "", t)                            # HTML tags
    t = t.replace("`", "").replace("*", "")
    t = "".join(c for c in t.lower() if c.isalnum() or c in " -_")
    return t.replace(" ", "-")


def anchors(path: Path, cache: dict[Path, set[str]]) -> set[str]:
    if path not in cache:
        seen: dict[str, int] = {}
        ids: set[str] = set()
        for _, line in strip_code(path.read_text(encoding="utf-8")):
            m = HEADING.match(line)
            if not m:
                continue
            base = slug(m.group(2))
            k = seen.get(base, 0)
            seen[base] = k + 1
            ids.add(base if k == 0 else f"{base}-{k}")
        for m in re.finditer(r"<a\s+(?:name|id)=\"([^\"]+)\"", path.read_text(encoding="utf-8")):
            ids.add(m.group(1))
        cache[path] = ids
    return cache[path]


def check_file(md: Path, root: Path, cache: dict[Path, set[str]], tracked: set[str] | None) -> list[str]:
    problems: list[str] = []
    for n, line in strip_code(md.read_text(encoding="utf-8")):
        targets = [m.group(1) for m in INLINE.finditer(line)]
        targets += [m.group(1) for m in HTML_ATTR.finditer(line)]
        m = REFDEF.match(line)
        if m:
            targets.append(m.group(1))
        for target in targets:
            if SCHEME.match(target) or target.startswith("//"):
                continue
            file_part, _, anchor = target.partition("#")
            dest = md if not file_part else (md.parent / unquote(file_part)).resolve()
            where = f"{md.relative_to(root).as_posix()}:{n}: {target}"
            try:
                dest.relative_to(root)
            except ValueError:
                problems.append(f"{where}: points outside the repository")
                continue
            if not dest.exists():
                problems.append(f"{where}: file not found")
                continue
            rel = dest.relative_to(root).as_posix()
            if tracked is not None and rel != "." and rel not in tracked and not any(
                    f.startswith(rel + "/") for f in tracked):
                problems.append(f"{where}: not tracked by Git (ignored or not added)")
                continue
            if anchor and dest.suffix.lower() == ".md" and dest.is_file():
                if unquote(anchor).lower() not in anchors(dest, cache):
                    problems.append(f"{where}: no heading with this anchor in {dest.relative_to(root).as_posix()}")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("files", nargs="*", help="Markdown files (default: every tracked *.md file)")
    ap.add_argument("--root", default=None, help="repository root (default: the checkout holding this script)")
    args = ap.parse_args()
    root = Path(args.root).resolve() if args.root else Path(__file__).resolve().parent.parent
    res = subprocess.run(["git", "-C", str(root), "ls-files", "-z"], capture_output=True, text=True)
    tracked = {f for f in res.stdout.split("\0") if f} if res.returncode == 0 else None
    if args.files:
        files = [Path(f).resolve() for f in args.files]
    elif tracked is None:
        print(f"git ls-files failed in {root}: {res.stderr.strip()}", file=sys.stderr)
        return 2
    else:
        files = [root / f for f in sorted(tracked) if f.lower().endswith(".md")]
    missing = [f for f in files if not f.is_file()]
    if missing:
        print("not a file: " + ", ".join(str(f) for f in missing), file=sys.stderr)
        return 2
    cache: dict[Path, set[str]] = {}
    problems: list[str] = []
    for md in files:
        problems += check_file(md, root, cache, tracked)
    for p in problems:
        print(p)
    print(f"{len(files)} file(s) checked, {len(problems)} broken link(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
