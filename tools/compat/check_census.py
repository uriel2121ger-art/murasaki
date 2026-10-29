#!/usr/bin/env python3
"""Run the Vulkan engine's --check on every installed Wallpaper Engine scene and fail on a refusal nobody accepted.

A scene the check refuses is drawn incompletely, so "universal" is measured here, on the verdict the product uses.

The accepted refusals live in check-census-allowed.tsv next to this script (id <TAB> reason prefix, '#' comments): a
refusal is accepted when its scene is listed with a prefix of that reason. Anything else (a new refusal, a check that
crashed or timed out) is a failure. Listed scenes that now pass are reported so the file can shrink.

Exit: 0 all refusals accepted, 1 a refusal not accepted, 77 skipped (no library or engine, or MURASAKI_NESTED_TESTS
unset when --ctest is given).
"""
import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import steamlib  # noqa: E402

ALLOWED = Path(__file__).resolve().parent / "check-census-allowed.tsv"


def read_allowed(path):
    allowed = {}
    if path.is_file():
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if not line.strip() or line.startswith("#"):
                continue
            sid, _, reason = line.partition("\t")
            # (an empty prefix is a prefix of every reason: it would accept whatever the scene is refused for)
            if not sid.strip() or not reason.strip():
                sys.exit(f"{path.name}:{number}: a line needs an id and a reason prefix, separated by a tab")
            allowed.setdefault(sid.strip(), []).append(reason.strip())
    return allowed


def check(engine, scene_dir, assets, timeout):
    """The engine's verdict: {"ok": bool, "reasons": [...], "degraded": [...]}, or ok None with the failure as reason."""
    try:
        p = subprocess.run([str(engine), "--check", str(scene_dir), "--assets-dir", str(assets)],
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {"ok": None, "reasons": [f"check timed out after {timeout:.0f} s"]}
    lines = p.stdout.decode("utf-8", "replace").strip().splitlines()
    try:
        return json.loads(lines[-1])
    except (IndexError, ValueError):
        return {"ok": None, "reasons": [f"check exited {p.returncode} without a verdict"]}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--engine", type=Path, default=Path.home() / ".local/bin/murasaki-scene-vk")
    ap.add_argument("--allowed", type=Path, default=ALLOWED)
    ap.add_argument("--timeout", type=float, default=120)
    ap.add_argument("--only", nargs="*", help="restrict to these item ids")
    ap.add_argument("--out", type=Path, help="write every verdict here (id <TAB> json)")
    ap.add_argument("--ctest", action="store_true", help="skip (77) unless MURASAKI_NESTED_TESTS=1")
    args = ap.parse_args()

    if args.ctest and os.environ.get("MURASAKI_NESTED_TESTS") != "1":
        print("check_census: skipped (set MURASAKI_NESTED_TESTS=1 to check the installed library)")
        return 77
    roots = steamlib.steam_roots()
    assets = steamlib.find_assets(roots)
    scenes = steamlib.list_scenes(roots)
    if args.only:
        scenes = [(sid, d) for sid, d in scenes if sid in set(args.only)]
    if not assets or not scenes or not args.engine.is_file():
        print(f"check_census: skipped (assets {assets}, {len(scenes)} scenes, engine {args.engine})")
        return 77

    allowed = read_allowed(args.allowed)
    started = time.monotonic()
    ok = degraded = 0
    accepted, failures, cleared = [], [], []
    out = args.out.open("w", encoding="utf-8") if args.out else None
    for sid, d in scenes:
        v = check(args.engine, d, assets, args.timeout)
        if out:
            out.write(f"{sid}\t{json.dumps(v, ensure_ascii=False)}\n")
        if v.get("ok") is True:
            ok += 1
            degraded += bool(v.get("degraded"))
            if sid in allowed:
                cleared.append(sid)
            continue
        reasons = v.get("reasons") or ["refused without a reason"]
        prefixes = allowed.get(sid, [])
        new = [r for r in reasons if v.get("ok") is None or not any(r.startswith(p) for p in prefixes)]
        (failures if new else accepted).append((sid, new or reasons))
    if out:
        out.close()

    total = len(scenes)
    print(f"check_census: {total} scenes in {time.monotonic() - started:.0f} s: {ok} ok ({degraded} degraded), "
          f"{len(accepted)} refused as accepted, {len(failures)} refused NOT accepted")
    for sid, reasons in accepted:
        print(f"  accepted {sid}: {'; '.join(reasons)}")
    for sid in cleared:
        print(f"  {sid} passes now: remove it from {args.allowed.name}")
    for sid, reasons in failures:
        print(f"  FAIL {sid}: {'; '.join(reasons)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
