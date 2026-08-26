"""Every parameter must actually change the picture.

A GLSL uniform name that does not match the C++ is silently ignored:
glGetUniformLocation returns -1, glUniform on -1 is a documented no-op, and
nothing in the build says a word. A control can therefore be completely dead
while everything compiles, links, loads and renders. Nothing else in this repo
catches that.

So: render each parameter at both ends of its range against a baseline where
every stage is switched on, and report any that made no difference.

    python3 tools/sweep.py

Run it after adding a parameter, renaming a uniform, or moving anything between
the C++ and the GLSL. Exit code 1 means something is dead.

------------------------------------------------------------------ the traps

**A control can be masked by the stage in front of it.** Tilt scales the
emphasis network, so with Emphasis at zero there is no network to mistrack and
Tilt is legitimately dead. Chroma only does anything where the picture has
colour and where the chain has changed the luma at all. The baseline therefore
has every stage doing something, and CONTEXT holds the extra settings a
particular control needs before it can be seen at all.

**The audio buffer must be SKIPPED.** `Audio` is an FF_TYPE_BUFFER whose float
value is meaningless -- the host writes spectrum bins into its elements and
never touches the parameter itself. Sweeping it sets a number nothing reads and
reports a false dead. Every plugin in this fleet that grew an FFT input had to
learn the same thing.

**Sidechain modes 1 and 2 do nothing without audio.** The offline harness
delivers no spectrum, so the audio envelope sits at its resting value and those
two modes render identically to mode 0. Sidechain is therefore in SKIP with a
reason, and it is checked by `cmtest --list` being able to name all three
elements rather than by a rendered difference.
"""
import os
import struct
import subprocess
import sys
import tempfile
import zlib

CMTEST = "./build/cmtest"
WIDTH, HEIGHT = 640, 360
SCRATCH = tempfile.mkdtemp(prefix="compandersweep")

# A baseline with every stage active, so nothing reads dead merely because the
# thing it modifies is switched off. Deliberately NOT the defaults: the two ends
# are set to mismatched ratios and the link is off level, because a perfectly
# matched pair cancels and half these controls would have nothing to show.
BASE = {
    "Chroma": 0.5,
    "Compress": 0.5,
    "Emphasis": 0.6,
    "Link Level": 0.35,
    "Noise": 0.3,
    "Headroom": 0.6,
    "Expand": 0.4,
    "Tilt": 0.6,
    "Time Constant": 0.45,
    "Pivot": 0.75,
    "Max Gain": 0.5,
    "Mix": 1.0,
}

# Endpoints to sweep between, where the full 0..1 range is not the right test.
ENDS = {
    # At 0 there is no emphasis network, so Tilt has nothing to scale and is
    # legitimately inert. Sweep it either side of matched instead.
    "Tilt": (0.15, 0.85),
    # Pivot at the very bottom puts the whole picture above it and the law
    # becomes a pure cut everywhere, which is a different question.
    "Pivot": (0.4, 0.95),
}

# Extra settings a control needs before it can be seen at all.
CONTEXT = {
    # Tilt scales the emphasis network. No network, nothing to mistrack.
    "Tilt": {"Emphasis": 0.8},
    # Chroma only shows where the chain has moved the luma.
    "Chroma": {"Compress": 0.7, "Expand": 0.2},
    # The preset dropdown must start somewhere that is not preset 1's values.
    "Preset": {},
}

SKIP = {
    "Audio": "FF_TYPE_BUFFER -- the host writes elements, the float value is meaningless",
    "Audio Amount": "inert without a spectrum, which the offline harness does not deliver",
    "Audio Band": "inert without a spectrum",
    "Audio Tilt": "inert without a spectrum",
    "Sidechain": "modes 1 and 2 need audio; checked by --list naming all three elements",
    "About": "text",
    "User guide": "event",
    "Project page": "event",
    "Source on GitHub": "event",
    "Support the work": "event",
}


