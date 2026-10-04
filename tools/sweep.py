#!/usr/bin/env python3
"""Render every parameter at both ends of its range, in both plugins, and fail
if any made no difference.

**This is the only thing in the repo that catches a dead control.** A GLSL
uniform whose name does not match the C++ is silently ignored --
`glGetUniformLocation` returns -1 and `glUniform` on -1 is a documented no-op
-- so a slider can be stone dead while everything compiles, links, loads and
renders.

Each control is swept where it can act (the CONTEXT column): Catalyst and
Break Wave in the BZ reaction (the default), the wave controls once a wave
exists, Clock Sync on the iodine clock with a dose due inside the render,
Light Coupling with Ru(bpy)3 through the Over, the shakes on the blue bottle,
the audio controls with a beat fed in. An option parameter reads back
0..1 whatever its count (the fleet's trap), so options are set here by
element index. A control whose ends differ by less than `--floor` (mean
8-bit difference per channel) is reported as barely alive.

Every render is 320x180 (CI's raster) and 160 frames at 60 fps: 2.7 s of
host time, 80 chemical seconds at the default 30x.

The context table was emptied once, and what went dead is recorded in
AGENTS.md ("The sweep's context table").

Usage::

    tools/sweep.py [--build BUILD_DIR] [--verbose] [--jobs N]
"""

import argparse
import concurrent.futures
import pathlib
import re
import subprocess
import sys
import tempfile
import zlib

REPO = pathlib.Path(__file__).resolve().parent.parent

# name -> (low setting, high setting, context settings, extra flags)
#
# The default render is 80 chemical seconds of a fresh BZ dish at 30x, which is
# before its pacemakers have made a target: controls that act on waves (a
# Drop's wave, a Break Wave, a Seed's pacemakers, Stir, Detail) are swept at
# Time-lapse 0.8 (96x: 256 chemical seconds), and a Break Wave is pressed at
# frame 100, once the drop's wave exists ("Name=V@F"). Shakes are swept on
# the blue bottle, where a shake turns the layer blue.
LATER = "Time-lapse=0.8"
SWEEP = {
    "Reaction": ("0", "7", [], []),
    "Catalyst": ("0", "2", [], []),
    # Flow washes the clock's iodine out; a Batch dish keeps it.
    "Reactor": ("0", "1", ["Reaction=2", "Reductant=0", "Time-lapse=1"], []),
    # A Reset takes the next soup of pacemakers.
    "Reset": ("0", "1", [LATER], []),
    "Seed": ("1", "2", [LATER], []),
    "Oxidant": ("0.2", "0.8", [], []),
    "Acid or Base": ("0.2", "0.8", [], []),
    "Reductant": ("0.2", "0.8", [], []),
    "Indicator": ("0.2", "0.8", [], []),
    "Vessel": ("0", "1", [], []),
    "Dish Width": ("0.2", "0.8", [], []),
    "Depth": ("0.2", "0.8", [], []),
    "Stir": ("0", "1", [LATER], []),
    "Time-lapse": ("0.2", "0.9", [], []),
    "Detail": ("0", "3", [LATER], []),
    # A 20 mm drop in a 28 mm dish: the wave covers the picture, on the Over's
    # dark card too.
    "Drop": ("0", "1", ["Drop Position=1", "Drop Size=1", "Dish Width=0.3", LATER], []),
    "Drop Size": ("0", "1", ["Drop=1", "Drop Position=1", "Dish Width=0.3", LATER], []),
    "Drop Position": ("0", "1", ["Drop=1", "Drop Size=1", "Dish Width=0.3", LATER], []),
    "Break Wave": ("0@100", "1@100", ["Drop=1", "Drop Position=1", "Drop Size=1", LATER], []),
    "Shake": ("0", "1", ["Reaction=5"], []),
    "Auto Drop": ("0", "1", ["Drop Position=1"], []),
    "Audio Drops": ("0", "1", ["Drop Position=1"], ["beat"]),
    "Audio Shakes": ("0", "1", ["Reaction=5"], ["beat"]),
    # The clock switches at 25 chemical seconds and the synced dose clears it.
    "Clock Sync": ("0", "2", ["Reaction=2"], []),
    "Light Coupling": ("0", "1", ["Catalyst=1", "Drop=1", "Drop Position=1", "Drop Size=1", LATER], []),
    "Lightbox": ("0", "2", [], []),
    "Exposure": ("0.2", "0.8", [], []),
    "Seed From Clip": ("0", "1", [], []),
    "Mix": ("0", "1", [], []),
    # BZ only (the default reaction): f 1 (fast bulk oscillation) against f 4
    # (excitable, quiet until a drop) over 256 chemical seconds.
    "Excitability": ("0", "1", [LATER], []),
}

