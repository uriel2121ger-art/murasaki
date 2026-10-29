#!/usr/bin/env python3
"""Soak test of the whole Murasaki stack in a nested session: the daemon, the surface (layer-shell windows on the nested output) and
whatever engines they start, while `murasakictl set` switches the wallpaper through a list of items every few minutes. Run it
inside tools/bench/nested-session.sh (a private bus, runtime dir, kwin and PipeWire); it never touches the user's session.

  tools/bench/nested-session.sh --size 1280x720 -- python3 tools/soak-stack.py --build-dir build/ci --hours 3 --switch-every 120 --scenes 20

Items come from the daemon's own library (`murasakictl list`, status ok): --scenes scenes picked at random (--seed), then one web and one
video item. Every --sample seconds the tool records, for the daemon, the surface and all their descendants (engines, mpv, web
processes): resident memory, GPU memory (drm-memory-* of /proc/PID/fdinfo), descriptors, threads, and the sizes of the engine's cache
folders (private to the session, so they start empty). The summary gives the growth per hour of each; `leaks` names what grew past
the limits (30 MB/h of memory or GPU memory, 40 descriptors/h). summary.json and soak.csv go to --out."""
import argparse
import csv
import json
import os
import random
import signal
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import soak


def tree(pid):
    kids = {}
    for d in os.listdir("/proc"):
        if d.isdigit():
            try:
                stat = open(f"/proc/{d}/stat").read()
                kids.setdefault(int(stat[stat.rindex(")") + 2:].split()[1]), []).append(int(d))
            except OSError:
                pass
    out, todo = [], [pid]
    while todo:
        p = todo.pop()
        out.append(p)
        todo += kids.get(p, [])
    return out


def combined(pids):
    total = {"rss_mb": 0.0, "fds": 0, "threads": 0, "vram_mb": 0.0, "gtt_mb": 0.0, "procs": 0, "cpu_s": 0.0}
    for p in pids:
        s = soak.sample_process(p)
        if s:
            total["rss_mb"] += s["rss_mb"]
            total["fds"] += s["fds"]
            total["threads"] += s["threads"]
            total["vram_mb"] += s["vram_mb"]
            total["gtt_mb"] += s["gtt_mb"]
            total["cpu_s"] += s["cpu_s"]
            total["procs"] += 1
    return total


def gfx_ns(pids):
    """GPU busy time (ns, drm-engine-gfx) of all DRM clients of the given processes; one count per client."""
    seen = {}
    for pid in pids:
        try:
            names = os.listdir(f"/proc/{pid}/fdinfo")
        except OSError:
            continue
        for n in names:
            try:
                text = open(f"/proc/{pid}/fdinfo/{n}").read()
            except OSError:
                continue
            if "drm-client-id" not in text:
                continue
            client = engine = None
            for line in text.splitlines():
                if line.startswith("drm-client-id:"):
                    client = line.split()[1]
                elif line.startswith("drm-engine-gfx:"):
                    engine = int(line.split()[1])
            if client is not None and engine is not None:
                seen[client] = engine
    return sum(seen.values())


def cmdline(pid):
    try:
        return open(f"/proc/{pid}/cmdline").read().replace("\0", " ")
    except OSError:
        return ""


def comm(pid):
    try:
        return open(f"/proc/{pid}/comm").read().strip()
    except OSError:
        return ""


def classify(pid):
    """check: a --check run (the surface asks it for a verdict before it launches an engine); engine: a running one."""
    line = cmdline(pid)
    if "murasaki-scene-vk" not in line:
        return None
    return "check" if " --check" in line else "engine"


