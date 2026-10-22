"""Tests for tools/refracture_brand.py and tools/refracture_package.py (run: python -m unittest discover -s tests -p "test_refracture*.py")."""
import hashlib, json, os, shutil, sys, tarfile, tempfile, unittest, zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import refracture_brand as brand
import refracture_package as package


class Brand(unittest.TestCase):
    def copy(self):
        work = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, work, True)
        shutil.copytree(ROOT / 'launcher', work / 'launcher', ignore=shutil.ignore_patterns('node_modules', 'dist', 'target'))
        return work

    def test_every_replacement_still_matches_refracts_files(self):
        self.assertEqual(brand.apply(ROOT, check=True), [])

    def test_branding_changes_the_copy_and_nothing_else(self):
        work = self.copy()
        before = (ROOT / 'launcher/src/components/chrome.jsx').read_text(encoding='utf-8')
        self.assertEqual(brand.apply(work), [])
        chrome = (work / 'launcher/src/components/chrome.jsx').read_text(encoding='utf-8')
        self.assertIn('<span>Refracture XR</span>', chrome)
        self.assertIn('data:image/png;base64,', (work / 'launcher/src/components/logo.jsx').read_text(encoding='utf-8'))
        self.assertIn("'DeliciousMeatPop/Refracture-XR'", (work / 'launcher/core/update.mjs').read_text(encoding='utf-8'))
        self.assertIn('"title": "Refracture XR"', (work / 'launcher/src-tauri/tauri.conf.json').read_text(encoding='utf-8'))
        self.assertEqual((work / 'launcher/src-tauri/icons/icon.png').read_bytes(), (ROOT / 'branding/icons/icon.png').read_bytes())
        # The checkout the script was run from is untouched.
        self.assertEqual((ROOT / 'launcher/src/components/chrome.jsx').read_text(encoding='utf-8'), before)
        self.assertIn('<span>Refract</span>', before)

    def test_running_twice_changes_nothing_more(self):
        work = self.copy()
        brand.apply(work)
        first = {p: p.read_bytes() for p in (work / 'launcher').rglob('*') if p.is_file()}
        self.assertEqual(brand.apply(work), [])
        self.assertEqual(first, {p: p.read_bytes() for p in (work / 'launcher').rglob('*') if p.is_file()})

    def test_the_version_is_stamped_on_the_copy_only(self):
        work = self.copy()
        before = (ROOT / 'launcher/package.json').read_text(encoding='utf-8')
        self.assertEqual(brand.apply(work, version='0.0.1'), [])
        for relative in brand.VERSION_FILES:
            self.assertEqual(json.loads((work / relative).read_text(encoding='utf-8'))['version'], '0.0.1')
        # Only the version line changed, and the repository's own files keep Refract's.
        self.assertEqual((work / 'launcher/package.json').read_text(encoding='utf-8').replace('0.0.1', '@'), before.replace(json.loads(before)['version'], '@'))
        self.assertEqual((ROOT / 'launcher/package.json').read_text(encoding='utf-8'), before)
        self.assertEqual(brand.apply(work, version='0.0.2'), [])  # Stamping again replaces it.
        self.assertEqual(json.loads((work / 'launcher/package.json').read_text(encoding='utf-8'))['version'], '0.0.2')

    def test_a_version_that_is_not_three_numbers_is_refused(self):
        work = self.copy()
        for bad in ('1.0', 'v1.0.0', '1.0.0-beta', ''):
            self.assertEqual(len(brand.apply(work, version=bad)), 1, bad)
        self.assertNotIn('Refracture XR', (work / 'launcher/src/components/chrome.jsx').read_text(encoding='utf-8'))

    def test_a_line_refract_changed_is_reported_not_skipped(self):
        work = self.copy()
        chrome = work / 'launcher/src/components/chrome.jsx'
        chrome.write_text(chrome.read_text(encoding='utf-8').replace('aria-label="Refract library"', 'aria-label="Home"'), encoding='utf-8')
        problems = brand.apply(work)
        self.assertEqual(len(problems), 1)
        self.assertIn('chrome.jsx', problems[0])
        # Nothing was written when a replacement could not be applied.
        self.assertNotIn('Refracture XR', chrome.read_text(encoding='utf-8'))


