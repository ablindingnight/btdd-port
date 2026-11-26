#!/usr/bin/env python3
"""package.py: build the tester zip (Windows, MSVC).

  python tools/package.py [--out DIR] [--skip-build] [--rom ROM] [--no-test]

1. Builds Release with Visual Studio's CMake into build-pkg: static CRT (/MT, no VC++ redistributable), SDL static,
   BTDD_LIBTCC=ON (the native compiler is libtcc, a separate LGPL DLL).
2. Stages PondToPavement/ in OUT/stage-<date>: the four exes, libtcc.dll, TinyCC's include/ + lib/ (tcc/) and its
   licence, meta/btdd.meta, our C headers the native build compiles against (src/, recomp/), fonts + OFL licences,
   art, LICENSE, "How to play.txt", "Play online.txt". Never the ROM, Gamefiles, private/, generated/ or caches.
3. Checks: tools/release_audit.py on the staged folder; the exes import only Windows DLLs (+ libtcc.dll).
4. Zips to OUT/PondToPavement-tester-<date>.zip, audits the zip's files again (extracted to a fresh folder) and, with
   --rom, runs "PondToPavement --selftest --rom ROM" from that folder with settings and cache kept inside it.
OUT defaults to ..\\..\\release (C:\\Users\\...\\Battle\\release), outside the repo.
"""
import argparse
import datetime
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BATTLE = os.path.dirname(REPO)
TINYCC = os.path.join(BATTLE, 'tools', 'tinycc')
VS = r'C:\Program Files\Microsoft Visual Studio\18\Community'
VS_CMAKE = os.path.join(VS, r'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe')
EXES = ['PondToPavement.exe', 'btdd_play.exe', 'btdd_jit.exe', 'btdd_headless.exe']
SYSTEM_DLLS = {'kernel32.dll', 'user32.dll', 'gdi32.dll', 'advapi32.dll', 'shell32.dll', 'ole32.dll', 'oleaut32.dll',
               'imm32.dll', 'version.dll', 'winmm.dll', 'setupapi.dll', 'ws2_32.dll', 'iphlpapi.dll', 'cfgmgr32.dll',
               'hid.dll', 'dxgi.dll', 'd3d11.dll', 'd3d12.dll', 'dwmapi.dll', 'uxtheme.dll', 'shlwapi.dll',
               'comdlg32.dll', 'msvcrt.dll', 'bcrypt.dll', 'crypt32.dll', 'combase.dll', 'ntdll.dll', 'opengl32.dll',
               'dinput8.dll', 'xinput1_4.dll', 'mfplat.dll', 'mf.dll', 'mfreadwrite.dll', 'avrt.dll', 'powrprof.dll',
               'userenv.dll', 'secur32.dll', 'rpcrt4.dll', 'sechost.dll', 'winhttp.dll', 'wininet.dll', 'normaliz.dll',
               'wtsapi32.dll', 'mmdevapi.dll', 'propsys.dll', 'dbghelp.dll', 'psapi.dll', 'libtcc.dll'}


def run(cmd, **kw):
    print('>', ' '.join(cmd))
    return subprocess.run(cmd, **kw)


def build():
    b = os.path.join(REPO, 'build-pkg')
    cfg = [VS_CMAKE, '-S', REPO, '-B', b, '-DBTDD_LIBTCC=ON', '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
           '-DBTDD_TINYCC_DIR=' + os.path.join(TINYCC, 'tcc')]
    if run(cfg, stdout=subprocess.DEVNULL).returncode:
        sys.exit('configure failed')
    targets = ['btdd_launcher', 'btdd_play', 'btdd_jit', 'btdd_headless']
    r = run([VS_CMAKE, '--build', b, '--config', 'Release', '--target'] + targets + ['--', '-m:4', '-nodeReuse:false'],
            capture_output=True, text=True)
    if r.returncode:
        print(r.stdout[-4000:])
        sys.exit('build failed')
    return os.path.join(b, 'Release')


def copytree(src, dst, pattern=None):
    for root, _, files in os.walk(src):
        for f in files:
            if pattern and not re.search(pattern, f):
                continue
            s = os.path.join(root, f)
            d = os.path.join(dst, os.path.relpath(s, src))
            os.makedirs(os.path.dirname(d), exist_ok=True)
            shutil.copy2(s, d)