def fd_classes(pid):
    """The descriptors of a process by what they point to (anonymous inodes by their kind, files by directory)."""
    count = {}
    try:
        names = os.listdir(f"/proc/{pid}/fd")
    except OSError:
        return count
    for n in names:
        try:
            t = os.readlink(f"/proc/{pid}/fd/{n}")
        except OSError:
            continue
        if t.startswith("/dmabuf:"):
            info = {}
            try:
                for line in open(f"/proc/{pid}/fdinfo/{n}").read().splitlines():
                    k, _, v = line.partition(":")
                    info[k] = v.strip()
            except OSError:
                pass
            key = f"dmabuf size={info.get('size', '?')} exp={info.get('exp_name', '?')}"
        elif t.startswith("/dev/dri/"):
            key = "drm"
        elif t.startswith("/memfd:"):
            key = "memfd:" + t[7:].split(" ")[0][:24]
        elif t.startswith("/home") or t.startswith("/usr") or t.startswith("/tmp"):
            key = "file:" + os.path.dirname(t)[:40]
        else:
            key = t[:28] if not t.startswith("socket:") else "socket"
        count[key] = count.get(key, 0) + 1
    return count


def anon_maps(pid):
    """Count and resident size (MB) of anonymous and dma-buf/memfd mappings by size class."""
    import re
    out = {}
    cur = None
    try:
        lines = open(f"/proc/{pid}/smaps").read().splitlines()
    except OSError:
        return out
    for l in lines:
        m = re.match(r"^([0-9a-f]+)-([0-9a-f]+) (\S+) \S+ \S+ \S+\s*(.*)", l)
        if m:
            size = (int(m.group(2), 16) - int(m.group(1), 16)) // 1024
            name = m.group(4) or "[anon]"
            kind = name if name.startswith("[") or name.startswith("/memfd") or name.startswith("/dmabuf") or name.startswith("/dev/") else "file"
            cur = [kind, size, 0]
            out.setdefault(kind, []).append(cur)
        elif l.startswith("Rss:") and cur:
            cur[2] = int(l.split()[1])
    return {k: {"maps": len(v), "rss_mb": round(sum(x[2] for x in v) / 1024, 1), "big_maps_over_8mb": sum(1 for x in v if x[2] > 8192)} for k, v in out.items()}


def parent(pid):
    try:
        stat = open(f"/proc/{pid}/stat").read()
        return int(stat[stat.rindex(")") + 2:].split()[1])
    except OSError:
        return 0


def leaves(kind, pids):
    """The processes of one kind that have no process of the same kind below them: the sandbox helper re-executes the
    engine, so each engine (or check) shows up twice in the tree."""
    found = {p for p in pids if classify(p) == kind}
    return {p for p in found if not any(parent(q) == p for q in found)}


def zombies(pids):
    n = 0
    for p in pids:
        try:
            stat = open(f"/proc/{p}/stat").read()
            n += stat[stat.rindex(")") + 2] == "Z"
        except OSError:
            pass
    return n


def percentile(values, q):
    values = sorted(values)
    return values[min(len(values) - 1, int(q * len(values)))] if values else None


class Probe(threading.Thread):
    """Watches the engines under the surface every few milliseconds, apart from the slow sampler: the sampler's own period
    (and its cost) would otherwise be the resolution of every latency. For each switch it keeps the time of the request, the
    first engine that was not there before (spawn) and the time the engines that were there went away (the old one's end:
    new frame shown, fade, quit)."""

    def __init__(self, surface_pid, interval):
        super().__init__(daemon=True)
        self.pid, self.interval, self.stop_flag = surface_pid, interval, False
        self.current = set()
        self.t0 = time.monotonic()
        self.marks = []  # dicts: index, lap, asked, before, spawn, gone

    def mark(self, index, lap, asked):
        self.marks.append({"index": index, "lap": lap, "asked": asked, "before": set(self.current), "spawn": None, "gone": None})

    def run(self):
        while not self.stop_flag:
            now = time.monotonic()
            engines = leaves("engine", tree(self.pid))
            if os.environ.get("SOAK_PROBE_LOG") and engines != self.current:
                print(f"probe {now - self.t0:8.3f} engines {sorted(engines)}", flush=True)
            self.current = engines
            for m in self.marks[-3:]:
                if m["spawn"] is None and engines - m["before"]:
                    m["spawn"] = now
                if m["gone"] is None and m["before"] and not (engines & m["before"]):
                    m["gone"] = now
            time.sleep(self.interval)

    def summary(self):
        def pct(values, q):
            return percentile(values, q)
        out = {}
        for name, laps in (("first_visit", lambda m: m["lap"] == 0), ("revisit", lambda m: m["lap"] > 0)):
            sel = [m for m in self.marks if laps(m)]
            spawn = [m["spawn"] - m["asked"] for m in sel if m["spawn"]]
            gone = [m["gone"] - m["asked"] for m in sel if m["gone"] and m["spawn"]]
            out[name] = {"switches": len(sel), "spawn_p50_s": pct(spawn, 0.5), "spawn_p95_s": pct(spawn, 0.95),
                         "old_engine_gone_p50_s": pct(gone, 0.5), "old_engine_gone_p95_s": pct(gone, 0.95)}
        return out


