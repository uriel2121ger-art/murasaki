#!/usr/bin/env python3
"""Cross-checks the geometry of a stage (src/overlay/stagegeometry.cpp, docs/pets/stage.md) against an independent implementation.

The reference below is written from the rules in the document, in exact rational arithmetic (fractions), not from the C++: the fitted picture's four edges are rounded
once to whole logical pixels (half away from zero), and each output shows the part of that rectangle on it, as a normalised source rectangle. The same layouts are
given to `murasaki-petprobe --stage` and the answers compared: the picture and the slices exactly (whole numbers), the source rectangles to 1e-12.

    stage_geometry_check.py --probe build/ci/src/overlay/murasaki-petprobe [--seed N] [--cases N]

Exit status 0: every case agrees; 1: a disagreement (printed); 77: no probe to run. A case where an edge falls within 1e-9 of a half pixel is skipped (the C++ works in
doubles, where the rounding of such an edge is not defined by the rules).
"""
import argparse
import json
import math
import os
import random
import subprocess
import sys
import tempfile
from fractions import Fraction as F

MAX_SIDE = 16384
MAX_OUTPUTS = 8


def round_half_away(x):
    """Rounds a Fraction to the nearest whole number, halves away from zero; also says whether x is within 1e-9 of a half."""
    ambiguous = abs(abs(x - math.floor(x)) - F(1, 2)) < F(1, 10**9)
    n = math.floor(abs(x) + F(1, 2))
    return (n if x >= 0 else -n), ambiguous


def fit(target, content, how):
    """The size `content` (w, h) takes in `target` (w, h): contain, cover or stretch."""
    tw, th = target
    cw, ch = content
    if how == "stretch":
        return tw, th
    sx, sy = F(tw) / cw, F(th) / ch
    s = min(sx, sy) if how == "contain" else max(sx, sy)
    return cw * s, ch * s


def reference(outputs, mode, output, footprint, how):
    """-> (picture [x, y, w, h], slices [{output, global, window, source}], skipped) for the layout, or None when there is no stage."""
    fw, fh = F(footprint[0]).limit_denominator(10**12), F(footprint[1]).limit_denominator(10**12)
    if fw <= 0 or fh <= 0:
        return None
    usable = [o for o in outputs if o["width"] > 0 and o["height"] > 0 and o["width"] <= MAX_SIDE and o["height"] <= MAX_SIDE]
    if mode == "output":
        covered = [o for o in usable if o["name"] == output][:1]
    else:
        covered, box = [], None
        for o in usable:
            grown = (o["x"], o["y"], o["x"] + o["width"], o["y"] + o["height"])
            if box:
                grown = (min(box[0], grown[0]), min(box[1], grown[1]), max(box[2], grown[2]), max(box[3], grown[3]))
            if len(covered) >= MAX_OUTPUTS or grown[2] - grown[0] > MAX_SIDE or grown[3] - grown[1] > MAX_SIDE:
                continue
            box = grown
            covered.append(o)
    if not covered:
        return None
    x0 = min(o["x"] for o in covered)
    y0 = min(o["y"] for o in covered)
    x1 = max(o["x"] + o["width"] for o in covered)
    y1 = max(o["y"] + o["height"] for o in covered)
    pw, ph = fit((x1 - x0, y1 - y0), (fw, fh), how)
    left = x0 + (F(x1 - x0) - pw) / 2
    top = y0 + (F(y1 - y0) - ph) / 2
    skipped = False
    edges = []
    for value in (left, top, left + pw, top + ph):
        n, amb = round_half_away(value)
        skipped = skipped or amb
        edges.append(n)
    left, top, right, bottom = edges
    right, bottom = max(right, left + 1), max(bottom, top + 1)
    picture = [left, top, right - left, bottom - top]
    slices = []
    for o in covered:
        gx0, gy0 = max(o["x"], left), max(o["y"], top)
        gx1, gy1 = min(o["x"] + o["width"], right), min(o["y"] + o["height"], bottom)
        if gx1 <= gx0 or gy1 <= gy0:
            continue
        slices.append({
            "output": o["name"],
            "global": [gx0, gy0, gx1 - gx0, gy1 - gy0],
            "window": [gx0 - o["x"], gy0 - o["y"], gx1 - gx0, gy1 - gy0],
            "source": [F(gx0 - left, picture[2]), F(gy0 - top, picture[3]), F(gx1 - gx0, picture[2]), F(gy1 - gy0, picture[3])],
        })
    return picture, slices, skipped


