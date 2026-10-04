"""The demo's copies of the plugin must be the plugin, character for character.

    python3 demo/tools/check_shaders.py           compare; exit 1 on any drift
    python3 demo/tools/check_shaders.py --write   regenerate demo/shaders.js

Run by `.github/workflows/deploy.yml` before every deploy, and meant to be run
by `tools/verify.sh` (polyhedral's arrangement). Exit code 1 means a copy has
drifted, or the .wasm was built from sources that have since changed.

------------------------------------------------------------------- why

The page at honeydew-demo.stoatworks-labs.com carries two things that are
copies of this repository, and copies drift quietly -- a dish that oscillates
with a *plausible* period looks exactly like one that oscillates with the
right one, and the whole claim of the page is that it runs the plugin.

1. **The GLSL.** `demo/shaders.js` holds the thirteen pieces of
   `source/Shaders.cpp` (kCommon, kStateCommon, kQuadVertex and the ten pass
   bodies) plus kVersion, and the ASSEMBLY table below: which pieces each pass
   is built from, in order, as `Honeydew.cpp`'s InitGL assembles them through
   `shaders::Assemble` (kVersion + kCommon + pieces). This compares the pieces
   exactly -- no whitespace normalisation, no comment stripping: a comment
   updated on one side only is drift worth catching -- and requires that every
   `R"( ... )"` literal in Shaders.cpp is listed here and that every listed
   piece is used by some pass, so a new pass cannot go unchecked (the same rule
   tools/glslc.sh applies).

   The page checks the assembly again at run time: demo/wasm/gl_shim.cpp hands
   every shader the compiled plugin passes to glShaderSource to the page, which
   REQUIRES it to equal one of the assembled programs of this table and
   otherwise refuses to compile it. So a mismatch between this table and
   InitGL's is not a silent wrong picture; it is a page that says so and stops.

   The one transformation is a decode: a backtick cannot sit raw in a template
   literal, so shaders.js escapes it as \\`. Any other backslash, or a `${`, on
   the JS side is rejected; the C++ bodies hold neither. `--write` produces
   shaders.js from the C++ by exactly that escape, so the copy is spliced by
   this script and never typed.

2. **The WebAssembly.** `demo/honeydew-core.wasm` is the plugin's C++ compiled
   by `demo/tools/build-wasm.sh`, and committed, because the deploy has no
   build step. Building it needs emscripten, which a verify run cannot assume,
   so this checks the next best thing: `demo/wasm/inputs.sha256`, written by
   the build, records the hash of every file that went in and of both outputs.
   A source file changed since the last build -- or an output that is not the
   one the build wrote -- fails here, and the fix is to rerun build-wasm.sh.

------------------------------------------------------------------- what it cannot

It cannot tell whether the .wasm really is what those inputs compile to (only
rebuilding can), and it says nothing about demo/plugin.js, the page around the
plugin: its panel is built from the plugin's own declarations at run time, but
its read-out units, its presets' intentions and its layout are the page's.
"""
import hashlib
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.dont_write_bytecode = True

# JS constant, C++ symbol (all in source/Shaders.cpp), in the order they are
# declared there.
SHADERS = [
    ("COMMON", "kCommon"),
    ("STATE_COMMON", "kStateCommon"),
    ("QUAD_VERTEX", "kQuadVertex"),
    ("SEED_FRAGMENT", "kSeedFragment"),
    ("STEP_FRAGMENT_A", "kStepFragmentA"),
    ("STEP_FRAGMENT_B", "kStepFragmentB"),
    ("ADVECT_FRAGMENT", "kAdvectFragment"),
    ("RELAX_FRAGMENT", "kRelaxFragment"),
    ("COLUMNS_FRAGMENT", "kColumnsFragment"),
    ("TOTAL_FRAGMENT", "kTotalFragment"),
    ("COLOUR_FRAGMENT", "kColourFragment"),
    ("COMPOSITE_FRAGMENT", "kCompositeFragment"),
    ("THUMB_FRAGMENT", "kThumbFragment"),
]

