"""Installs an Android APK for refract_native (no emulator).

Creates <apps>/<package>/ with:
  lib/arm64/*.so     the APK's ARM64 native libraries
  classes.jar        the APK's dex code as JVM bytecode (dex2jar + ShimBuilder app)
  app.properties     package, launch activity, Application class, meta-data

usage: python install_apk.py app.apk [--apps DIR] [--sdk DIR] [--jdk DIR]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
NATIVE = HERE.parent
REPO = NATIVE.parent
QUEST = REPO.parent


def find_aapt2(sdk: Path) -> Path:
    tools = sorted((sdk / "build-tools").glob("*/aapt2.exe"), reverse=True)
    if not tools:
        sys.exit(f"aapt2 not found under {sdk}/build-tools")
    return tools[0]


def parse_value(raw: str) -> str:
    """aapt2 xmltree attribute value -> 'type:value' for app.properties."""
    raw = raw.strip()
    m = re.match(r'^"(.*)" \(Raw: ".*"\)$', raw)
    if m:
        return "string:" + m.group(1)
    if raw in ("true", "false"):
        return "boolean:" + raw
    if re.match(r"^-?(0x[0-9a-fA-F]+|\d+)$", raw):
        return "int:" + raw
    if re.match(r"^-?\d+\.\d+$", raw):
        return "float:" + raw
    if raw.startswith("@"):
        return "int:" + raw[1:]
    return "string:" + raw.strip('"')


def manifest(aapt2: Path, apk: Path) -> dict:
    badging = subprocess.run([str(aapt2), "dump", "badging", str(apk)], capture_output=True, text=True,
                             encoding="utf-8", errors="replace").stdout
    info = {"meta": {}, "activity_meta": {}}
    m = re.search(r"package: name='([^']+)' versionCode='([^']*)' versionName='([^']*)'", badging)
    info["package"], info["versionCode"], info["versionName"] = m.group(1), m.group(2) or "1", m.group(3) or "1.0"
    m = re.search(r"application-label:'([^']*)'", badging)
    info["label"] = m.group(1) if m else info["package"]
    m = re.search(r"targetSdkVersion:'(\d+)'", badging)
    info["targetSdk"] = m.group(1) if m else "32"
    m = re.search(r"launchable-activity: name='([^']+)'", badging)
    info["activity"] = m.group(1) if m else None

    tree = subprocess.run([str(aapt2), "dump", "xmltree", "--file", "AndroidManifest.xml", str(apk)],
                          capture_output=True, text=True, encoding="utf-8", errors="replace").stdout
    # Walk elements by indentation.
    stack = []  # (indent, element dict)
    activities = []
    for line in tree.splitlines():
        em = re.match(r"^(\s*)E: ([\w-]+)", line)
        if em:
            indent = len(em.group(1))
            while stack and stack[-1][0] >= indent:
                stack.pop()
            el = {"tag": em.group(2), "attrs": {}, "children": []}
            if stack:
                stack[-1][1]["children"].append(el)
            stack.append((indent, el))
            if el["tag"] in ("activity", "activity-alias"):
                activities.append(el)
            if el["tag"] == "application":
                info["application_el"] = el
            continue
        am = re.match(r"^\s*A: (?:http://schemas.android.com/apk/res/android:)?(\w+)(?:\(0x[0-9a-f]+\))?=(.*)$", line)
        if am and stack:
            stack[-1][1]["attrs"][am.group(1)] = am.group(2)

    def name_of(el):
        v = el["attrs"].get("name", "")
        m = re.match(r'^"([^"]*)"', v)
        return m.group(1) if m else v

    app_el = info.pop("application_el", None)
    if app_el:
        cls = name_of(app_el)
        if cls:
            info["application"] = cls if not cls.startswith(".") else info["package"] + cls
        for c in app_el["children"]:
            if c["tag"] == "meta-data" and "value" in c["attrs"]:
                info["meta"][name_of(c)] = parse_value(c["attrs"]["value"])
    launch = None
    for a in activities:
        for f in (c for c in a["children"] if c["tag"] == "intent-filter"):
            names = [name_of(x) for x in f["children"]]
            if "android.intent.action.MAIN" in names and any(
                    n in names for n in ("android.intent.category.LAUNCHER", "com.oculus.intent.category.VR")):
                launch = launch or a
    if launch:
        if not info["activity"]:
            n = name_of(launch)
            info["activity"] = n if not n.startswith(".") else info["package"] + n
        for c in launch["children"]:
            if c["tag"] == "meta-data" and "value" in c["attrs"]:
                info["activity_meta"][name_of(c)] = parse_value(c["attrs"]["value"])
    if not info["activity"]:
        sys.exit("no launchable activity found in the manifest")
    return info


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("apk", type=Path)
    ap.add_argument("--apps", type=Path, default=Path(os.environ.get("LOCALAPPDATA", ".")) / "Refract" / "native" / "apps")
    ap.add_argument("--sdk", type=Path, default=Path(os.environ.get("USERPROFILE", ".")) / "Android" / "Sdk")
    ap.add_argument("--jdk", type=Path, default=Path("C:/Program Files/Java/jdk-27"))
    ap.add_argument("--dex2jar", type=Path, default=QUEST / "external" / "dex2jar" / "d2j-dex2jar.bat")
    ap.add_argument("--runtime", type=Path,
                    default=REPO / "build-android-runtime-windows-arm64-v8a" / "systemdriver" / "package" / "lib" / "arm64-v8a",
                    help="Refract's ARM64 libopenxr_runtime.so + librefract_driver.so (android-runtime-apk/build_apk.ps1 -Abi arm64-v8a)")
    args = ap.parse_args()
    apk = args.apk.resolve()

    info = manifest(find_aapt2(args.sdk), apk)
    dest = args.apps / info["package"]
    print(f"installing {info['package']} ({info['label']}) -> {dest}")
    libdir = dest / "lib" / "arm64"
    if libdir.exists():
        shutil.rmtree(libdir)
    libdir.mkdir(parents=True)

    with zipfile.ZipFile(apk) as z, tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        n = 0
        for e in z.infolist():
            if e.filename.startswith("lib/arm64-v8a/") and e.filename.endswith(".so"):
                with z.open(e) as src, open(libdir / Path(e.filename).name, "wb") as out:
                    shutil.copyfileobj(src, out)
                n += 1
        print(f"  {n} native libraries")
        # Refract's OpenXR runtime, reached the way a Quest game reaches Horizon's: Meta's loader
        # calls com.oculus.systemdriver.DriverLoader (Refract's stand-in, android-runtime-apk/systemdriver).
        for name in ("libopenxr_runtime.so", "librefract_driver.so"):
            if not (args.runtime / name).exists():
                sys.exit(f"{args.runtime / name} missing: run android-runtime-apk/build_apk.ps1 -Abi arm64-v8a")
            shutil.copy2(args.runtime / name, libdir / name)
        print("  Refract OpenXR runtime (com.oculus.systemdriver stand-in)")
        # Unity dlopen()s plugins by bare name ("OculusXRPlugin"); the same file under that name
        # lets the linker match it with the already loaded library (same inode).
        for so in sorted(libdir.glob("lib*.so")):
            alias = libdir / so.name[3:-3]
            if not alias.exists():
                os.link(so, alias)
        dexes = sorted((e for e in z.namelist() if re.match(r"^classes\d*\.dex$", e)),
                       key=lambda s: int(re.sub(r"\D", "", s) or 1))
        jars = []
        for d in dexes:
            z.extract(d, tmp)
            out = tmp / (d + ".jar")
            subprocess.run([str(args.dex2jar), "-f", "-o", str(out), str(tmp / d)], check=True,
                           capture_output=True)
            jars.append(out)
        java = args.jdk / "bin" / "java.exe"
        shim = REPO / "build-native" / "java" / "refract-android.jar"
        # DriverLoader joins the app's classes, so the app rewrite below sends its loadLibrary calls
        # to the guest linker like the app's own.
        driver_src = REPO / "android-runtime-apk" / "systemdriver" / "src"
        driver_classes = tmp / "systemdriver"
        subprocess.run([str(args.jdk / "bin" / "javac.exe"), "-nowarn", "--release", "21", "-proc:none",
                        "-cp", str(shim), "-d", str(driver_classes)]
                       + [str(p) for p in driver_src.rglob("*.java")], check=True)
        driver_jar = tmp / "systemdriver.jar"
        with zipfile.ZipFile(driver_jar, "w") as dz:
            for p in driver_classes.rglob("*.class"):
                dz.write(p, p.relative_to(driver_classes).as_posix())
        jars.append(driver_jar)
        # Merge dex jars (first one wins for duplicates, like ART's class loading order).
        merged = tmp / "merged.jar"
        seen = set()
        with zipfile.ZipFile(merged, "w", zipfile.ZIP_DEFLATED) as mz:
            for j in jars:
                with zipfile.ZipFile(j) as jz:
                    for e in jz.infolist():
                        if e.filename in seen or e.is_dir():
                            continue
                        seen.add(e.filename)
                        mz.writestr(e.filename, jz.read(e))
        subprocess.run([str(java), "-cp", str(NATIVE / "java" / "tools" / "lib" / "asm-9.8.jar"), f"-Drefract.shim={shim}", str(NATIVE / "java" / "tools" / "ShimBuilder.java"), "app", str(merged),
                        str(dest / "classes.jar")], check=True)

    props = [
        f"package={info['package']}",
        f"activity={info['activity']}",
        f"label={info['label']}",
        f"versionCode={info['versionCode']}",
        f"versionName={info['versionName']}",
        f"targetSdk={info['targetSdk']}",
        f"apk={apk}".replace("\\", "/"),
    ]
    if info.get("application"):
        props.append(f"application={info['application']}")
    props += [f"meta.{k}={v}" for k, v in sorted(info["meta"].items())]
    props += [f"activity.meta.{k}={v}" for k, v in sorted(info["activity_meta"].items())]
    (dest / "app.properties").write_text("\n".join(props) + "\n", encoding="utf-8")
    print(f"  launch activity {info['activity']}")
    print(f"run: refract_native --sysroot <dump fs> --app {dest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
