r"""Builds the release of the modpack (modpack/release.ini): modkit/dist/RoL_Remastered_<version>/ and its zips.

  py -3.14 modkit/tools/release.py              the main package and the HD pack
  py -3.14 modkit/tools/release.py --no-hd      the main package only

Main package - the player unpacks it into the game folder:
  Install.bat, Uninstall.bat, Remastered_README_EN/RU.txt   modkit/release/, {version} filled in, CRLF line ends
  modkit\python      the embeddable CPython for 64-bit Windows from python.org (cached in work/python), trimmed: the
                     standard library unpacked into Lib\ (no zip inside the zip), the modules the installer never
                     imports left out (PY_DROP); its ._pth gets Lib and ..\tools
  modkit\tools       modmgr and the modules the mods use (bigtool, bxml, entrypatch)
  modkit\modpack     the release's mods with what they require, release.ini, README.md (the English README.en.md)
  ...                the files the mods install from outside their folders (build\native\ddraw.dll - the
                     players' build from build\native\player, checked for FORBIDDEN_IMPORTS -,
                     native\rolhook\rolhook.ini - the core mod)
HD pack - modkit\mods\hd zipped under 2 GB (GitHub's limit for a release file; HD_PARTS splits it when it grows past
that): units, buildings, effects, trees, terrain, the campaign map, the landscapes. Every file of the pack goes in
(pack.json, the QA numbers of the first build, stays out); older HD zips of the version are deleted first.
The package is checked with its own Python (modmgr lists the mods); each zip's sha256 is printed and written next
to it. Nothing here is committed (dist/ is ignored).
"""
import argparse
import configparser
import hashlib
import io
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile

KIT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = '3.14.8'
PY_URL = f'https://www.python.org/ftp/python/{PY}/python-{PY}-embed-amd64.zip'
PY_CACHE = os.path.join(KIT, 'work', 'python', f'python-{PY}-embed-amd64.zip')
TOOLS = ['modmgr.py', 'bigtool.py', 'bxml.py', 'entrypatch.py']
# parts of the embeddable Python the installer never imports (its code and the mods' patch scripts need the standard
# library's pure modules, zlib and SHA-256 - both built in - and sockets: xml.sax.saxutils pulls urllib in)
PY_DROP = ['pythonw.exe', '_asyncio.pyd', '_bz2.pyd', '_ctypes.pyd', 'libffi-8.dll', '_decimal.pyd',
           '_elementtree.pyd', 'pyexpat.pyd', '_hashlib.pyd', '_ssl.pyd', 'libcrypto-3.dll', 'libssl-3.dll',
           '_lzma.pyd', '_multiprocessing.pyd', '_overlapped.pyd', '_queue.pyd', '_remote_debugging.pyd',
           '_sqlite3.pyd', 'sqlite3.dll', '_uuid.pyd', '_wmi.pyd', '_zoneinfo.pyd', '_zstd.pyd', 'winsound.pyd',
           'unicodedata.pyd', 'libtommath.dll']
# standard library packages nothing of the installer reaches (ctypes also carries a .bat of its own)
PY_DROP_LIB = ['ctypes', 'asyncio', 'multiprocessing', 'concurrent', '_pyrepl', 'pydoc_data', 'unittest', 'xmlrpc',
               'wsgiref', 'curses', 'dbm', 'sqlite3', 'zoneinfo', 'tomllib', '__phello__']
# the HD pack's zips: name suffix, folders of modkit\mods\hd. One while the pack fits a release file (1.0: 1.7 GB);
# grown past it, split into complete parts, e.g. ('HD_1of2', [...]), ('HD_2of2', [...])
HD_PARTS = [('HD', ['art/units', 'art/buildings', 'art/effects', 'art/trees', 'art/terrain', 'art/ctw', 'landscapes'])]
HD_SKIP = {'pack.json'}  # QA numbers of the first build (hd/build_battle01_pack.py); rolhook does not read it
# the package gets the players' build of rolhook (no developer tools; native/rolhook/mk.bat, player_stubs.cpp) where
# the core mod names the full one
PLAYERS_BUILD = {os.path.normpath('build/native/ddraw.dll'): os.path.normpath('build/native/player/ddraw.dll')}
# what the players' DLL must not import (it was the developer tools' - input of its own, thread control, reading
# memory through the process API, memory dumps): upload scanners and antivirus heuristics flag them
FORBIDDEN_IMPORTS = {'SendInput', 'keybd_event', 'mouse_event', 'SetCursorPos', 'GetAsyncKeyState', 'GetKeyState',
                     'SetWindowsHookExW', 'SetWindowsHookExA', 'GetThreadContext', 'SetThreadContext', 'SuspendThread',
                     'ResumeThread', 'OpenThread', 'OpenProcess', 'CreateToolhelp32Snapshot', 'Thread32First',
                     'Thread32Next', 'ReadProcessMemory', 'WriteProcessMemory', 'CreateRemoteThread',
                     'AddVectoredExceptionHandler', 'MiniDumpWriteDump', 'URLDownloadToFileW', 'InternetOpenW',
                     'WinHttpOpen', 'ShellExecuteW', 'CreateProcessW'}
