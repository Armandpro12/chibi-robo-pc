"""Windows helpers for automated runs that must never take the foreground.

Generic (no game-specific code). Two pieces:

- `HiddenDesktop` + `launch_on_desktop()`: starts a GUI program on a separate desktop of the user's window
  station, created for the run and never switched to. Microsoft's documentation ("Desktops", Win32 docs,
  learn.microsoft.com/windows/win32/winstation/desktops) states that only one desktop of WinSta0 is active at a
  time (the "input desktop", the one visible to the user and receiving user input), and that window messages can
  only be sent between processes on the same desktop. A program started on another desktop therefore cannot
  become the user's foreground window nor receive the user's keystrokes, whatever it does with its windows.
  The desktop is given to the child through `STARTUPINFOW.lpDesktop` ("WinSta\\Desktop" form; see the
  STARTUPINFOW reference page) and created with `CreateDesktopW` (CreateDesktopW reference page). It is a
  per-session object, released when the last handle is closed: no system setting is changed.

- `ForegroundSampler`: samples `GetForegroundWindow()` at a fixed interval in a background thread and counts the
  samples where the foreground window belongs to watched process ids or image names. Used to prove that a run
  did not take the foreground.

Command line (for diagnosis): `py -3 tools/winbg.py watch --seconds N [--image NAME.exe] [--interval S]` prints
every foreground change and a summary.
"""

from __future__ import annotations

import ctypes
import os
import subprocess
import sys
import threading
import time
from ctypes import wintypes as wt

if sys.platform != "win32":  # pragma: no cover - the helpers are Windows-only
    raise ImportError("tools/winbg.py is Windows-only")

_user32 = ctypes.WinDLL("user32", use_last_error=True)
_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

# Constants (winuser.h / winbase.h / winnt.h).
GENERIC_ALL = 0x10000000
UOI_NAME = 2
WM_CLOSE = 0x0010
CREATE_NO_WINDOW = 0x08000000
CREATE_UNICODE_ENVIRONMENT = 0x00000400
STARTF_USESHOWWINDOW = 0x00000001
SW_SHOWMINNOACTIVE = 7
WAIT_OBJECT_0 = 0
WAIT_TIMEOUT = 0x102
INFINITE = 0xFFFFFFFF
STILL_ACTIVE = 259
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000


class STARTUPINFOW(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("lpReserved", wt.LPWSTR), ("lpDesktop", wt.LPWSTR), ("lpTitle", wt.LPWSTR),
                ("dwX", wt.DWORD), ("dwY", wt.DWORD), ("dwXSize", wt.DWORD), ("dwYSize", wt.DWORD),
                ("dwXCountChars", wt.DWORD), ("dwYCountChars", wt.DWORD), ("dwFillAttribute", wt.DWORD),
                ("dwFlags", wt.DWORD), ("wShowWindow", wt.WORD), ("cbReserved2", wt.WORD),
                ("lpReserved2", ctypes.POINTER(ctypes.c_byte)), ("hStdInput", wt.HANDLE),
                ("hStdOutput", wt.HANDLE), ("hStdError", wt.HANDLE)]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [("hProcess", wt.HANDLE), ("hThread", wt.HANDLE), ("dwProcessId", wt.DWORD),
                ("dwThreadId", wt.DWORD)]


_user32.CreateDesktopW.restype = wt.HANDLE
_user32.CreateDesktopW.argtypes = [wt.LPCWSTR, wt.LPCWSTR, wt.LPVOID, wt.DWORD, wt.DWORD, wt.LPVOID]
_user32.CloseDesktop.argtypes = [wt.HANDLE]
_user32.SetThreadDesktop.argtypes = [wt.HANDLE]
_user32.GetProcessWindowStation.restype = wt.HANDLE
_user32.GetUserObjectInformationW.argtypes = [wt.HANDLE, ctypes.c_int, wt.LPVOID, wt.DWORD, ctypes.POINTER(wt.DWORD)]
_user32.GetForegroundWindow.restype = wt.HWND
_user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
_user32.GetWindowThreadProcessId.restype = wt.DWORD
_user32.IsWindowVisible.argtypes = [wt.HWND]
_user32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
_WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
_user32.EnumWindows.argtypes = [_WNDENUMPROC, wt.LPARAM]
_kernel32.CreateProcessW.argtypes = [wt.LPCWSTR, wt.LPWSTR, wt.LPVOID, wt.LPVOID, wt.BOOL, wt.DWORD, wt.LPVOID,
                                     wt.LPCWSTR, ctypes.POINTER(STARTUPINFOW), ctypes.POINTER(PROCESS_INFORMATION)]
_kernel32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
_kernel32.WaitForSingleObject.restype = wt.DWORD
_kernel32.GetExitCodeProcess.argtypes = [wt.HANDLE, ctypes.POINTER(wt.DWORD)]
_kernel32.TerminateProcess.argtypes = [wt.HANDLE, wt.UINT]
_kernel32.CloseHandle.argtypes = [wt.HANDLE]
_kernel32.OpenProcess.restype = wt.HANDLE
_kernel32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
_kernel32.QueryFullProcessImageNameW.argtypes = [wt.HANDLE, wt.DWORD, wt.LPWSTR, ctypes.POINTER(wt.DWORD)]


