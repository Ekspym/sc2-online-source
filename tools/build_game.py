#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Lift Soulcalibur II's XBE and build the native runner.

The runtime is a dependency, not a copy: third_party/doa2u-re (pinned) holds
the shared DOA runtime. The build merges its recomp-runtime/ with this repo's
runtime/ into build/src/, where a file in runtime/ replaces the shared file of
the same name. Every replaced file is game-specific (addresses, layouts or
game hooks for XDK 5455); docs/runtime.md lists them and why.

  tools/build_game.py lift     disassemble and lift private/disc/Default.xbe
  tools/build_game.py current  lift only if the last lift is out of date (another lifter, runtime/ binding other
                               functions, or no lift yet): what tools/release/release.sh runs
  tools/build_game.py build    merge the runtime and build the runner
  tools/build_game.py launcher build the launcher (launcher/)
  tools/build_game.py app      both, as build/Soulcalibur II Recomp.app (the launcher with the game beside it)
  tools/build_game.py all      lift and build

Generated code, the disc and every derived file stay under private/.
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# Release scripts build elsewhere (tools/release/linux.sh): SC2_SRC_DIR, SC2_RUNNER_DIR, SC2_LAUNCHER_DIR and extra
# CMake arguments in SC2_CMAKE_ARGS (space separated). With --pal, "-pal" is added to SC2_SRC_DIR and SC2_RUNNER_DIR, so
# a release's two programs build side by side.
EXTRA_CMAKE = os.environ.get("SC2_CMAKE_ARGS", "").split()
LIFTER = ROOT / "tools/xboxrecomp"
SHARED = ROOT / "third_party/doa2u-re"
PRIVATE = ROOT / "private"
# --pal: the PAL (Europe, Australia) disc's executable, private/disc-pal/Default.xbe, lifted into
# private/program-pal and built with runtime/ rewritten to its addresses (tools/sc2/pal_port.py: every address
# literal through tools/sc2/pal_addresses.txt) as build/runner-pal.
PAL = "--pal" in sys.argv
if PAL:
    sys.argv.remove("--pal")
    sys.path.insert(0, str(ROOT / "tools/sc2"))
    import pal_port
PROGRAM = PRIVATE / ("program-pal" if PAL else "program")
XBE = PRIVATE / ("disc-pal/Default.xbe" if PAL else "disc/Default.xbe")
XBE_SHA256 = pal_port.PAL_XBE_SHA256 if PAL else "7ce12851850ab46766f77c612b251afe7c8bd81f879e11b4836bdf766be542c6"
SEEDS = ROOT / ("tools/sc2/seed_functions_pal.json" if PAL else "tools/sc2/seed_functions.json")
BUILD = "build/%s-pal" if PAL else "build/%s"
RUNTIME_OWN = ROOT / "runtime"


def runtime_own():
    """runtime/, or for --pal its copy at the PAL addresses (made once per run)."""
    global RUNTIME_OWN
    if PAL and RUNTIME_OWN == ROOT / "runtime":
        RUNTIME_OWN = pal_port.write_runtime(ROOT / (BUILD % "runtime"))
    return RUNTIME_OWN


def build_dir(variable, name):
    """A build folder: the release script's (SC2_SRC_DIR, SC2_RUNNER_DIR; "-pal" added for --pal) or build/<name>."""
    given = os.environ.get(variable)
    if given:
        return ROOT / (given.rstrip("/") + ("-pal" if PAL else ""))
    return ROOT / (BUILD % name)
PYTHON = PRIVATE / ("venv/Scripts/python.exe" if os.name == "nt" else "venv/bin/python")


def link(target, name):
    """A symlink; on Windows without symlink rights a copy (a directory junction for directories)."""
    try:
        os.symlink(target, name)
    except OSError:
        if Path(target).is_dir():
            subprocess.check_call(["cmd", "/c", "mklink", "/J", str(name), str(Path(target).resolve())],
                                  stdout=subprocess.DEVNULL)
        else:
            shutil.copy2(target, name)
