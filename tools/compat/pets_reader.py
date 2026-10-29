#!/usr/bin/env python3
"""The independent reader of the pets sweep (pets_sweep.py): what an animation file says by itself, and Pillow's decode of its frames. It runs in a
process of its own (`python3 -I pets_reader.py OP ...`, with a time limit and a memory limit set by the sweep), so that a hostile or broken item can
neither hang the sweep nor take it down. One JSON object on stdout.

  animated FILE               {"animated": true} for a PNG that is an APNG (acTL before the first IDAT), whatever its name
  header FILE                 frame count, summed delays, canvas (APNG and WebP from their chunks, GIF through Pillow)
  compare FILE PICKED.json    PICKED is {role: {"index": N, "file": OURS.png}}: the mean and largest difference of each of our frames from Pillow's
                              decode of the same frame, as premultiplied RGBA
"""
import json
import struct
import sys
import zlib


# The pets' one rule for a frame delay, the same for GIF, APNG and WebP (src/core/petlimits.h, petFrameDelayMs, docs/pets/timing.md): a delay of
# 10 ms or less, which is what encoders write for "as fast as you can", plays for 100 ms. Browsers do this for GIF; the other two formats follow it by our choice.
TINY_DELAY_MS = 10.0
TINY_DELAY_MARGIN_MS = 0.001  # as in petlimits.h: a delay computed from a timestamp is 10.000000000000002 where the file says 10
TINY_DELAY_REPLACEMENT_MS = 100.0


def pet_delay_ms(raw_ms):
    return raw_ms if raw_ms > TINY_DELAY_MS + TINY_DELAY_MARGIN_MS else TINY_DELAY_REPLACEMENT_MS


def apng_header(path):
    """The frame count, the summed delays (ms, by the pets' rule for tiny delays: `raw_duration_ms` is the sum as the file writes them and `tiny_delays` the frames the rule changed), the canvas and the frame count the file declares (acTL) of an APNG, from
    its chunks alone: the chunk headers are read and the data seeked over (the small chunks that matter are read whole and their CRC checked), so a
    300 MB file costs a few hundred thousand small reads and no pixels. None when it is not an APNG. `problems` names what is wrong with the chunks
    (no IEND, a chunk longer than the file, a fcTL of the wrong size, a bad CRC); `default_image_hidden` is true when the first IDAT comes before
    the first fcTL, which makes the default image no part of the animation (Pillow counts it as a frame, FFmpeg and the spec do not)."""
    frames, total, raw_total, tiny, width, height, plays, declared = 0, 0.0, 0.0, 0, 0, 0, None, None
    problems, hidden, idat_seen, ended = [], False, False, False
    with open(path, "rb") as f:
        size = f.seek(0, 2)
        f.seek(0)
        if f.read(8) != b"\x89PNG\r\n\x1a\n":
            return None
        while True:
            head = f.read(8)
            if len(head) < 8:
                break
            length, kind = struct.unpack(">I4s", head)
            if length > 0x7FFFFFFF or f.tell() + length + 4 > size:
                problems.append(f"{kind.decode('latin-1')} claims {length} bytes and the file ends first")
                break
            if kind in (b"IHDR", b"acTL", b"fcTL") and length <= 64:
                data = f.read(length)
                crc = struct.unpack(">I", f.read(4))[0]
                if zlib.crc32(kind + data) & 0xFFFFFFFF != crc:
                    problems.append(f"bad CRC in {kind.decode()}")
                if kind == b"IHDR" and length == 13:
                    width, height = struct.unpack(">II", data[:8])
                elif kind == b"acTL" and length == 8:
                    if idat_seen:
                        problems.append("acTL after the first IDAT: the file is a still picture (the specification ignores it)")
                    else:
                        declared, plays = struct.unpack(">II", data)
                elif kind == b"fcTL":
                    if length != 26:
                        problems.append(f"fcTL of {length} bytes (26 expected)")
                    else:
                        delay_num, delay_den = struct.unpack(">HH", data[20:24])
                        raw = 1000.0 * delay_num / (delay_den or 100)
                        raw_total += raw
                        total += pet_delay_ms(raw)
                        tiny += raw <= TINY_DELAY_MS + TINY_DELAY_MARGIN_MS
                        if frames == 0 and idat_seen:
                            hidden = True
                        frames += 1
                elif kind != b"IHDR":
                    problems.append(f"{kind.decode()} of {length} bytes")
            else:
                if kind == b"IDAT":
                    idat_seen = True
                f.seek(length + 4, 1)
            if kind == b"IEND":
                ended = True
                break
    if plays is None:
        return None  # a still PNG
    if not ended:
        problems.append("no IEND: the file is cut short")
    return {"frames": frames, "declared_frames": declared, "duration_ms": total, "raw_duration_ms": raw_total, "tiny_delays": tiny, "width": width, "height": height, "loops": plays,
            "default_image_hidden": hidden, "problems": problems}