def _win_error(what: str) -> OSError:
    err = ctypes.get_last_error()
    return OSError(err, f"{what} failed: {ctypes.FormatError(err).strip()} (error {err})")


def window_station_name() -> str:
    """Name of the calling process's window station (normally "WinSta0" for an interactive session)."""
    h = _user32.GetProcessWindowStation()
    if not h:
        raise _win_error("GetProcessWindowStation")
    buf = ctypes.create_unicode_buffer(256)
    needed = wt.DWORD(0)
    if not _user32.GetUserObjectInformationW(h, UOI_NAME, buf, ctypes.sizeof(buf), ctypes.byref(needed)):
        raise _win_error("GetUserObjectInformationW")
    return buf.value


class HiddenDesktop:
    """A desktop created for one run and never made the input desktop. Use as a context manager."""

    def __init__(self, name: str | None = None):
        self.name = name or f"gcrt-bg-{os.getpid()}-{int(time.time() * 1000) & 0xFFFFFF:x}"
        self.handle = None

    def __enter__(self) -> "HiddenDesktop":
        h = _user32.CreateDesktopW(self.name, None, None, 0, GENERIC_ALL, None)
        if not h:
            raise _win_error("CreateDesktopW")
        self.handle = h
        return self

    @property
    def full_name(self) -> str:
        """The `lpDesktop` value: "<window station>\\<desktop>"."""
        return f"{window_station_name()}\\{self.name}"

    def close(self) -> None:
        if self.handle:
            _user32.CloseDesktop(self.handle)
            self.handle = None

    def __exit__(self, *exc) -> None:
        self.close()

    def post_close(self, pid: int) -> int:
        """Posts WM_CLOSE to the top-level windows of `pid` on this desktop (visible ones first; all of them if
        none is visible), like a click on their close button. Window messages only travel between processes of
        the same desktop, so this runs in a helper thread attached to this desktop with SetThreadDesktop (which
        requires a thread without windows or hooks, true for a fresh thread). Returns the number of windows."""
        result: dict = {"count": 0, "error": None}

        def worker() -> None:
            if not _user32.SetThreadDesktop(self.handle):
                result["error"] = _win_error("SetThreadDesktop")
                return
            found: list[tuple[int, bool]] = []

            def cb(hwnd, _lparam):
                p = wt.DWORD(0)
                _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
                if p.value == pid:
                    found.append((hwnd, bool(_user32.IsWindowVisible(hwnd))))
                return True

            _user32.EnumWindows(_WNDENUMPROC(cb), 0)
            targets = [h for h, vis in found if vis] or [h for h, _ in found]
            for h in targets:
                _user32.PostMessageW(h, WM_CLOSE, 0, 0)
            result["count"] = len(targets)

        t = threading.Thread(target=worker, name="winbg-post-close", daemon=True)
        t.start()
        t.join()
        if result["error"] is not None:
            raise result["error"]
        return result["count"]


class BgProcess:
    """Minimal subset of subprocess.Popen for a process created by `launch_on_desktop`."""

    def __init__(self, pi: PROCESS_INFORMATION, args):
        self.args = args
        self.pid = pi.dwProcessId
        self._handle = pi.hProcess
        _kernel32.CloseHandle(pi.hThread)
        self.returncode: int | None = None

    def poll(self) -> int | None:
        if self.returncode is None and self._handle:
            if _kernel32.WaitForSingleObject(self._handle, 0) == WAIT_OBJECT_0:
                self._reap()
        return self.returncode

    def wait(self, timeout: float | None = None) -> int:
        if self.returncode is not None:
            return self.returncode
        ms = INFINITE if timeout is None else max(0, int(timeout * 1000))
        r = _kernel32.WaitForSingleObject(self._handle, ms)
        if r == WAIT_TIMEOUT:
            raise subprocess.TimeoutExpired(self.args, timeout)
        if r != WAIT_OBJECT_0:
            raise _win_error("WaitForSingleObject")
        self._reap()
        return self.returncode

    def kill(self) -> None:
        if self.poll() is None:
            _kernel32.TerminateProcess(self._handle, 1)

    def _reap(self) -> None:
        code = wt.DWORD(0)
        _kernel32.GetExitCodeProcess(self._handle, ctypes.byref(code))
        self.returncode = code.value
        _kernel32.CloseHandle(self._handle)
        self._handle = None


