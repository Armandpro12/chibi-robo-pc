"""REA (Reverse Engineer Anything) on the main executable of a GameCube game. Optional tool.

Usage:
  py -3 tools/rea.py <game> [--version ID] [--native] elf [--force]   # DOL + symbol map -> ELF (tools/dol2elf.py)
  py -3 tools/rea.py <game> [--version ID] image [--rebuild]          # build the Docker image (tools/rea/Dockerfile)
  py -3 tools/rea.py <game> [--version ID] [--native] run -- <rea arguments, {elf} = the ELF>
  py -3 tools/rea.py <game> [--version ID] [--native] mcp             # `rea mcp` over stdio, for an MCP client
  py -3 tools/rea.py <game> [--version ID] [--native] mcp-config      # prints an MCP client registration
Examples:
  py -3 tools/rea.py chibi-robo --version GGTE01 run -- analyze {elf} --provider ghidra --json
  py -3 tools/rea.py chibi-robo --version GGTP01 run -- function {elf} OSInit --provider ghidra --json

REA (npm package `rea-agents`, MIT License, https://github.com/morluto/rea) drives Ghidra and answers
questions about a binary (overview, functions, pseudo-C, references) from the command line or as an MCP
server. Its PowerPC lane ("PowerPC ELF through Ghidra", REA docs/ghidra-ppc.md) admits 32-bit big-endian
ELF executables without the EF_PPC_EMB flag, on Linux/macOS hosts only; GameCube DOL files are not admitted.
Hence:
  - `elf` converts the DOL into such an ELF with the version's symbol map: tools/dol2elf.py, output
    `generated/<game>/<ID>/rea/<ID>.elf` (derived from the game: ignored by Git). Rebuilt automatically when
    the DOL, the map or dol2elf.py is newer;
  - by default REA runs in a Linux container (Docker), from the image of tools/rea/Dockerfile (versions
    pinned there), with the host's Ghidra mounted read-only at /opt/ghidra and the ELF folder mounted at
    /work. `--native` runs a `rea` found on the PATH instead (Linux/macOS hosts only);
  - the image is built only by the `image` command (it downloads software: see IMAGE_DOWNLOADS); `run` and
    `mcp` stop with a message when it is missing, or when it was built from another version of the
    Dockerfile (label DOCKERFILE_LABEL = the Dockerfile's SHA-256);
  - the container has no network (`--network none`), no capabilities (`--cap-drop ALL`), cannot gain
    privileges (`--security-opt no-new-privileges`) and runs as a non-root user (container_user());
  - the Ghidra language comes from `[rea] language` in games/<game>/game.toml (passed as
    REA_GHIDRA_LANGUAGE_ID) when that language id is declared by an installed `.ldefs` file of the Ghidra
    installation; otherwise REA's default PowerPC language is used and a warning is printed.

Placeholders in `run` arguments: {elf} = the ELF path as REA sees it, {work} = its folder (writable: REA
outputs such as snapshots may be written there; it is under generated/, ignored by Git).
Containers are named `rea-pc-<game>-<ID>-<pid>` and removed when they exit (`--rm`); `mcp` prints the name
on stderr, so a stuck server can be stopped with `docker stop <that exact name>`.

Each CLI call imports the program into Ghidra again (about 2 minutes for a 2 MiB DOL); the MCP server keeps
its session open between questions.

Paths: Ghidra from tools/toolchain.py (config/local.toml [paths] ghidra, then external/ghidra); docker from
[paths] docker, then the PATH. Commands are run without a shell (argument lists), so paths with spaces work;
paths with a comma are refused in Docker mode (`docker run --mount` splits its value on commas).
The Docker daemon is checked (`docker version`) before any image or container command.

Warning: the ELF is derived from the game. `--out` can make this tool write it anywhere (and `--dol` convert
any DOL); the default folder (generated/<game>/<ID>/rea) is ignored by Git. Never commit the ELF or anything
in the work folder.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dol2elf  # noqa: E402
import toolchain as tc  # noqa: E402

REA_VERSION = "6.7.0"                        # keep in step with ARG REA_VERSION in tools/rea/Dockerfile
IMAGE = f"gcrt-rea:{REA_VERSION}"
DOCKERFILE_DIR = Path(__file__).resolve().parent / "rea"
CONTAINER_GHIDRA = "/opt/ghidra"
CONTAINER_WORK = "/work"
CONTAINER_HOME = "/home/rea"                 # world-writable (sticky) home created by the Dockerfile
IMAGE_USER = "1000:1000"                     # the base image's `ubuntu` user
DOCKERFILE_LABEL = "org.chibi-robo-pc.rea.dockerfile-sha256"
IMAGE_DOWNLOADS = ("The image build downloads: the Eclipse Temurin JDK 21 base image (pinned by digest), the "
                   "Ubuntu packages curl, ca-certificates and xz-utils (build stage only), Node.js "
                   "(pinned, SHA-256 checked) and the npm package rea-agents with its npm dependencies "
                   "(rea-agents pinned; its transitive dependencies are not). See tools/rea/Dockerfile.")


def dockerfile_hash() -> str:
    """SHA-256 of tools/rea/Dockerfile, line endings normalized (a Windows checkout may have CRLF)."""
    data = (DOCKERFILE_DIR / "Dockerfile").read_bytes().replace(b"\r\n", b"\n")
    return hashlib.sha256(data).hexdigest()


def container_user() -> str:
    """Non-root user of the container. Linux/macOS: the host's uid:gid, so that files REA writes in the work
    folder belong to the user (root on the host: the image's user instead, never root in the container).
    Windows (Docker Desktop): the image's user; writes to the bind-mounted work folder are allowed for it."""
    if hasattr(os, "getuid") and os.getuid() != 0:
        return f"{os.getuid()}:{os.getgid()}"
    return IMAGE_USER


def isolation_options() -> list[str]:
    """`docker run` options: no network (REA, Ghidra and the MCP server over stdio need none), no Linux
    capabilities, no privilege gain through setuid binaries, non-root user with a writable HOME."""
    return ["--network", "none", "--cap-drop", "ALL", "--security-opt", "no-new-privileges",
            "--user", container_user(), "-e", f"HOME={CONTAINER_HOME}"]


def log(msg: str) -> None:
    # stderr only: in `mcp` mode stdout carries the MCP protocol.
    print(msg, file=sys.stderr, flush=True)


def version_of(game: dict, requested: str | None) -> str:
    vid = requested or game["game"]["default_version"]
    known = [v["game_id"] for v in game.get("versions", [])]
    if vid not in known:
        sys.exit(f"unknown version {vid} (known: {', '.join(known)})")
    return vid


def installed_languages(ghidra: Path) -> set[str]:
    """Language ids declared by the .ldefs files of a Ghidra installation (processors and extensions)."""
    ids: set[str] = set()
    for ldefs in (ghidra / "Ghidra").glob("*/*/data/languages/*.ldefs"):
        try:
            root = ET.parse(ldefs).getroot()
        except ET.ParseError:
            continue
        ids.update(lang.get("id", "") for lang in root.iter("language"))
    ids.discard("")
    return ids


class Context:
    def __init__(self, a: argparse.Namespace):
        self.game = tc.load_game(a.game)
        self.slug = a.game
        self.vid = version_of(self.game, a.version)
        self.native = a.native
        self.image = a.image
        self.extra_env = a.env or []
        code = self.game.get("code", {})
        base = tc.GENERATED / self.slug / self.vid
        self.dol = Path(a.dol) if a.dol else base / "disc" / code.get("dol", "sys/main.dol")
        self.work = Path(a.out) if a.out else base / "rea"
        self.elf = self.work / f"{self.vid}.elf"
        sym = self.game.get("symbols", {})
        default_map = self.game["_dir"] / sym.get("map_dir", "symbols") / f"{self.vid}.map"
        self.map = None if a.no_map else (Path(a.map) if a.map else (default_map if default_map.exists() else None))
        self.rea_cfg = self.game.get("rea", {})
        self.sda = a.sda or self.rea_cfg.get("sda", "auto")

    # --- ELF -------------------------------------------------------------------------------------------
    def ensure_elf(self, force: bool = False) -> Path:
        if not self.dol.exists():
            sys.exit(f"DOL not found: {self.dol} (extract the game first: tools/extract.py {self.slug} "
                     f"--version {self.vid})")
        inputs = [self.dol, Path(dol2elf.__file__)] + ([self.map] if self.map else [])
        # The options used for the ELF are kept next to it: a change (map, small-data bases) rebuilds it.
        stamp = self.elf.with_name(self.elf.name + ".options.json")
        options = {"dol": str(self.dol.resolve()), "map": str(self.map.resolve()) if self.map else None,
                   "sda": self.sda}
        try:
            same = json.loads(stamp.read_text(encoding="utf-8")) == options
        except (OSError, ValueError):
            same = False
        if (not force and same and self.elf.exists()
                and self.elf.stat().st_mtime >= max(p.stat().st_mtime for p in inputs)):
            return self.elf
        log(f"dol2elf: {self.dol} -> {self.elf}" + (f" (map {self.map.name})" if self.map else " (no map)"))
        p = dol2elf.convert(self.dol, self.elf, self.map, self.sda)
        stamp.write_text(json.dumps(options, indent=1), encoding="utf-8")
        log(f"dol2elf: {len(p.funcs)} functions"
            + "".join(f", r{r} = {v:#010x}" for r, v in sorted(p.sda.items(), reverse=True)))
        return self.elf

    # --- Environment -----------------------------------------------------------------------------------
    def ghidra(self) -> Path:
        root = tc.find_ghidra_root()
        if not root:
            sys.exit("Ghidra not found (config/local.toml [paths] ghidra, or tools/bootstrap.py --only ghidra)")
        return root

    def rea_env(self, ghidra: Path) -> dict[str, str]:
        env: dict[str, str] = {}
        lang = self.rea_cfg.get("language")
        if lang:
            if lang in installed_languages(ghidra):
                env["REA_GHIDRA_LANGUAGE_ID"] = lang
                if self.rea_cfg.get("compiler_spec"):
                    env["REA_GHIDRA_COMPILER_SPEC_ID"] = self.rea_cfg["compiler_spec"]
            else:
                log(f"warning: Ghidra language {lang} ([rea] language of games/{self.slug}/game.toml) is not "
                    f"installed in {ghidra}; REA will use its default PowerPC language "
                    "(install the extension that provides it: tools/bootstrap.py --only ghidra_gc_loader)")
        for item in self.extra_env:
            key, sep, value = item.partition("=")
            if not sep:
                sys.exit(f"--env expects NAME=VALUE, got {item!r}")
            env[key] = value
        return env

    def docker(self) -> str:
        exe = tc.find_docker()
        if not exe:
            sys.exit("docker not found (install Docker, or use --native on a Linux/macOS host with REA installed)")
        return str(exe)

    def ensure_daemon(self) -> None:
        """Fails clearly when the Docker daemon is unreachable, instead of a confusing build or run error."""
        if getattr(self, "_daemon_ok", False):
            return
        try:
            r = subprocess.run([self.docker(), "version", "--format", "{{.Server.Version}}"],
                               capture_output=True, text=True, timeout=60)
        except subprocess.TimeoutExpired:
            sys.exit("Docker daemon not responding (docker version timed out): start Docker Desktop or the "
                     "docker service, then retry")
        if r.returncode != 0 or not r.stdout.strip():
            detail = next((x.strip() for x in r.stderr.splitlines() if x.strip()), "no server version")
            sys.exit(f"Docker daemon unreachable: {detail}\n"
                     "Start Docker Desktop (Windows/macOS) or the docker service (Linux), then retry.")
        self._daemon_ok = True

    def image_label(self) -> str | None:
        """The Dockerfile hash label of the image, "" when the image has no such label, None when it is absent."""
        self.ensure_daemon()
        r = subprocess.run([self.docker(), "image", "inspect", "--format",
                            "{{ index .Config.Labels \"" + DOCKERFILE_LABEL + "\" }}", self.image],
                           capture_output=True, text=True)
        if r.returncode != 0:
            return None
        label = r.stdout.strip()
        return "" if label == "<no value>" else label

    def image_exists(self) -> bool:
        return self.image_label() is not None

    def require_image(self) -> None:
        """`run` and `mcp` never build the image (a build downloads software): `image` must be run first."""
        label = self.image_label()
        build = (f"py -3 tools/rea.py {self.slug}" + (f" --image {self.image}" if self.image != IMAGE else "")
                 + " image")
        if label is None:
            sys.exit(f"Docker image {self.image} not found. Build it first (once) with:\n  {build}\n"
                     f"{IMAGE_DOWNLOADS}")
        if label != dockerfile_hash():
            msg = (f"Docker image {self.image} was not built from the current tools/rea/Dockerfile "
                   f"(label {DOCKERFILE_LABEL}: {label or 'none'}). Rebuild it with:\n  {build} --rebuild")
            if self.image == IMAGE:
                sys.exit(msg)
            log("warning: " + msg)

    def build_image(self, rebuild: bool = False) -> None:
        self.ensure_daemon()
        if not rebuild and self.image_exists():
            log(f"image {self.image}: present (`image --rebuild` to build it again)")
            return
        log(IMAGE_DOWNLOADS)
        cmd = [self.docker(), "build", "--build-arg", f"REA_VERSION={REA_VERSION}",
               "--label", f"{DOCKERFILE_LABEL}={dockerfile_hash()}", "-t", self.image, str(DOCKERFILE_DIR)]
        log("+ " + " ".join(cmd))
        # Build output to stderr, so that `mcp` keeps a clean stdout.
        if subprocess.run(cmd, stdout=sys.stderr, stderr=sys.stderr).returncode != 0:
            sys.exit("docker build failed")

    def command(self, rea_args: list[str], interactive: bool) -> tuple[list[str], dict | None, str | None]:
        """Full command line for `rea <rea_args>`: (argv, environment for a native run, container name)."""
        if self.native and tc.IS_WINDOWS:
            sys.exit("--native: REA's PowerPC lane needs a Linux or macOS host; on Windows, use Docker (default)")
        rea = tc._which("rea") if self.native else None
        if self.native and not rea:
            sys.exit(f"--native: `rea` not found on the PATH (npm install -g rea-agents@{REA_VERSION})")
        ghidra = self.ghidra()
        if not self.native:
            # `--mount` takes comma-separated key=value fields: a comma in a path would split it.
            # Checked before the ELF is written and before Docker runs.
            for what, path in (("Ghidra folder", ghidra.resolve()), ("work folder (--out)", self.work.resolve())):
                if "," in str(path):
                    sys.exit(f"{what} path contains a comma, which `docker run --mount` cannot take: {path}\n"
                             "Use a path without a comma (move the folder, or pass another --out).")
            self.ensure_daemon()
            self.require_image()
        elf = self.ensure_elf()
        env = self.rea_env(ghidra)
        if self.native:
            subst = {"{elf}": str(elf.resolve()), "{work}": str(self.work.resolve())}
            args = [rea_arg_subst(x, subst) for x in rea_args]
            full_env = dict(os.environ, GHIDRA_INSTALL_DIR=str(ghidra), **env)
            return [str(rea), *args], full_env, None
        name = f"rea-pc-{self.slug}-{self.vid}-{os.getpid()}"
        subst = {"{elf}": f"{CONTAINER_WORK}/{elf.name}", "{work}": CONTAINER_WORK}
        args = [rea_arg_subst(x, subst) for x in rea_args]
        cmd = [self.docker(), "run", "--rm", "--init", "--name", name, *isolation_options()]
        if interactive:
            cmd.append("-i")
        cmd += ["--mount", f"type=bind,source={ghidra.resolve()},target={CONTAINER_GHIDRA},readonly",
                "--mount", f"type=bind,source={self.work.resolve()},target={CONTAINER_WORK}"]
        for k, v in env.items():
            cmd += ["-e", f"{k}={v}"]
        cmd += [self.image, "rea", *args]
        return cmd, None, name


def rea_arg_subst(arg: str, subst: dict[str, str]) -> str:
    for k, v in subst.items():
        arg = arg.replace(k, v)
    return arg


def run_cmd(ctx: Context, rea_args: list[str], interactive: bool) -> int:
    cmd, env, name = ctx.command(rea_args, interactive)
    log("+ " + " ".join(f'"{c}"' if " " in c else c for c in cmd))
    if name:
        log(f"container: {name}")
    proc = subprocess.Popen(cmd, env=env)
    try:
        return proc.wait()
    except KeyboardInterrupt:
        if name:
            subprocess.run([ctx.docker(), "stop", name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        else:
            proc.terminate()
        proc.wait()
        return 130


def mcp_config(ctx: Context, a: argparse.Namespace) -> int:
    script = Path(__file__).resolve()
    opts = ["--version", ctx.vid] + (["--native"] if ctx.native else [])
    if a.image != IMAGE:
        opts += ["--image", a.image]
    for name in ("dol", "map", "out"):
        if getattr(a, name):
            opts += [f"--{name}", str(Path(getattr(a, name)).resolve())]
    if a.no_map:
        opts.append("--no-map")
    if a.sda:
        opts += ["--sda", a.sda]
    for item in a.env or []:
        opts += ["--env", item]
    args = [str(script), ctx.slug, *opts, "mcp"]
    if tc.IS_WINDOWS:
        command, args = "py", ["-3", *args]
    else:
        command = sys.executable
    server = f"rea-{ctx.slug}-{ctx.vid}"
    print(json.dumps({"mcpServers": {server: {"command": command, "args": args}}}, indent=2))
    elf_path = str(ctx.elf.resolve()) if ctx.native else f"{CONTAINER_WORK}/{ctx.vid}.elf"
    log(f"The server sees the ELF at {elf_path} (MCP tool open_binary, provider_id \"ghidra\").")
    log("Claude Code: claude mcp add --scope local " + server + " -- " + " ".join(
        f'"{x}"' if " " in x else x for x in [command, *args]))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game")
    ap.add_argument("--version", help="game ID (default: [game] default_version of game.toml)")
    ap.add_argument("--native", action="store_true", help="run a `rea` from the PATH (Linux/macOS), not Docker")
    ap.add_argument("--image", default=IMAGE, help=f"Docker image tag (default {IMAGE})")
    ap.add_argument("--env", action="append", metavar="NAME=VALUE", help="extra environment variable for REA")
    ap.add_argument("--dol", help="DOL to convert (default: generated/<game>/<ID>/disc/<[code] dol>); the ELF "
                    "derived from it goes to the --out folder")
    ap.add_argument("--map", help="symbol map (default: games/<game>/<[symbols] map_dir>/<ID>.map)")
    ap.add_argument("--no-map", action="store_true", help="no symbol map")
    ap.add_argument("--sda", help="small-data bases for dol2elf: auto, none or R13,R2 (default: [rea] sda)")
    ap.add_argument("--out", help="ELF and REA work folder (default: generated/<game>/<ID>/rea, ignored by Git). "
                    "The ELF is derived from the game: never commit it, keep --out outside the repository's "
                    "tracked folders")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("elf", help="build the ELF")
    p.add_argument("--force", action="store_true")
    p = sub.add_parser("image", help="build the Docker image")
    p.add_argument("--rebuild", action="store_true")
    p = sub.add_parser("run", help="run `rea <args>`, {elf} replaced by the ELF")
    p.add_argument("rea_args", nargs=argparse.REMAINDER)
    sub.add_parser("mcp", help="`rea mcp` over stdio")
    sub.add_parser("mcp-config", help="print an MCP client registration (JSON)")
    a = ap.parse_args()
    ctx = Context(a)

    if a.cmd == "elf":
        print(ctx.ensure_elf(force=a.force))
        return 0
    if a.cmd == "image":
        ctx.build_image(rebuild=a.rebuild)
        return 0
    if a.cmd == "run":
        args = a.rea_args[1:] if a.rea_args[:1] == ["--"] else a.rea_args
        if not args:
            sys.exit("run: give the REA arguments after `--`, e.g. run -- analyze {elf} --provider ghidra --json")
        return run_cmd(ctx, args, interactive=False)
    if a.cmd == "mcp":
        return run_cmd(ctx, ["mcp"], interactive=True)
    if a.cmd == "mcp-config":
        return mcp_config(ctx, a)
    return 2


if __name__ == "__main__":
    sys.exit(main())