def webp_header(path):
    """Frame count, summed delays (by the pets' rule) and canvas of an animated WebP from its RIFF chunks (the ANMF chunks hold each frame's delay;
    Pillow fills in `info["duration"]` only while it loads a frame's pixels, which this sweep does not do here). None when the file has no ANMF
    chunk (a still WebP), so the caller falls back to Pillow."""
    frames, total, raw_total, tiny, width, height = 0, 0.0, 0.0, 0, 0, 0
    with open(path, "rb") as f:
        size = f.seek(0, 2)
        f.seek(0)
        head = f.read(12)
        if len(head) < 12 or head[:4] != b"RIFF" or head[8:12] != b"WEBP":
            return None
        pos = 12
        while pos + 8 <= size:
            f.seek(pos)
            kind, length = struct.unpack("<4sI", f.read(8))
            if pos + 8 + length > size:
                break
            if kind == b"VP8X" and length >= 10:
                data = f.read(10)
                width = int.from_bytes(data[4:7], "little") + 1
                height = int.from_bytes(data[7:10], "little") + 1
            elif kind == b"ANMF" and length >= 16:
                data = f.read(16)
                raw = float(int.from_bytes(data[12:15], "little"))
                raw_total += raw
                total += pet_delay_ms(raw)
                tiny += raw <= TINY_DELAY_MS + TINY_DELAY_MARGIN_MS
                frames += 1
            pos += 8 + length + (length & 1)
    if frames == 0:
        return None
    return {"frames": frames, "duration_ms": total, "raw_duration_ms": raw_total, "tiny_delays": tiny, "width": width, "height": height}


def pillow_header(path):
    """Frame count, summed delays (by the same rule) and canvas of a GIF or WebP as Pillow reads them, one frame at a time (the sweep's limits on this process bound
    what a big or hostile file can cost)."""
    from PIL import Image

    with Image.open(path) as im:
        n = getattr(im, "n_frames", 1)
        total, raw_total, tiny = 0.0, 0.0, 0
        for i in range(n):
            im.seek(i)
            raw = float(im.info.get("duration", 0) or 0)
            raw_total += raw
            total += pet_delay_ms(raw)
            tiny += raw <= TINY_DELAY_MS + TINY_DELAY_MARGIN_MS
        return {"frames": n, "duration_ms": total, "raw_duration_ms": raw_total, "tiny_delays": tiny, "width": im.size[0], "height": im.size[1]}


def file_header(path):
    suffix = str(path).lower().rsplit(".", 1)[-1]
    try:
        if suffix in ("apng", "png"):
            return apng_header(path) or pillow_header(path)
        if suffix == "webp":
            return webp_header(path) or pillow_header(path)
        return pillow_header(path)
    except Exception as error:  # a broken file is a finding, not a crash
        return {"error": f"{type(error).__name__}: {str(error)[:120]}"}


def pillow_frames(path, indexes):
    """The frames at `indexes`, as premultiplied RGBA images: Pillow's own decode, composited, in one forward pass."""
    from PIL import Image

    out = {}
    with Image.open(path) as im:
        # Pillow numbers an APNG's default image as frame 0 when it is no part of the animation: ours are the animation's frames
        shift = 1 if im.info.get("default_image") else 0
        for i in sorted(set(indexes)):
            im.seek(i + shift)
            out[i] = im.convert("RGBA").convert("RGBa")
    return out


def pixel_difference(ours_png, theirs):
    """(mean, max) absolute difference over the four channels of two premultiplied images; None when the sizes differ."""
    from PIL import Image, ImageChops, ImageStat

    with Image.open(ours_png) as picture:
        ours = picture.convert("RGBA").convert("RGBa")
    if ours.size != theirs.size:
        return None
    diff = ImageChops.difference(ours, theirs)
    stat = ImageStat.Stat(diff)
    return round(sum(stat.mean) / len(stat.mean), 4), max(hi for _, hi in diff.getextrema())


SPEC_MAX_CHUNK = 256 << 20     # the compositor holds one chunk in memory at a time
SPEC_MAX_SIDE = 16384          # canvas limits of the hand compositor (the pets' own are smaller: petlimits.h)
SPEC_MAX_PIXELS = 100_000_000