# How InitGL assembles each program (Honeydew.cpp: the Stage table, through
# shaders::Assemble, which is kVersion + kCommon + the pieces). The vertex
# stage is kVersion + kQuadVertex with no kCommon. The page refuses any shader
# text the compiled plugin hands to GL that is not one of these.
ASSEMBLY = {
    "vertex": ["QUAD_VERTEX"],
    "seed": ["COMMON", "STATE_COMMON", "SEED_FRAGMENT"],
    "step": ["COMMON", "STATE_COMMON", "STEP_FRAGMENT_A", "STEP_FRAGMENT_B"],
    "advect": ["COMMON", "STATE_COMMON", "ADVECT_FRAGMENT"],
    "relax": ["COMMON", "STATE_COMMON", "RELAX_FRAGMENT"],
    "columns": ["COMMON", "STATE_COMMON", "COLUMNS_FRAGMENT"],
    "total": ["COMMON", "STATE_COMMON", "TOTAL_FRAGMENT"],
    "colour": ["COMMON", "STATE_COMMON", "COLOUR_FRAGMENT"],
    "composite": ["COMMON", "COMPOSITE_FRAGMENT"],
    "thumb": ["COMMON", "THUMB_FRAGMENT"],
}

HEADER = """// GENERATED from source/Shaders.cpp by demo/tools/check_shaders.py --write.
// Do not edit: that script (run by deploy.yml, and meant for tools/verify.sh)
// fails if a character of this differs from the plugin's. The one escape is
// \\` for a backtick inside a comment.
"""

MANIFEST = os.path.join("demo", "wasm", "inputs.sha256")
OUTPUTS = ("demo/honeydew-core.js", "demo/honeydew-core.wasm")


def read(*parts):
    with open(os.path.join(REPO, *parts)) as handle:
        return handle.read()


def cpp_version(source):
    match = re.search(r'const char\* const kVersion = "(.*?)";', source)
    return None if match is None else match.group(1)


def from_cpp(source, symbol):
    match = re.search(r'const char\* const ' + symbol + r' = R"\((.*?)\)";', source, re.S)
    return None if match is None else match.group(1)


def cpp_literals(source):
    """Every raw-string shader piece Shaders.cpp declares, in order."""
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


def assembly_js():
    lines = ["\n// How InitGL assembles each program: kVersion, then these pieces in order",
             "// (Honeydew.cpp's Stage table, through shaders::Assemble). The page refuses",
             "// any other text the compiled plugin hands to glShaderSource.",
             "export const ASSEMBLY = {"]
    for stage, pieces in ASSEMBLY.items():
        lines.append(f"  {stage}: [{', '.join(repr(p) for p in pieces)}],")
    lines.append("};\n")
    return "\n".join(lines)


def check_listing(cpp):
    """Every literal listed, every listed piece assembled into some program."""
    problems = 0
    declared = cpp_literals(cpp)
    listed = [symbol for _, symbol in SHADERS]
    for symbol in declared:
        if symbol not in listed:
            print(f"FAIL  {symbol} is a shader piece in source/Shaders.cpp that this check does not list")
            problems += 1
    for symbol in listed:
        if symbol not in declared:
            print(f"FAIL  {symbol} is listed here and no longer declared in source/Shaders.cpp")
            problems += 1
    used = {piece for pieces in ASSEMBLY.values() for piece in pieces}
    for name, symbol in SHADERS:
        if name not in used:
            print(f"FAIL  {symbol} is a piece no ASSEMBLY entry uses: a pass is not being checked")
            problems += 1
    for stage, pieces in ASSEMBLY.items():
        for piece in pieces:
            if piece not in {name for name, _ in SHADERS}:
                print(f"FAIL  ASSEMBLY {stage} names {piece}, which is not a listed piece")
                problems += 1
    if problems == 0:
        print(f"ok    {'LISTING':<20} {len(declared)} literals in Shaders.cpp, all listed, all assembled into {len(ASSEMBLY)} programs")
    return problems


def write(cpp):
    if check_listing(cpp):
        return 1
    out = [HEADER]
    out.append(f"export const VERSION = '{cpp_version(cpp)}';\n")
    for name, symbol in SHADERS:
        body = from_cpp(cpp, symbol)
        if body is None:
            print(f"FAIL  {symbol} not found in source/Shaders.cpp")
            return 1
        if "\\" in body or "${" in body:
            print(f"FAIL  {symbol} holds a backslash or ${{; the escape scheme cannot carry it")
            return 1
        out.append(f"\n// {symbol}, source/Shaders.cpp\nexport const {name} = `{body.replace('`', chr(92) + '`')}`;\n")
    out.append(assembly_js())
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


