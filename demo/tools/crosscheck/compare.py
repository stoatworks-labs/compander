"""Compare printer.cpp's output with run.mjs's, token for token.

    python3 compare.py cpp.txt js.txt [--max-ulps N]

Integers and labels must be identical. Floats (printed as 0x%08x bits) are
compared in units in the last place of a float: the port runs on V8's Math and
the plugin on Apple's libm, which round some transcendental results to the
other neighbour, so the two need not be identical to the bit -- but anything
past a couple of ulps is the port computing something else. Exit 1 on any
failure.

One quantity is allowed more, and only by the arithmetic that makes it so: a
doubling pass's decay is a^offset, offset up to 8192, so the 1 or 2 ulp by
which two libms may disagree about a = exp(-1/tau) is raised to that power. a
itself is printed and held to the same limit as everything else, and each
decay may then differ by limit + 2 * limit * offset ulps -- the relative error
of a, times the power, in ulps of the result.
"""
import struct
import sys


def ordered(bits):
    """A float's bits as an integer that orders like the float does."""
    return bits if bits < 0x80000000 else 0x80000000 - bits


def value(bits):
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def main(argv):
    limit = 2
    if "--max-ulps" in argv:
        limit = int(argv[argv.index("--max-ulps") + 1])
    cpp = open(argv[0]).read().splitlines()
    js = open(argv[1]).read().splitlines()

    problems = 0
    if len(cpp) != len(js):
        print(f"FAIL  {len(cpp)} lines from the C++, {len(js)} from the port")
        problems += 1

    floats = identical = 0
    worst = (0, None)
    worst_decay = (0.0, None)
    histogram = {}
    for number, (a, b) in enumerate(zip(cpp, js), start=1):
        ta, tb = a.split(), b.split()
        if len(ta) != len(tb):
            print(f"FAIL  line {number}: {len(ta)} tokens against {len(tb)}\n        C++: {a[:120]}\n        js : {b[:120]}")
            problems += 1
            continue
        for position, (x, y) in enumerate(zip(ta, tb)):
            if x.startswith("0x") and y.startswith("0x"):
                floats += 1
                ulps = abs(ordered(int(x, 16)) - ordered(int(y, 16)))
                histogram[min(ulps, 99)] = histogram.get(min(ulps, 99), 0) + 1
                if ulps == 0:
                    identical += 1
                # plan W k tau plan.tau blend seconds narrow wide a passes
                # [offset decay]... : a decay sits at 12, 14, ... after its offset.
                if ta[0] == "plan" and position >= 12 and position % 2 == 0:
                    offset = int(ta[position - 1])
                    allowed = limit + 2 * limit * offset
                    if ulps / allowed > worst_decay[0]:
                        worst_decay = (ulps / allowed, f"line {number} (plan {ta[1]} {ta[2]}), offset {offset}: "
                                                        f"{ulps} ulp of {allowed} allowed")
                    if ulps > allowed:
                        print(f"FAIL  line {number}: a decay at offset {offset} is {ulps} ulp out, past {allowed}")
                        problems += 1
                    continue
                if ulps > worst[0]:
                    worst = (ulps, f"line {number} ({ta[0]} {ta[1]}): C++ {value(int(x, 16))!r} js {value(int(y, 16))!r}")
            elif x != y:
                print(f"FAIL  line {number}: '{x}' against '{y}'\n        C++: {a[:120]}\n        js : {b[:120]}")
                problems += 1
                break

    spread = ", ".join(f"{k} ulp: {v}" for k, v in sorted(histogram.items()))
    print(f"      {len(cpp)} lines, {floats} floats, {identical} identical to the bit ({spread})")
    if worst[1]:
        print(f"      worst (decays aside): {worst[0]} ulp at {worst[1]}")
    if worst_decay[1]:
        print(f"      worst decay: {worst_decay[1]}")
    if worst[0] > limit:
        print(f"FAIL  a float is {worst[0]} ulp from the plugin's, past the {limit} allowed")
        problems += 1
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
