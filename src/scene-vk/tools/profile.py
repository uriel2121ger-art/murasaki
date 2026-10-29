#!/usr/bin/env python3
"""scene-vk-profile: turns the engine's MURASAKI_SCENE_VK_PROFILE=1 log into a table of the top GPU costs.

    MURASAKI_SCENE_VK_PROFILE=1 murasaki-scene-vk ... 2> engine.log
    python3 profile.py engine.log [--top 15]

Each report block of the log holds, per label, the average GPU milliseconds per frame (over all frames so far), the
calls per frame and the slowest call. The last block is used."""
import argparse
import sys


def last_block(lines):
    block, frames = [], 0
    for line in lines:
        line = line.rstrip("\n")
        if "profile-begin" in line:
            block, frames = [], int(line.split("frames=")[1])
        elif "profile-pass\t" in line:
            block.append(line.split("profile-pass\t", 1)[1].split("\t"))
    return frames, block


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--top", type=int, default=15)
    args = ap.parse_args()
    with open(args.log, errors="replace") as f:
        frames, block = last_block(f)
    if not block:
        print("no profile report in the log (was MURASAKI_SCENE_VK_PROFILE=1 set, and did the engine run 150 frames?)", file=sys.stderr)
        return 1
    rows = [(label, float(avg), float(calls), float(peak)) for label, avg, calls, peak in block]
    total = sum(r[1] for r in rows)
    print(f"{frames} frames, {total:.2f} ms of GPU time per frame\n")
    print(f"{'ms/frame':>9} {'%':>5} {'calls':>6} {'max ms':>8}  pass")
    for label, avg, calls, peak in sorted(rows, key=lambda r: -r[1])[: args.top]:
        print(f"{avg:9.3f} {100 * avg / total:5.1f} {calls:6.2f} {peak:8.3f}  {label}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
