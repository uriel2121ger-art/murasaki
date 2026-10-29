#!/usr/bin/env python3
"""The pets sweep: every animation item of Anima Engine's Workshop folder and every local pet, measured the way the scene sweeps measure scenes.

  pets_sweep.py [--name NAME] [--prev NAME] [--only ID ...] [--force] [--skip-pixels] [--overlay BIN] [--probe BIN]

Per item (a Workshop folder `anima:<id>`, or a file the user added as `local:<sha1>`):
  * the item check: `murasaki-overlay --check ITEM`, one JSON line {ok, reasons, degraded, engine, version}; the state is refused (ok false),
    degraded (named parts missing), complete, or unchecked (no binary, or the check failed: check_error says why);
  * per animation file, a decode with our own path (`murasaki-petprobe`, the overlay's SpriteDecoder as a stream: the first, a middle and the
    last frame as PNG, the frames it produced and the span of their times) set against an independent reading: the frame count and the delays
    from the file's own chunks (APNG) or Pillow (GIF, WebP), and the three frames decoded by Pillow, compared as premultiplied RGBA;
  * the score of the middle frame against the Workshop preview (the histogram distance of compat/compare.py), when there is a preview.
Output: ~/.cache/murasaki-tests/compat/anima-NAME/<item>/result.json, plus results.json and report.md (and, with --prev, the diff against the
earlier run: states, reasons by count, what got worse, previews closer / tie / further). Resumable: an item with a result.json is skipped
(--force redoes it). The GPU lock (~/.cache/murasaki-tests/bench.lock) is taken for one item at a time, and everything runs at nice 19.
Big files are streamed (a 300 MB APNG of 868 frames costs the memory of a couple of frames); nothing is ever run from an item.
"""
import argparse
import hashlib
import json
import os
import resource
import signal
import subprocess
import sys
import threading
import time
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from steamlib import CACHE_ROOT, scene_lock, steam_roots  # noqa: E402

ANIMA_APP = "3474900"
ANIMATION_SUFFIXES = {".apng", ".gif", ".webp"}  # and a .png that is an APNG (is_animated_png): Anima Engine plays it
NOT_PLAYED_SUFFIXES = {".mdaa", ".zip", ".mp4", ".webm", ".png", ".piskel", ".moc3"}  # named, not decoded (an item may carry them)
PIXEL_TOLERANCE = 4         # largest difference of a premultiplied channel between our decode and Pillow's
DURATION_TOLERANCE_MS = 2.0  # and a share of the total, whichever is larger
DURATION_TOLERANCE_SHARE = 0.005
PREVIEW_STEP = 0.02         # a change of the preview score that is closer or further, as in the scene sweeps
HOME = Path.home()


# ---- what the files say, read independently of the engine ----
# (pets_reader.py holds the readers; they run in a process of their own, see run_reader)
from pets_reader import apng_header, file_header, pillow_frames, pillow_header, pixel_difference, spec_frames  # noqa: E402,F401  (the tests use them directly)

READER = Path(__file__).resolve().parent / "pets_reader.py"


def run_reader(args, op, *params):
    """Runs one of pets_reader.py's operations in `python3 -I` with a time limit (--reader-timeout) and an address-space limit
    (--reader-memory-mb, RLIMIT_AS): a file that makes Pillow loop or balloon costs that process, not the sweep. The result is the reader's JSON
    object, or {"error": ...} (a timeout, a crash, no answer)."""
    def limit():
        cap = int(args.reader_memory_mb) * 1024 * 1024
        resource.setrlimit(resource.RLIMIT_AS, (cap, cap))
        os.nice(19)

    cmd = [sys.executable, "-I", str(getattr(args, "reader", None) or READER), op, *[str(p) for p in params]]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=args.reader_timeout, preexec_fn=limit)
    except subprocess.TimeoutExpired:
        return {"error": f"the independent reader took over {args.reader_timeout:g}s and was stopped"}
    result = last_json_line(proc.stdout)
    if result is None:
        tail = proc.stderr.strip().splitlines()[-1:] or [""]
        return {"error": f"the independent reader died (exit status {proc.returncode}): {tail[0][:100]}"}
    return result