def refract_release(directory, platform):
    """A stand-in for Refract's release package: a folder with its launcher, backend, node and a runtime file."""
    program = 'Refract.exe' if platform == 'win64' else 'Refract'
    name = f'Refract-1.0.3-{platform}'
    top = directory / name
    for relative, text in {program: 'upstream launcher', 'launcher/backend/server.mjs': 'old server', 'launcher/backend/gone.mjs': 'removed',
                           'launcher/core/runtime.mjs': 'old runtime', 'launcher/package.json': '{"version": "1.0.3"}', 'README.txt': 'Refract readme',
                           'node/node': 'node', 'build-native/refract_native.exe': 'runtime'}.items():
        (top / relative).parent.mkdir(parents=True, exist_ok=True)
        (top / relative).write_text(text, encoding='utf-8')
    if platform == 'win64':
        archive = directory / f'{name}.zip'
        with zipfile.ZipFile(archive, 'w') as z:
            for path in sorted(top.rglob('*')):
                z.write(path, f'{name}/{path.relative_to(top).as_posix()}')
    else:
        archive = directory / f'{name}.tar.gz'
        with tarfile.open(archive, 'w:gz') as t:
            t.add(top, arcname=name)
    return archive


class Package(unittest.TestCase):
    def setUp(self):
        self.work = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.work, True)
        self.binary = self.work / 'refract-launcher'
        self.binary.write_text('refracture launcher', encoding='utf-8')

    def names(self, result):
        if result.suffix == '.zip':
            with zipfile.ZipFile(result) as z: return {n.split('/', 1)[1]: z.read(n) for n in z.namelist() if not n.endswith('/')}
        with tarfile.open(result) as t: return {m.name.split('/', 1)[1]: t.extractfile(m).read() for m in t.getmembers() if m.isfile()}

    def check(self, platform, program):
        upstream = refract_release(self.work, platform)
        digest = hashlib.sha256(upstream.read_bytes()).hexdigest()
        result = package.build(platform, upstream, self.binary, self.work / 'out', digest)
        version = json.loads((ROOT / 'launcher/package.json').read_text(encoding='utf-8'))['version']
        self.assertEqual(result.name, f'Refracture-XR-{version}-{platform}{".zip" if platform == "win64" else ".tar.gz"}')
        files = self.names(result)
        self.assertEqual(files[program], b'refracture launcher')
        self.assertEqual(files['launcher/backend/server.mjs'], (ROOT / 'launcher/backend/server.mjs').read_bytes())
        self.assertIn('launcher/backend/sideload.mjs', files)
        self.assertNotIn('launcher/backend/gone.mjs', files)  # Refract's older launcher files do not linger.
        self.assertEqual(files['launcher/core/runtime.mjs'], (ROOT / 'launcher/core/runtime.mjs').read_bytes())
        # Refract's own runtime files and readme are kept, and the notices are added.
        self.assertEqual(files['build-native/refract_native.exe'], b'runtime')
        self.assertEqual(files['README.txt'], b'Refract readme')
        for added in ('NOTICE', 'LICENSE', 'CREDITS.md', 'REFRACTURE.txt', 'docs/unknown_sources.md'):
            self.assertIn(added, files)
        self.assertIn(digest, files['UPSTREAM.txt'].decode())
        self.assertIn(f'{package.sha256(result)}  {result.name}', (result.parent / f'{result.name}.sha256').read_text())

    def test_windows_package(self):
        self.check('win64', 'Refract.exe')

    @unittest.skipIf(sys.platform == 'win32', 'file modes are a Unix thing; the Linux package is built on Linux')
    def test_linux_package_keeps_the_launcher_executable(self):
        self.check('linux-x86_64', 'Refract')
        result = next((self.work / 'out').glob('*.tar.gz'))
        with tarfile.open(result) as t:
            self.assertTrue(t.getmember(f'{result.name[:-7]}/Refract').mode & 0o100)

    def test_the_published_checksum_comes_from_the_digest_or_the_notes(self):
        name, digest = 'Refract-1.0.3-win64.zip', 'ab' * 32
        other = 'cd' * 32
        self.assertEqual(package.published_sha256({'assets': [{'name': name, 'digest': f'sha256:{digest.upper()}'}]}, name), digest)
        self.assertEqual(package.published_sha256({'assets': [{'name': 'x.zip', 'digest': f'sha256:{other}'}]}, name), None)
        self.assertEqual(package.published_sha256({'body': f'```\n{digest}  {name}\n{other}  Refract-1.0.3-linux-x86_64.tar.gz\n```'}, name), digest)
        self.assertEqual(package.published_sha256({'body': f'{name}: `{digest}`'}, name), digest)
        self.assertEqual(package.published_sha256({'body': 'nothing here'}, name), None)

    def test_the_command_line_refuses_to_go_on_without_a_checksum(self):
        upstream = refract_release(self.work, 'win64')
        argv = ['x', '--platform', 'win64', '--upstream', str(upstream), '--binary', str(self.binary), '--out', str(self.work / 'out')]
        old, sys.argv = sys.argv, argv
        try:
            with self.assertRaises(SystemExit) as error: package.main()
            self.assertIn('No published SHA-256', str(error.exception))
            release = self.work / 'release.json'
            release.write_text(json.dumps({'assets': [{'name': upstream.name, 'digest': 'sha256:' + hashlib.sha256(upstream.read_bytes()).hexdigest()}]}))
            sys.argv = argv + ['--release-json', str(release)]
            package.main()
        finally:
            sys.argv = old
        self.assertTrue(any((self.work / 'out').glob('Refracture-XR-*-win64.zip')))

    def test_a_newer_refract_than_the_merged_one_is_warned_about(self):
        import contextlib, io
        upstream = refract_release(self.work, 'win64')  # Refract 1.0.3
        for merged, warns in (('1.0.3', False), ('1.0.2', True), (None, False)):
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                package.build('win64', upstream, self.binary, self.work / 'out', merged=merged)
            self.assertEqual('::warning::' in err.getvalue(), warns, merged)
            if warns: self.assertIn('1.0.3', err.getvalue()); self.assertIn('1.0.2', err.getvalue())

    def test_the_newest_release_with_a_package_is_used(self):
        def release(tag, *assets, **flags): return {'tag_name': tag, 'assets': [{'name': a} for a in assets], **flags}
        releases = [release('v1.0.3'),                                                   # Tagged, nothing uploaded.
                    release('v1.0.2-test', 'Refract-1.0.2-win64.zip', prerelease=True),  # Pre-release.
                    release('v1.0.2', 'Refract-1.0.2-win64.zip', 'Refract-1.0.2-linux-x86_64.tar.gz', 'notes.txt'),
                    release('v1.0.0', 'Refract-1.0.0-win64.zip')]
        chosen, asset = package.pick_release(releases, 'win64')
        self.assertEqual((chosen['tag_name'], asset), ('v1.0.2', 'Refract-1.0.2-win64.zip'))
        self.assertEqual(package.pick_release(releases, 'linux-x86_64')[1], 'Refract-1.0.2-linux-x86_64.tar.gz')
        # A draft is never used; two packages for one platform in a release is ambiguous, so that release is passed over.
        again = [release('v2', 'Refract-2.0.0-win64.zip', draft=True), release('v1.5', 'Refract-1.5.0-win64.zip', 'Refract-1.5.1-win64.zip')] + releases
        self.assertEqual(package.pick_release(again, 'win64')[0]['tag_name'], 'v1.0.2')

    def test_when_no_release_has_a_package_the_log_says_what_each_has(self):
        releases = [{'tag_name': 'v1.0.3', 'assets': []}, {'tag_name': 'v1.0.2', 'assets': [{'name': 'Refract-win.zip'}]}]
        with self.assertRaises(SystemExit) as error:
            package.pick_release(releases, 'win64')
        message = str(error.exception)
        self.assertIn('v1.0.3: no files', message)
        self.assertIn('v1.0.2: Refract-win.zip', message)

    def test_the_command_line_picks_a_release_and_hands_it_to_the_workflow(self):
        releases = self.work / 'releases.json'
        releases.write_text(json.dumps([{'tag_name': 'v1.0.3', 'assets': []}, {'tag_name': 'v1.0.2', 'assets': [{'name': 'Refract-1.0.2-win64.zip', 'digest': 'sha256:' + 'ab' * 32}]}]))
        env = self.work / 'github_env'
        old_argv, old_env = sys.argv, dict(os.environ)
        os.environ['GITHUB_ENV'] = str(env)
        sys.argv = ['x', '--platform', 'win64', '--pick-release', str(releases), '--write-release', str(self.work / 'release.json')]
        try:
            package.main()
        finally:
            sys.argv = old_argv; os.environ.clear(); os.environ.update(old_env)
        self.assertEqual(env.read_text(), 'UPSTREAM_TAG=v1.0.2\nASSET=Refract-1.0.2-win64.zip\n')
        # The chosen release is what the packaging step reads its checksum from.
        chosen = json.loads((self.work / 'release.json').read_text())
        self.assertEqual(package.published_sha256(chosen, 'Refract-1.0.2-win64.zip'), 'ab' * 32)

    def test_a_package_that_does_not_match_its_checksum_is_refused(self):
        upstream = refract_release(self.work, 'win64')
        with self.assertRaises(SystemExit) as error:
            package.build('win64', upstream, self.binary, self.work / 'out', '0' * 64)
        self.assertIn('SHA-256', str(error.exception))
        self.assertFalse((self.work / 'out').exists())

    def test_something_that_is_not_a_refract_package_is_refused(self):
        archive = self.work / 'Other-1.0.0-win64.zip'
        with zipfile.ZipFile(archive, 'w') as z:
            z.writestr('Other/readme.txt', 'x')
        with self.assertRaises(SystemExit) as error:
            package.build('win64', archive, self.binary, self.work / 'out')
        self.assertIn('not a Refract', str(error.exception))


if __name__ == '__main__':
    unittest.main()
