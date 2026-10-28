"""The demo's shaders must be the plugin's shaders, character for character.

    python3 demo/tools/check_shaders.py           compare; exit 1 on any drift
    python3 demo/tools/check_shaders.py --write   regenerate demo/shaders.js

Called from `tools/verify.sh`. Exit code 1 means a copy has drifted.

------------------------------------------------------------------- why

`demo/shaders.js` holds the ten GLSL pieces of `source/shaders/` -- kVertex
(Vertex.cpp), kCommon (Common.cpp) and the eight fragment shaders of
Passes.cpp. That is two copies of the same text, and two copies drift --
quietly, because a compander drawn by a *plausible* shader looks exactly like
one drawn by the right one. The whole claim of the page is that it runs the
plugin's own shaders, so the claim needs something enforcing it. `cmtest`
drives the real plugin class and has never heard of this page, and the glslc
step in `tools/verify.sh` compiles the C++ copies and never looks at the JS one.

------------------------------------------------------------------- what it does

Pulls each piece out of `source/shaders/*.cpp` and the matching backtick
literal out of `demo/shaders.js`, and compares them exactly -- no whitespace
normalisation, no comment stripping: a comment updated on one side only is
exactly the drift worth catching.

It also fails when a shader file grows a piece this script does not list (a new
pass the page would silently lack), and it checks the JOIN as well as the
pieces: `assemble()` in Passes.cpp splices kCommon in at the first `@COMMON@`
marker of the encode, decode and output fragments, and InitGL in Plugin.cpp
compiles every one of the eight passes against kVertex. plugin.js must build
the same eight programs the same way. A change to either side's assembly fails
here and needs a reader.

And it checks the preset table: the ten rows of `source/Presets.h`, names and
numbers, must be the rows plugin.js's PRESETS carries, in the same order. A
preset is data the shaders are fed, and a page whose Satellite Link differed
from the plugin's would be drawing a different circuit under the same name.

The one transformation is a decode, not a normalisation: a backtick cannot
appear raw inside a template literal, so --write escapes one as \\`. The
shaders have several, in comments. This undoes that escape and REJECTS any
other backslash or any `${` on the JS side; there are none in the C++ bodies,
so either could only be somebody hiding a difference.

`--write` produces shaders.js from the C++ by exactly that escape, so the copy
is spliced by this script and never typed.

------------------------------------------------------------------- what it cannot

Nothing here checks the PORTED half: Controls.cpp (the mappings),
Compander.cpp (the gain tables), Detector.cpp (the pass plan and the frame
follower's units), Chain.cpp's crossoverFor and the frame logic of Plugin.cpp's
ProcessOpenGL are a hand translation in demo/plugin.js's PORT block.
demo/tools/crosscheck/ holds the first four to the C++ (verify.sh runs it), and
its pixels.mjs holds the whole page to the real plugin class frame for frame.
When you change one of those, change the port too.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.dont_write_bytecode = True

# JS constant, C++ symbol, the file under source/shaders/ that defines it, in
# the order they are declared there.
SHADERS = [
    ("VERTEX", "kVertex", "Vertex.cpp"),
    ("COMMON", "kCommon", "Common.cpp"),
    ("LUMA_FRAGMENT", "kLumaFragment", "Passes.cpp"),
    ("BLUR_FRAGMENT", "kBlurFragment", "Passes.cpp"),
    ("REDUCE_FRAGMENT", "kReduceFragment", "Passes.cpp"),
    ("SCAN_FRAGMENT", "kScanFragment", "Passes.cpp"),
    ("FRAME_FRAGMENT", "kFrameFragment", "Passes.cpp"),
    ("ENCODE_FRAGMENT", "kEncodeFragment", "Passes.cpp"),
    ("DECODE_FRAGMENT", "kDecodeFragment", "Passes.cpp"),
    ("OUTPUT_FRAGMENT", "kOutputFragment", "Passes.cpp"),
]
SHADER_FILES = ["Vertex.cpp", "Common.cpp", "Passes.cpp"]

# How the pieces are joined, on each side. Each entry is (file, text that must
# appear in it verbatim). The C++ lines are the plugin's assembly; the JS lines
# are plugin.js's copy of it. A change to either is a change to what runs.
ASSEMBLY = [
    ("source/shaders/Passes.cpp", 'const std::string marker = "@COMMON@";'),
    ("source/shaders/Passes.cpp", "const size_t at = s.find( marker );"),
    ("source/shaders/Passes.cpp", "s.replace( at, marker.size(), kCommon );"),
    ("source/shaders/Passes.cpp", "static const std::string s = assemble( kEncodeFragment );"),
    ("source/shaders/Passes.cpp", "static const std::string s = assemble( kDecodeFragment );"),
    ("source/shaders/Passes.cpp", "static const std::string s = assemble( kOutputFragment );"),
    ("source/Plugin.cpp", '{ &lumaShader, shaders::kLumaFragment, "luma" },'),
    ("source/Plugin.cpp", '{ &blurShader, shaders::kBlurFragment, "blur" },'),
    ("source/Plugin.cpp", '{ &reduceShader, shaders::kReduceFragment, "reduce" },'),
    ("source/Plugin.cpp", '{ &scanShader, shaders::kScanFragment, "scan" },'),
    ("source/Plugin.cpp", '{ &frameShader, shaders::kFrameFragment, "frame" },'),
    ("source/Plugin.cpp", '{ &encodeShader, shaders::EncodeFragment(), "encode" },'),
    ("source/Plugin.cpp", '{ &decodeShader, shaders::DecodeFragment(), "decode" },'),
    ("source/Plugin.cpp", '{ &outputShader, shaders::OutputFragment(), "output" },'),
    ("source/Plugin.cpp", "if( !b.shader->Compile( shaders::kVertex, b.fragment ) )"),
    ("demo/plugin.js", "const assemble = (piece) => piece.replace('@COMMON@', () => S.COMMON);"),
    ("demo/plugin.js", "luma: new Program(gl, S.VERTEX, S.LUMA_FRAGMENT, 'luma'),"),
    ("demo/plugin.js", "blur: new Program(gl, S.VERTEX, S.BLUR_FRAGMENT, 'blur'),"),
    ("demo/plugin.js", "reduce: new Program(gl, S.VERTEX, S.REDUCE_FRAGMENT, 'reduce'),"),
    ("demo/plugin.js", "scan: new Program(gl, S.VERTEX, S.SCAN_FRAGMENT, 'scan'),"),
    ("demo/plugin.js", "frame: new Program(gl, S.VERTEX, S.FRAME_FRAGMENT, 'frame'),"),
    ("demo/plugin.js", "encode: new Program(gl, S.VERTEX, assemble(S.ENCODE_FRAGMENT), 'encode'),"),
    ("demo/plugin.js", "decode: new Program(gl, S.VERTEX, assemble(S.DECODE_FRAGMENT), 'decode'),"),
    ("demo/plugin.js", "output: new Program(gl, S.VERTEX, assemble(S.OUTPUT_FRAGMENT), 'output'),"),
]

HEADER = """// GENERATED from source/shaders/*.cpp by demo/tools/check_shaders.py --write.
// Do not edit: tools/verify.sh fails if a character of this differs from the
// plugin's. The one escape allowed is \\` for a backtick inside a comment.
"""


def read(*parts):
    with open(os.path.join(REPO, *parts)) as handle:
        return handle.read()


def from_cpp(source, symbol):
    """The piece's text, or None if the symbol is not one raw string."""
    match = re.search(r'const char\* const ' + symbol + r' = R"\((.*?)\)";', source, re.S)
    return None if match is None else match.group(1)