def read_png(path):
    data = open(path, "rb").read()
    pos, width, height, idat = 8, None, None, b""
    while pos < len(data):
        length = struct.unpack(">I", data[pos : pos + 4])[0]
        kind = data[pos + 4 : pos + 8]
        chunk = data[pos + 8 : pos + 8 + length]
        if kind == b"IHDR":
            width, height = struct.unpack(">II", chunk[:8])
        elif kind == b"IDAT":
            idat += chunk
        pos += 12 + length

    raw = zlib.decompress(idat)
    out, prev, i = [], bytearray(width * 4), 0
    for _ in range(height):
        filt = raw[i]
        i += 1
        line = bytearray(raw[i : i + width * 4])
        i += width * 4
        for x in range(width * 4):
            a = line[x - 4] if x >= 4 else 0
            b = prev[x]
            c = prev[x - 4] if x >= 4 else 0
            if filt == 1:
                line[x] = (line[x] + a) & 255
            elif filt == 2:
                line[x] = (line[x] + b) & 255
            elif filt == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif filt == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pred) & 255
        out.append(bytes(line))
        prev = line
    return b"".join(out)


def render(settings, name):
    path = os.path.join(SCRATCH, name + ".png")
    args = [CMTEST, "--out", path, "--width", str(WIDTH), "--height", str(HEIGHT)]
    for key, value in settings.items():
        args += ["--set", "%s=%s" % (key, value)]

    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit("render failed: %s\n%s" % (name, result.stderr))
    return read_png(path)


def difference(a, b):
    if len(a) != len(b):
        return 255.0
    total = sum(abs(a[i] - b[i]) for i in range(0, len(a), 4))
    return total / (len(a) / 4)


def parameters():
    """Read the control list out of the plugin itself.

    Not a table beside this file: a hand-written list is a second place for a
    name to live, and the failure it produces is a sweep that silently stops
    covering a control after it is renamed.
    """
    result = subprocess.run([CMTEST, "--list"], capture_output=True, text=True)
    names = []
    for line in result.stdout.splitlines()[1:]:
        parts = line.split()
        if len(parts) < 3 or not parts[0].isdigit():
            continue
        kind = None
        for token in parts:
            if token in ("standard", "option", "boolean", "buffer", "text", "event"):
                kind = token
                break
        if kind is None:
            continue
        name = line[5 : line.index(kind)].strip()
        names.append((name, kind))
    return names


def main():
    if not os.path.exists(CMTEST):
        raise SystemExit("build cmtest first: cmake --build build")

    dead, checked, skipped = [], 0, 0

    for name, kind in parameters():
        if name in SKIP:
            print("SKIP %-18s %s" % (name, SKIP[name]))
            skipped += 1
            continue

        base = dict(BASE)
        base.update(CONTEXT.get(name, {}))

        if kind == "option" and name == "Preset":
            # Every preset against every other, so a duplicate pair shows up as
            # well as a dead dropdown.
            frames = {}
            for index in range(1, 11):
                settings = dict(base)
                settings[name] = index
                frames[index] = render(settings, "preset%d" % index)

            duplicates = []
            for i in sorted(frames):
                for j in sorted(frames):
                    if i < j and difference(frames[i], frames[j]) < 0.5:
                        duplicates.append((i, j))

            if duplicates:
                dead.append("%s (presets %s render identically)" % (name, duplicates))
                print("DEAD %-18s %s" % (name, duplicates))
            else:
                print("ok   %-18s %d presets, all distinct" % (name, len(frames)))
            checked += 1
            continue

        low, high = ENDS.get(name, (0.0, 1.0))

        settings_low = dict(base)
        settings_low[name] = low
        settings_high = dict(base)
        settings_high[name] = high

        delta = difference(render(settings_low, name + "_lo"), render(settings_high, name + "_hi"))
        checked += 1

        if delta < 0.05:
            dead.append(name)
            print("DEAD %-18s mean |dE| %.4f between %.2f and %.2f" % (name, delta, low, high))
        else:
            print("ok   %-18s mean |dE| %.4f" % (name, delta))

    print("\n%d checked, %d skipped, %d dead" % (checked, skipped, len(dead)))
    return 1 if dead else 0


if __name__ == "__main__":
    sys.exit(main())