def launch_on_desktop(args: list[str], desktop: str | None, *, show_window: int | None = None,
                      cwd: str | None = None, no_console: bool = False) -> BgProcess:
    """CreateProcessW with `lpDesktop = desktop` ("WinSta\\Desktop"; None: the caller's desktop).
    `show_window`: optional STARTUPINFO.wShowWindow (applies only to the child's first ShowWindow call, per the
    STARTUPINFOW reference page). Handles are not inherited (a GUI child gets no standard streams)."""
    si = STARTUPINFOW()
    si.cb = ctypes.sizeof(si)
    desk_buf = ctypes.create_unicode_buffer(desktop) if desktop else None
    if desk_buf is not None:
        si.lpDesktop = ctypes.cast(desk_buf, wt.LPWSTR)
    if show_window is not None:
        si.dwFlags |= STARTF_USESHOWWINDOW
        si.wShowWindow = show_window
    pi = PROCESS_INFORMATION()
    cmdline = ctypes.create_unicode_buffer(subprocess.list2cmdline([str(a) for a in args]))
    flags = CREATE_UNICODE_ENVIRONMENT | (CREATE_NO_WINDOW if no_console else 0)
    if not _kernel32.CreateProcessW(None, cmdline, None, None, False, flags, None, cwd, ctypes.byref(si),
                                    ctypes.byref(pi)):
        raise _win_error("CreateProcessW")
    return BgProcess(pi, args)


def process_image(pid: int) -> str:
    """Executable file name of `pid` ("" if unavailable)."""
    hp = _kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not hp:
        return ""
    try:
        buf = ctypes.create_unicode_buffer(1024)
        n = wt.DWORD(len(buf))
        if _kernel32.QueryFullProcessImageNameW(hp, 0, buf, ctypes.byref(n)):
            return buf.value.rsplit("\\", 1)[-1]
        return ""
    finally:
        _kernel32.CloseHandle(hp)


class ForegroundSampler:
    """Samples the foreground window every `interval` seconds in a daemon thread.

    `pids` / `images`: what counts as a hit (process ids, or executable names compared case-insensitively).
    `pids` may be extended after start (`watch_pid`). `on_change(entry)`: optional callback for every change of
    foreground window (diagnosis trace). `summary()` returns counts and the list of changes."""

    def __init__(self, *, pids=(), images=(), interval: float = 0.2, on_change=None):
        self.interval = interval
        self._pids = set(pids)
        self._images = {i.lower() for i in images}
        self._on_change = on_change
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._lock = threading.Lock()
        self.samples = 0
        self.hits = 0
        self.null_samples = 0
        self.changes: list[dict] = []
        self.t0 = 0.0

    def watch_pid(self, pid: int) -> None:
        with self._lock:
            self._pids.add(pid)

    def _run(self) -> None:
        last = object()
        image_cache: dict[int, str] = {}
        while not self._stop.is_set():
            hwnd = _user32.GetForegroundWindow()
            pid = 0
            if hwnd:
                p = wt.DWORD(0)
                _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
                pid = p.value
            with self._lock:
                image = image_cache.get(pid)
                if image is None and pid:
                    image = image_cache[pid] = process_image(pid)
                hit = bool(pid) and (pid in self._pids or (image or "").lower() in self._images)
                self.samples += 1
                self.hits += hit
                self.null_samples += not hwnd
                if hwnd != last:
                    entry = {"t": round(time.time() - self.t0, 3), "pid": pid, "image": image or "", "hit": hit}
                    self.changes.append(entry)
                    last = hwnd
                    if self._on_change:
                        self._on_change(entry)
            self._stop.wait(self.interval)

    def start(self) -> "ForegroundSampler":
        self.t0 = time.time()
        self._thread = threading.Thread(target=self._run, name="winbg-fg-sampler", daemon=True)
        self._thread.start()
        return self

    def stop(self) -> dict:
        self._stop.set()
        if self._thread:
            self._thread.join()
        return self.summary()

    def summary(self) -> dict:
        with self._lock:
            return {"interval_s": self.interval, "samples": self.samples, "hits": self.hits,
                    "hit_seconds_approx": round(self.hits * self.interval, 1), "null_samples": self.null_samples,
                    "changes": list(self.changes)}

    def __enter__(self) -> "ForegroundSampler":
        return self.start()

    def __exit__(self, *exc) -> None:
        self.stop()


def _main() -> int:
    import argparse
    import json
    parser = argparse.ArgumentParser(description="Foreground sampler: prints every foreground change.")
    sub = parser.add_subparsers(dest="cmd", required=True)
    w = sub.add_parser("watch", help="sample the foreground window for N seconds")
    w.add_argument("--seconds", type=float, default=30)
    w.add_argument("--interval", type=float, default=0.2)
    w.add_argument("--image", action="append", default=[], help="executable name counted as a hit (repeatable)")
    args = parser.parse_args()
    s = ForegroundSampler(images=args.image, interval=args.interval,
                          on_change=lambda e: print(json.dumps(e), flush=True))
    with s:
        time.sleep(args.seconds)
    summary = s.summary()
    summary.pop("changes")
    print(json.dumps(summary))
    return 1 if summary["hits"] else 0


if __name__ == "__main__":
    sys.exit(_main())