# runtime/ files with no shared counterpart, built into the runner.
GAME_SOURCES = ["sc2_hooks.c", "sc2_settings.cpp", "sc2_input.cpp", "sc2_menu.cpp", "sc2_training.cpp", "sc2_practice.cpp", "sc2_guest.cpp", "sc2_netplay.cpp", "sc2_state.cpp", "sc2_dirty.cpp", "sc2_direct.c", "ggpo/backends/p2p.cpp", "ggpo/bitvector.cpp", "ggpo/game_input.cpp", "ggpo/input_queue.cpp", "ggpo/log.cpp", "ggpo/main.cpp", "ggpo/network/udp.cpp", "ggpo/network/udp_proto.cpp", "ggpo/poll.cpp", "ggpo/sync.cpp", "ggpo/timesync.cpp", "sc2_frame.cpp", "sc2_wm_select.c", "sc2_guest_items.c", "sc2_guest_gallery.c", "sc2_android.cpp", "sc2_touch.cpp", "sc2_pictures.cpp", "sc2_jpeg.cpp", "x87_math/sin.c", "x87_math/cos.c", "x87_math/__sin.c", "x87_math/__cos.c", "x87_math/__rem_pio2.c", "x87_math/__rem_pio2_large.c", "x87_math/atan.c", "x87_math/atan2.c", "x87_math/log2.c", "x87_math/log2_data.c", "x87_math/expm1.c", "../recomp-common/launcher/src/online.cpp", "../recomp-common/launcher/src/run_program.cpp", "../recomp-common/menu.cpp", "../recomp-common/settings.cpp"]


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def run(args, log, cwd=None):
    log.parent.mkdir(parents=True, exist_ok=True)
    print("  " + " ".join(str(a) for a in args[:4]) + (" ..." if len(args) > 4 else ""), flush=True)
    with open(log, "w") as out:
        if subprocess.call([str(a) for a in args], cwd=cwd, stdout=out, stderr=subprocess.STDOUT):
            tail = log.read_text(errors="replace").splitlines()[-30:]
            sys.exit("failed, see %s:\n%s" % (log, "\n".join(tail)))


def program_manifest(generated):
    """The runtime CMake's manifest of the generated chunks, and their uninitialized-EBP count."""
    names = sorted(p.name for p in generated.glob("recomp_[0-9][0-9][0-9][0-9].c"))
    names += ["recomp_dispatch.c", "recomp_funcs.h"]
    text, ebp = "", 0
    for name in names:
        path = generated / name
        text += "%s\t%d\t%s\n" % (name, path.stat().st_size, sha256(path))
        if name.endswith(".c"):
            ebp += len(re.findall(r"\n[ \t]+uint32_t ebp;[ \r]*\n", path.read_text(errors="replace")))
    return hashlib.sha256(text.encode()).hexdigest(), ebp


def bound_addresses():
    """Every guest address a runtime/ lookup table binds.

    The lifter gets them as --manual-functions: their converted bodies are
    left out and every call or tail jump to them goes through the runtime's
    lookup, so a tail jump into a replaced SDK function cannot run the
    original (docs/bring-up.md)."""
    found = set()
    for path in sorted(runtime_own().glob("*.c")):
        text = path.read_text()
        # Hooks wrap the converted function and call it, so it stays converted.
        for body in re.findall(r"RecompFunction recomp_(?!sc2_hooks_)\w*lookup_manual\(uint32_t \w+\)\n\{(.*?)\n\}",
                               text, re.S):
            found.update(int(m, 16) for m in re.findall(r"0x([0-9a-fA-F]{5,8})u", body))
        # Named enum constants (input adapter) referenced by those tables.
        for name, value in re.findall(r"(\w+_ADDRESS) = 0x([0-9a-fA-F]+)u", text):
            found.add(int(value, 16))
    return sorted(found)


def manual_list():
    path = PROGRAM / "manual_functions.json"
    entries = ["0x%08X" % a for a in bound_addresses()]
    path.write_text(json.dumps(entries, indent=0) + "\n")
    return path, hashlib.sha256(path.read_bytes()).hexdigest()