# ---- the engine's side ----

def run_measured(cmd, timeout, memory_mb=0):
    """Runs a command, returns (returncode, stdout, peak RSS in MB, wall seconds); the peak is the child's own (wait4's rusage). A command
    that takes longer than `timeout` is killed and its returncode is None."""
    started = time.monotonic()
    def limit():
        if memory_mb:
            cap = int(memory_mb) * 1024 * 1024
            resource.setrlimit(resource.RLIMIT_AS, (cap, cap))

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, preexec_fn=limit)
    timer = threading.Timer(timeout, proc.kill)
    timer.start()
    try:
        out = proc.stdout.read()
        _, status, usage = os.wait4(proc.pid, 0)
        proc.returncode = os.waitstatus_to_exitcode(status)
    finally:
        timer.cancel()
        proc.stdout.close()
    wall = round(time.monotonic() - started, 2)
    if proc.returncode == -signal.SIGKILL and wall >= timeout:
        return None, out, 0.0, wall
    return proc.returncode, out, round(usage.ru_maxrss / 1024.0, 1), wall


def last_json_line(text):
    for line in reversed(text.splitlines()):
        try:
            value = json.loads(line)
        except ValueError:
            continue
        if isinstance(value, dict):
            return value
    return None


def run_check(args, item_arg):
    """`murasaki-overlay --check ITEM`: (verdict, error). The verdict is {ok, reasons, degraded, engine, version}; nothing is run when the
    binary is missing (the sweep then measures the decode only and the state is unchecked)."""
    if not args.overlay or not Path(args.overlay).is_file():
        return None, f"no overlay binary at {args.overlay}"
    try:
        code, out, _, _ = run_measured(["nice", "-n", "19", str(args.overlay), "--check", str(item_arg)], args.check_timeout, args.child_memory_mb)
    except OSError as error:
        return None, f"cannot run: {error.strerror or error}"
    if code is None:
        return None, f"timeout after {args.check_timeout:g}s"
    verdict = last_json_line(out)
    if not verdict or "ok" not in verdict:
        return None, f"no verdict on stdout (exit status {code})"
    return {"ok": bool(verdict["ok"]), "reasons": [str(r) for r in verdict.get("reasons", [])], "degraded": [str(r) for r in verdict.get("degraded", [])],
            "engine": verdict.get("engine"), "version": verdict.get("version")}, None


def state_of(verdict):
    if verdict is None:
        return "unchecked"
    if not verdict["ok"]:
        return "refused"
    return "degraded" if verdict["degraded"] else "complete"


def run_probe(args, path, out_dir):
    """Our own decode of one file: {ok, frames, promised, duration_ms, width, height, picked{first,middle,last: {index, file}}}, rss_mb, wall_s."""
    out_dir.mkdir(parents=True, exist_ok=True)
    if not args.probe or not Path(args.probe).is_file():
        return None, f"no probe binary at {args.probe}", 0.0, 0.0
    code, out, rss, wall = run_measured(["nice", "-n", "19", str(args.probe), str(path), str(out_dir)], args.probe_timeout, args.child_memory_mb)
    if code is None:
        return None, f"timeout after {args.probe_timeout:g}s", rss, wall
    result = last_json_line(out)
    if result is None:
        return None, f"no result on stdout (exit status {code})", rss, wall
    return result, None, rss, wall


def preview_for(args, item):
    for candidate in sorted(item["dir"].glob("preview.*")) if item.get("dir") and item["dir"].is_dir() else []:
        if candidate.suffix.lower() in (".jpg", ".jpeg", ".png", ".gif", ".webp"):
            return candidate
    for suffix in (".png", ".jpg"):
        candidate = Path(args.previews) / (item["id"].split(":", 1)[1] + suffix)
        if candidate.is_file():
            return candidate
    return None