LIMIT = 2_000_000_000


def release_info():
    cfg = configparser.ConfigParser(interpolation=None)
    cfg.read(os.path.join(KIT, 'modpack', 'release.ini'), encoding='utf-8')
    r = cfg['release']
    return r['version'], [m.strip() for m in r['mods'].split(',') if m.strip()]


def with_requirements(mods):
    """The release's mods and every mod they require, in a stable order."""
    out, todo = [], list(mods)
    while todo:
        m = todo.pop(0)
        if m in out:
            continue
        out.append(m)
        cfg = configparser.ConfigParser(interpolation=None, delimiters=('=',))
        cfg.optionxform = str
        cfg.read(os.path.join(KIT, 'modpack', m, 'mod.ini'), encoding='utf-8')
        todo += [r.strip() for r in cfg['mod'].get('requires', '').split(',') if r.strip()]
    return out


def outside_files(mod):
    """Files a mod installs from outside its own folder (paths relative to modkit)."""
    cfg = configparser.ConfigParser(interpolation=None, delimiters=('=',))
    cfg.optionxform = str
    d = os.path.join(KIT, 'modpack', mod)
    cfg.read(os.path.join(d, 'mod.ini'), encoding='utf-8')
    out = []
    if cfg.has_section('files'):
        for src in cfg['files'].values():
            p = os.path.normpath(os.path.join(d, src))
            if not p.startswith(os.path.normpath(d) + os.sep):
                out.append(os.path.relpath(p, KIT))
    return out


def check_players_dll(path):
    """The packaged ddraw.dll is the players' build: none of FORBIDDEN_IMPORTS."""
    import pefile
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
    names = {i.name.decode() for e in pe.DIRECTORY_ENTRY_IMPORT for i in e.imports if i.name}
    bad = sorted(names & FORBIDDEN_IMPORTS)
    if bad:
        raise SystemExit(f'the packaged ddraw.dll imports {", ".join(bad)}: not the players\' build '
                         f'(native\\rolhook\\mk.bat builds it into build\\native\\player)')
    print(f'ddraw.dll: the players\' build, {len(names)} imports, none of the developer tools\' ones')


def text_file(src, dst, version, bom=False):
    with open(src, encoding='utf-8') as f:
        text = f.read().replace('{version}', version)
    with open(dst, 'w', encoding='utf-8-sig' if bom else 'utf-8', newline='\r\n') as f:
        f.write(text)


def python_embed(dst):
    """The embeddable CPython, trimmed for mod sites' upload scanners (Nexus Mods quarantined 1.0's first upload for
    executables and a zip inside the zip): the standard library unpacked into Lib\\ instead of pythonXY.zip, and the
    extension modules and DLLs the installer never imports left out (PY_DROP)."""
    if not os.path.isfile(PY_CACHE):
        os.makedirs(os.path.dirname(PY_CACHE), exist_ok=True)
        print(f'downloading {PY_URL}')
        urllib.request.urlretrieve(PY_URL, PY_CACHE + '.part')
        os.replace(PY_CACHE + '.part', PY_CACHE)
    with zipfile.ZipFile(PY_CACHE) as z:
        z.extractall(dst)
    pth = [n for n in os.listdir(dst) if n.endswith('._pth')]
    if len(pth) != 1:
        raise SystemExit(f'the embeddable Python has no single ._pth file: {pth}')
    stdlib = os.path.join(dst, pth[0].replace('._pth', '.zip'))
    with zipfile.ZipFile(stdlib) as z:
        z.extractall(os.path.join(dst, 'Lib'))
    os.remove(stdlib)
    for n in PY_DROP:
        os.remove(os.path.join(dst, n))
    for n in PY_DROP_LIB:
        shutil.rmtree(os.path.join(dst, 'Lib', n))
    with open(os.path.join(dst, pth[0]), 'w', encoding='utf-8', newline='\r\n') as f:
        f.write('Lib\n.\n..\\tools\n')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def finish_zip(path):
    size = os.path.getsize(path)
    digest = sha256(path)
    with open(path + '.sha256', 'w', encoding='utf-8') as f:
        f.write(f'{digest}  {os.path.basename(path)}\n')
    print(f'  {os.path.basename(path)}: {size / 1e6:.1f} MB, sha256 {digest}')
    if size >= LIMIT:
        raise SystemExit(f'{path} is {size} bytes - over the 2 GB a release file may have')


