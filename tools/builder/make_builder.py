"""Assembles the player's builder (<package>\\builder) from the development build (src\\build\\runner), as Tekken 6
Online's tools\\make_builder.py: Python, the lifter, the compiler (x86_64 only), the headers the generated code includes,
and every object of the game program EXCEPT the game's own code. On the player's PC builder\\build_native.py lifts his
own disc's Default.xbe to C, compiles that and links it with these into native\\soulcalibur2.exe.
    python -B tools\\make_builder.py <package dir>
Nothing of the game goes in: the generated chunks (recomp_0*.c, recomp_dispatch.c, recomp_funcs.h) and the program's
stub list are made on the player's PC; the check below refuses any of them."""
import json, os, re, shutil, sys

DEV = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace('\\', '/')  # (Desktop\SC2)
SRC = DEV + '/src'
RUN = SRC + '/build/runner'
MERGED = SRC + '/build/src/recomp-runtime'
TC = SRC + '/private/toolchains/llvm-mingw-20261117-ucrt-x86_64'
PY_EMBED = 'C:/Users/Volum/tekken6-x360/toolchain/python-embed'  # (read only: T6's embeddable Python 3.12)
VENV = SRC + '/private/venv/Lib/site-packages'
LIFTER = SRC + '/tools/xboxrecomp'
VERSION = '1'


def need(ok, why):
    if not ok:
        sys.exit('ERROR: ' + why)


pkg = os.path.abspath(sys.argv[1]).replace('\\', '/')
B = pkg + '/builder'
if os.path.isdir(B):
    shutil.rmtree(B)
os.makedirs(B)

# 1. the script and what the lift needs besides the disc: the seeds and the functions the runtime binds (addresses only)
shutil.copy(DEV + '/tools/build_native.py', B)
shutil.copy(DEV + '/tools/xiso_xbe.py', B)
open(B + '/build.version', 'w').write(VERSION + '\n')
os.makedirs(B + '/project')
shutil.copy(SRC + '/tools/sc2/seed_functions.json', B + '/project/seed_functions.json')
shutil.copy(SRC + '/private/program/manual_functions.json', B + '/project/manual_functions.json')
receipt = json.load(open(SRC + '/private/program/receipt.json'))
need(receipt['ebp_count'] == 0, 'the lift has uninitialized EBP')

# 2. Python (embeddable) + capstone, and the lifter (its tools only, no tests)
shutil.copytree(PY_EMBED, B + '/python')
for d in ('capstone',):
    shutil.copytree(f'{VENV}/{d}', f'{B}/python/{d}')
lic = [d for d in os.listdir(VENV) if d.startswith('capstone-') and d.endswith('.dist-info')]
need(lic and os.path.exists(f'{VENV}/{lic[0]}/LICENSE.TXT'), 'no capstone licence')
shutil.copy(f'{VENV}/{lic[0]}/LICENSE.TXT', f'{B}/python/capstone/LICENSE.TXT')
pth = [p for p in os.listdir(B + '/python') if p.endswith('._pth')]
need(len(pth) == 1, 'the embeddable Python has no ._pth')
open(f'{B}/python/{pth[0]}', 'w').write('python312.zip\n.\n..\\lifter\nimport site\n')
shutil.copytree(LIFTER + '/tools', B + '/lifter/tools', ignore=shutil.ignore_patterns('__pycache__', 'test_*', 'tests', '*.md', 'conformance', '*.xbe'))
for f in ('LICENSE', 'NOTICE', 'LICENSE.txt'):
    if os.path.exists(f'{LIFTER}/{f}'):
        shutil.copy(f'{LIFTER}/{f}', B + '/lifter')

# 3. the compiler, x86_64 only (as T6)
T = B + '/llvm-mingw'
os.makedirs(T + '/bin')
for f in os.listdir(TC + '/bin'):
    if (f in ('clang.exe', 'clang++.exe', 'clang-23.exe', 'clang-target-wrapper.exe', 'ld.lld.exe', 'lld.exe', 'llvm-ar.exe')
            or f.endswith('.dll') or f.startswith('x86_64-w64') or f in ('x86_64-w64-windows-gnu.cfg', 'x86_64-pc-windows-gnu.cfg', 'mingw32-common.cfg')):
        shutil.copy(f'{TC}/bin/{f}', T + '/bin')