def spec_frames(path, indexes):
    """The frames at `indexes` of an APNG, composited by a hand written compositor of the APNG specification (dispose none, background, previous;
    blend source and over), as premultiplied RGBA: a third opinion for when our decoder and Pillow's disagree. Only plain PNG decoding is Pillow's,
    each frame as a PNG of its own; one frame's data, the canvas and the saved canvas of dispose-previous are in memory at a time."""
    import io
    import zlib
    from PIL import Image

    wanted = set(indexes)
    out = {}
    with open(path, "rb") as f:
        file_size = f.seek(0, 2)
        f.seek(8)
        width = height = 0
        rest = b""
        canvas = saved = None
        cursor = {"n": -1, "frame": None, "previous": None}

        def png_chunk(kind, data):
            return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

        def draw(frame, n):
            nonlocal canvas, saved
            previous = cursor["previous"]
            if n > 0 and previous is not None:
                if previous["dispose"] == 1:
                    canvas.paste((0, 0, 0, 0), (previous["x"], previous["y"], previous["x"] + previous["w"], previous["y"] + previous["h"]))
                elif previous["dispose"] == 2 and saved is not None:
                    canvas = saved.copy()
            if frame["dispose"] == 2:
                saved = canvas.copy()
            png = (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">II", frame["w"], frame["h"]) + rest) + png_chunk(b"IDAT", b"".join(frame["data"]))
                   + png_chunk(b"IEND", b""))
            with Image.open(io.BytesIO(png)) as picture:
                image = picture.convert("RGBA")
            if frame["blend"] == 0:
                canvas.paste(image, (frame["x"], frame["y"]))
            else:
                layer = Image.new("RGBA", (width, height), (0, 0, 0, 0))
                layer.paste(image, (frame["x"], frame["y"]))
                canvas = Image.alpha_composite(canvas, layer)
            if n in wanted:
                out[n] = canvas.convert("RGBa")
            cursor["previous"] = frame

        def finish():
            frame = cursor["frame"]
            if frame is not None:
                draw(frame, cursor["n"])
                cursor["frame"] = None

        while len(out) < len(wanted):
            head = f.read(8)
            if len(head) < 8:
                break
            length, kind = struct.unpack(">I4s", head)
            if length > SPEC_MAX_CHUNK or f.tell() + length + 4 > file_size:
                raise ValueError(f"{kind.decode('latin-1')} chunk of {length} bytes is over the limit or past the end of the file")
            data = f.read(length)
            f.read(4)
            if kind == b"IHDR":
                if len(data) != 13:
                    raise ValueError("IHDR of the wrong size")
                width, height = struct.unpack(">II", data[:8])
                if not (0 < width <= SPEC_MAX_SIDE and 0 < height <= SPEC_MAX_SIDE and width * height <= SPEC_MAX_PIXELS):
                    raise ValueError(f"canvas {width}x{height} is over the limit")
                rest = data[8:]
                canvas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
            elif kind == b"fcTL":
                finish()
                if len(data) != 26 or canvas is None:
                    raise ValueError("fcTL of the wrong size, or before the IHDR")
                _, w, h, x, y, _, _, dispose, blend = struct.unpack(">IIIIIHHBB", data)
                if not (0 < w <= width and 0 < h <= height):
                    raise ValueError(f"frame {w}x{h} does not fit the canvas")
                cursor["n"] += 1
                cursor["frame"] = {"w": w, "h": h, "x": x, "y": y, "dispose": dispose, "blend": blend, "data": []}
            elif kind == b"IDAT" and cursor["frame"] is not None:
                cursor["frame"]["data"].append(data)
            elif kind == b"fdAT" and cursor["frame"] is not None:
                cursor["frame"]["data"].append(data[4:])
            elif kind == b"IEND":
                break
        if len(out) < len(wanted):
            finish()
    return out


def compare(path, picked):
    """Our frames against Pillow's. Where they differ in an APNG, a third opinion (spec_frames) decides which one is wrong: a role then carries
    `spec_max` (our difference from the specification's) and `pillow_wrong` when we agree with the specification and Pillow does not."""
    try:
        theirs = pillow_frames(path, [v["index"] for v in picked.values()])
        pixels = {}
        for role, entry in picked.items():
            diff = pixel_difference(entry["file"], theirs[entry["index"]]) if entry.get("file") else None
            pixels[role] = {"index": entry["index"], "mean": diff[0] if diff else None, "max": diff[1] if diff else None}
        apng = False
        with open(path, "rb") as f:
            apng = f.read(8) == b"\x89PNG\r\n\x1a\n"
        differing = {r: p for r, p in pixels.items() if p["max"] is None or p["max"] > 4}
        if apng and differing:
            try:
                reference = spec_frames(path, [p["index"] for p in differing.values()])
            except Exception as error:  # no third opinion for a file it cannot read: the difference stands as it is
                reference = {}
                for p in differing.values():
                    p["spec_error"] = f"{type(error).__name__}: {str(error)[:100]}"
            for role, p in differing.items():
                entry = picked[role]
                mine = pixel_difference(entry["file"], reference[p["index"]]) if p["index"] in reference and entry.get("file") else None
                if mine is not None:
                    p["spec_max"] = mine[1]
                    p["pillow_wrong"] = mine[1] <= 4
        return {"pixels": pixels}
    except Exception as error:
        return {"error": f"{type(error).__name__}: {str(error)[:100]}"}


def is_animated_png(path):
    """True for a PNG whose chunks put an acTL before the first IDAT: an APNG, whatever its file name says (Anima Engine plays a `.png` such as
    that as an animation). Chunk headers only."""
    try:
        header = apng_header(path)
    except Exception:
        return False
    return header is not None and header.get("declared_frames") is not None


def main(argv):
    if len(argv) >= 3 and argv[1] == "animated":
        print(json.dumps({"animated": is_animated_png(argv[2])}))
    elif len(argv) >= 3 and argv[1] == "header":
        print(json.dumps(file_header(argv[2])))
    elif len(argv) >= 4 and argv[1] == "compare":
        print(json.dumps(compare(argv[2], json.loads(argv[3]))))
    else:
        print(json.dumps({"error": "usage: pets_reader.py header FILE | compare FILE PICKED"}))
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
