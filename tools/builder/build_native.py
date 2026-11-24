"""SC2 Online: builds the game program (native\\soulcalibur2.exe) on the player's PC from his OWN Soulcalibur II (USA) Xbox
disc image, as Tekken 6 Online's builder: nothing of the game is in the package. The disc's Default.xbe is read from the
image, translated to C here (the lifter), compiled with the bundled compiler and linked with the prebuilt runtime.
The game reads its data from the image itself at every start (the launcher gives it the path).

usage:  python build_native.py <image.iso>          build (progress lines below, log native\\build.log)
        python build_native.py --check <image.iso>  only check that it is the supported disc
        python build_native.py --relink             link again (an update changed only the prebuilt parts)
Output lines for the launcher: STEP <n>/<N> <text>, PROGRESS <0-100>, DONE <exe>, OK, ERROR <text>; exit 0 / 1."""
import concurrent.futures, hashlib, json, os, re, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))     # builder
ROOT = os.path.dirname(HERE)                          # package
OUT = os.path.join(ROOT, 'native')
WORK = os.path.join(OUT, 'work')
PROGRAM = os.path.join(WORK, 'program')
GEN = os.path.join(PROGRAM, 'generated')
OBJ = os.path.join(OUT, 'obj')
EXE = os.path.join(OUT, 'soulcalibur2.exe')
TC = os.path.join(HERE, 'llvm-mingw', 'bin')
PY = sys.executable
CFG = json.load(open(os.path.join(HERE, 'compile.json')))
STEPS = 4
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
log_file = None
sys.path.insert(0, HERE)
import xiso_xbe  # noqa: E402


def say(msg):
    print(msg, flush=True)
    if log_file:
        log_file.write(msg + '\n')
        log_file.flush()


def fail(msg):
    say('ERROR ' + msg)
    sys.exit(1)


def step(n, text):
    say(f'STEP {n}/{STEPS} {text}')


def run(args, what, cwd=None):
    t = time.time()
    p = subprocess.run([str(a) for a in args], cwd=cwd, capture_output=True, text=True, errors='replace', creationflags=NO_WINDOW)
    if log_file:
        log_file.write(f'$ {" ".join(str(a) for a in args)[:400]}\n{p.stdout[-4000:]}{p.stderr[-4000:]}\n')
    if p.returncode:
        fail(f'{what} failed (code {p.returncode}), see native\\build.log')
    return time.time() - t


def read_xbe(image):
    """The image's Default.xbe as the port takes it, or an error text"""
    try:
        with open(image, 'rb') as f:
            partition, directory = xiso_xbe.root(f)
            xbe = bytearray(xiso_xbe.find(f, partition, directory, 'default.xbe'))
    except SystemExit as e:
        return None, str(e)
    except OSError as e:
        return None, 'the disc image cannot be read (%s)' % e.strerror
    digest = hashlib.sha256(xbe).hexdigest()
    if digest == xiso_xbe.RETAIL and xbe[xiso_xbe.PATCH_AT] == xiso_xbe.PATCH_RETAIL:
        xbe[xiso_xbe.PATCH_AT] = xiso_xbe.PATCH_BYTE
        digest = hashlib.sha256(xbe).hexdigest()
    if digest != CFG['xbe_sha256']:
        return None, 'this is not Soulcalibur II (USA) for the Xbox (its Default.xbe begins %s)' % digest[:12]
    return bytes(xbe), None


def manifest(gen):
    names = sorted(n for n in os.listdir(gen) if re.fullmatch(r'recomp_\d{4}\.c', n)) + ['recomp_dispatch.c', 'recomp_funcs.h']
    text = ''
    for n in names:
        data = open(os.path.join(gen, n), 'rb').read()
        text += '%s\t%d\t%s\n' % (n, len(data), hashlib.sha256(data).hexdigest())
    return hashlib.sha256(text.encode()).hexdigest(), names