def preview_score(frame_png, preview):
    from compare import chi_square, hsv_hist, load_rgb
    from PIL import Image

    with Image.open(frame_png) as frame:
        flat = Image.new("RGBA", frame.size, (0, 0, 0, 255))
        flat.alpha_composite(frame.convert("RGBA"))
    return round(chi_square(hsv_hist(flat.convert("RGB")), hsv_hist(load_rgb(preview))), 4)


# ---- the items ----

def workshop_dirs(args):
    roots = [Path(args.workshop)] if args.workshop else [r / "steamapps/workshop/content" / ANIMA_APP for r in steam_roots()]
    seen = set()
    for base in roots:
        if not base.is_dir():
            continue
        for d in sorted(base.iterdir(), key=lambda p: p.name):
            if d.is_dir() and d.name not in seen:
                seen.add(d.name)
                yield d


def is_animation_by_content(args, path):
    """A `.png` that is an APNG (acTL before the first IDAT), asked of the isolated reader."""
    return path.suffix.lower() == ".png" and bool(run_reader(args, "animated", path).get("animated"))


def list_items(args):
    """[{key, id, kind, dir, files, skipped}] for the Workshop items and the local pets (config dir's local-pets.json)."""
    items = []
    for d in workshop_dirs(args):
        files = sorted(p for p in d.iterdir() if p.is_file() and (p.suffix.lower() in ANIMATION_SUFFIXES or is_animation_by_content(args, p)))
        skipped = sorted(p.name for p in d.iterdir() if p.is_file() and p.suffix.lower() in NOT_PLAYED_SUFFIXES and p not in files)
        items.append({"key": f"anima_{d.name}", "id": f"anima:{d.name}", "kind": "anima", "dir": d, "files": files, "skipped": skipped})
    store = Path(args.config) / "local-pets.json" if args.config else HOME / ".config/murasaki/local-pets.json"
    if store.is_file():
        try:
            listed = json.loads(store.read_text()).get("files", [])
        except (OSError, ValueError):
            listed = []
        for name in listed:
            p = Path(os.path.abspath(os.path.expanduser(name)))  # absolute: a name that starts with "-" never reaches a command as an option
            digest = hashlib.sha1(str(p).encode()).hexdigest()
            items.append({"key": f"local_{digest}", "id": f"local:{digest}", "kind": "local", "dir": p.parent, "files": [p] if p.suffix.lower() in ANIMATION_SUFFIXES else [],
                          "skipped": [] if p.suffix.lower() in ANIMATION_SUFFIXES else [p.name], "path": str(p)})
    if args.only:
        items = [i for i in items if i["id"] in args.only or i["key"] in args.only or i["id"].split(":", 1)[1] in args.only]
    return items


