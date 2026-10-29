"""The Steam side of the sweeps: where the libraries, the Wallpaper Engine assets and the scene items are, and the GPU lock a
sweep takes per scene. Shared by run.py, check_census.py and pets_sweep.py."""
import contextlib
import fcntl
import json
import os
import re
import time
from pathlib import Path

HOME = Path.home()
WORKSHOP_APP = "431960"
CACHE_ROOT = HOME / ".cache/murasaki-tests/compat"


def steam_roots():
    env = os.environ.get("MURASAKI_STEAM_ROOT")
    cands = [env] if env else [
        HOME / ".steam/root", HOME / ".steam/steam", HOME / ".local/share/Steam",
        HOME / ".steam/debian-installation",
        HOME / ".var/app/com.valvesoftware.Steam/.local/share/Steam",
        HOME / "snap/steam/common/.local/share/Steam",
    ]
    roots = []
    for c in cands:
        p = Path(c).resolve()
        if p.is_dir() and p not in roots:
            roots.append(p)
        vdf = p / "steamapps/libraryfolders.vdf"
        if vdf.is_file():
            for m in re.finditer(r'"path"\s+"([^"]+)"', vdf.read_text(errors="replace")):
                q = Path(m.group(1)).resolve()
                if q.is_dir() and q not in roots:
                    roots.append(q)
    return roots


def find_assets(roots):
    for r in roots:
        a = r / "steamapps/common/wallpaper_engine/assets"
        if a.is_dir():
            return a
    return None


def list_scenes(roots):
    """Return [(id, dir)] of workshop items whose project.json type is scene."""
    seen, items = set(), []
    for r in roots:
        base = r / "steamapps/workshop/content" / WORKSHOP_APP
        if not base.is_dir():
            continue
        for d in sorted(base.iterdir(), key=lambda p: p.name):
            pj = d / "project.json"
            if d.name in seen or not pj.is_file():
                continue
            try:
                data = json.loads(pj.read_text(encoding="utf-8-sig"))
            except (OSError, ValueError):
                continue
            if str(data.get("type", "")).lower() == "scene":
                seen.add(d.name)
                items.append((d.name, d))
    return items


@contextlib.contextmanager
def scene_lock(args):
    """Holds the shared GPU lock (`flock -x`, the same file the ctest, snap and --check take) for one scene: its check and its run,
    not the sweep. Waits at most --lock-wait seconds (then the scene fails with a timeout, to be resumed). The clocks of the check and of
    the run start after it is held. Without --lock nothing is taken."""
    path = getattr(args, "lock", None)
    if not path:
        yield
        return
    with open(path, "a") as handle:
        deadline = time.monotonic() + args.lock_wait
        while True:
            try:
                fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() > deadline:
                    raise TimeoutError(f"lock {path} not free after {args.lock_wait:g}s")
                time.sleep(0.5)
        try:
            yield
        finally:
            fcntl.flock(handle, fcntl.LOCK_UN)