def cpp_pieces(source):
    """Every raw-string piece a shader file defines, by symbol."""
    return re.findall(r'const char\* const (k\w+) = R"\(', source)


def from_js(source, name):
    match = re.search(r'^export const ' + name + r' = `(.*?)`;$', source, re.S | re.M)
    if match is None:
        return None, None
    body = match.group(1)
    stray = re.search(r"\\(?!`)", body)
    if stray is not None:
        line = body[: stray.start()].count("\n") + 1
        return None, f"backslash that is not an escaped backtick, at line {line}"
    if "${" in body:
        return None, "template substitution"
    return body.replace("\\`", "`"), None


def cpp_presets():
    """Presets.h's rows: [(name, [11 numbers as written])]."""
    text = read("source", "Presets.h")
    rows = re.findall(r'\{\s*"([^"]+)",\s*\{([^}]*)\}\s*\}', text)
    return [(name, [v.strip().rstrip("f") for v in values.split(",")]) for name, values in rows]


def js_presets(js):
    """plugin.js's PRESETS rows, in the same shape."""
    block = re.search(r"^const PRESETS = \[(.*?)^\];", js, re.S | re.M)
    if block is None:
        return None
    rows = re.findall(r"\[\s*'([^']+)',\s*\[([^\]]*)\]\s*\]", block.group(1))
    return [(name, [v.strip() for v in values.split(",")]) for name, values in rows]


