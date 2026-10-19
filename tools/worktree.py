"""Safe removal of git worktrees that contain NTFS junctions or symbolic links (issue 37).

Usage:
  py -3 tools/worktree.py list
  py -3 tools/worktree.py remove <path> [--discard] [--delete-branch] [--dry-run]

Why: `tools/ab.py` makes worktrees whose `external/`, `orig/` and `generated/<game>/<ID>/disc` are junctions that
point into the main checkout. On Windows, `git worktree remove --force` or `rm -rf` can follow a junction and delete
the files of its target (it emptied the extracted disc of the main checkout once).

`remove` therefore:
  1. refuses anything that is not a linked worktree of this repository (never the main checkout, never a link);
  2. scans the worktree for reparse points (junctions and symbolic links) WITHOUT descending into them;
  3. deletes each link itself (`rmdir` / `unlink`, which never touches the target and fails on a real folder);
  4. scans again and refuses to go on if a link is left;
  5. refuses to go on if `git status --porcelain` is not empty (unless `--discard`);
  6. runs `git worktree remove` (with `--force` only for `--discard`), then `git worktree prune`;
  7. with `--delete-branch`, deletes the worktree's branch only if it is fully merged into `main`.
`--dry-run` only lists the links that would be removed.
"""

from __future__ import annotations

import argparse
import os
import stat
import subprocess
import sys
from pathlib import Path

REPARSE = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)  # os.stat_result.st_file_attributes (Windows only)
DIRECTORY = getattr(stat, "FILE_ATTRIBUTE_DIRECTORY", 0x10)


class Refusal(Exception):
    """The helper stops without having removed the worktree."""


def is_link(path: str | os.PathLike) -> bool:
    """True for a symbolic link or, on Windows, any reparse point (NTFS junction included). Does not follow it."""
    try:
        st = os.lstat(path)
    except OSError:
        return False
    return stat.S_ISLNK(st.st_mode) or bool(getattr(st, "st_file_attributes", 0) & REPARSE)


def find_links(root: Path) -> list[Path]:
    """All links under `root`, found without ever descending into a link."""
    found: list[Path] = []
    stack = [root]
    while stack:
        folder = stack.pop()
        try:
            with os.scandir(folder) as it:
                entries = list(it)
        except OSError as exc:
            raise Refusal(f"cannot read {folder}: {exc}")
        for e in entries:
            if is_link(e.path):
                found.append(Path(e.path))
            elif e.is_dir(follow_symlinks=False):
                stack.append(Path(e.path))
    return sorted(found)


def link_target(link: Path) -> str:
    try:
        return os.readlink(link).removeprefix("\\\\?\\")
    except OSError:
        return "?"


def remove_link(link: Path) -> None:
    """Removes the link only. `os.rmdir` on a junction or directory symlink removes the link itself; on a real
    folder it fails if the folder is not empty, so it is a safe call even if the scan was wrong."""
    st = os.lstat(link)
    if getattr(st, "st_file_attributes", 0) & DIRECTORY or (stat.S_ISLNK(st.st_mode) and link.is_dir()):
        os.rmdir(link)
    else:
        os.unlink(link)


def git(*args: str, cwd: Path, check: bool = True) -> str:
    r = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if check and r.returncode != 0:
        raise Refusal(f"git {' '.join(args)}: {(r.stderr or r.stdout).strip()}")
    return r.stdout


def worktrees(cwd: Path) -> list[dict[str, str]]:
    """`git worktree list --porcelain`; the first entry is the main checkout."""
    out, cur = [], {}
    for line in git("worktree", "list", "--porcelain", cwd=cwd).splitlines() + [""]:
        if not line:
            if cur:
                out.append(cur)
            cur = {}
        else:
            k, _, v = line.partition(" ")
            cur[k] = v
    return out


def same_path(a: str | os.PathLike, b: str | os.PathLike) -> bool:
    return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))


def inside(path: Path, folder: Path) -> bool:
    a, b = os.path.normcase(os.path.realpath(path)), os.path.normcase(os.path.realpath(folder))
    return a == b or a.startswith(b.rstrip(os.sep) + os.sep)