# Parameters with no pixel to sweep: the FFT buffer (its float is meaningless;
# Audio Drops and Audio Shakes are its sweepable proof) and the About block.
SKIP = {"Audio", "About", "Project page", "Source on GitHub", "Support the work", "User guide"}


def read_png(path):
    """Enough of PNG for hdtest's own writer: 8-bit RGBA, filter 0 rows."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, width, height, idat = 8, 0, 0, b""
    while pos < len(data):
        length = int.from_bytes(data[pos:pos + 4], "big")
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(body[0:4], "big")
            height = int.from_bytes(body[4:8], "big")
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    for y in range(height):
        start = y * (stride + 1)
        out += raw[start + 1:start + 1 + stride]
    return bytes(out)


def difference(a, b):
    if len(a) != len(b):
        return 255.0
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def render(hdtest, out, settings, extra, effect):
    args = [str(hdtest), "--out", str(out), "--size", "320x180", "--frames", "160"]
    if effect:
        args.append("--over")
    if "beat" in extra:
        args.append("--beat")
    for setting in settings:
        args += ["--set", setting]
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"hdtest failed: {' '.join(args)}\n{result.stderr.strip()}")
    return read_png(out)


def parameters(hdtest, effect):
    result = subprocess.run([str(hdtest), "--list"] + (["--over"] if effect else []), capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"hdtest --list failed: {result.stderr.strip()}")
    names = []
    for line in result.stdout.splitlines()[1:]:
        parts = re.split(r"\s{2,}", line.strip())
        if len(parts) >= 3 and parts[0].isdigit():
            names.append(parts[1].strip())
    return names


def sweep_one(hdtest, scratch, effect, name, declared):
    low, high, context, extra = SWEEP[name]
    # A context names controls of either plugin; each keeps its own.
    context = [c for c in context if c.split("=")[0] in declared]
    tag = f"{'o' if effect else 's'}{abs(hash(name))}"
    a = scratch / f"{tag}-a.png"
    b = scratch / f"{tag}-b.png"
    before = render(hdtest, a, context + [f"{name}={low}"], extra, effect)
    after = render(hdtest, b, context + [f"{name}={high}"], extra, effect)
    return effect, name, difference(before, after)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=pathlib.Path, default=REPO / "build")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--floor", type=float, default=0.05,
                        help="mean 8-bit difference below which a control is 'barely alive'")
    parser.add_argument("--no-context", action="store_true",
                        help="empty the context table once, to see what goes dead (AGENTS.md)")
    args = parser.parse_args()

    build = args.build if args.build.is_absolute() else REPO / args.build
    hdtest = build / "hdtest"
    if not hdtest.exists():
        print(f"{hdtest} not found", file=sys.stderr)
        return 1
    if args.no_context:
        for name in SWEEP:
            low, high, _, extra = SWEEP[name]
            SWEEP[name] = (low, high, [], extra)

    jobs, names = [], {}
    for effect in (False, True):
        declared = parameters(hdtest, effect)
        names[effect] = set(declared)
        unknown = [n for n in declared if n not in SWEEP and n not in SKIP]
        if unknown:
            # A new parameter with no sweep is a hole, not a pass.
            print(f"no sweep defined for: {', '.join(unknown)}", file=sys.stderr)
            return 1
        jobs += [(effect, n) for n in declared if n in SWEEP]
    unused = [n for n in SWEEP if not any(n == j[1] for j in jobs)]
    if unused:
        print(f"the sweep names parameters neither plugin has: {', '.join(unused)}", file=sys.stderr)
        return 1

    dead, weak = [], []
    with tempfile.TemporaryDirectory() as scratch:
        scratch = pathlib.Path(scratch)
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            results = list(pool.map(lambda j: sweep_one(hdtest, scratch, j[0], j[1], names[j[0]]), jobs))
    for effect, name, delta in results:
        who = "SW Honeydew Over" if effect else "SW Honeydew"
        if delta == 0.0:
            dead.append(f"{who}: {name}")
            print(f"  DEAD {who:17s} {name:16s} both ends identical")
        elif delta < args.floor:
            weak.append(f"{who}: {name}")
            print(f"  WEAK {who:17s} {name:16s} mean delta {delta:.4f}")
        elif args.verbose:
            print(f"  ok   {who:17s} {name:16s} mean delta {delta:.3f}")

    print(f"{len(results)} parameters swept over both plugins, {len(dead)} dead, {len(weak)} barely alive")
    if dead or weak:
        print("\nA parameter that changes nothing is usually a uniform name that does not match\n"
              "the C++, or a setting nothing reads. Both are silent everywhere else.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