def write():
    out = [HEADER]
    for name, symbol, path in SHADERS:
        body = from_cpp(read("source", "shaders", path), symbol)
        if body is None:
            print(f"FAIL  {symbol} not found in source/shaders/{path}")
            return 1
        if "\\" in body or "${" in body:
            print(f"FAIL  {symbol} holds a backslash or ${{; the escape scheme cannot carry it")
            return 1
        out.append(f"\n// {symbol}, source/shaders/{path}\nexport const {name} = `{body.replace('`', chr(92) + '`')}`;\n")
    with open(os.path.join(REPO, "demo", "shaders.js"), "w") as handle:
        handle.write("".join(out))
    print("wrote demo/shaders.js")
    return 0


def first_difference(a, b):
    left, right = a.splitlines(), b.splitlines()
    for i in range(max(len(left), len(right))):
        x = left[i] if i < len(left) else "<missing>"
        y = right[i] if i < len(right) else "<missing>"
        if x != y:
            return i + 1, x, y
    return None


def check(js):
    problems = 0

    listed = {symbol for _, symbol, _ in SHADERS}
    for path in SHADER_FILES:
        for symbol in cpp_pieces(read("source", "shaders", path)):
            if symbol not in listed:
                print(f"FAIL  source/shaders/{path} defines {symbol}, which this check does not list -- "
                      "add it to SHADERS (and to the page, or say on the page why not)")
                problems += 1

    for name, symbol, path in SHADERS:
        cpp_text = from_cpp(read("source", "shaders", path), symbol)
        js_text, complaint = from_js(js, name)
        if cpp_text is None:
            print(f"FAIL  {symbol} not found in source/shaders/{path}")
            problems += 1
        elif complaint is not None:
            print(f"FAIL  {name} in demo/shaders.js has a {complaint}")
            problems += 1
        elif js_text is None:
            print(f"FAIL  {name} not found in demo/shaders.js")
            problems += 1
        elif cpp_text == js_text:
            print(f"ok    {name:<16} matches {symbol} ({len(cpp_text)} chars)")
        else:
            problems += 1
            print(f"FAIL  {name} has drifted from {symbol}")
            where = first_difference(cpp_text, js_text)
            if where:
                print(f"        first difference at line {where[0]}")
                print(f"          C++: {where[1]}")
                print(f"          js : {where[2]}")

    for path, text in ASSEMBLY:
        try:
            body = read(*path.split("/"))
        except FileNotFoundError:
            print(f"FAIL  {path} is missing")
            problems += 1
            continue
        if text in body:
            print(f"ok    assembly: {path} still says {text[:60]}")
        else:
            print(f"FAIL  assembly: {path} no longer says: {text}")
            problems += 1

    try:
        plugin = read("demo", "plugin.js")
    except FileNotFoundError:
        plugin = ""
    cpp_rows, js_rows = cpp_presets(), js_presets(plugin)
    if not cpp_rows:
        print("FAIL  no preset rows found in source/Presets.h")
        problems += 1
    elif js_rows is None:
        print("FAIL  no `const PRESETS = [` block in demo/plugin.js")
        problems += 1
    elif cpp_rows != js_rows:
        problems += 1
        print("FAIL  plugin.js's PRESETS are not Presets.h's rows")
        for i in range(max(len(cpp_rows), len(js_rows))):
            a = cpp_rows[i] if i < len(cpp_rows) else None
            b = js_rows[i] if i < len(js_rows) else None
            if a != b:
                print(f"        row {i + 1}: C++ {a}")
                print(f"        row {i + 1}: js  {b}")
                break
    else:
        print(f"ok    presets: all {len(cpp_rows)} rows of Presets.h, names and values as written")

    return problems


def main(argv):
    if "--write" in argv:
        return write()
    try:
        js = read("demo", "shaders.js")
    except FileNotFoundError:
        print("FAIL  demo/shaders.js is missing -- run demo/tools/check_shaders.py --write")
        return 1
    problems = check(js)
    print()
    if problems:
        print(f"{problems} problem(s) -- rerun demo/tools/check_shaders.py --write for a drifted copy "
              "(never edit shaders.js by hand); an assembly, preset or unlisted-piece failure needs a reader")
        return 1
    print(f"all {len(SHADERS)} shader pieces, their assembly and the preset table are the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