def lift():
    if sha256(XBE) != XBE_SHA256:
        sys.exit("%s is not the supported Soulcalibur II %s executable" % (XBE, "(Europe, Australia)" if PAL else "(USA)"))
    logs = PRIVATE / ("logs-pal" if PAL else "logs")
    logs.mkdir(parents=True, exist_ok=True)
    PROGRAM.mkdir(parents=True, exist_ok=True)
    seeds = SEEDS
    run([PYTHON, "-u", "-m", "tools.xbe_parser", XBE, "--json", PROGRAM / "analysis.json"],
        logs / "xbe-parser.log", LIFTER)
    run([PYTHON, "-u", "-m", "tools.disasm", XBE, "-o", PROGRAM / "disasm",
         "--analysis-json", PROGRAM / "analysis.json", "--seed-functions", seeds],
        logs / "disasm.log", LIFTER)
    rows = json.loads((PROGRAM / "disasm/functions.json").read_text())
    rows = [{k: v for k, v in r.items() if k not in ("calls_to", "called_by")} for r in rows]
    (PROGRAM / "functions.json").write_text(json.dumps(rows))
    none = PROGRAM / "none"
    manual, manual_sha256 = manual_list()
    run([PYTHON, "-u", "-m", "tools.recomp", XBE, "--all", "--split", "1000",
         "--manual-functions", manual,
         "--game-name", "Soulcalibur II", "--disasm-dir", none,
         "--functions", PROGRAM / "functions.json", "--func-id-dir", none, "--abi-dir", none,
         "--icall-sites", PROGRAM / "none.json", "--gen-dir", PROGRAM / "generated",
         "--output-dir", PROGRAM / "metadata"], logs / "lift.log", LIFTER)
    manifest, ebp = program_manifest(PROGRAM / "generated")
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=LIFTER, text=True).strip()
    (PROGRAM / "receipt.json").write_text(json.dumps(dict(
        xbe_sha256=XBE_SHA256, lifter_commit=revision, manifest_sha256=manifest, ebp_count=ebp,
        manual_sha256=manual_sha256), indent=2) + "\n")
    print("lifted: manifest %s, uninitialized EBP prologues %d" % (manifest[:12], ebp))


def lift_is_current():
    """The lift in PROGRAM is the one lift() would make now: same executable, lifter and bound functions."""
    try:
        receipt = json.loads((PROGRAM / "receipt.json").read_text())
    except (OSError, ValueError):
        return False
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=LIFTER, text=True).strip()
    return (receipt.get("xbe_sha256") == XBE_SHA256 and receipt.get("lifter_commit") == revision
            and manual_list()[1] == receipt.get("manual_sha256")
            and program_manifest(PROGRAM / "generated") == (receipt["manifest_sha256"], receipt["ebp_count"]))