def random_layout(rng):
    """A layout of one to four outputs along a row with a possible gap and a vertical offset, in the shapes real desks have."""
    scales = [1.0, 1.25, 1.5, 2.0]
    outputs, x = [], rng.randint(-3000, 0)
    for i in range(rng.randint(1, 4)):
        w, h = rng.randint(200, 2600), rng.randint(200, 1600)
        outputs.append({"name": f"O{i}", "x": x, "y": rng.randint(-300, 300), "width": w, "height": h, "scale": rng.choice(scales)})
        x += w + (rng.randint(1, 80) if rng.randint(0, 3) == 0 else 0)
    return outputs


def cases(rng, count):
    for _ in range(count):
        outputs = random_layout(rng)
        footprint = (rng.randint(1, 4000), rng.randint(1, 4000))
        for how in ("contain", "cover", "stretch"):
            yield {"outputs": outputs, "mode": "desktop", "output": "", "footprint": list(footprint), "fit": how}
            yield {"outputs": outputs, "mode": "output", "output": rng.choice(outputs)["name"], "footprint": list(footprint), "fit": how}
    # corners: a nine-output desk, a picture that rounds to a pixel, an L shape with negative coordinates
    nine = [{"name": f"N{i}", "x": i * 100, "y": 0, "width": 100, "height": 100, "scale": 1.0} for i in range(9)]
    yield {"outputs": nine, "mode": "desktop", "output": "", "footprint": [900, 100], "fit": "stretch"}
    yield {"outputs": nine[:1], "mode": "output", "output": "N0", "footprint": [100000, 1], "fit": "contain"}
    ell = [{"name": "A", "x": -1920, "y": 0, "width": 1920, "height": 1080, "scale": 1.0},
           {"name": "B", "x": 0, "y": 0, "width": 2560, "height": 1440, "scale": 1.5},
           {"name": "C", "x": 0, "y": 1440, "width": 1920, "height": 1080, "scale": 1.0}]
    for how in ("contain", "cover", "stretch"):
        yield {"outputs": ell, "mode": "desktop", "output": "", "footprint": [1000, 333], "fit": how}


def probe(binary, layout):
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as handle:
        json.dump(layout, handle)
        path = handle.name
    try:
        done = subprocess.run([binary, "--stage", path], capture_output=True, text=True, timeout=30, check=False)
    finally:
        os.unlink(path)
    if done.returncode != 0:
        raise RuntimeError(f"the probe failed ({done.returncode}): {done.stderr.strip()}")
    return json.loads(done.stdout)


def compare(layout, answer):
    """-> a list of differences (empty: they agree), or None when the case is skipped."""
    ref = reference(layout["outputs"], layout["mode"], layout["output"], layout["footprint"], layout["fit"])
    if ref is None:
        return [] if not answer["slices"] else ["the reference has no stage, the probe has slices"]
    picture, slices, skipped = ref
    if skipped:
        return None
    problems = []
    if [round(v) for v in answer["picture"]] != picture or any(abs(a - round(a)) > 1e-9 for a in answer["picture"]):
        problems.append(f"picture {answer['picture']} != {picture}")
    if [s["output"] for s in answer["slices"]] != [s["output"] for s in slices]:
        problems.append(f"slices of {[s['output'] for s in answer['slices']]} != {[s['output'] for s in slices]}")
        return problems
    for got, want in zip(answer["slices"], slices):
        for key in ("global", "window"):
            if [round(v) for v in got[key]] != want[key]:
                problems.append(f"{want['output']} {key} {got[key]} != {want[key]}")
        for g, w in zip(got["source"], want["source"]):
            if abs(g - float(w)) > 1e-12:
                problems.append(f"{want['output']} source {got['source']} != {[float(v) for v in want['source']]}")
                break
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--probe", help="murasaki-petprobe")
    parser.add_argument("--seed", type=int, default=20261022)
    parser.add_argument("--cases", type=int, default=150)
    args = parser.parse_args()
    if not args.probe or not os.access(args.probe, os.X_OK):
        print("SKIP: no murasaki-petprobe (build it with FFmpeg and pass --probe)")
        return 77
    rng = random.Random(args.seed)
    checked = skipped = failed = 0
    for layout in cases(rng, args.cases):
        problems = compare(layout, probe(args.probe, layout))
        if problems is None:
            skipped += 1
            continue
        checked += 1
        if problems:
            failed += 1
            print("DISAGREE", json.dumps(layout), *problems, sep="\n   ")
    print(f"stage geometry: {checked} layouts agree with the independent implementation, {skipped} skipped (an edge on a half pixel), {failed} disagree")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