def measure_file(args, item, path, scratch):
    """Everything the sweep records of one animation file."""
    record = {"name": path.name, "bytes": path.stat().st_size}
    header = run_reader(args, "header", path)
    record["header"] = header
    notes = []
    probe, error, rss, wall = run_probe(args, path, scratch / path.stem)
    record["rss_mb"], record["wall_s"] = rss, wall
    problems = []
    if error or not probe or not probe.get("ok"):
        record["probe_error"] = error or (probe or {}).get("error", "the probe failed")
        problems.append(f"our decode failed: {record['probe_error']}")
        record["problems"] = problems
        return record
    record["probe"] = {k: probe.get(k) for k in ("frames", "promised", "duration_ms", "width", "height")}
    if "error" in header:
        problems.append(f"the independent reader failed: {header['error']}")
    else:
        record["frames_match"] = probe["frames"] == header["frames"]
        if not record["frames_match"]:
            problems.append(f"frames: we decode {probe['frames']}, the file says {header['frames']}")
        if probe.get("promised") and probe["promised"] != probe["frames"]:
            # FFmpeg's count before decoding is an estimate (a GIF with a second stream appended is counted up to the second header only): it
            # is a problem when the independent reader sides with it, a note when the reader and the decode agree
            message = f"the container header promises {probe['promised']} frames and the decode gives {probe['frames']}"
            if header["frames"] == probe["frames"]:
                notes.append(message + f" (the file's own count, {header['frames']}, agrees with the decode: the promise is FFmpeg's estimate)")
            else:
                problems.append(message)
        if header.get("tiny_delays"):
            # not a defect: one rule for the three formats (docs/pets/timing.md)
            record["delay_rule"] = {"tiny_frames": header["tiny_delays"], "raw_duration_ms": round(header.get("raw_duration_ms", 0), 2),
                                    "played_duration_ms": round(header["duration_ms"], 2)}
            notes.append(f"{header['tiny_delays']} of {header['frames']} frames carry a delay of 10 ms or less and play for 100 ms (the pets' rule, the same for GIF, "
                         f"APNG and WebP); the file's delays add up to {header.get('raw_duration_ms', 0):.1f} ms, the animation plays {header['duration_ms']:.1f} ms")
        tolerance = max(DURATION_TOLERANCE_MS, DURATION_TOLERANCE_SHARE * header["duration_ms"])
        record["duration_diff_ms"] = round(probe["duration_ms"] - header["duration_ms"], 2)
        record["duration_match"] = abs(record["duration_diff_ms"]) <= tolerance
        if not record["duration_match"]:
            problems.append(f"duration: we get {probe['duration_ms']:.1f} ms, the file's delays sum to {header['duration_ms']:.1f} ms (after the tiny-delay rule)")
        if (probe["width"], probe["height"]) != (header["width"], header["height"]):
            problems.append(f"canvas: we decode {probe['width']}x{probe['height']}, the file says {header['width']}x{header['height']}")
        problems.extend(f"the file: {p}" for p in header.get("problems", []))
        if header.get("declared_frames") is not None and header["declared_frames"] != header["frames"]:
            problems.append(f"the file declares {header['declared_frames']} frames (acTL) and holds {header['frames']} (fcTL)")
    if probe.get("resized_frames"):
        problems.append(f"{probe['resized_frames']} frames are not the stream's size (first: {probe.get('other_size')}) and were scaled to it")
    picked = probe.get("picked", {})
    if not args.skip_pixels and picked and "error" not in header:
        compared = run_reader(args, "compare", path, json.dumps({role: {"index": e["index"], "file": e.get("file")} for role, e in picked.items()}))
        if "error" in compared:
            problems.append(f"the independent decode failed: {compared['error']}")
        else:
            pixels = compared["pixels"]
            for role, p in pixels.items():
                if p["max"] is None:
                    problems.append(f"{role} frame: the sizes differ")
                elif p["max"] > PIXEL_TOLERANCE and p.get("pillow_wrong"):
                    # a third opinion, a hand compositor of the specification, agrees with us and not with Pillow: Pillow's APNG disposal is what differs
                    notes.append(f"{role} frame {p['index']}: Pillow's decode differs from ours by {p['max']}; ours matches a hand compositing by the APNG "
                                 f"specification (largest difference {p['spec_max']}), so Pillow's disposal is what is off")
                elif p["max"] > PIXEL_TOLERANCE:
                    spec = (f", {p['spec_max']} from the specification's" if p.get("spec_max") is not None
                            else f", no third opinion ({p['spec_error']})" if p.get("spec_error") else "")
                    problems.append(f"{role} frame {p['index']}: largest channel difference {p['max']} from Pillow's (mean {p['mean']}){spec}")
            record["pixels"] = pixels
            record["pixels_ok"] = all(p["max"] is not None and (p["max"] <= PIXEL_TOLERANCE or p.get("pillow_wrong")) for p in pixels.values())
    preview = preview_for(args, item)
    middle = picked.get("middle") or picked.get("first")
    if preview and middle and middle.get("file"):
        try:
            record["preview_score"] = preview_score(middle["file"], preview)
        except Exception as error:
            record["preview_error"] = f"{type(error).__name__}: {str(error)[:100]}"
    record["problems"] = problems
    if notes:
        record["notes"] = notes
    return record