def direct_calls(runtime):
    """Each direct call of the lifted program asked the runtime whether the function is one it wraps (RECOMP_ABI_CALL,
    recomp_lookup_manual): thousands a frame, for the few dozen that are. Which those are is known here: the addresses
    in the runtime's own tables among the functions called directly. recomp_types.h (a copy of the shared one) asks
    only for them, the compiler deciding by the call's constant address; sc2_direct.c's list lets the program check at
    its start that the tables bind no other (runner.cpp)."""
    targets = set()
    for path in sorted((PROGRAM / "generated").glob("recomp_0*.c")):
        targets |= {int(m, 16) for m in re.findall(r"RECOMP_ABI_CALL\(0x([0-9A-Fa-f]+)u", path.read_text(encoding="utf-8"))}
    named = set()
    for path in sorted(runtime_own().glob("*.c")):
        named |= {int(m, 16) for m in re.findall(r"0x([0-9a-fA-F]{5,8})u", path.read_text(encoding="utf-8"))}
    hooked = sorted(targets & named)
    types = runtime / "recomp_types.h"
    text = types.read_text(encoding="utf-8")
    old = "    RecompFunction manual = recomp_lookup_manual((uint32_t)(va)); \\\n    if (manual != NULL) manual(); \\\n    else (fn)(); \\\n"
    if text.count(old) != 1:
        sys.exit("recomp_types.h: RECOMP_ABI_CALL is not as direct_calls knows it")
    text = text.replace(old, "    RecompFunction manual = SC2_MAY_BE_HOOKED((uint32_t)(va)) ? recomp_lookup_manual((uint32_t)(va)) : NULL; \\\n"
                             "    if (manual != NULL) manual(); \\\n    else (fn)(); \\\n")
    text = text.replace("#define RECOMP_ABI_CALL(va, fn)", "#define SC2_MAY_BE_HOOKED(va) (0 \\\n" + "".join(
        "    || (va) == 0x%08Xu \\\n" % a for a in hooked) + "    )\n#define RECOMP_ABI_CALL(va, fn)", 1)
    types.unlink()
    types.write_text(text, encoding="utf-8")
    (runtime / "sc2_direct.c").write_text("#include <stdint.h>\nconst uint32_t sc2_direct_targets[] = {%s};\n"
        "const unsigned sc2_direct_target_count = %du;\nconst uint32_t sc2_direct_hooked[] = {%s};\n"
        "const unsigned sc2_direct_hooked_count = %du;\n" % (
            ",".join("0x%Xu" % a for a in sorted(targets)), len(targets), ",".join("0x%Xu" % a for a in hooked) or "0",
            len(hooked)), encoding="utf-8")


def merge():
    """build/src/recomp-runtime: shared files linked, runtime/ files replacing them."""
    src = build_dir("SC2_SRC_DIR", "src")
    if src.exists():
        # (junctions first, so the tree's removal never reaches what they point at)
        for entry in [src / n for n in ("xbe", "recomp-common", "tools", "private", "recomp-runtime/x87_math", "recomp-runtime/ggpo")]:
            if entry.is_junction() or entry.is_symlink():
                os.unlink(entry) if entry.is_symlink() else os.rmdir(entry)
        shutil.rmtree(src)
    runtime = src / "recomp-runtime"
    runtime.mkdir(parents=True)
    own = {p.name: p for p in runtime_own().iterdir() if p.is_file()}
    for path in sorted((SHARED / "recomp-runtime").iterdir()):
        if path.is_file():
            link(own.pop(path.name, path), runtime / path.name)
    for name, path in own.items():
        link(path, runtime / name)
    # (the x87's math, musl's: the same on every host)
    link(ROOT / "runtime/x87_math", runtime / "x87_math")
    link(ROOT / "runtime/ggpo", runtime / "ggpo")   # (T6's rollback library)
    direct_calls(runtime)
    # (the version, which online play says to the other side: two copies must have the same)
    (runtime / "sc2_version.h").write_text('#define SC2_VERSION "%s"\n#define SC2_PAL %d\n' % (
        (ROOT / "VERSION").read_text().strip(), int(PAL)))
    link(SHARED / "xbe", src / "xbe")
    link(ROOT / "recomp-common", src / "recomp-common")
    link(SHARED / "tools", src / "tools")
    link(PRIVATE, src / "private")
    return runtime


