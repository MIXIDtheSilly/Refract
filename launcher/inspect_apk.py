"""Read APK identity with Android SDK tools; never install or modify the APK."""
import argparse, base64, json, re, subprocess, zipfile
from pathlib import Path

MAIN = 'android.intent.action.MAIN'
# Launch categories in order of preference. Quest apps often omit LAUNCHER and declare only Horizon's VR/2D
# categories (the Quest home reads those), so `aapt2 dump badging` prints no launchable-activity for them.
CATEGORIES = ['android.intent.category.LAUNCHER', 'com.oculus.intent.category.VR', 'com.oculus.intent.category.2D',
    'android.intent.category.INFO']

def aapt2(tool, *args):
    return subprocess.run([str(tool), *args], capture_output=True, encoding='utf-8', errors='replace', timeout=60,
        creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))

def manifest(tool, apk):
    """Parse `aapt2 dump xmltree` into {'tag', 'attrs', 'children'} nodes; returns the <manifest> node."""
    result = aapt2(tool, 'dump', 'xmltree', '--file', 'AndroidManifest.xml', str(apk))
    root, stack = None, []
    for line in result.stdout.splitlines():
        indent = len(line) - len(line.lstrip())
        text = line.strip()
        if text.startswith('E: '):
            node = {'tag': text[3:].split(' ')[0], 'attrs': {}, 'children': []}
            while stack and stack[-1][0] >= indent: stack.pop()
            if stack: stack[-1][1]['children'].append(node)
            elif root is None: root = node
            stack.append((indent, node))
        elif text.startswith('A: ') and stack:
            match = re.match(r'A: (?:\S*:)?([\w-]+)(?:\(0x[0-9a-f]+\))?=("(?:[^"\\]|\\.)*"|\S+)', text)
            if match: stack[-1][1]['attrs'].setdefault(match[1], match[2].strip('"'))
    if root is None:
        raise RuntimeError((result.stderr.strip().splitlines() or ['Cannot read AndroidManifest.xml'])[-1])
    return root

def children(node, *tags):
    return [c for c in node['children'] if c['tag'] in tags]

def launch_activity(root, package):
    best = None
    for application in children(root, 'application'):
        for order, component in enumerate(children(application, 'activity', 'activity-alias')):
            name = component['attrs'].get('name', '')
            if not name or component['attrs'].get('enabled') == 'false': continue
            for intent in children(component, 'intent-filter'):
                actions = {c['attrs'].get('name') for c in children(intent, 'action')}
                categories = {c['attrs'].get('name') for c in children(intent, 'category')}
                rank = min([CATEGORIES.index(c) for c in categories if c in CATEGORIES] or [len(CATEGORIES)])
                if MAIN not in actions:
                    if rank == len(CATEGORIES): continue
                    rank += len(CATEGORIES) + 1
                if best is None or (rank, order) < best[:2]: best = (rank, order, name)
    if not best: return None
    name = best[2]
    return package + name if name.startswith('.') else name if '.' in name else f'{package}.{name}'

def inspect(apk, sdk):
    versions = [p for p in (sdk / 'build-tools').glob('*/aapt2.exe')]
    if not versions:
        raise RuntimeError('Android SDK build-tools (aapt2) are required')
    tool = max(versions, key=lambda p: tuple(int(x) for x in re.findall(r'\d+', p.parent.name)))
    if not zipfile.is_zipfile(apk):
        raise RuntimeError(f'{apk.name} is not an APK file')
    with zipfile.ZipFile(apk) as archive:
        names = archive.namelist()
    if 'AndroidManifest.xml' not in names:
        if any(n.lower().endswith('.apk') for n in names):
            raise RuntimeError(f'{apk.name} is an app bundle (.apks/.xapk/.apkm). Extract it and import the base APK inside.')
        raise RuntimeError(f'{apk.name} is not an APK file')
    output = aapt2(tool, 'dump', 'badging', str(apk)).stdout
    package = re.search(r"^package: name='([^']+)'", output, re.M)
    package = package[1] if package else None
    activity = re.search(r"^launchable-activity: name='([^']+)'", output, re.M)
    activity = activity[1] if activity else None
    if not package or not activity:
        root = manifest(tool, apk)
        if root['attrs'].get('split'):
            raise RuntimeError(f"{apk.name} is a split APK ({root['attrs']['split']}). Import the game's base APK instead.")
        package = package or root['attrs'].get('package')
        if not package:
            raise RuntimeError(f'{apk.name} has no package name')
        activity = activity or launch_activity(root, package)
        if not activity:
            raise RuntimeError(f'{apk.name} ({package}) has no activity that can be started; it is a library or service, not a game')
        if 'versionName=' not in output: output += f"\nversionName='{root['attrs'].get('versionName', '')}'"
    label = re.search(r"^application-label:'(.*)'$", output, re.M) or re.search(r"^application: label='([^']+)'", output, re.M)
    version = re.search(r"versionName='([^']*)'", output)
    result = {'package': package, 'activity': package + '/' + activity,
        'name': label[1] if label else package, 'version': version[1] if version else '', 'image': ''}
    with zipfile.ZipFile(apk) as archive:
        result['nativeAbis'] = sorted({n.split('/')[1] for n in names if n.startswith('lib/') and n.endswith('.so')})
        # The launcher tells OpenXR games from VrApi ones by these (launcher/core/apk_sdk.mjs).
        result['libraries'] = sorted(n for n in names if n.startswith('lib/') and n.endswith('.so') and n.count('/') == 2)
        icons = re.findall(r"^application-icon-\d+:'([^']+)'", output, re.M)
        for icon in reversed(icons):
            if icon in names and icon.endswith('.png') and archive.getinfo(icon).file_size < 2 * 1024 * 1024:
                result['image'] = 'data:image/png;base64,' + base64.b64encode(archive.read(icon)).decode()
                break
    return result

if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--apk', type=Path, required=True)
    p.add_argument('--sdk', type=Path, required=True)
    a = p.parse_args()
    try: print(json.dumps(inspect(a.apk, a.sdk)))
    except Exception as error: raise SystemExit(str(error))
