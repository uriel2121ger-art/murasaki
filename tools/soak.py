#!/usr/bin/env python3
"""Soak test of the scene engine, headless: start an engine on one item after another against a bare bridge consumer (it releases every
frame), switch every few seconds for a number of switches or hours, and sample what leaks: resident memory, GPU memory
(drm-memory-* of /proc/PID/fdinfo), open descriptors, threads, CPU, the frames delivered, and the sizes of the engine's cache
directories. A summary says which runs grew.

  soak.py --engine build/ci/src/scene-vk/murasaki-scene-vk --items 111 222 --hours 8 --switch-every 20
  soak.py --engine ... --library ~/.steam/.../workshop/content/431960 --scenes 12 --switches 200 --size 960x540 --seed 1

The engine is started at nice 19. Output goes to --out (default ~/.cache/murasaki-tests/soak/<time>): soak.csv (one row per sample),
summary.json (also printed). A run's items come from --items, or are picked at random from the `--library` folder's items whose
project.json says scene (one in `--scenes`, default all). Exit status 1 when a run leaked (see `leaks`)."""
import argparse
import array
import csv
import json
import os
import random
import re
import signal
import socket
import statistics
import struct
import subprocess
import sys
import threading
import time

FRAME, ERROR = 5, 6
RELEASE, QUIT = 32, 42
CLK = os.sysconf("SC_CLK_TCK")


# ---- /proc readers (pure functions of text where they can be, so the tests feed them canned files) ----
def parse_fdinfo_memory(text):
    """KiB of GPU memory in one fdinfo file: {'vram': n, 'gtt': n, 'cpu': n}, from the drm-memory-<region> lines (amdgpu writes KiB)."""
    out = {}
    for m in re.finditer(r"^drm-memory-(\w+):\s*(\d+)\s*(KiB|MiB)?", text, re.M):
        n = int(m.group(2)) * (1024 if m.group(3) == "MiB" else 1)
        out[m.group(1)] = n
    return out


def parse_status_kb(text, key):
    m = re.search(rf"^{key}:\s*(\d+)", text, re.M)
    return int(m.group(1)) if m else 0


def gpu_memory_kb(pid):
    """GPU memory of a process, one count per DRM client (the same client has several descriptors): {'vram', 'gtt'} in KiB."""
    clients = {}
    try:
        fds = os.listdir(f"/proc/{pid}/fdinfo")
    except OSError:
        return {"vram": 0, "gtt": 0}
    for fd in fds:
        try:
            text = open(f"/proc/{pid}/fdinfo/{fd}").read()
        except OSError:
            continue
        c = re.search(r"drm-client-id:\s*(\d+)", text)
        if c:
            clients[c.group(1)] = parse_fdinfo_memory(text)
    return {"vram": sum(v.get("vram", 0) for v in clients.values()), "gtt": sum(v.get("gtt", 0) for v in clients.values())}


def directory_bytes(path):
    total = 0
    for root, _dirs, files in os.walk(path):
        for f in files:
            try:
                total += os.lstat(os.path.join(root, f)).st_size
            except OSError:
                pass
    return total


def sample_process(pid):
    try:
        status = open(f"/proc/{pid}/status").read()
        stat = open(f"/proc/{pid}/stat").read()
        rest = stat[stat.rindex(")") + 2:].split()
        cpu = (int(rest[11]) + int(rest[12])) / CLK
        fds = len(os.listdir(f"/proc/{pid}/fd"))
    except OSError:
        return None
    gpu = gpu_memory_kb(pid)
    return {"rss_mb": parse_status_kb(status, "VmRSS") / 1024, "hwm_mb": parse_status_kb(status, "VmHWM") / 1024,
            "threads": parse_status_kb(status, "Threads"), "fds": fds, "cpu_s": cpu, "vram_mb": gpu["vram"] / 1024, "gtt_mb": gpu["gtt"] / 1024}