def check_shaders(cpp, js):
    problems = check_listing(cpp)
    version_cpp = cpp_version(cpp)
    version_js = re.search(r"^export const VERSION = '(.*?)';$", js, re.M)
    if version_cpp is None or version_js is None or version_cpp != version_js.group(1):
        print("FAIL  VERSION does not match kVersion")
        problems += 1
    else:
        print(f"ok    {'VERSION':<20} matches kVersion")

    for name, symbol in SHADERS:
        cpp_text = from_cpp(cpp, symbol)
        js_text, complaint = from_js(js, name)
        if cpp_text is None:
            print(f"FAIL  {symbol} not found in source/Shaders.cpp")
            problems += 1
        elif complaint is not None:
            print(f"FAIL  {name} in demo/shaders.js has a {complaint}")
            problems += 1
        elif js_text is None:
            print(f"FAIL  {name} not found in demo/shaders.js")
            problems += 1
        elif cpp_text == js_text:
            print(f"ok    {name:<20} matches {symbol} ({len(cpp_text)} chars)")
        else:
            problems += 1
            print(f"FAIL  {name} has drifted from {symbol}")
            where = first_difference(cpp_text, js_text)
            if where:
                print(f"        first difference at line {where[0]}")
                print(f"          C++: {where[1]}")
                print(f"          js : {where[2]}")

    if assembly_js() not in js:
        print("FAIL  the ASSEMBLY table in demo/shaders.js is not this script's -- rerun --write")
        problems += 1
    else:
        print(f"ok    {'ASSEMBLY':<20} {len(ASSEMBLY)} programs, as written")
    return problems


def sha256(path):
    digest = hashlib.sha256()
    with open(os.path.join(REPO, path), "rb") as handle:
        for block in iter(lambda: handle.read(1 << 16), b""):
            digest.update(block)
    return digest.hexdigest()


def check_wasm():
    try:
        lines = read(MANIFEST).splitlines()
    except FileNotFoundError:
        print(f"FAIL  {MANIFEST} is missing -- run demo/tools/build-wasm.sh")
        return 1

    problems = 0
    entries = 0
    for line in lines:
        pin = re.match(r"^# FFGL SDK ([0-9a-f]{40})$", line)
        if pin:
            try:
                head = subprocess.run(["git", "-C", os.path.join(REPO, "external", "ffgl"), "rev-parse", "HEAD"],
                                      capture_output=True, text=True, check=True).stdout.strip()
            except (OSError, subprocess.CalledProcessError):
                print("FAIL  cannot read the FFGL SDK's commit -- is the submodule checked out?")
                problems += 1
                continue
            if head != pin.group(1):
                print(f"FAIL  the .wasm was built against FFGL SDK {pin.group(1)[:7]}, the submodule is at {head[:7]}")
                problems += 1
            continue
        if not line or line.startswith("#"):
            continue
        want, path = line.split(None, 1)
        entries += 1
        if not os.path.exists(os.path.join(REPO, path)):
            print(f"FAIL  {path} went into the .wasm and no longer exists")
            problems += 1
        elif sha256(path) != want:
            print(f"FAIL  {path} has changed since demo/honeydew-core.wasm was built")
            problems += 1

    for output in OUTPUTS:
        if not any(line.endswith("  " + output) for line in lines):
            print(f"FAIL  {MANIFEST} does not record {output}")
            problems += 1
    # Every plugin source file must be an input: a new .cpp under source/ that
    # the build does not compile is a plugin the page no longer runs.
    hashed = {line.split(None, 1)[1] for line in lines if line and not line.startswith("#")}
    for name in sorted(os.listdir(os.path.join(REPO, "source"))):
        path = "source/" + name
        if name in ("SourcePlugin.cpp", "EffectPlugin.cpp"):
            continue  # the registrations: the page constructs the plugin itself
        if (name.endswith(".cpp") or name.endswith(".h")) and path not in hashed:
            print(f"FAIL  {path} is not an input of the .wasm (demo/tools/build-wasm.sh does not know it)")
            problems += 1
    if problems == 0:
        print(f"ok    {'WASM':<20} built from these sources ({entries} files hashed, SDK pin agrees)")
    return problems


def main(argv):
    cpp = read("source", "Shaders.cpp")
    if "--write" in argv:
        return write(cpp)
    try:
        js = read("demo", "shaders.js")
    except FileNotFoundError:
        print("FAIL  demo/shaders.js is missing -- run demo/tools/check_shaders.py --write")
        return 1
    problems = check_shaders(cpp, js) + check_wasm()
    print()
    if problems:
        print(f"{problems} problem(s) -- rerun demo/tools/check_shaders.py --write for the GLSL and"
              " demo/tools/build-wasm.sh for the .wasm; never edit either output by hand")
        return 1
    print(f"all {len(SHADERS) + 1} shader pieces are the plugin's, and the .wasm is built from the current sources")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
