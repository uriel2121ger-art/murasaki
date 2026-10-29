#!/usr/bin/env python3
"""The automation sweep (docs/pets/automation.md).

  automation_sweep.py [--build-dir DIR] [--name NAME] [--storm N] [--switches N] [--only CASE ...]

Feeds the daemon's validators files and calls they should refuse and checks what the plan promises, on a private session bus with its own XDG directories
(nothing of the user's session is touched, no overlay is started):

  hostile_triggers / hostile_timelines / hostile_profiles   every malformed spec is refused with its expected words; the lists and the files are what they were
  caps                                                       100 triggers, 50 profiles, 100 timelines, 16 tracks, 256 actions: the next one is refused
  hostile_files                                              an unusable triggers.json / timelines.json / profiles.json is set aside as .bad-<time> and the store starts empty
  fresh_install                                              no store file, no extra thread, no child process, CPU idle
  storm                                                      FireTrigger as fast as a process can call it: the fires that ran stay within the rate limits, and the daemon is idle again
  soak                                                       N profile switches: no growth of the daemon's memory worth the name, no leftover files
  screen_packs                                               Triggers packs with captured frames (.npz) through the real import and ImportPackAutomation: valid ones keep their pictures,
                                                             hostile ones (object dtype, float, Fortran order, wrong shape, huge shape, truncated, trailing bytes, huge header, future
                                                             version, a pickle, two files for a region, ...) are refused by name, keep nothing and cost the daemon nothing
  no_children                                                the daemon never started a process (a trigger can not)

Output: ~/.cache/murasaki-tests/compat/automation-NAME/results.json and report.md. Exit status 1 when a case fails.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CACHE = Path(os.environ.get("MURASAKI_SWEEP_CACHE") or Path.home() / ".cache" / "murasaki-tests")  # (the real one: the daemon runs with a HOME of its own)
MD = "io.github.uriel2121ger_art.Murasaki"
MD_PATH = "/io/github/uriel2121ger_art/Murasaki"
MD_IF = "io.github.uriel2121ger_art.Murasaki1"


MARKER = ".murasaki-automation-sweep"


def refuse_unless_private():
    """The inner half deletes triggers, timelines and profiles to test the caps: it must never be able to do that to the user's real ones. It runs only in the
    directory the outer half made (a marker file with a token only the outer half passes on), with that directory as its HOME and its own XDG directories, and
    only when no murasakid is on the bus yet."""
    tmp = os.environ.get("MURASAKI_SWEEP_TMP", "")
    token = os.environ.get("MURASAKI_SWEEP_TOKEN", "")
    problems = []
    if os.environ.get("MURASAKI_SWEEP_BUS") != "1" or not tmp or not token:
        problems.append("it was not started by the outer half")
    else:
        marker = Path(tmp) / MARKER
        if not marker.is_file() or marker.read_text() != token:
            problems.append("the marker of the private directory is missing or different")
    if tmp and os.environ.get("HOME") != tmp:
        problems.append("HOME is not the private directory")
    for var in ("XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME", "XDG_RUNTIME_DIR"):
        if tmp and not os.environ.get(var, "").startswith(tmp + "/"):
            problems.append("%s is not inside the private directory" % var)
    if not problems and busctl("get-property", MD, MD_PATH, MD_IF, "ApiVersion").returncode == 0:
        problems.append("a murasakid is already on this bus: it is not the private one")
    if problems:
        sys.exit("automation_sweep: refusing to run: " + "; ".join(problems))


def reexec_under_private_bus(argv):
    """Runs this script again inside dbus-run-session with a bus that can activate nothing, and XDG directories of its own."""
    CACHE.mkdir(parents=True, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="murasaki-automation-sweep-", dir=str(CACHE))
    token = os.urandom(16).hex()
    (Path(tmp) / MARKER).write_text(token)
    env = dict(os.environ, MURASAKI_SWEEP_CACHE=str(CACHE), MURASAKI_SWEEP_TOKEN=token, MURASAKI_SWEEP_BUS="1", MURASAKI_SWEEP_TMP=tmp, HOME=tmp, XDG_CONFIG_HOME=tmp + "/config", XDG_DATA_HOME=tmp + "/data",
               XDG_CACHE_HOME=tmp + "/cache", XDG_STATE_HOME=tmp + "/state", XDG_RUNTIME_DIR=tmp + "/rt", MURASAKI_OVERLAY_UNIT="0")
    for d in ("config", "data", "cache", "state", "rt"):
        os.makedirs(tmp + "/" + d, mode=0o700, exist_ok=True)
    conf = ROOT / "tests" / "dbus-isolated-session.conf"
    try:
        return subprocess.call(["dbus-run-session", "--config-file=%s" % conf, "--", sys.executable, "-I", __file__] + argv, env=env)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def die_with_parent():
    """The daemon and the monitor end if this script is killed (no orphan is left behind)."""
    import ctypes
    ctypes.CDLL("libc.so.6", use_errno=True).prctl(1, 15)  # PR_SET_PDEATHSIG, SIGTERM


class Daemon:
    def __init__(self, build):
        self.build = Path(build)
        self.proc = None
        self.config = Path(os.environ["XDG_CONFIG_HOME"]) / "murasaki"

    def start(self):
        self.proc = subprocess.Popen([str(self.build / "src/daemon/murasakid")], stdout=subprocess.DEVNULL, stderr=open(os.environ["MURASAKI_SWEEP_TMP"] + "/daemon.log", "ab"), preexec_fn=die_with_parent)
        for _ in range(100):
            r = busctl("get-property", MD, MD_PATH, MD_IF, "ApiVersion")
            if r.returncode == 0:
                return
            if self.proc.poll() is not None:
                raise RuntimeError("murasakid exited: %s" % self.proc.returncode)
            time.sleep(0.1)
        raise RuntimeError("murasakid did not come up")

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.proc = None

    @property
    def pid(self):
        return self.proc.pid

    def threads(self):
        for line in Path("/proc/%d/status" % self.pid).read_text().splitlines():
            if line.startswith("Threads:"):
                return int(line.split()[1])

    def rss_kib(self):
        for line in Path("/proc/%d/status" % self.pid).read_text().splitlines():
            if line.startswith("VmRSS:"):
                return int(line.split()[1])

    def cpu_ticks(self):
        fields = Path("/proc/%d/stat" % self.pid).read_text().rsplit(")", 1)[1].split()
        return int(fields[11]) + int(fields[12])

    def children(self):
        out = []
        for p in Path("/proc").iterdir():
            if p.name.isdigit():
                try:
                    stat = (p / "stat").read_text().rsplit(")", 1)[1].split()
                    if int(stat[1]) == self.pid:
                        out.append(int(p.name))
                except (OSError, IndexError, ValueError):
                    pass
        return out

    def cpu_percent(self, seconds):
        hz = os.sysconf("SC_CLK_TCK")
        before = self.cpu_ticks()
        time.sleep(seconds)
        return (self.cpu_ticks() - before) / hz / seconds * 100


def busctl(*args, timeout=30):
    return subprocess.run(["busctl", "--user", "--json=short"] + list(args), capture_output=True, text=True, timeout=timeout)


def call(method, signature="", *args):
    r = busctl("call", MD, MD_PATH, MD_IF, method, signature, *args)
    if r.returncode != 0:
        return False, r.stderr.strip()
    try:
        data = json.loads(r.stdout)["data"]
    except (ValueError, KeyError):
        return True, None
    return True, data[0] if data else None


def lst(method):
    ok, text = call(method)
    return json.loads(text) if ok and text else None


def ctl(build, *args):
    return subprocess.run([str(Path(build) / "src/ctl/murasakictl")] + list(args), capture_output=True, text=True, timeout=60)


def store_files_ok(daemon):
    """The stores on disk parse, and nothing half-written is left beside them."""
    problems = []
    for name in ("profiles.json", "triggers.json", "timelines.json", "pets.json"):
        path = daemon.config / name
        if path.exists():
            try:
                json.loads(path.read_text())
            except ValueError as e:
                problems.append("%s: %s" % (name, e))
    for f in daemon.config.iterdir():
        if f.name.endswith((".tmp", ".new", ".part")) or ".tmp" in f.name:
            problems.append("left behind: %s" % f.name)
    return problems


def make_pet(daemon):
    from PIL import Image
    path = Path(os.environ["MURASAKI_SWEEP_TMP"]) / "pet.png"
    frames = [Image.new("RGBA", (40, 30), c) for c in ((255, 0, 0, 255), (0, 255, 0, 255))]
    frames[0].save(path, save_all=True, append_images=frames[1:], duration=100, loop=0, disposal=0)
    ok, item = call("AddLocalPetItem", "s", str(path))
    if not ok:
        raise RuntimeError("AddLocalPetItem: %s" % item)
    ok, pet = call("AddPet", "ssss", item, "", "DP-1", "{}")
    if not ok:
        raise RuntimeError("AddPet: %s" % pet)
    return item, pet


def trigger(name="t", source=None, action=None, **extra):
    d = {"name": name, "source": source or {"kind": "fire", "name": "go"}, "action": action or {"type": "hide-all"}}
    d.update(extra)
    return d


# ---- the cases ----

def case_hostile_triggers(ctx):
    deep = {}
    cur = deep
    for _ in range(300):
        cur["a"] = {}
        cur = cur["a"]
    bad = [
        (trigger(source={"kind": "exec"}), "source.kind"),
        (trigger(source={"kind": "screen-grab"}), "source.kind"),
        (trigger(action={"type": "run", "command": "ls"}), "action.type"),
        (trigger(action={"type": "open-url", "url": "https://example.org"}), "action.type"),
        (trigger(action={"type": "timeline"}), "action.id"),
        (trigger(source={"kind": "fire", "name": "../../etc/passwd"}), "name"),
        (trigger(source={"kind": "signal-file", "name": "a/b"}), "name"),
        (trigger(source={"kind": "time", "at": "25:61"}), "HH:MM"),
        (trigger(source={"kind": "time", "at": "10:00", "days": list(range(100))}), "days"),
        (trigger(source={"kind": "interval", "everyMinutes": 0}), "everyMinutes"),
        (trigger(source={"kind": "interval", "everyMinutes": 10 ** 12}), "everyMinutes"),
        (trigger(source={"kind": "dbus", "interface": "no spaces allowed", "member": "M"}), "D-Bus"),
        (trigger(cooldownMs=-5), "cooldownMs"),
        (trigger(cooldownMs=10 ** 15), "cooldownMs"),
        (trigger(cooldownMs="fast"), "cooldownMs"),
        (trigger(enabled="yes"), "enabled"),
        (trigger(mode="hold"), "shortcut"),
        (trigger(name=""), "name"),
        (trigger(name="x" * 5000), "name"),
        (trigger(source="fire"), "source"),
        (trigger(source=[1, 2, 3]), "source"),
        (trigger(action=7), "action"),
        (trigger(source={"kind": "fire", "name": "go", "extra": deep}), None),   # unknown keys are ignored: accepted, removed below
        ({}, "name"),
    ]
    refused, accepted = 0, 0
    before = lst("ListTriggers")
    for spec, words in bad:
        ok, text = call("SetTrigger", "ss", "", json.dumps(spec))
        if words is None:
            if ok:
                accepted += 1
                call("DeleteTrigger", "s", text)
            continue
        if ok:
            return False, "accepted: %s" % json.dumps(spec)[:120]
        if words not in text:
            return False, "refused without %r: %s -> %s" % (words, json.dumps(spec)[:80], text[:120])
        refused += 1
    if lst("ListTriggers") != before:
        return False, "the list changed"
    problems = store_files_ok(ctx["daemon"])
    return (not problems), "%d refused, %d tolerated; files: %s" % (refused, accepted, problems or "ok")


def case_hostile_timelines(ctx):
    action = lambda **k: k
    track = lambda actions, pet=None: {"pet": {"slot": 0} if pet is None else pet, "actions": actions}
    bad = [
        ({"name": "t", "tracks": [track([{"kind": "exec", "command": "ls"}])]}, "action 0"),
        ({"name": "t", "tracks": [track([{"kind": "wait", "ms": 10 ** 9}])]}, "ms"),
        ({"name": "t", "tracks": [track([{"kind": "wait"}])]}, "ms"),
        ({"name": "t", "tracks": [track([{"kind": "move", "x": 1e30, "y": 0}])]}, "100000"),
        ({"name": "t", "tracks": [track([{"kind": "move", "x": "a", "y": 0}])]}, "numbers"),
        ({"name": "t", "tracks": [track([{"kind": "move", "x": 0, "y": 0, "mode": "smooth", "durationMs": 10 ** 9}])]}, "durationMs"),
        ({"name": "t", "tracks": [track([], pet={})]}, "track 0"),
        ({"name": "t", "tracks": [track([], pet={"petId": "a", "slot": 1})]}, "exactly one"),
        ({"name": "t", "tracks": [track([{"kind": "wait", "ms": 3600000}] * 30)]}, "24 hours"),
        ({"name": "t", "speed": 0}, "speed"),
        ({"name": "t", "speed": 1e9}, "speed"),
        ({"name": "t", "restore": "yes"}, "restore"),
        ({"name": "t", "tracks": "none"}, "tracks"),
        ({"name": "x" * 1000}, "name"),
        ({"name": ""}, "name"),
    ]
    for spec, words in bad:
        ok, text = call("SetTimeline", "ss", "", json.dumps(spec))
        if ok:
            return False, "accepted: %s" % json.dumps(spec)[:120]
        if words not in text:
            return False, "refused without %r: %s" % (words, text[:160])
    problems = store_files_ok(ctx["daemon"])
    return (not problems), "%d refused; files: %s" % (len(bad), problems or "ok")


def case_hostile_profiles(ctx):
    pet = {"itemId": "local:x", "variant": "v", "output": "*"}
    bad = [
        ({"name": "p", "pets": {}}, "pets"),
        ({"name": "p", "pets": [3]}, "pet 0"),
        ({"name": "p", "pets": [dict(pet, width=1)]}, "width"),
        ({"name": "p", "pets": [dict(pet, slot=1), dict(pet, slot=1)]}, "slot"),
        ({"name": "p", "pets": [dict(pet, x="far")]}, "x"),
        ({"name": "p", "pets": [dict(pet, rotation=1e300)]}, "rotation"),
        ({"name": "p", "pets": [pet] * 65}, "pets"),
        ({"name": ""}, "name"),
    ]
    # profiles are made by SaveProfile (from the pets); a hostile profile can only arrive as a file, which the next case covers: here, the name rules
    for name in ("", "x" * 101):
        ok, text = call("SaveProfile", "s", name)
        if ok:
            return False, "SaveProfile accepted the name %r" % name[:20]
    return True, "names refused (the entries are only ever read from the file: see hostile_files)"


def clear_all():
    for method, delete in (("ListTriggers", "DeleteTrigger"), ("ListTimelines", "DeleteTimeline"), ("ListProfiles", "DeleteProfile")):
        for e in lst(method) or []:
            call(delete, "s", e["id"])


def case_caps(ctx):
    clear_all()
    notes = []
    # triggers
    ids = []
    for i in range(100):
        ok, r = call("SetTrigger", "ss", "", json.dumps(trigger(name="t%d" % i, source={"kind": "fire", "name": "n%d" % i})))
        if not ok:
            return False, "trigger %d refused: %s" % (i, r)
        ids.append(r)
    ok, r = call("SetTrigger", "ss", "", json.dumps(trigger(name="one more")))
    if ok or "at most" not in r:
        return False, "the 101st trigger: %s" % (r,)
    for i in ids:
        call("DeleteTrigger", "s", i)
    notes.append("100 triggers, the 101st refused")
    # timelines
    ids = []
    for i in range(100):
        ok, r = call("SetTimeline", "ss", "", json.dumps({"name": "l%d" % i}))
        if not ok:
            return False, "timeline %d refused: %s" % (i, r)
        ids.append(r)
    ok, r = call("SetTimeline", "ss", "", json.dumps({"name": "one more"}))
    if ok or "at most" not in r:
        return False, "the 101st timeline: %s" % (r,)
    for i in ids:
        call("DeleteTimeline", "s", i)
    notes.append("100 timelines, the 101st refused")
    ok, r = call("SetTimeline", "ss", "", json.dumps({"name": "tracks", "tracks": [{"pet": {"slot": i}, "actions": []} for i in range(17)]}))
    if ok or "16" not in r:
        return False, "17 tracks: %s" % (r,)
    ok, r = call("SetTimeline", "ss", "", json.dumps({"name": "actions", "tracks": [{"pet": {"slot": 0}, "actions": [{"kind": "hide"}] * 257}]}))
    if ok or "256" not in r:
        return False, "257 actions: %s" % (r,)
    notes.append("17 tracks and 257 actions refused")
    # profiles (a profile is the pets on the desktop: they are given names)
    for i in range(50):
        ok, r = call("SaveProfile", "s", "p%d" % i)
        if not ok:
            return False, "profile %d refused: %s" % (i, r)
    ok, r = call("SaveProfile", "s", "p-extra")
    if ok or "at most" not in r:
        return False, "the 51st profile: %s" % (r,)
    for p in lst("ListProfiles"):
        call("DeleteProfile", "s", p["id"])
    notes.append("50 profiles, the 51st refused")
    problems = store_files_ok(ctx["daemon"])
    return (not problems), "; ".join(notes) + "; files: " + (str(problems) if problems else "ok")


def case_hostile_files(ctx):
    daemon = ctx["daemon"]
    import uuid
    uid = lambda: str(uuid.uuid4())
    variants = {
        "not json": b"{{{ nope",
        "wrong version": json.dumps({"version": 9, "triggers": []}).encode(),
        "not an array": json.dumps({"version": 1, "triggers": {}}).encode(),
        "too many entries": json.dumps({"version": 1, "triggers": [trigger(name="t%d" % i, id=uid()) for i in range(101)]}).encode(),
        "an entry that runs a program": json.dumps({"version": 1, "triggers": [dict(trigger(), id=uid(), action={"type": "run", "command": "ls"})]}).encode(),
        "huge and nested": b'{"version":1,"triggers":' + b"[" * 100000 + b"]" * 100000 + b"}",
        "duplicate ids": (lambda i: json.dumps({"version": 1, "triggers": [dict(trigger(), id=i), dict(trigger(name="b"), id=i)]}).encode())(uid()),
    }
    daemon.stop()
    results = []
    for name, content in variants.items():
        for old in daemon.config.glob("triggers.json*"):
            old.unlink()
        (daemon.config / "triggers.json").write_bytes(content)
        daemon.start()
        listed = lst("ListTriggers")
        bad = list(daemon.config.glob("triggers.json.bad-*"))
        ok_saved, _ = call("SetTrigger", "ss", "", json.dumps(trigger()))
        daemon.stop()
        if listed != [] or len(bad) != 1 or (daemon.config / "triggers.json").exists() and not ok_saved:
            return False, "%s: listed=%s bad=%d saved=%s" % (name, str(listed)[:60], len(bad), ok_saved)
        results.append(name)
    for old in daemon.config.glob("triggers.json*"):
        old.unlink()
    daemon.start()
    return True, "%d files set aside: %s" % (len(results), ", ".join(results))


def case_fresh_install(ctx):
    # a daemon that has never been asked for anything: no store, no extra thread, no child, no CPU
    daemon = ctx["daemon"]
    daemon.stop()
    for n in ("profiles.json", "triggers.json", "timelines.json"):
        (daemon.config / n).unlink(missing_ok=True)
    daemon.start()
    time.sleep(1.5)
    threads, cpu = daemon.threads(), daemon.cpu_percent(3)
    present = [n for n in ("profiles.json", "triggers.json", "timelines.json") if (daemon.config / n).exists()]
    good = not present and not daemon.children() and cpu < 2
    return good, "threads=%d, cpu=%.2f %%, store files: %s, children: %s" % (threads, cpu, present or "none", daemon.children() or "none")


def case_storm(ctx):
    daemon = ctx["daemon"]
    ok, tid = call("SetTrigger", "ss", "", json.dumps(trigger(name="storm", source={"kind": "fire", "name": "storm"}, action={"type": "stop-sounds"}, cooldownMs=100)))
    if not ok:
        return False, tid
    threads_before = daemon.threads()
    out = Path(os.environ["MURASAKI_SWEEP_TMP"]) / "monitor.json"
    mon = subprocess.Popen(["busctl", "--user", "--json=short", "monitor", MD, "--match", "type='signal',member='TriggerFired'"], stdout=open(out, "w"), stderr=subprocess.DEVNULL, preexec_fn=die_with_parent)
    time.sleep(0.5)
    n = ctx["storm"]
    start = time.time()
    calls = 0
    for _ in range(n):
        call("FireTrigger", "s", "storm")
        calls += 1
    seconds = time.time() - start
    time.sleep(0.5)
    mon.terminate()
    ran = sum(1 for line in out.read_text().splitlines() if "TriggerFired" in line)
    ctx["storm_seconds"] = seconds
    limit = 20 * (seconds + 1)
    state = json.loads(call("TriggerState")[1])
    outcomes = {e["outcome"] for e in state["log"]}
    cpu = daemon.cpu_percent(2)
    call("DeleteTrigger", "s", tid)
    good = 0 < ran <= limit and cpu < 3 and daemon.threads() <= threads_before and ("cooldown" in outcomes or "rate limited" in outcomes)
    return good, "%d calls in %.1f s (%.0f/s): %d ran (limit %d), outcomes %s, cpu after %.2f %%, threads %d" % (calls, seconds, calls / seconds, ran, limit, sorted(outcomes), cpu, daemon.threads())


def case_soak(ctx):
    daemon = ctx["daemon"]
    clear_all()
    if not lst("ListPets"):
        ctx["item"], ctx["pet"] = make_pet(daemon)
    ok, a = call("SaveProfile", "s", "soak-a")
    if not ok:
        return False, a
    item, pet = ctx["item"], ctx["pet"]
    call("AddPet", "ssss", item, "", "DP-1", '{"x": 400}')
    ok, b = call("SaveProfile", "s", "soak-b")
    call("RemovePet", "s", lst("ListPets")[-1]["petId"])
    rss0 = daemon.rss_kib()
    ticks0 = daemon.cpu_ticks()
    start = time.time()
    for i in range(ctx["switches"]):
        ok, r = call("SwitchProfile", "s", "soak-a" if i % 2 == 0 else "soak-b")
        if not ok:
            return False, "switch %d: %s" % (i, r)
    seconds = time.time() - start
    rss1 = daemon.rss_kib()
    pets = lst("ListPets")
    for p in lst("ListProfiles"):
        call("DeleteProfile", "s", p["id"])
    problems = store_files_ok(daemon)
    growth = rss1 - rss0
    good = growth < 8192 and not problems and len(pets) in (1, 2)
    return good, "%d switches in %.1f s, RSS %d -> %d KiB (%+d), cpu %.1f s, files: %s" % (ctx["switches"], seconds, rss0, rss1, growth, (daemon.cpu_ticks() - ticks0) / os.sysconf("SC_CLK_TCK"), problems or "ok")


def case_no_children(ctx):
    kids = ctx["daemon"].children()
    return not kids, "children of the daemon: %s" % (kids or "none")


# ---- screen triggers: Triggers packs with captured frames (.npz), through the real import and ImportPackAutomation ----

def npy_bytes(frames, h, w, descr="|u1", fortran=False, shape=None, version=(1, 0), data=None, header_pad=0):
    """A .npy of [frames, h, w, 3] uint8 written the way NumPy writes it, with the knobs a hostile file turns."""
    import struct
    shape = shape if shape is not None else "(%d, %d, %d, 3)" % (frames, h, w)
    d = "{'descr': '%s', 'fortran_order': %s, 'shape': %s, }" % (descr, fortran, shape)
    d += " " * header_pad
    while (10 + len(d) + 1) % 64:
        d += " "
    d += "\n"
    if version == (1, 0):
        head = b"\x93NUMPY\x01\x00" + struct.pack("<H", len(d)) + d.encode()
    else:
        head = b"\x93NUMPY" + bytes(version) + struct.pack("<I", len(d)) + d.encode()
    if data is None:
        data = bytes((f * 20 + x + y) & 255 for f in range(frames) for y in range(h) for x in range(w) for _ in range(3))
    return head + data


def npz_bytes(members):
    import io
    import zipfile
    b = io.BytesIO()
    with zipfile.ZipFile(b, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in members.items():
            z.writestr(name, data)
    return b.getvalue()


def screen_mdaa(path, files, regions=1, ref_mode="anchor", extra_match=None):
    import io
    import json as j
    import zipfile
    from PIL import Image
    gif = io.BytesIO()
    fs = [Image.new("RGBA", (24, 20), c) for c in ((255, 0, 0, 255), (0, 0, 255, 255))]
    fs[0].save(gif, "GIF", save_all=True, append_images=fs[1:], duration=80, loop=0)
    match = {"mode": "ORB_HASH", "threshold": 0.8, "cooldown_ms": 600, "stop_grace_ms": 250, "ref_mode": ref_mode, "ref_frame_index": 2, "seq_len": 3, "seq_step": 2}
    match.update(extra_match or {})
    pack = {"id": "sweep", "name": "Sweep", "regions": [{"id": "r%d" % i, "kind": "rect", "x": 10 + i * 5, "y": 20, "w": 100, "h": 60} for i in range(regions)],
            "event": {"name": "e", "fps": 10, "frame_count": 8, "start_index": 0, "end_index": None}, "match": match,
            "animations": [{"id": "t1", "name": "Hello", "asset_path": "walk.gif", "start_visible": False, "actions": [{"t_ms": 0, "kind": "show"}, {"t_ms": 500, "kind": "hide"}]}]}
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("pack.json", j.dumps(pack))
        z.writestr("walk.gif", gif.getvalue())
        for name, data in files.items():
            z.writestr(name, data)


def import_record(newest_after):
    """The record (source.json) of the newest imported package folder made after `newest_after`."""
    root = Path(os.environ["XDG_DATA_HOME"]) / "murasaki" / "pets" / "imported"
    folders = [f for f in root.glob("*") if f.is_dir() and not f.name.startswith(".") and (f / "source.json").exists() and f.stat().st_mtime >= newest_after]
    if not folders:
        return None, None
    folder = max(folders, key=lambda f: f.stat().st_mtime)
    try:
        return folder, json.loads((folder / "source.json").read_text())
    except (OSError, ValueError):
        return folder, None


def case_screen_packs(ctx):
    d = ctx["daemon"]
    build = d.build
    tmp = Path(os.environ["MURASAKI_SWEEP_TMP"])
    good = npy_bytes(8, 24, 32)
    big_header = npy_bytes(2, 4, 4, header_pad=70000, version=(2, 0))
    cases = [
        ("valid", {"frames/region0.npz": npz_bytes({"frames.npy": good})}, 1, "ready", None),
        ("valid-v2", {"frames/region0.npz": npz_bytes({"frames.npy": npy_bytes(3, 8, 8, version=(2, 0))})}, 1, "ready", None),
        ("object-dtype", {"frames/region0.npz": npz_bytes({"frames.npy": npy_bytes(2, 4, 4, descr="|O")})}, 1, "npy: dtype not uint8", None),
        ("float-dtype", {"frames/region0.npz": npz_bytes({"frames.npy": npy_bytes(2, 4, 4, descr="<f4")})}, 1, "npy: dtype not uint8", None),
        ("fortran", {"frames/region0.npz": npz_bytes({"frames.npy": npy_bytes(2, 4, 4, fortran=True)})}, 1, "npy: fortran order", None),
        ("three-dimensions", {"frames/region0.npz": npz_bytes({"frames.npy": npy_bytes(2, 4, 4, shape="(2, 4, 12)", data=b"\0" * 96)})}, 1, "npy: shape", None),
        ("huge-shape", {"frames/region0.npz": npz_bytes({"frames.npy": npy_bytes(9, 4096, 4096, data=b"")})}, 1, "npy: over the size limit", None),
        ("too-many-frames", {"frames/region0.npz": npz_bytes({"frames.npy": npy_bytes(2000, 2, 2, data=b"")})}, 1, "npy: over the size limit", None),
        ("truncated", {"frames/region0.npz": npz_bytes({"frames.npy": good[:-500]})}, 1, "npy: truncated", None),
        ("trailing-data", {"frames/region0.npz": npz_bytes({"frames.npy": good + b"extra bytes"})}, 1, "npy: data after the array", None),
        ("huge-header", {"frames/region0.npz": npz_bytes({"frames.npy": big_header})}, 1, "npy: header too large", None),
        ("future-version", {"frames/region0.npz": npz_bytes({"frames.npy": npy_bytes(2, 4, 4, version=(9, 0))})}, 1, "npy: version not supported", None),
        ("not-npy", {"frames/region0.npz": npz_bytes({"frames.npy": b"not an array at all"})}, 1, "npy: not an npy file", None),
        ("pickle", {"frames/region0.npz": b"PK\x03\x04 cos\nsystem\n(S'id'\ntR."}, 1, "npz: not a zip", None),
        ("no-frames-member", {"frames/region0.npz": npz_bytes({"other.npy": good})}, 1, "npz: no frames array", None),
        ("two-files-one-region", {"frames/region0.npz": npz_bytes({"frames.npy": good}), "frames/region_0.npz": npz_bytes({"frames.npy": good})}, 1, "screen refs: two frames files", None),
        ("unknown-region", {"frames/region7.npz": npz_bytes({"frames.npy": good})}, 1, "screen refs:", None),
        ("no-frames-file", {}, 1, "screen refs: no frames for the region", None),
    ]
    before_rss = d.rss_kib()
    failures = []
    ready = 0
    for name, files, regions, expect, extra in cases:
        started = time.time() - 1
        path = tmp / ("screen-%s.mdaa" % name)
        screen_mdaa(path, files, regions)
        r = ctl(build, "pets", "import", "mdaa", str(path))
        if r.returncode != 0:
            failures.append("%s: import failed: %s" % (name, r.stderr.strip()[:100]))
            continue
        item = r.stdout.strip()
        folder, record = import_record(started)
        status = (((record or {}).get("mdaa") or {}).get("screenRefs") or {}).get("status", "")
        if not status.startswith(expect):
            failures.append("%s: status %r, expected %r (folder %s, record keys %s)" % (name, status, expect, folder, sorted(((record or {}).get("mdaa") or record or {}).keys())))
        made = ctl(build, "timeline", "from-pack", item)
        if made.returncode != 0:
            failures.append("%s: from-pack failed: %s" % (name, made.stderr.strip()[:100]))
            continue
        triggers = lst("ListTriggers") or []
        screens = [t for t in triggers if (t.get("source") or {}).get("kind") == "screen" and t.get("name", "").startswith("Sweep")]
        if not screens:
            failures.append("%s: no screen trigger made" % name)
            continue
        screen = screens[-1]
        kept = Path(os.environ["XDG_DATA_HOME"]) / "murasaki" / "triggers" / screen["id"]
        pictures = sorted(p.name for p in kept.glob("region*/step*.png")) if kept.exists() else []
        notes = made.stderr
        if expect == "ready":
            ready += 1
            if not pictures or "screen: ready" not in notes:
                failures.append("%s: ready but pictures=%s notes=%r" % (name, pictures, notes[:120]))
        elif pictures:
            failures.append("%s: refused but pictures were kept: %s" % (name, pictures))
        if screen.get("enabled"):
            failures.append("%s: an imported screen trigger must start disabled" % name)
        ctl(build, "triggers", "remove", screen["id"])
        ctl(build, "pets", "import", "remove", item)
    grew = d.rss_kib() - before_rss
    if grew > 60 * 1024:
        failures.append("the daemon grew by %d MiB" % (grew // 1024))
    # a pack's screen trigger must never fire or watch anything by itself: no child process, nothing in the state
    if d.children():
        failures.append("the daemon has a child process: %s" % d.children())
    state = lst_state()
    return not failures, "; ".join(failures) if failures else "%d packs (%d ready, %d refused by name), daemon +%d MiB, no child" % (len(cases), ready, len(cases) - ready, max(0, grew // 1024))


def lst_state():
    ok, text = call("TriggerState")
    return json.loads(text) if ok and text else None


CASES = [("fresh_install", case_fresh_install), ("hostile_triggers", case_hostile_triggers), ("hostile_timelines", case_hostile_timelines),
         ("hostile_profiles", case_hostile_profiles), ("caps", case_caps), ("hostile_files", case_hostile_files), ("storm", case_storm),
         ("soak", case_soak), ("screen_packs", case_screen_packs), ("no_children", case_no_children)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", default=str(ROOT / "build" / "ci"))
    ap.add_argument("--name", default=time.strftime("%Y%m%d-%H%M%S"))
    ap.add_argument("--storm", type=int, default=1500)
    ap.add_argument("--switches", type=int, default=300)
    ap.add_argument("--only", nargs="*")
    args = ap.parse_args()
    if os.environ.get("MURASAKI_SWEEP_BUS") != "1":
        return reexec_under_private_bus(sys.argv[1:])
    refuse_unless_private()
    daemon = Daemon(args.build_dir)
    daemon.start()
    ctx = {"daemon": daemon, "storm": args.storm, "switches": args.switches}
    ctx["item"], ctx["pet"] = make_pet(daemon)
    results = []
    try:
        for name, fn in CASES:
            if args.only and name not in args.only:
                continue
            started = time.time()
            try:
                ok, detail = fn(ctx)
            except Exception as e:  # a crashing case is a failed case
                ok, detail = False, "exception: %r" % (e,)
            if daemon.proc is None or daemon.proc.poll() is not None:
                ok, detail = False, detail + " (the daemon is not running any more)"
                daemon.start()
            results.append({"case": name, "ok": ok, "detail": detail, "seconds": round(time.time() - started, 1)})
            print("%-18s %s  %s" % (name, "ok  " if ok else "FAIL", detail), flush=True)
    finally:
        daemon.stop()
    out = CACHE / "compat" / ("automation-" + args.name)
    out.mkdir(parents=True, exist_ok=True)
    (out / "results.json").write_text(json.dumps(results, indent=2))
    lines = ["# Automation sweep %s" % args.name, "", "| case | result | seconds | detail |", "|---|---|---|---|"]
    lines += ["| %s | %s | %s | %s |" % (r["case"], "ok" if r["ok"] else "FAIL", r["seconds"], r["detail"].replace("|", "/")) for r in results]
    (out / "report.md").write_text("\n".join(lines) + "\n")
    print("written to %s" % out)
    return 0 if all(r["ok"] for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