# ---- the consumer: one engine on one end of a SOCK_SEQPACKET pair ----
class Run:
    def __init__(self, argv, env):
        mine, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.sock = mine
        self.frames = 0
        self.error = None
        self.stop_flag = False
        self.proc = subprocess.Popen([a.replace("{fd}", str(theirs.fileno())) for a in argv], pass_fds=[theirs.fileno()], env=env,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        theirs.close()
        self.thread = threading.Thread(target=self._loop, daemon=True)
        self.thread.start()

    def _loop(self):
        self.sock.settimeout(0.2)
        while not self.stop_flag:
            try:
                data, anc, _f, _a = self.sock.recvmsg(65536, socket.CMSG_SPACE(16 * 4))
            except socket.timeout:
                if self.proc.poll() is not None:
                    return
                continue
            except OSError:
                return
            if not data:
                return
            for level, typ, payload in anc:
                if level == socket.SOL_SOCKET and typ == socket.SCM_RIGHTS:
                    fds = array.array("i")
                    fds.frombytes(payload[:len(payload) - len(payload) % 4])
                    for fd in fds:
                        os.close(fd)
            mtype, _size = struct.unpack_from("<II", data)
            if mtype == FRAME:
                self.frames += 1
                viewport, buffer, _seq, _flip, generation = struct.unpack_from("<IIQII", data, 8)
                try:
                    self.sock.send(struct.pack("<IIIII", RELEASE, 12, viewport, buffer, generation))
                except OSError:
                    return
            elif mtype == ERROR:
                self.error = data[12:].split(b"\0", 1)[0].decode(errors="replace")

    def stop(self):
        self.stop_flag = True
        try:
            self.sock.send(struct.pack("<III", QUIT, 4, 0))  # header, then Quit {reserved}
        except OSError:
            pass
        try:
            self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self.thread.join(2)
        self.sock.close()


def engine_pid(run):
    """The engine itself: under `nice` it is the exec'd child itself; the pid of the Popen is already the engine."""
    return run.proc.pid


# ---- analysis ----
def slope_per_hour(points):
    """Least squares slope of (seconds, value) pairs, in value per hour; 0 for fewer than three points."""
    if len(points) < 3:
        return 0.0
    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    mx, my = statistics.fmean(xs), statistics.fmean(ys)
    den = sum((x - mx) ** 2 for x in xs)
    return 0.0 if den == 0 else sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / den * 3600.0


def judge(runs, rss_slope_mb_h=30.0, fd_growth=8, vram_slope_mb_h=30.0, min_seconds=60.0):
    """Which runs leaked. A run is the samples of one engine process: [{t, rss_mb, fds, vram_mb, ...}]. Growth is judged on runs
    long enough to mean something (min_seconds); a run of the engine that died is reported as a crash."""
    leaks = []
    for r in runs:
        s = r["samples"]
        name = f"{r['item']} (run {r['index']})"
        if r.get("exit") not in (None, 0, -signal.SIGTERM, -signal.SIGKILL):
            leaks.append(f"{name}: the engine exited with {r['exit']} before it was stopped")
        if len(s) < 2 or s[-1]["t"] - s[0]["t"] < min_seconds:
            continue
        rss = slope_per_hour([(x["t"], x["rss_mb"]) for x in s[len(s) // 4:]])  # the first quarter is loading, not leaking
        vram = slope_per_hour([(x["t"], x["vram_mb"]) for x in s[len(s) // 4:]])
        if rss > rss_slope_mb_h:
            leaks.append(f"{name}: resident memory grows {rss:.0f} MB/h")
        if vram > vram_slope_mb_h:
            leaks.append(f"{name}: GPU memory grows {vram:.0f} MB/h")
        if s[-1]["fds"] - s[len(s) // 4]["fds"] > fd_growth:
            leaks.append(f"{name}: descriptors grew from {s[len(s) // 4]['fds']} to {s[-1]['fds']}")
    return leaks


def library_items(library, every):
    out = []
    for d in sorted(os.listdir(library)):
        p = os.path.join(library, d, "project.json")
        try:
            if json.load(open(p, errors="replace")).get("type", "").lower() == "scene":
                out.append(d)
        except (OSError, ValueError):
            pass
    return out[::max(1, every)]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--engine", required=True)
    ap.add_argument("--items", nargs="*", default=[])
    ap.add_argument("--library", default=os.path.expanduser("~/.steam/debian-installation/steamapps/workshop/content/431960"))
    ap.add_argument("--scenes", type=int, default=1, help="with no --items: use one scene in this many of the library")
    ap.add_argument("--size", default="960x540")
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--hours", type=float, default=1.0)
    ap.add_argument("--switches", type=int, default=0, help="stop after this many engine starts (0: only --hours counts)")
    ap.add_argument("--switch-every", type=float, default=20.0, help="seconds an engine runs before the next one starts")
    ap.add_argument("--sample", type=float, default=10.0, help="seconds between samples")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--out", default="")
    ap.add_argument("--extra", nargs="*", default=[], help="more engine arguments")
    a = ap.parse_args()
    items = a.items or library_items(a.library, a.scenes)
    if not items:
        sys.exit("no items")
    out = a.out or os.path.expanduser(f"~/.cache/murasaki-tests/soak/{time.strftime('%Y%m%d-%H%M%S')}")
    os.makedirs(out, exist_ok=True)
    rng = random.Random(a.seed)
    cache_home = os.environ.get("XDG_CACHE_HOME", os.path.expanduser("~/.cache"))
    caches = {"texture_cache_mb": os.path.join(cache_home, "murasaki/texture-cache"), "shader_cache_mb": os.path.join(cache_home, "murasaki/shader-cache")}
    deadline = time.monotonic() + a.hours * 3600
    runs = []
    t0 = time.monotonic()
    with open(os.path.join(out, "soak.csv"), "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["t", "run", "item", "rss_mb", "hwm_mb", "vram_mb", "gtt_mb", "fds", "threads", "cpu_pct", "frames"] + list(caches))
        index = 0
        while time.monotonic() < deadline and (not a.switches or index < a.switches):
            item = rng.choice(items)
            argv = ["nice", "-n", "19", a.engine, "--murasaki-bridge-fd", "{fd}", "--murasaki-size", a.size, "--screen-root", "soak", "--bg",
                    os.path.join(a.library, item), "--fps", str(a.fps), "--volume", "0"] + a.extra
            run = Run(argv, dict(os.environ))
            record = {"index": index, "item": item, "samples": [], "exit": None}
            runs.append(record)
            print(f"[{time.monotonic() - t0:7.0f}s] run {index}: {item}", flush=True)
            start = time.monotonic()
            last_cpu, last_t = None, None
            while time.monotonic() - start < a.switch_every and time.monotonic() < deadline:
                time.sleep(min(a.sample, max(0.1, a.switch_every - (time.monotonic() - start))))
                if run.proc.poll() is not None:
                    record["exit"] = run.proc.returncode
                    break
                s = sample_process(engine_pid(run))
                if not s:
                    continue
                now = time.monotonic()
                cpu_pct = 0.0 if last_cpu is None else (s["cpu_s"] - last_cpu) / (now - last_t) * 100
                last_cpu, last_t = s["cpu_s"], now
                s.update({"t": now - start, "frames": run.frames})
                record["samples"].append(s)
                row = [round(now - t0, 1), index, item, round(s["rss_mb"], 1), round(s["hwm_mb"], 1), round(s["vram_mb"], 1), round(s["gtt_mb"], 1),
                       s["fds"], s["threads"], round(cpu_pct, 1), run.frames]
                row += [round(directory_bytes(p) / 1048576, 1) for p in caches.values()]
                writer.writerow(row)
                f.flush()
            if record["exit"] is None:
                run.stop()
                record["exit"] = run.proc.returncode
            record["error"] = run.error
            index += 1
    leaks = judge(runs)
    summary = {"runs": len(runs), "items": sorted({r["item"] for r in runs}), "seconds": round(time.monotonic() - t0),
               "max_rss_mb": round(max((x["rss_mb"] for r in runs for x in r["samples"]), default=0), 1),
               "max_vram_mb": round(max((x["vram_mb"] for r in runs for x in r["samples"]), default=0), 1),
               "max_fds": max((x["fds"] for r in runs for x in r["samples"]), default=0),
               "errors": [f"{r['item']}: {r['error']}" for r in runs if r.get("error")], "leaks": leaks}
    with open(os.path.join(out, "summary.json"), "w") as f:
        json.dump(summary, f, indent=1)
    print(json.dumps(summary, indent=1))
    sys.exit(1 if leaks else 0)


if __name__ == "__main__":
    main()