def lift(xbe_path):
    """The lifter's three passes, with the arguments of the development build (tools/build_game.py lift)"""
    lifter = os.path.join(HERE, 'lifter')
    seeds, manual = os.path.join(HERE, 'project', 'seed_functions.json'), os.path.join(HERE, 'project', 'manual_functions.json')
    if hashlib.sha256(open(manual, 'rb').read()).hexdigest() != CFG['manual_sha256']:
        fail('the builder is damaged (manual_functions.json) - extract the whole package again')
    if os.path.isdir(PROGRAM):
        shutil.rmtree(PROGRAM)
    os.makedirs(PROGRAM)
    none = os.path.join(PROGRAM, 'none')
    say('PROGRESS 5')
    run([PY, '-u', '-m', 'tools.xbe_parser', xbe_path, '--json', os.path.join(PROGRAM, 'analysis.json')], 'reading Default.xbe', lifter)
    say('PROGRESS 10')
    run([PY, '-u', '-m', 'tools.disasm', xbe_path, '-o', os.path.join(PROGRAM, 'disasm'), '--analysis-json',
         os.path.join(PROGRAM, 'analysis.json'), '--seed-functions', seeds], 'disassembly', lifter)
    say('PROGRESS 60')
    rows = json.load(open(os.path.join(PROGRAM, 'disasm', 'functions.json')))
    rows = [{k: v for k, v in r.items() if k not in ('calls_to', 'called_by')} for r in rows]
    json.dump(rows, open(os.path.join(PROGRAM, 'functions.json'), 'w'))
    run([PY, '-u', '-m', 'tools.recomp', xbe_path, '--all', '--split', '1000', '--manual-functions', manual,
         '--game-name', 'Soulcalibur II', '--disasm-dir', none, '--functions', os.path.join(PROGRAM, 'functions.json'),
         '--func-id-dir', none, '--abi-dir', none, '--icall-sites', os.path.join(PROGRAM, 'none.json'),
         '--gen-dir', GEN, '--output-dir', os.path.join(PROGRAM, 'metadata')], 'translation to C', lifter)
    say('PROGRESS 75')
    got, names = manifest(GEN)
    if got != CFG['manifest_sha256']:
        fail('the translated program differs from the one this version was made for (%s) - report it with native\\build.log' % got[:12])
    # (as the runtime's CMake: a function the program calls but has no body of fails loudly)
    defs, refs = set(), set()
    for n in names:
        if n.endswith('.c'):
            text = open(os.path.join(GEN, n), encoding='utf-8', errors='replace').read()
            defs |= set(re.findall(r'void\s+(sub_[0-9A-Fa-f]+)\s*\(\s*void\s*\)\s*\{', text))
            refs |= set(re.findall(r'sub_[0-9A-Fa-f]+', text))
    with open(os.path.join(GEN, 'recomp_program_missing.c'), 'w', newline='\n') as f:
        f.write('#include <stdint.h>\nvoid recomp_program_missing(uint32_t);\n')
        for s in sorted(refs - defs):
            f.write('void %s(void) { recomp_program_missing(0x%su); }\n' % (s, s[4:]))
    return [n for n in names if n.endswith('.c')] + ['recomp_program_missing.c']


def sub(a):
    return (a.replace('{SDK}', os.path.join(HERE, 'sdk')).replace('{XBE}', os.path.join(HERE, 'xbe'))
            .replace('{GEN}', GEN).replace('{LIB}', os.path.join(HERE, 'lib')))


def compile_all(sources):
    os.makedirs(OBJ, exist_ok=True)
    cc = os.path.join(TC, 'x86_64-w64-mingw32-clang.exe')
    incs = [x for i in CFG['includes'] for x in ('-I', sub(i))]
    jobs = []
    for s in sources:
        o = os.path.join(OBJ, s + '.obj')
        jobs.append(([cc, *CFG['defines'], *incs, *[sub(a) for a in CFG['cflags']], '-ffile-prefix-map=%s=.' % ROOT, '-c', os.path.join(GEN, s), '-o', o], o))
    done = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for fut in concurrent.futures.as_completed([pool.submit(run, a, 'compiling the game code') for a, _ in jobs]):
            fut.result()
            done += 1
            say('PROGRESS %d' % (75 + 20 * done // len(jobs)))
    return [o for _, o in jobs]


def link(objs):
    tmp = EXE + '.new'
    run([os.path.join(TC, 'x86_64-w64-mingw32-clang++.exe'), *CFG['link'], *[os.path.join(HERE, 'lib', o) for o in CFG['objects']], *objs,
         *[sub(l) for l in CFG['libs']], '-Wl,--no-insert-timestamp', '-o', tmp], 'link')
    os.replace(tmp, EXE)
    open(os.path.join(OUT, 'build.version'), 'w').write(CFG['version'] + '\n')


def main():
    global log_file
    args = sys.argv[1:]
    if not args:
        fail('usage: build_native.py <image.iso> | --check <image.iso> | --relink')
    if args[0] == '--check':
        _, err = read_xbe(args[1])
        if err:
            fail(err)
        say('OK Soulcalibur II (USA)')
        return
    os.makedirs(OUT, exist_ok=True)
    log_file = open(os.path.join(OUT, 'build.log'), 'w', encoding='utf-8')
    t = time.time()
    if args[0] == '--relink':
        step(4, 'Linking the game…')
        objs = [os.path.join(OBJ, n) for n in sorted(os.listdir(OBJ))] if os.path.isdir(OBJ) else []
        if not objs:
            fail('the game was never built here - build it from your disc image')
        link(objs)
    else:
        step(1, 'Reading your disc image…')
        xbe, err = read_xbe(args[0])
        if err:
            fail(err)
        os.makedirs(WORK, exist_ok=True)
        xbe_path = os.path.join(WORK, 'Default.xbe')
        open(xbe_path, 'wb').write(xbe)
        step(2, 'Translating the game program… (several minutes)')
        sources = lift(xbe_path)
        step(3, 'Compiling the game…')
        if os.path.isdir(OBJ):
            shutil.rmtree(OBJ)
        objs = compile_all(sources)
        step(4, 'Linking the game…')
        link(objs)
    say('PROGRESS 100')
    say('DONE %s (%.0f s)' % (EXE, time.time() - t))


if __name__ == '__main__':
    main()
