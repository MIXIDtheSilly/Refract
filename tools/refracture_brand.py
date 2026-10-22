#!/usr/bin/env python3
"""Puts Refracture XR's name and R mark into a copy of the launcher, at build time.

The checked-in launcher keeps Refract's own ring logo, name and icons, so merging Refract's changes never conflicts with
branding. The release build (.github/workflows/release.yml) runs this on its throwaway checkout before building:

    python tools/refracture_brand.py            # brand this checkout in place (use a copy, or `git checkout .` to undo)
    python tools/refracture_brand.py --check    # change nothing; fail if Refract's files no longer match what is replaced
    python tools/refracture_brand.py --version 0.0.1   # also give the launcher Refracture XR's own version

Every replacement must find its text. When Refract changes one of these lines, the check fails and names the spot,
rather than a release going out half branded; update REPLACEMENTS below. Running it twice is harmless.
"""
import argparse, base64, re, shutil, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NAME = 'Refracture XR'
CREDIT_URL = 'https://github.com/MIXIDtheSilly/Refract'
CSS_MARK = '/* Refracture XR branding (tools/refracture_brand.py) */'

# (file, text in Refract's source, text it becomes). Only the app's own identity: the title bar, sidebar, welcome and update
# dialogs, the window title, and the update source. Names of Refract's runtimes and components ("Refract Native", "Refract components") stay.
REPLACEMENTS = [
    ('launcher/src/components/chrome.jsx', '<span data-tauri-drag-region>Refract</span>', f'<span data-tauri-drag-region>{NAME}</span>'),
    ('launcher/src/components/chrome.jsx', 'aria-label="Refract library"', f'aria-label="{NAME} library"'),
    ('launcher/src/components/chrome.jsx', '<LogoMark /><span>Refract</span>', f'<LogoMark /><span>{NAME}</span>'),
    ('launcher/src/components/welcome.jsx', 'Welcome to Refract</h1>', f'Welcome to {NAME}</h1>'),
    ('launcher/src/components/update.jsx', 'You have Refract {state.version}.', f'You have {NAME} {{state.version}}.'),
    ('launcher/index.html', '<title>Refract</title>', f'<title>{NAME}</title>'),
    ('launcher/src-tauri/tauri.conf.json', '"title": "Refract",', f'"title": "{NAME}",'),
    # A packaged build looks for updates in Refracture XR's releases, not Refract's (whose packages have no Unknown sources).
    ('launcher/core/update.mjs', "const REPOSITORY = 'MIXIDtheSilly/Refract';", "const REPOSITORY = 'DeliciousMeatPop/Refracture-XR';"),
    ('launcher/core/update.mjs', 'const assetName = /^Refract-(', 'const assetName = /^Refracture-XR-('),
    # The licence asks for a visible credit wherever the app shows legal notices; Settings is where this one goes.
    ('launcher/src/pages/settings.jsx', '      {saveBar}\n    </form>',
     '      <section className="panel">\n        <h2>About</h2>\n'
     f'        <p>{NAME}, an independent fork of Refract. Based on Refract: {CREDIT_URL}</p>\n'
     '      </section>\n      {saveBar}\n    </form>'),
]

CSS = f'''
{CSS_MARK}
.brand span {{ font-size: 20px; white-space: nowrap; }}
.now-playing .orbit svg, .welcome-card .orbit svg {{ animation: none; }}
.cover-art .ring {{ left: 50%; right: auto; top: 50%; width: 30%; transform: translate(-50%, -50%); }}
.hero .cover-art .ring {{ left: auto; right: 12%; width: 17%; }}
'''


def logo_component(mark):
    data = base64.b64encode(mark.read_bytes()).decode()
    return f'''// {NAME}'s R mark in place of Refract's ring (written by tools/refracture_brand.py). The decorative uses (a game's empty
// cover, the empty-library watermark) are drawn faint, as the ring was.
export function LogoMark({{ className, title }}) {{
  const faint = className === 'ring' ? 0.4 : className === 'mark' ? 0.12 : undefined;
  return <svg viewBox="0 0 100 100" className={{className}} role={{title ? 'img' : undefined}} aria-label={{title}} aria-hidden={{title ? undefined : true}}
    style={{faint === undefined ? undefined : {{ opacity: faint }}}}>
    <image href="data:image/png;base64,{data}" width="100" height="100" />
  </svg>;
}}
'''


VERSION_FILES = ('launcher/package.json', 'launcher/src-tauri/tauri.conf.json')


def apply(root, check=False, version=None):
    """Returns the problems found (empty when everything applied or would apply)."""
    problems, writes = [], {}
    if version is not None and not re.fullmatch(r'\d+\.\d+\.\d+', version):
        return [f'version {version!r}: use three numbers like 0.0.1']
    def text(relative):
        if relative not in writes:
            path = root / relative
            if not path.is_file():
                problems.append(f'{relative}: file not found'); return None
            writes[relative] = path.read_text(encoding='utf-8')
        return writes[relative]
    for relative, old, new in REPLACEMENTS:
        current = text(relative)
        if current is None: continue
        if new in current:  # Already applied (checked first: some replacements end with the text they replace).
            continue
        if old not in current:
            problems.append(f'{relative}: Refract changed this line, update tools/refracture_brand.py: {old!r}')
        elif current.count(old) > 1:
            problems.append(f'{relative}: {old!r} appears more than once')
        else:
            writes[relative] = current.replace(old, new)
    # Refracture XR's own version (the updater compares it with this repository's releases), not Refract's.
    for relative in VERSION_FILES if version is not None else ():
        current = text(relative)
        if current is None: continue
        stamped, found = re.subn(r'^(\s*"version"\s*:\s*")[^"]*(")', rf'\g<1>{version}\g<2>', current, count=1, flags=re.M)
        if not found: problems.append(f'{relative}: no "version" to set')
        else: writes[relative] = stamped
    css = text('launcher/src/styles.css')
    if css is not None and CSS_MARK not in css:
        writes['launcher/src/styles.css'] = css.rstrip('\n') + '\n' + CSS
    for relative in ('launcher/src/components/logo.jsx',):
        if text(relative) is not None: writes[relative] = logo_component(ROOT / 'branding/mark.png')
    if problems or check:
        return problems
    for relative, content in writes.items():
        (root / relative).write_text(content, encoding='utf-8', newline='\n')
    for icon in sorted((ROOT / 'branding/icons').iterdir()):
        shutil.copy2(icon, root / 'launcher/src-tauri/icons' / icon.name)
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', default=str(ROOT), help='the checkout to brand (default: this one)')
    parser.add_argument('--check', action='store_true', help='change nothing, only report what no longer matches')
    parser.add_argument('--version', help="Refracture XR's own version, three numbers (default: leave Refract's)")
    args = parser.parse_args()
    problems = apply(Path(args.root), args.check, args.version)
    for problem in problems:
        print(f'error: {problem}', file=sys.stderr)
    if problems:
        raise SystemExit(1)
    print('Branding matches Refract\'s files.' if args.check else f'Branded as {NAME}.')


if __name__ == '__main__':
    main()
