r"""Publishes a release's source to the public repository, github.com/ROK10/rol-remastered: mod sites ask for the source
of the compiled part (ddraw.dll) to clear an upload, and players can read what the mod does.

  py -3.14 modkit/tools/publish_source.py --version 1.0 [--push]

A snapshot of this repository - every file git tracks or would track here, minus PRIVATE - as one commit per release,
in a clone at work/public_source (made on first use). The public repository is in English: this repository's Russian
documents stay out (PRIVATE), English counterparts take their place (RENAMES), and the snapshot is refused if Russian
text is left anywhere but the product's own Russian localization (LOCALIZATION). The commit is the user's (name from
git config), with GitHub's no-reply address so the private address stays out of a public history; its message names
the private commit it comes from.
"""
import argparse
import os
import re
import shutil
import subprocess

KIT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PUBLIC = 'https://github.com/ROK10/rol-remastered.git'
EMAIL = 'ROK10@users.noreply.github.com'
PRIVATE = {
    'docs/TODO.md',                  # the working plan
    'docs/ENGINE_NOTES.md',          # research notes, in Russian
    'README.md',                     # this repository's Russian README (release/SOURCE_README.md replaces it)
    'modpack/README.md',             # Russian (modpack/README.en.md replaces it)
    'release/MOD_PAGE.md',           # notes for filling in the mod pages
    'hd/review_page.py', 'hd/compare_page.py', 'hd/compare_page_template.html', 'hd/make_battle_page.py',
    'hd/battle_page_template.html',  # the HD reviews' pages, written for the user in Russian
}
RENAMES = {'release/SOURCE_README.md': 'README.md', 'modpack/README.en.md': 'modpack/README.md'}
# Russian that is the product's own localization: the Russian player README, the Mods tab's translations, the
# installers' two-language messages, the mods' Russian names and descriptions, the language list's own names
LOCALIZATION = {'release/README_RU.txt', 'modpack/mods_tab/patches/tab_strings.py', 'release/Install.bat',
                'release/Uninstall.bat'}
LOCALIZED_LINE = re.compile(r'^(name_ru|description_ru)=|English, Deutsch')
CYRILLIC = re.compile('[\\u0400-\\u04ff]')


def git(*args, cwd=KIT):
    r = subprocess.run(['git', *args], cwd=cwd, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if r.returncode:
        raise SystemExit(f'git {" ".join(args)} failed:\n{r.stdout}{r.stderr}')
    return r.stdout


def russian_left(files):
    """Lines with Russian text outside the localization, as 'file:line: text'."""
    out = []
    for f in files:
        if f in LOCALIZATION:
            continue
        try:
            with open(os.path.join(KIT, f), encoding='utf-8') as h:
                lines = h.read().splitlines()
        except (UnicodeDecodeError, OSError):
            continue   # binary
        out += [f'{f}:{i}: {line.strip()[:100]}' for i, line in enumerate(lines, 1)
                if CYRILLIC.search(line) and not LOCALIZED_LINE.search(line)]
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--version', required=True)
    ap.add_argument('--push', action='store_true', help='push the commit to the public repository')
    args = ap.parse_args()
    if git('status', '--porcelain').strip():
        raise SystemExit('commit the private repository first: the snapshot names its commit')
    files = [f for f in git('ls-files', '--cached', '--others', '--exclude-standard').splitlines()
             if f and f not in PRIVATE]
    left = russian_left(files)
    if left:
        raise SystemExit('Russian text outside the localization (translate it, or list the file in PRIVATE):\n  '
                         + '\n  '.join(left))
    head = git('rev-parse', '--short', 'HEAD').strip()
    dest = os.path.join(KIT, 'work', 'public_source')
    if not os.path.isdir(os.path.join(dest, '.git')):
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        git('clone', PUBLIC, dest, cwd=os.path.dirname(dest))
    for n in os.listdir(dest):
        if n != '.git':
            p = os.path.join(dest, n)
            shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)
    for f in files:
        target = os.path.join(dest, RENAMES.get(f, f))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shutil.copy2(os.path.join(KIT, f), target)
    name = git('config', 'user.name').strip()
    git('add', '-A', cwd=dest)
    if not git('status', '--porcelain', cwd=dest).strip():
        print('the public repository already has this source')
        return
    git('-c', f'user.name={name}', '-c', f'user.email={EMAIL}', 'commit', '-q', '-m',
        f'Rise of Legends Remastered {args.version} (rol-modkit {head})', cwd=dest)
    print(f'{len(files)} files committed in {dest}: Rise of Legends Remastered {args.version} (rol-modkit {head})')
    if args.push:
        git('push', '-q', 'origin', 'HEAD:main', cwd=dest)
        print(f'pushed to {PUBLIC}')


if __name__ == '__main__':
    main()