def listing(ctl):
    out = subprocess.run([ctl, "list"], capture_output=True, text=True, timeout=60).stdout
    items = []
    for line in out.splitlines():
        c = line.split("\t")
        if len(c) >= 3 and c[2] == "ok":
            items.append((c[0], c[1]))
    return items


WORKSHOP = os.path.expanduser("~/.steam/debian-installation/steamapps/workshop/content/431960")
WE_ASSETS = os.path.expanduser("~/.steam/debian-installation/steamapps/common/wallpaper_engine/assets")


def accepted(item):
    """True when murasaki-scene-vk accepts the scene: a refused one is drawn incompletely and its samples would not compare."""
    cmd = ["nice", "-n", "19", os.environ.get("MURASAKI_SCENE_VK") or os.path.expanduser("~/.local/bin/murasaki-scene-vk"), "--check", os.path.join(WORKSHOP, item.split(":", 1)[-1])]
    if os.path.isdir(WE_ASSETS):
        cmd += ["--assets-dir", WE_ASSETS]
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=60).stdout
        return json.loads(out.strip().splitlines()[-1]).get("ok") is True
    except (subprocess.SubprocessError, ValueError, IndexError):
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--hours", type=float, default=3.0)
    ap.add_argument("--switch-every", type=float, default=120.0)
    ap.add_argument("--sample", type=float, default=30.0)
    ap.add_argument("--scenes", type=int, default=20)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", default="")
    ap.add_argument("--probe-ms", type=int, default=0, help="watch the engines every N ms for switch latencies (0: off)")
    ap.add_argument("--exclude", default="", help="item ids (or a file of them) that are left out of the random plan")
    ap.add_argument("--only", nargs="*", default=[], help="item ids to cycle through instead of the library's")
    a = ap.parse_args()
    build = os.path.abspath(a.build_dir)
    out = a.out or os.path.expanduser(f"~/.cache/murasaki-tests/soak/stack-{time.strftime('%Y%m%d-%H%M%S')}")
    os.makedirs(out, exist_ok=True)
    log = os.environ.get("LOG_DIR", out)
    # The surface starts the engine named by MURASAKI_SCENE_VK, else the installed one. A build's own engine does not run
    # behind the sandbox (its --check never answers, so every scene falls back to a preview): set it only for a run without it.
    os.environ.setdefault("MURASAKI_SCENE_VK", "")
    if not os.environ["MURASAKI_SCENE_VK"]:
        del os.environ["MURASAKI_SCENE_VK"]
    ctl = os.path.join(build, "src/ctl/murasakictl")
    daemon = subprocess.Popen(["nice", "-n", "19", os.path.join(build, "src/daemon/murasakid")], stdout=open(os.path.join(log, "soak-murasakid.log"), "w"), stderr=subprocess.STDOUT)
    items = []
    for _ in range(100):
        time.sleep(0.5)
        items = listing(ctl)
        if items:
            break
    if not items:
        sys.exit("the private daemon sees no library items")
    surface = subprocess.Popen(["nice", "-n", "19", os.path.join(build, "src/surface/murasaki-surface")], stdout=open(os.path.join(log, "soak-surface.log"), "w"), stderr=subprocess.STDOUT)
    time.sleep(3)
    rng = random.Random(a.seed)
    if a.only:
        plan = list(a.only)
        # The first --check of a scene translates all its shaders into the session's (empty) shader cache: seconds for a
        # heavy scene, 0.3 s afterwards. Run them once here so the switches below measure revisits, not cold starts.
        for item in dict.fromkeys(plan):
            if items and next((k for i, k in items if i == item), "") == "scene":
                accepted(item)
    else:
        left_out = set()
        for part in a.exclude.split(","):
            if part and os.path.isfile(part):
                left_out |= {("we:" + t if not t.startswith("we:") else t) for t in open(part).read().split()}
            elif part:
                left_out.add(part if part.startswith("we:") else "we:" + part)
        scenes = [i for i, k in items if k == "scene" and i not in left_out]
        rng.shuffle(scenes)
        chosen = []
        for i in scenes:  # the seeded order decides; only the ones the engine takes count (and warm its shader cache)
            if len(chosen) >= a.scenes:
                break
            if accepted(i):
                chosen.append(i)
        webs = [i for i, k in items if k == "web" and i not in left_out]
        videos = [i for i, k in items if k == "video" and i not in left_out]
        rng.shuffle(webs)
        rng.shuffle(videos)
        plan = chosen + webs[:1] + videos[:1]
    print(f"plan: {len(plan)} items", flush=True)
    cache_home = os.environ.get("XDG_CACHE_HOME", os.path.expanduser("~/.cache"))
    caches = {"texture_cache_mb": os.path.join(cache_home, "murasaki/texture-cache"), "shader_cache_mb": os.path.join(cache_home, "murasaki/shader-cache"),
              "cache_total_mb": cache_home}
    t0 = time.monotonic()
    deadline = t0 + a.hours * 3600
    rows = []
    storm = {"latencies": [], "failed_sets": 0, "peak_engines": 0, "overlap_samples": 0, "peak_cpu_pct": 0.0, "peak_gpu_pct": 0.0}
    last_cpu = (time.monotonic(), 0.0, 0)
    seen_engines = {}  # pid -> [first seen, last seen]
    probe = Probe(surface.pid, a.probe_ms / 1000) if a.probe_ms else None
    if probe:
        probe.start()
    last_snap = 0.0
    with open(os.path.join(out, "soak.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["t", "item", "kind", "daemon_rss_mb", "surface_rss_mb", "children_rss_mb", "children", "vram_mb", "gtt_mb", "fds", "threads"] + list(caches))
        index = 0
        while time.monotonic() < deadline:
            item = plan[index % len(plan)]
            kind = next((k for i, k in items if i == item), "?")
            before_engines = leaves("engine", tree(surface.pid))
            asked = time.monotonic()
            if subprocess.run([ctl, "set", item], capture_output=True, timeout=60).returncode != 0:
                storm["failed_sets"] += 1
            if probe:
                probe.mark(index, index // len(plan), asked)
                if os.environ.get("SOAK_PROBE_LOG"):
                    print(f"probe {asked - probe.t0:8.3f} set {item}", flush=True)
            pending_latency = bool(before_engines)
            pending_spawn = True
            print(f"[{time.monotonic() - t0:7.0f}s] {index}: {item} ({kind})", flush=True)
            start = time.monotonic()
            while time.monotonic() - start < a.switch_every and time.monotonic() < deadline:
                time.sleep(min(a.sample, max(0.2, a.switch_every - (time.monotonic() - start))))
                if surface.poll() is not None or daemon.poll() is not None:
                    print("the daemon or the surface died", flush=True)
                    storm["died"] = f"surface {surface.poll()}, daemon {daemon.poll()}"
                    deadline = 0
                    break
                d = combined([daemon.pid])
                s = combined([surface.pid])
                everyone = tree(surface.pid)
                kids = combined([p for p in everyone if p != surface.pid])
                engines = leaves("engine", everyone)
                checks = len(leaves("check", everyone))
                storm["peak_checks"] = max(storm.get("peak_checks", 0), checks)
                if os.environ.get("SOAK_FDSNAP") and time.monotonic() - last_snap > 60:
                    last_snap = time.monotonic()
                    with open(os.path.join(out, "fdsnap.jsonl"), "a") as cf:
                        cf.write(json.dumps({"t": round(last_snap - t0), "fds": fd_classes(surface.pid), "maps": anon_maps(surface.pid)}) + "\n")
                if os.environ.get("SOAK_CENSUS"):
                    with open(os.path.join(out, "census.jsonl"), "a") as cf:
                        cf.write(json.dumps({"t": round(time.monotonic() - t0, 1), "item": item, "procs": [[p, comm(p), cmdline(p)[:110]] for p in everyone if p != surface.pid]}) + "\n")
                for p in engines:
                    seen_engines.setdefault(p, [time.monotonic(), 0])[1] = time.monotonic()
                storm["peak_engines"] = max(storm["peak_engines"], len(engines))
                storm["overlap_samples"] += len(engines) > 2
                if pending_spawn and (engines - before_engines):
                    storm.setdefault("spawn_laps", []).append((index // len(plan), time.monotonic() - asked))  # a new engine process exists
                    pending_spawn = False
                if pending_latency and not (engines & before_engines):
                    storm["latencies"].append(time.monotonic() - asked)
                    storm.setdefault("latency_laps", []).append((index // len(plan), time.monotonic() - asked))  # the old engine is gone: the new frame was shown and the fade ended
                    pending_latency = False
                now = time.monotonic()
                cpu_total = d["cpu_s"] + s["cpu_s"] + kids["cpu_s"]
                gfx = gfx_ns([daemon.pid] + everyone)
                if now - last_cpu[0] > 0.5:
                    storm["peak_cpu_pct"] = max(storm["peak_cpu_pct"], 100 * (cpu_total - last_cpu[1]) / (now - last_cpu[0]))
                    storm["peak_gpu_pct"] = max(storm["peak_gpu_pct"], 100 * (gfx - last_cpu[2]) / 1e9 / (now - last_cpu[0]))
                    last_cpu = (now, cpu_total, gfx)
                row = {"t": time.monotonic() - t0, "item": item, "kind": kind, "daemon_rss_mb": d["rss_mb"], "surface_rss_mb": s["rss_mb"], "children_rss_mb": kids["rss_mb"],
                       "children": kids["procs"], "vram_mb": d["vram_mb"] + s["vram_mb"] + kids["vram_mb"], "gtt_mb": d["gtt_mb"] + s["gtt_mb"] + kids["gtt_mb"], "fds": d["fds"] + s["fds"] + kids["fds"],
                       "threads": d["threads"] + s["threads"] + kids["threads"]}
                for key, path in caches.items():
                    row[key] = soak.directory_bytes(path) / 1048576
                rows.append(row)
                w.writerow([round(row[k], 1) if isinstance(row[k], float) else row[k] for k in ["t", "item", "kind", "daemon_rss_mb", "surface_rss_mb", "children_rss_mb", "children", "vram_mb", "gtt_mb", "fds", "threads"] + list(caches)])
                f.flush()
            index += 1
    subprocess.run([ctl, "unset"], capture_output=True, timeout=60)
    cooldown = float(os.environ.get("SOAK_COOLDOWN", "8"))
    if os.environ.get("SOAK_FDSNAP"):
        with open(os.path.join(out, "fdsnap.jsonl"), "a") as cf:
            cf.write(json.dumps({"t": "end", "fds": fd_classes(surface.pid), "maps": anon_maps(surface.pid)}) + "\n")
    time.sleep(cooldown)
    storm["fds_after_cooldown"] = fd_classes(surface.pid) if os.environ.get("SOAK_FDSNAP") else None
    storm["fds_total_after_cooldown"] = sum(combined([p])["fds"] for p in [surface.pid, daemon.pid])
    leftovers = [p for p in tree(surface.pid) if p != surface.pid]
    storm["zombies"] = zombies(leftovers)
    storm["children_after_unset"] = sorted({comm(p) for p in leftovers})  # QtWebEngine's zygotes live as long as the surface
    for p in (surface, daemon):
        p.send_signal(signal.SIGTERM)
    for p in (surface, daemon):
        try:
            p.wait(10)
        except subprocess.TimeoutExpired:
            p.kill()
    time.sleep(5)
    storm["outlived_the_surface"] = [cmdline(p)[:80] for p in leftovers if os.path.exists(f"/proc/{p}") and zombies([p]) == 0]
    growth = {}
    for key in ["daemon_rss_mb", "surface_rss_mb", "children_rss_mb", "vram_mb", "fds", "threads"] + list(caches):
        growth[key + "_per_hour"] = round(soak.slope_per_hour([(r["t"], r[key]) for r in rows[len(rows) // 5:]]), 1)  # the first fifth is warming up
    leaks = []
    for key, limit in (("daemon_rss_mb", 30), ("surface_rss_mb", 30), ("vram_mb", 30), ("fds", 40)):
        if growth[key + "_per_hour"] > limit:
            leaks.append(f"{key} grows {growth[key + '_per_hour']} per hour")
    summary = {"seconds": round(time.monotonic() - t0), "switches": index, "items": len(plan), "samples": len(rows),
               "max": {k: round(max((r[k] for r in rows), default=0), 1) for k in ("daemon_rss_mb", "surface_rss_mb", "children_rss_mb", "vram_mb", "fds", "threads")},
               "growth": growth, "leaks": leaks, "storm": {
                   "switches_ok": index - storm["failed_sets"], "failed_sets": storm["failed_sets"], "latency_p50_s": percentile(storm["latencies"], 0.5),
                   "latency_p95_s": percentile(storm["latencies"], 0.95), "latency_samples": len(storm["latencies"]),
                   "latency_first_visit_p50_s": percentile([v for lap, v in storm.get("latency_laps", []) if lap == 0], 0.5),
                   "latency_revisit_p50_s": percentile([v for lap, v in storm.get("latency_laps", []) if lap > 0], 0.5),
                   "spawn_revisit_p50_s": percentile([v for lap, v in storm.get("spawn_laps", []) if lap > 0], 0.5),
                   "latency_revisit_p95_s": percentile([v for lap, v in storm.get("latency_laps", []) if lap > 0], 0.95),
                   "peak_engines": storm["peak_engines"], "engine_lifetime_p50_s": percentile([max(0.0, b - a) for a, b in seen_engines.values()], 0.5), "engine_lifetime_p95_s": percentile([b - a for a, b in seen_engines.values()], 0.95),
                   "engine_lifetime_max_s": max([b - a for a, b in seen_engines.values()], default=None), "engines_seen": len(seen_engines), "peak_concurrent_checks": storm.get("peak_checks", 0), "samples_with_3plus_engines": storm["overlap_samples"],
                   "peak_cpu_pct": round(storm["peak_cpu_pct"], 1), "peak_gpu_pct": round(storm["peak_gpu_pct"], 1),
                   "children_after_unset": storm["children_after_unset"], "outlived_the_surface": storm["outlived_the_surface"], "zombies": storm["zombies"], "died": storm.get("died"), "probe": probe.summary() if probe else None,
                   "fds_after_cooldown": storm.get("fds_after_cooldown"), "fds_total_after_cooldown": storm.get("fds_total_after_cooldown")}, "alive": surface.returncode is None}
    with open(os.path.join(out, "summary.json"), "w") as fh:
        json.dump(summary, fh, indent=1)
    print(json.dumps(summary, indent=1))
    sys.exit(1 if leaks else 0)


if __name__ == "__main__":
    main()