def measure_item(args, item, out_dir):
    result = {"key": item["key"], "id": item["id"], "kind": item["kind"], "files": [], "skipped": item["skipped"]}
    arg = item.get("path") or item["dir"]
    verdict, error = run_check(args, arg)
    result["check"] = verdict
    if error:
        result["check_error"] = error
    result["state"] = state_of(verdict)
    if not item["files"]:
        result["problems"] = ["no animation file in the item" + (f" (skipped: {', '.join(item['skipped'])})" if item["skipped"] else "")]
    for path in item["files"]:
        result["files"].append(measure_file(args, item, path, out_dir / "frames"))
    problems = [f"{f['name']}: {p}" for f in result["files"] for p in f.get("problems", [])]
    result.setdefault("problems", [])
    result["problems"] += problems
    result["decode_ok"] = bool(result["files"]) and not problems
    result["rss_mb"] = max([f.get("rss_mb", 0) for f in result["files"]] or [0])
    result["wall_s"] = round(sum(f.get("wall_s", 0) for f in result["files"]), 2)
    return result


def write_atomically(path, text):
    """Writes next to the target and renames over it: a sweep stopped half way leaves no half-written result.json."""
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(text)
    os.replace(temporary, path)


def run_sweep(args):
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    items = list_items(args)
    print(f"{len(items)} items, out={out}", flush=True)
    results = []
    for n, item in enumerate(items, 1):
        item_dir = out / item["key"]
        stored = item_dir / "result.json"
        if stored.is_file() and not args.force:
            try:
                results.append(json.loads(stored.read_text()))
                continue
            except ValueError:
                print(f"[{n}/{len(items)}] {item['id']}: the stored result does not parse, done again", flush=True)
        item_dir.mkdir(parents=True, exist_ok=True)
        try:
            with scene_lock(args):
                result = measure_item(args, item, item_dir)
        except TimeoutError as error:
            print(f"[{n}/{len(items)}] {item['id']}: {error}, left for the next run", flush=True)
            continue
        write_atomically(stored, json.dumps(result, indent=1))
        results.append(result)
        print(f"[{n}/{len(items)}] {item['id']} {result['state']} decode {'ok' if result['decode_ok'] else 'PROBLEMS'} rss {result['rss_mb']} MB "
              f"{result['wall_s']} s", flush=True)
    return results


# ---- the report and the diff ----

def summarize(results):
    states = Counter(r["state"] for r in results)
    reasons = Counter()
    for r in results:
        if r.get("check"):
            reasons.update(r["check"]["degraded"])
            reasons.update(f"refused: {x}" for x in r["check"]["reasons"])
    return {"items": len(results), "states": dict(states), "decode_ok": sum(1 for r in results if r["decode_ok"]),
            "decode_problems": [r["id"] for r in results if not r["decode_ok"]], "reasons": dict(reasons)}


