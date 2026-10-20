# Run the odb compiler on the odb-tests headers (with the per-test options
# from the buildfiles) and syntax-check the generated code with g++.
#
import os, re, sys, shlex, subprocess, concurrent.futures as cf

S = os.path.dirname(os.path.abspath(__file__))
ROOT = os.environ.get('ROOT', os.path.normpath(os.path.join(S, '..', '..', '..')))
TESTS = os.path.join(ROOT, 'odb-tests')
ODB = os.environ['ODB']
INC = os.path.join(S, 'inc')
OUT = os.path.join(os.environ.get('WORK', os.getcwd()), os.environ.get('OUTDIR', 'out'))
GXX = os.environ.get('GXX', 'g++')

DB = sys.argv[1] if len(sys.argv) > 1 else 'sqlite'
ONLY = sys.argv[2:]

def var(txt, name):
    m = re.search(r'^' + name + r'\s*=\s*((?:.*\\\n)*.*)$', txt, re.M)
    if not m:
        return None
    s = m.group(1).replace('\\\n', ' ')
    s = re.sub(r'\(\$cplusplus >= \d+ \? (\S+) : \S+\)', r'\1', s)
    return s

def includes(d, out):
    return ['-I' + out, '-I' + d, '-I' + OUT, '-I' + os.path.join(OUT, 'common'),
            '-I' + INC, '-I' + TESTS, '-I' + os.path.join(TESTS, 'common'),
            '-I' + os.path.join(ROOT, 'libodb'),
            '-I' + os.path.join(ROOT, 'libodb-' + DB)]

def run(d):
    txt = open(os.path.join(d, 'buildfile'), encoding='utf-8').read()
    o = var(txt, 'odb_options')
    opts = shlex.split(o, posix=True) if o else []
    hdrs = sorted(set(re.findall(r'<\{hxx ixx cxx\}\{([\w-]+?)-odb', txt)))
    h = var(txt, 'hdrs')
    if h and '$h-odb' in txt:
        hdrs = h.split()
    if not hdrs:
        hdrs = ['test'] if os.path.exists(os.path.join(d, 'test.hxx')) else []
    name = os.path.relpath(d, TESTS).replace('\\', '/')
    out = os.path.join(OUT, name)
    os.makedirs(out, exist_ok=True)
    opts = [o.replace('$src_base', d).replace('$out_base', out) for o in opts]
    std = '--std' in opts
    stdv = opts[opts.index('--std') + 1] if std else 'c++11'
    res = []
    gen = []
    for h in hdrs:
        hp = os.path.join(d, h + '.hxx')
        cmd = [ODB] + ([] if std else ['--std', 'c++11']) + \
              ['--database', DB, '--output-dir', out] + opts + \
              (['--sqlite-version', '3.45.0'] if DB == 'sqlite' else []) + \
              includes(d, out) + [hp]
        p = subprocess.run(cmd, capture_output=True, text=True, cwd=out)
        if p.returncode != 0:
            res.append((name, h, 'odb', p.stderr.strip()[-3000:]))
        else:
            gen.append(h)
    gstd = {'c++11': 'c++11', 'c++14': 'c++14', 'c++17': 'c++17',
            'c++20': 'c++20'}.get(stdv, 'c++17')
    for h in gen:
        cxx = os.path.join(out, h + '-odb.cxx')
        p = subprocess.run([GXX, '-std=' + gstd, '-fsyntax-only', '-w',
                            '-DDATABASE_' + DB.upper()] + includes(d, out) +
                           [cxx], capture_output=True, text=True, cwd=out)
        if p.returncode != 0:
            res.append((name, h, 'g++', p.stderr.strip()[:3000]))
        else:
            res.append((name, h, 'ok', ''))
    return res

if __name__ == '__main__':
    dirs = []
    for grp in ['common', DB]:
        for r, ds, fs in os.walk(os.path.join(TESTS, grp)):
            ds.sort()
            if 'buildfile' in fs and any(f.endswith('.hxx') for f in fs):
                if ONLY and not any(o in r for o in ONLY):
                    continue
                dirs.append(r)

    ok = fail = 0
    with cf.ThreadPoolExecutor(8) as ex:
        for res in ex.map(run, dirs):
            for name, h, st, err in res:
                if st == 'ok':
                    ok += 1
                else:
                    fail += 1
                    print('=== FAIL [%s] %s/%s' % (st, name, h))
                    print(err)
    print('ok=%d fail=%d' % (ok, fail))
    sys.exit(1 if fail != 0 or ok == 0 else 0)