def build():
    receipt = json.loads((PROGRAM / "receipt.json").read_text())
    if program_manifest(PROGRAM / "generated") != (receipt["manifest_sha256"], receipt["ebp_count"]):
        sys.exit("private/program/generated differs from its receipt; lift again")
    if manual_list()[1] != receipt.get("manual_sha256"):
        sys.exit("runtime/ binds a different set of functions than the last lift; lift again")
    runtime = merge()
    out = build_dir("SC2_RUNNER_DIR", "runner")
    logs = PRIVATE / ("logs-pal" if PAL else "logs")
    run(["cmake", "-S", runtime, "-B", out, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", *EXTRA_CMAKE,
         "-DRECOMP_PROGRAM_DIR=%s" % (PROGRAM / "generated"),
         "-DRECOMP_XBE_SHA256=%s" % XBE_SHA256,
         "-DRECOMP_PROGRAM_MANIFEST_SHA256=%s" % receipt["manifest_sha256"],
         "-DRECOMP_PROGRAM_EBP_EXPECTED=%d" % receipt["ebp_count"],
         "-DRECOMP_GAME_SOURCES=%s" % ";".join(GAME_SOURCES + (["sc2.rc"] if os.name == "nt" or "LLVM_MINGW" in os.environ else [])),
         "-DRECOMP_PROGRAM_INCLUDE=%s" % (runtime / "x87_math.h"),
         "-DCMAKE_PROJECT_INCLUDE=%s" % (ROOT / "tools/file_prefix.cmake"), "-DSC2_SOURCE_ROOT=%s" % ROOT],
        logs / "configure.log")
    jobs = os.environ.get("SC2_JOBS", str(min(6, os.cpu_count() or 1)))
    run(["cmake", "--build", out, "--parallel", jobs, "--target", "recomp_program_runner"],
        logs / "build.log")
    print("built %s" % (out / "recomp_program_runner"))


def launcher():
    out = ROOT / os.environ.get("SC2_LAUNCHER_DIR", "build/launcher")
    logs = PRIVATE / "logs"
    run(["cmake", "-S", ROOT / "launcher", "-B", out, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", *EXTRA_CMAKE],
        logs / "launcher-configure.log")
    run(["cmake", "--build", out, "--parallel", os.environ.get("SC2_JOBS", str(min(6, os.cpu_count() or 1)))], logs / "launcher-build.log")
    print("built %s" % (out / "soulcalibur2-launcher"))


APP_NAME = "Soulcalibur II Recomp"
APP_ID = "io.github.ekspym.sc2online"


def app():
    """build/<APP_NAME>.app: the launcher, the game beside it as soulcalibur2, signed ad hoc. It links the
    system's SDL3 and shader libraries (Homebrew): a build for this Mac, not a release."""
    bundle = ROOT / "build" / (APP_NAME + ".app")
    if bundle.exists():
        shutil.rmtree(bundle)
    macos = bundle / "Contents/MacOS"
    macos.mkdir(parents=True)
    (bundle / "Contents/Resources").mkdir()
    shutil.copy2(ROOT / "build/launcher/soulcalibur2-launcher", macos / "soulcalibur2-launcher")
    shutil.copy2(ROOT / "build/runner/recomp_program_runner", macos / "soulcalibur2")
    # (and the PAL disc's program, when its lift is here: the US program hands a PAL disc to it, runner.cpp)
    if (PRIVATE / "program-pal").is_dir():
        subprocess.check_call([sys.executable, str(Path(__file__)), "--pal", "build"])
        shutil.copy2(ROOT / "build/runner-pal/recomp_program_runner", macos / "soulcalibur2-pal")
    else:
        print("(no private/program-pal: the app plays the US disc only)")
    # (the Shader feature's presets, sc2_settings.h ShaderPreset: tools/shaders.sh)
    shaders = subprocess.check_output([str(ROOT / "tools/shaders.sh")], text=True).strip()
    shutil.copytree(shaders, bundle / "Contents/Resources/shaders")
    version = (ROOT / "VERSION").read_text().strip()
    plist = (ROOT / "tools/release/Info.plist").read_text()
    for key, value in (("@VERSION@", version), ("@NAME@", APP_NAME), ("@APP_ID@", APP_ID),
                       ("@EXE@", "soulcalibur2-launcher")):
        plist = plist.replace(key, value)
    (bundle / "Contents/Info.plist").write_text(plist)
    subprocess.check_call(["codesign", "--force", "--deep", "-s", "-", str(bundle)])
    print("built %s" % bundle)


if __name__ == "__main__":
    step = sys.argv[1] if len(sys.argv) > 1 else "all"
    if step in ("lift", "all"):
        lift()
    if step == "current":
        if lift_is_current():
            print("%s is current" % PROGRAM.relative_to(ROOT))
        else:
            lift()
    if step in ("build", "all", "app"):
        build()
    if step in ("launcher", "app"):
        launcher()
    if step == "app":
        app()