def zip_tree(root, zpath, prefix=''):
    with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for base, dirs, files in os.walk(root):
            dirs.sort()
            for n in sorted(files):
                p = os.path.join(base, n)
                z.write(p, prefix + os.path.relpath(p, root).replace(os.sep, '/'))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--no-hd', action='store_true', help='the main package only')
    args = ap.parse_args()
    version, mods = release_info()
    name = f'RoL_Remastered_{version}'
    dist = os.path.join(KIT, 'dist')
    pkg = os.path.join(dist, name)
    if os.path.isdir(pkg):
        shutil.rmtree(pkg)
    kit = os.path.join(pkg, 'modkit')
    os.makedirs(os.path.join(kit, 'tools'))
    rel = os.path.join(KIT, 'release')
    for bat in ('Install.bat', 'Uninstall.bat'):
        text_file(os.path.join(rel, bat), os.path.join(pkg, bat), version)
    for lang in ('EN', 'RU'):
        text_file(os.path.join(rel, f'README_{lang}.txt'), os.path.join(pkg, f'Remastered_README_{lang}.txt'), version,
                  bom=True)
    for t in TOOLS:
        shutil.copy2(os.path.join(KIT, 'tools', t), os.path.join(kit, 'tools', t))
    every = with_requirements(mods)
    ignore = shutil.ignore_patterns('__pycache__', '*.pyc')
    for m in every:
        shutil.copytree(os.path.join(KIT, 'modpack', m), os.path.join(kit, 'modpack', m), ignore=ignore)
        for f in outside_files(m):
            src = os.path.join(KIT, PLAYERS_BUILD.get(os.path.normpath(f), f))
            if not os.path.isfile(src):
                raise SystemExit(f'{m}: {os.path.relpath(src, KIT)} is missing - build it first (native\\rolhook\\mk.bat)')
            os.makedirs(os.path.dirname(os.path.join(kit, f)), exist_ok=True)
            shutil.copy2(src, os.path.join(kit, f))
    check_players_dll(os.path.join(kit, 'build', 'native', 'ddraw.dll'))
    shutil.copy2(os.path.join(KIT, 'modpack', 'release.ini'), os.path.join(kit, 'modpack', 'release.ini'))
    shutil.copy2(os.path.join(KIT, 'modpack', 'README.en.md'), os.path.join(kit, 'modpack', 'README.md'))
    python_embed(os.path.join(kit, 'python'))
    # the package's own Python runs its modmgr
    r = subprocess.run([os.path.join(kit, 'python', 'python.exe'), os.path.join(kit, 'tools', 'modmgr.py'), 'mods'],
                       capture_output=True, text=True, encoding='utf-8', errors='replace', cwd=pkg)
    listed = [m for m in every if m in r.stdout]
    if r.returncode or len(listed) != len(every):
        raise SystemExit(f'the package\'s modmgr failed (exit {r.returncode}):\n{r.stdout}{r.stderr}')
    # everything the tools and the mods' patch scripts import, on the trimmed Python (PY_DROP)
    r = subprocess.run([os.path.join(kit, 'python', 'python.exe'), '-c',
                        'import bigtool, bxml, entrypatch, modmgr, xml.sax.saxutils, hashlib, json, zlib, configparser, '
                        'difflib, importlib.util, fnmatch, struct, shutil, subprocess, math, argparse, time; '
                        'print(hashlib.sha256(zlib.compress(b"rol")).hexdigest()[:8])'],
                       capture_output=True, text=True, encoding='utf-8', errors='replace', cwd=pkg)
    if r.returncode:
        raise SystemExit(f'the package\'s Python misses a module the installer needs:\n{r.stderr}')
    print(f'{name}: {len(every)} mods ({", ".join(every)}); its Python {PY} lists them all')
    os.makedirs(dist, exist_ok=True)
    main_zip = os.path.join(dist, f'{name}.zip')
    zip_tree(pkg, main_zip)
    finish_zip(main_zip)
    if args.no_hd:
        return
    hd = os.path.join(KIT, 'mods', 'hd')
    covered = tuple(p + '/' for _, parts in HD_PARTS for p in parts)
    left = []
    for base, dirs, files in os.walk(hd):
        for n in files:
            rel = os.path.relpath(os.path.join(base, n), hd).replace(os.sep, '/')
            if rel not in HD_SKIP and not rel.startswith(covered):
                left.append(rel)
    if left:
        raise SystemExit(f'{len(left)} HD pack files in no part, e.g. {sorted(left)[:5]}')
    for n in os.listdir(dist):
        if n.startswith(f'{name}_HD'):
            os.remove(os.path.join(dist, n))
    for tag, parts in HD_PARTS:
        zpath = os.path.join(dist, f'{name}_{tag}.zip')
        with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
            for part in parts:
                for base, dirs, files in os.walk(os.path.join(hd, part)):
                    dirs.sort()
                    for n in sorted(files):
                        p = os.path.join(base, n)
                        z.write(p, 'modkit/mods/hd/' + os.path.relpath(p, hd).replace(os.sep, '/'))
        finish_zip(zpath)


if __name__ == '__main__':
    main()