def stage(bin_dir, dest):
    if os.path.exists(dest):
        shutil.rmtree(dest)
    os.makedirs(dest)
    for e in EXES:
        shutil.copy2(os.path.join(bin_dir, e), dest)
    tcc = os.path.join(TINYCC, 'tcc')
    shutil.copy2(os.path.join(tcc, 'libtcc.dll'), dest)
    copytree(os.path.join(tcc, 'include'), os.path.join(dest, 'tcc', 'include'))
    # the native build only needs the C library headers; the Windows API headers are not used (and their text trips
    # the ROM-string audit: they contain a word that is also one of the game's strings)
    shutil.rmtree(os.path.join(dest, 'tcc', 'include', 'winapi'), ignore_errors=True)
    os.remove(os.path.join(dest, 'tcc', 'include', 'excpt.h'))
    copytree(os.path.join(tcc, 'lib'), os.path.join(dest, 'tcc', 'lib'))
    lic = os.path.join(dest, 'licenses')
    os.makedirs(lic)
    shutil.copy2(os.path.join(TINYCC, 'tcc-0.9.27', 'COPYING'), os.path.join(lic, 'TinyCC-COPYING-LGPL.txt'))
    shutil.copy2(os.path.join(REPO, 'LICENSE'), os.path.join(dest, 'LICENSE.txt'))
    os.makedirs(os.path.join(dest, 'meta'))
    shutil.copy2(os.path.join(REPO, 'meta', 'btdd.meta'), os.path.join(dest, 'meta', 'btdd.meta'))
    copytree(os.path.join(REPO, 'src'), os.path.join(dest, 'src'), r'\.h$')  # the native build includes these
    copytree(os.path.join(REPO, 'recomp'), os.path.join(dest, 'recomp'), r'^native_rt\.h$')
    copytree(os.path.join(REPO, 'res', 'fonts'), os.path.join(dest, 'fonts'))
    copytree(os.path.join(REPO, 'res', 'art'), os.path.join(dest, 'art'))
    for f in glob.glob(os.path.join(dest, 'fonts', 'OFL-*.txt')):
        shutil.copy2(f, lic)
    shutil.copy2(os.path.join(REPO, 'docs', 'HOW_TO_PLAY.txt'), os.path.join(dest, 'How to play.txt'))
    shutil.copy2(os.path.join(REPO, 'docs', 'PLAY_ONLINE.md'), os.path.join(dest, 'Play online.txt'))


def audit(folder):
    r = run([sys.executable, os.path.join(REPO, 'tools', 'release_audit.py'), folder], capture_output=True, text=True)
    line = (r.stdout.strip().splitlines() or ['?'])[-1]
    print('  audit:', line)
    return r.returncode == 0 and 'CLEAN' in line


def dumpbin():
    m = sorted(glob.glob(os.path.join(VS, r'VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe')))
    return m[-1] if m else None


def deps_ok(folder):
    db = dumpbin()
    if not db:
        print('  dependencies: dumpbin not found, not checked')
        return True
    ok = True
    for e in EXES + ['libtcc.dll']:
        out = subprocess.run([db, '/nologo', '/dependents', os.path.join(folder, e)], capture_output=True,
                             text=True).stdout
        dlls = sorted({l.strip().lower() for l in out.splitlines()
                       if l.strip().lower().endswith('.dll') and not l.strip().lower().startswith('dump of file')})
        bad = [d for d in dlls if d not in SYSTEM_DLLS and not d.startswith('api-ms-win-')]
        print('  %-20s imports %s%s' % (e, ', '.join(dlls), ('  NOT SYSTEM: ' + ', '.join(bad)) if bad else ''))
        ok &= not bad
    return ok


def selftest(folder, rom):
    env = dict(os.environ)
    env['BTDD_CACHE_DIR'] = os.path.join(folder, '_test_cache')
    env['LOCALAPPDATA'] = os.path.join(folder, '_test_localappdata')
    os.makedirs(env['BTDD_CACHE_DIR'], exist_ok=True)
    os.makedirs(env['LOCALAPPDATA'], exist_ok=True)
    open(os.path.join(folder, 'btdd.cfg'), 'w').close()  # settings stay in the test folder (not the user's)
    r = subprocess.run([os.path.join(folder, 'PondToPavement.exe'), '--selftest', '--rom', rom], capture_output=True,
                       text=True, env=env, cwd=folder, timeout=900)
    for l in r.stdout.strip().splitlines():
        print('  ' + l)
    return r.returncode == 0 and 'ready: yes' in r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(BATTLE, 'release'))
    ap.add_argument('--skip-build', action='store_true')
    ap.add_argument('--rom')
    ap.add_argument('--no-test', action='store_true')
    a = ap.parse_args()
    date = datetime.date.today().strftime('%Y%m%d')
    bin_dir = os.path.join(REPO, 'build-pkg', 'Release') if a.skip_build else build()
    os.makedirs(a.out, exist_ok=True)
    dest = os.path.join(a.out, 'stage-' + date, 'PondToPavement')
    stage(bin_dir, dest)
    ok = True
    print('staged:', dest)
    ok &= audit(dest)
    ok &= deps_ok(dest)
    zpath = os.path.join(a.out, 'PondToPavement-tester-%s.zip' % date)
    if os.path.exists(zpath):
        os.remove(zpath)
    with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for root, _, files in os.walk(dest):
            for f in files:
                p = os.path.join(root, f)
                z.write(p, os.path.join('PondToPavement', os.path.relpath(p, dest)))
    print('zip: %s (%.1f MB)' % (zpath, os.path.getsize(zpath) / 1e6))
    if not a.no_test:
        t = tempfile.mkdtemp(prefix='btdd_pkg_test_')
        with zipfile.ZipFile(zpath) as z:
            z.extractall(t)
        folder = os.path.join(t, 'PondToPavement')
        print('zip contents:')
        ok &= audit(folder)
        if a.rom:
            print('selftest from a fresh folder:')
            ok &= selftest(folder, a.rom)
        shutil.rmtree(t, ignore_errors=True)
    print('PACKAGE OK' if ok else 'PACKAGE CHECKS FAILED')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