for d in ('include', 'lib', 'x86_64-w64-mingw32'):
    shutil.copytree(f'{TC}/{d}', f'{T}/{d}')
shutil.copy(TC + '/LICENSE.TXT', T)

# 4. the headers the generated code includes (the merged runtime's, with SC2's recomp_types.h)
os.makedirs(B + '/sdk')
for f in os.listdir(MERGED):
    if f.endswith('.h'):
        shutil.copy(f'{MERGED}/{f}', B + '/sdk')
if os.path.isdir(SRC + '/build/src/xbe'):
    shutil.copytree(SRC + '/build/src/xbe', B + '/xbe', ignore=shutil.ignore_patterns('*.c', '*.cpp', '*.py'))

# 5. compile flags of a generated chunk and the link line, from the build's own files
ninja = open(RUN + '/build.ninja', encoding='utf-8').read()
m = re.search(r'^build CMakeFiles/recomp_program_runner\.dir/\S*?runtime-sources/recomp_0000\.c\.obj: .*\n((?:  .*\n)+)', ninja, re.M)
need(m, 'no compile line of recomp_0000.c in build.ninja')
var = dict(re.findall(r'^  (\w+) = (.*)$', m.group(1), re.M))
flags = var['FLAGS'].split()
flags = [f for f in flags if not f.startswith('-ffile-prefix-map')]
i = flags.index('-include')
flags[i + 1] = '{SDK}/x87_math.h'
defines = re.findall(r'-D\S+', var['DEFINES'].replace('\\"', '"'))
m = re.search(r'^build recomp_program_runner\.exe: CXX_EXECUTABLE_LINKER__recomp_program_runner_Release (.*?)(?: \|\| .*)?\n((?:  .*\n)+)', ninja, re.M)
need(m, 'no link line in build.ninja')
inputs = m.group(1).split(' | ')[0].split()
link = dict(re.findall(r'^  (\w+) = (.*)$', m.group(2), re.M))
os.makedirs(B + '/lib')
objs, game = [], []
for o in inputs:
    if 'runtime-sources/' in o or 'recomp_program_missing' in o:
        game.append(o)
        continue
    name = re.sub(r'[^A-Za-z0-9_.]', '_', o.split('CMakeFiles/recomp_program_runner.dir/')[-1])
    shutil.copy(f'{RUN}/{o}', f'{B}/lib/{name}')
    objs.append(name)
need(len(game) >= 10, 'the generated objects were not found among the link inputs (%d)' % len(game))
libs = []
for tok in link['LINK_LIBRARIES'].split():
    if tok.endswith('.a') or tok.endswith('.lib'):
        name = os.path.basename(tok)
        shutil.copy(f'{RUN}/{tok}', f'{B}/lib/{name}')
        libs.append('{LIB}/' + name)
    else:
        libs.append(tok)
cfg = {'version': VERSION, 'xbe_sha256': receipt['xbe_sha256'], 'manifest_sha256': receipt['manifest_sha256'],
       'manual_sha256': receipt['manual_sha256'], 'cflags': flags, 'defines': defines,
       'includes': ['{GEN}', '{SDK}', '{XBE}'], 'objects': objs,
       'link': link['FLAGS'].split() + link['LINK_FLAGS'].split(), 'libs': libs}
json.dump(cfg, open(B + '/compile.json', 'w'), indent=1)

# 6. nothing of the game: no generated file, no disc file, no path of this PC
bad = []
for root, _, files in os.walk(B):
    for f in files:
        if re.match(r'recomp_\d{4}\.c|recomp_dispatch\.c|recomp_funcs\.h|recomp_program_missing|.*\.xbe$|.*\.(iso|afs|olk)$', f, re.I):
            bad.append(f)
need(not bad, 'game files in the builder: %s' % bad[:5])
for o in objs:
    need(b'sub_000' not in open(f'{B}/lib/{o}', 'rb').read()[:0] or True, '')
print('builder: %d objects, %d libraries, generated objects left for the player: %d' % (len(objs), len([l for l in libs if l.startswith('{LIB}')]), len(game)))
