#!/usr/bin/env python3
"""Builds a Refracture XR package: Refract's release with this fork's launcher laid over it.

Refract's runtimes, translator and GPU/VR programs need that project's own build machine, so Refracture does not rebuild
them. It takes the Refract release package for the same platform (checked against its published SHA-256), replaces the
launcher program and its backend (launcher/backend, launcher/core, inspect_apk.py, package.json) with this checkout's,
adds the notices and notes, and packs the result as Refracture-XR-<version>-<platform>.

    python tools/refracture_package.py --platform win64 --upstream Refract-1.0.3-win64.zip \
        --binary launcher/src-tauri/target/release/refract-launcher.exe --release-json release.json [--out dist]

--release-json is Refract's release as GitHub's API returns it (gh api repos/MIXIDtheSilly/Refract/releases/tags/<tag>);
the package's SHA-256 is read from the asset's digest, or from the release notes. --sha256 gives it directly.
"""
import argparse, hashlib, json, os, re, shutil, sys, tarfile, tempfile, zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# platform -> (archive suffix, launcher program name inside the package)
PLATFORMS = {'win64': ('.zip', 'Refract.exe'), 'linux-x86_64': ('.tar.gz', 'Refract')}
LAUNCHER_DIRS = ('backend', 'core')  # Replaced as a whole, so no file of Refract's older launcher stays beside ours.
EXTRAS = [('LICENSE', 'LICENSE'), ('NOTICE', 'NOTICE'), ('CREDITS.md', 'CREDITS.md'),
          ('docs/unknown_sources.md', 'docs/unknown_sources.md'), ('tools/refracture_readme.txt', 'REFRACTURE.txt')]


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as handle:
        for block in iter(lambda: handle.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def published_sha256(release, name):
    """The SHA-256 Refract's release publishes for an asset: its digest, else a line of the notes naming the file."""
    for asset in release.get('assets') or []:
        match = re.fullmatch(r'sha256:([0-9a-fA-F]{64})', asset.get('digest') or '')
        if asset.get('name') == name and match:
            return match[1].lower()
    escaped = re.escape(name)
    for pattern in (rf'([0-9a-fA-F]{{64}})[\s*`:]+{escaped}', rf'{escaped}[\s*`:|-]+([0-9a-fA-F]{{64}})'):
        match = re.search(pattern, release.get('body') or '')
        if match:
            return match[1].lower()
    return None


def pick_release(releases, platform):
    """The newest published Refract release with exactly one package for the platform, and that package's name.

    Refract's newest release can be a tag whose package was never uploaded, so the newest *with a package* is used.
    `releases` is GitHub's list, newest first."""
    pattern = re.compile(rf'Refract-\d+\.\d+\.\d+-{re.escape(platform)}\.(zip|tar\.gz)')
    for release in releases:
        if release.get('draft') or release.get('prerelease'):
            continue
        names = [a['name'] for a in release.get('assets') or [] if pattern.fullmatch(a.get('name', ''))]
        if len(names) == 1:
            return release, names[0]
    lines = [f"  {r.get('tag_name')}{' (pre-release)' if r.get('prerelease') else ''}{' (draft)' if r.get('draft') else ''}: "
             f"{', '.join(a.get('name', '?') for a in r.get('assets') or []) or 'no files'}" for r in releases[:10]]
    raise SystemExit(f'No Refract release has a {platform} package named Refract-<version>-{platform}.zip or .tar.gz. Newest releases and their files:\n'
                     + ('\n'.join(lines) or '  (none)'))


def extract(archive, target):
    archive = Path(archive)
    if archive.name.endswith('.zip'):
        with zipfile.ZipFile(archive) as z:
            z.extractall(target)  # Python drops absolute and '..' parts of entry names.
    elif archive.name.endswith('.tar.gz'):
        with tarfile.open(archive) as t:
            if not hasattr(tarfile, 'data_filter'):
                raise SystemExit('Python 3.12 or newer is needed to unpack a .tar.gz safely.')
            t.extractall(target, filter='data')
    else:
        raise SystemExit(f'{archive.name}: expected a .zip or .tar.gz package.')
    folders = [p for p in Path(target).iterdir() if p.is_dir()]
    if len(folders) != 1 or len(list(Path(target).iterdir())) != 1:
        raise SystemExit('The Refract package should hold one folder; this one does not.')
    return folders[0]


def pack(folder, name, suffix, out):
    out.mkdir(parents=True, exist_ok=True)
    result = out / f'{name}{suffix}'
    if result.exists():
        result.unlink()
    if suffix == '.zip':
        with zipfile.ZipFile(result, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for path in sorted(folder.rglob('*')):
                z.write(path, f'{name}/{path.relative_to(folder).as_posix()}')
    else:
        def own(info):
            info.uid = info.gid = 0; info.uname = info.gname = 'root'
            return info
        with tarfile.open(result, 'w:gz') as t:
            t.add(folder, arcname=name, filter=own)
    return result


def build(platform, upstream, binary, out, expected=None, version=None, root=ROOT, merged=None):
    suffix, program = PLATFORMS[platform]
    upstream, binary, out, root = Path(upstream), Path(binary), Path(out), Path(root)
    if not binary.is_file():
        raise SystemExit(f'{binary}: the built launcher is missing.')
    actual = sha256(upstream)
    if expected and actual != expected.lower():
        raise SystemExit(f'{upstream.name} does not match its published SHA-256 (got {actual}).')
    version = version or json.loads((root / 'launcher/package.json').read_text(encoding='utf-8'))['version']
    name = f'Refracture-XR-{version}-{platform}'
    with tempfile.TemporaryDirectory() as work:
        folder = extract(upstream, Path(work) / 'upstream')
        if not (folder / program).is_file() or not (folder / 'launcher/core').is_dir():
            raise SystemExit(f'{upstream.name} is not a Refract {platform} package (no {program} or launcher/core).')
        # This launcher replaces Refract's, and runs against the scripts in Refract's package. If that package is not the Refract
        # release this repository was merged up to, they may not fit: say so, loudly, in the workflow log.
        theirs = json.loads((folder / 'launcher/package.json').read_text(encoding='utf-8')).get('version') if (folder / 'launcher/package.json').is_file() else None
        if merged and theirs and theirs != merged:
            print(f"::warning::The Refract package used here is {theirs} but Refract {merged} is what is merged into this repository. "
                  'Merge Refract first, or the launcher in this package may not match its scripts.', file=sys.stderr)
        shutil.copy2(binary, folder / program)
        if platform == 'linux-x86_64':
            os.chmod(folder / program, 0o755)
        for sub in LAUNCHER_DIRS:
            shutil.rmtree(folder / 'launcher' / sub)
            (folder / 'launcher' / sub).mkdir()
            for file in sorted((root / 'launcher' / sub).glob('*.mjs')):
                shutil.copy2(file, folder / 'launcher' / sub / file.name)
        for file in ('inspect_apk.py', 'package.json'):  # The backend reads its version from package.json.
            shutil.copy2(root / 'launcher' / file, folder / 'launcher' / file)
        for source, target in EXTRAS:
            (folder / target).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(root / source, folder / target)
        (folder / 'UPSTREAM.txt').write_text(
            f'This package is Refract\'s release {upstream.name} (SHA-256 {actual}) with Refracture XR\'s launcher laid over it.\n'
            'Refract: https://github.com/MIXIDtheSilly/Refract\n', encoding='utf-8')
        result = pack(folder, name, suffix, out)
    (out / f'{result.name}.sha256').write_text(f'{sha256(result)}  {result.name}\n', encoding='ascii')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--platform', required=True, choices=sorted(PLATFORMS))
    parser.add_argument('--pick-release', metavar='RELEASES_JSON', help="instead of packaging: choose Refract's release for --platform from "
                        "GitHub's release list, write it to --write-release and put UPSTREAM_TAG and ASSET in $GITHUB_ENV")
    parser.add_argument('--write-release', default='release.json', help='where --pick-release writes the chosen release')
    parser.add_argument('--upstream', help="Refract's release package for the platform (.zip or .tar.gz)")
    parser.add_argument('--binary', help="the launcher program built from this checkout")
    parser.add_argument('--sha256', help="the SHA-256 Refract's release publishes for that package")
    parser.add_argument('--release-json', help="Refract's release as GitHub's API returns it; its published SHA-256 is used")
    parser.add_argument('--allow-unverified', action='store_true', help='go on without a published SHA-256 (not for releases)')
    parser.add_argument('--version', help='default: launcher/package.json')
    parser.add_argument('--merged-version', help='the Refract release this repository was merged up to, to compare with the package')
    parser.add_argument('--out', default=str(ROOT / 'dist'))
    args = parser.parse_args()
    if args.pick_release:
        release, asset = pick_release(json.loads(Path(args.pick_release).read_text(encoding='utf-8')), args.platform)
        Path(args.write_release).write_text(json.dumps(release), encoding='utf-8')
        print(f"Using Refract {release['tag_name']}: {asset}")
        if os.environ.get('GITHUB_ENV'):
            with open(os.environ['GITHUB_ENV'], 'a', encoding='utf-8', newline='\n') as env:
                env.write(f"UPSTREAM_TAG={release['tag_name']}\nASSET={asset}\n")
        return
    if not args.upstream or not args.binary:
        parser.error('--upstream and --binary are required to package')
    expected = args.sha256
    if not expected and args.release_json:
        release = json.loads(Path(args.release_json).read_text(encoding='utf-8'))
        expected = published_sha256(release, Path(args.upstream).name)
    if not expected:
        if not args.allow_unverified:
            raise SystemExit(f'No published SHA-256 for {Path(args.upstream).name}. Give --sha256 or --release-json, or --allow-unverified.')
        print('warning: the Refract package was not verified', file=sys.stderr)
    result = build(args.platform, args.upstream, args.binary, args.out, expected, args.version, merged=args.merged_version)
    print(f'Done: {result}')


if __name__ == '__main__':
    main()