def compare_runs(prev, cur):
    """What changed from `prev` to `cur` (lists of result dicts): states, reasons by count, what got worse, previews closer / tie / further."""
    before = {r["id"]: r for r in prev}
    rank = {"complete": 0, "degraded": 1, "unchecked": 1, "refused": 2}
    worse, moves, previews = [], Counter(), Counter()
    for r in cur:
        b = before.get(r["id"])
        if b is None:
            continue
        if b["state"] != r["state"]:
            moves[f"{b['state']} -> {r['state']}"] += 1
        why = []
        if rank.get(r["state"], 1) > rank.get(b["state"], 1):
            why.append(f"{b['state']} before, {r['state']} now")
        new = sorted(set((r.get("check") or {}).get("degraded", [])) - set((b.get("check") or {}).get("degraded", [])))
        if new and r["state"] != "refused" and b["state"] != "refused":  # (an item that was refused and plays now has its first reasons, not new ones)
            why.append("new reasons: " + ", ".join(new))
        if b["decode_ok"] and not r["decode_ok"]:
            why.append("the decode agreed with the independent one before and does not now: " + "; ".join(r["problems"][:2]))
        bf = {f["name"]: f for f in b["files"]}
        for f in r["files"]:
            old = bf.get(f["name"])
            if not old:
                continue
            if old.get("probe") and f.get("probe") and old["probe"]["frames"] != f["probe"]["frames"]:
                why.append(f"{f['name']}: {old['probe']['frames']} frames before, {f['probe']['frames']} now")
            if old.get("rss_mb") and f.get("rss_mb") and f["rss_mb"] > old["rss_mb"] * 1.25 and f["rss_mb"] - old["rss_mb"] > 10:
                why.append(f"{f['name']}: peak RSS {old['rss_mb']} -> {f['rss_mb']} MB")
            if old.get("wall_s") and f.get("wall_s") and f["wall_s"] > old["wall_s"] * 1.5 and f["wall_s"] - old["wall_s"] > 1.0:
                why.append(f"{f['name']}: decode {old['wall_s']} -> {f['wall_s']} s")
            if old.get("preview_score") is not None and f.get("preview_score") is not None:
                delta = f["preview_score"] - old["preview_score"]
                previews["closer" if delta <= -PREVIEW_STEP + 1e-9 else "further" if delta >= PREVIEW_STEP - 1e-9 else "tie"] += 1
        if why:
            worse.append({"id": r["id"], "why": why})
    return {"moves": dict(moves), "worse": worse, "previews": dict(previews),
            "new_items": [r["id"] for r in cur if r["id"] not in before], "gone_items": [i for i in before if i not in {r["id"] for r in cur}]}


def write_report(out, results, diff=None, prev_name=None):
    summary = summarize(results)
    md = [f"# Pets sweep {out.name}\n\n{summary['items']} items.\n"]
    md.append("\n## States\n\n| state | items |\n|---|---:|\n" + "".join(f"| {s} | {n} |\n" for s, n in sorted(summary["states"].items())))
    md.append(f"\nDecode against the independent reader: {summary['decode_ok']} of {summary['items']} agree.\n")
    if summary["reasons"]:
        md.append("\n## Reasons (items)\n\n| reason | items |\n|---|---:|\n" + "".join(f"| {r} | {n} |\n" for r, n in sorted(summary["reasons"].items(), key=lambda x: -x[1])))
    md.append("\n## Items\n\n| item | state | files | frames | duration s | canvas | decode | peak RSS MB | decode s | preview |\n|---|---|---:|---:|---:|---|---|---:|---:|---:|\n")
    for r in results:
        frames = sum((f.get("probe") or {}).get("frames", 0) for f in r["files"])
        seconds = sum((f.get("probe") or {}).get("duration_ms", 0) for f in r["files"]) / 1000.0
        canvas = ", ".join(sorted({f"{p['width']}x{p['height']}" for f in r["files"] if (p := f.get("probe")) and "width" in p}))
        scores = [f["preview_score"] for f in r["files"] if f.get("preview_score") is not None]
        md.append(f"| {r['id']} | {r['state']} | {len(r['files'])} | {frames} | {seconds:.1f} | {canvas} | {'ok' if r['decode_ok'] else 'PROBLEMS'} | {r['rss_mb']} | {r['wall_s']} | "
                  f"{min(scores) if scores else ''} |\n")
    bad = [r for r in results if r["problems"]]
    if bad:
        md.append("\n## Problems\n\n" + "".join(f"- {r['id']}: " + "; ".join(r["problems"][:4]) + "\n" for r in bad))
    if diff is not None:
        md.append(f"\n## Against {prev_name}\n\n")
        md.append("Moves between states: " + (", ".join(f"{k}: {v}" for k, v in sorted(diff["moves"].items())) or "none") + "\n")
        md.append("\nPreviews (files whose score was in both runs): " + (", ".join(f"{v} {k}" for k, v in sorted(diff["previews"].items())) or "none") + "\n")
        if diff["new_items"] or diff["gone_items"]:
            md.append(f"\nNew items: {', '.join(diff['new_items']) or 'none'}. Items gone: {', '.join(diff['gone_items']) or 'none'}.\n")
        md.append(f"\n### Worse ({len(diff['worse'])})\n\n" + ("".join(f"- {w['id']}: " + "; ".join(w["why"]) + "\n" for w in diff["worse"]) or "none\n"))
    write_atomically(out / "report.md", "".join(md))
    write_atomically(out / "results.json", json.dumps({"summary": summary, "diff": diff}, indent=1))
    return "".join(md)