def cmd_list(_args: argparse.Namespace) -> int:
    wts = worktrees(Path.cwd())
    for i, wt in enumerate(wts):
        path = Path(wt["worktree"])
        label = wt.get("branch", "detached").replace("refs/heads/", "")
        print(f"{'main checkout' if i == 0 else 'worktree'}: {path}  [{label}]")
        if i and path.is_dir():
            for link in find_links(path):
                print(f"    link: {link.relative_to(path)} -> {link_target(link)}")
    return 0


def cmd_remove(args: argparse.Namespace) -> int:
    target = Path(args.path)
    if not target.exists():
        raise Refusal(f"{target}: no such folder (use `git worktree prune` for a worktree whose folder is gone)")
    if is_link(target):
        raise Refusal(f"{target} is itself a link: refusing")
    wts = worktrees(target)  # cwd = the worktree: works for any worktree of the repository
    main = Path(wts[0]["worktree"])
    wt = next((w for w in wts if same_path(w["worktree"], target)), None)
    if wt is None:
        raise Refusal(f"{target} is not a worktree of this repository")
    if same_path(main, target):
        raise Refusal(f"{target} is the main checkout: refusing")
    if inside(Path.cwd(), target):
        raise Refusal("the current folder is inside that worktree: run the command from elsewhere")
    root = Path(wt["worktree"])
    branch = wt.get("branch", "").replace("refs/heads/", "")

    links = find_links(root)
    print(f"worktree {root} ({branch or 'detached'}): {len(links)} link(s)")
    for link in links:
        print(f"  link: {link.relative_to(root)} -> {link_target(link)}")
    if args.dry_run:
        print("dry run: nothing removed")
        return 0

    # Check the status BEFORE touching anything, so that a refusal leaves the worktree usable. A junction that is
    # not ignored shows up as "?? <link>/": those lines are the links themselves, not changes.
    link_names = {str(link.relative_to(root)).replace(os.sep, "/") for link in links}
    status = "\n".join(line for line in git("status", "--porcelain", cwd=root).splitlines()
                       if not (line.startswith("?? ") and line[3:].rstrip("/") in link_names)).strip()
    if status and not args.discard:
        raise Refusal("the worktree has uncommitted changes (shown below); nothing was removed. Commit them, or use "
                      "--discard to throw them away:\n" + status)

    for link in links:
        try:
            remove_link(link)
        except OSError as exc:
            raise Refusal(f"cannot remove the link {link}: {exc}. The worktree is NOT removed")
        print(f"  removed link {link.relative_to(root)} (target untouched)")
    left = find_links(root)
    if left:
        raise Refusal("links still present, the worktree is NOT removed: " + ", ".join(str(p) for p in left))

    git("worktree", "remove", *(["--force"] if args.discard else []), str(root), cwd=main)
    git("worktree", "prune", cwd=main)
    print(f"worktree removed: {root}")

    if args.delete_branch:
        if not branch:
            print("no branch to delete (detached worktree)")
        elif subprocess.run(["git", "merge-base", "--is-ancestor", branch, "main"], cwd=main,
                            capture_output=True).returncode != 0:
            print(f"branch {branch} kept: not fully merged into main")
        else:
            git("branch", "-d", branch, cwd=main)
            print(f"branch deleted: {branch} (fully merged into main)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("list", help="worktrees of this repository and the links they contain").set_defaults(func=cmd_list)
    rm = sub.add_parser("remove", help="remove a worktree after removing its junctions and symlinks")
    rm.add_argument("path")
    rm.add_argument("--discard", action="store_true", help="remove even if the worktree has uncommitted changes")
    rm.add_argument("--delete-branch", action="store_true", help="also delete its branch if fully merged into main")
    rm.add_argument("--dry-run", action="store_true", help="only list the links that would be removed")
    rm.set_defaults(func=cmd_remove)
    args = parser.parse_args()
    try:
        return args.func(args)
    except Refusal as exc:
        print(f"worktree.py: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