def build_parser():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", default="sweep", help="the run's name: output goes to compat/anima-NAME")
    ap.add_argument("--prev", help="the name of an earlier run to compare with (compat/anima-PREV)")
    ap.add_argument("--only", nargs="*", help="only these items (anima:ID, local:HASH, or the bare id)")
    ap.add_argument("--force", action="store_true", help="redo the items that have a result")
    ap.add_argument("--skip-pixels", action="store_true", help="skip the comparison with Pillow's decode (the headers and the check only)")
    ap.add_argument("--overlay", default=os.environ.get("MURASAKI_OVERLAY", str(Path(__file__).resolve().parents[2] / "build/dev/src/overlay/murasaki-overlay")))
    ap.add_argument("--probe", default=os.environ.get("MURASAKI_PETPROBE", str(Path(__file__).resolve().parents[2] / "build/dev/src/overlay/murasaki-petprobe")))
    ap.add_argument("--workshop", help="the Anima Workshop content folder (default: from the Steam libraries)")
    ap.add_argument("--config", help="the folder of local-pets.json (default ~/.config/murasaki)")
    ap.add_argument("--previews", default=str(HOME / ".cache/murasaki/previews"), help="where the daemon caches Workshop previews (<id>.png)")
    ap.add_argument("--cache-root", type=Path, default=CACHE_ROOT)
    ap.add_argument("--lock", type=Path, default=HOME / ".cache/murasaki-tests/bench.lock")
    ap.add_argument("--lock-wait", type=float, default=580)
    ap.add_argument("--check-timeout", type=float, default=120)
    ap.add_argument("--probe-timeout", type=float, default=600)
    ap.add_argument("--child-memory-mb", type=int, default=4096, help="address space the check and the probe may use (RLIMIT_AS; 0: no limit)")
    ap.add_argument("--reader-timeout", type=float, default=300, help="seconds the independent reader may take on one file before it is stopped")
    ap.add_argument("--reader-memory-mb", type=int, default=4096, help="address space the independent reader may use (RLIMIT_AS)")
    ap.add_argument("--reader", help="the reader script (default pets_reader.py next to this one; the tests put a slow one here)")
    return ap


def parse(argv=None):
    args = build_parser().parse_args(argv)
    args.out = args.cache_root / f"anima-{args.name}"
    return args


def main(argv=None):
    args = parse(argv)
    results = run_sweep(args)
    diff, prev_name = None, None
    if args.prev:
        prev_dir = args.cache_root / f"anima-{args.prev}"
        prev = []
        for stored in sorted(prev_dir.glob("*/result.json")):
            try:
                prev.append(json.loads(stored.read_text()))
            except ValueError:
                print(f"{stored}: does not parse, left out of the comparison", flush=True)
        diff, prev_name = compare_runs(prev, results), f"anima-{args.prev}"
    print(write_report(args.out, results, diff, prev_name))


if __name__ == "__main__":
    main()
