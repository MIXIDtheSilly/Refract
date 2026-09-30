param(
    [string]$Sdk = "$env:LOCALAPPDATA\Android\Sdk",
    [string]$Jdk = "$env:ProgramFiles\Android\Android Studio\jbr",
    [string]$NdkVersion = '27.3.13750724',
    [string]$BuildToolsVersion = '36.1.0',
    [ValidateSet('x86_64', 'arm64-v8a')][string]$Abi = 'x86_64'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root "build-android-runtime-windows-$Abi"
$bt = Join-Path $Sdk "build-tools\$BuildToolsVersion"
$androidJar = Join-Path $Sdk 'platforms\android-29\android.jar'
$toolchain = Join-Path $Sdk "ndk\$NdkVersion\build\cmake\android.toolchain.cmake"
$ninja = Join-Path $Sdk 'cmake\3.22.1\bin\ninja.exe'
$cmake = Join-Path $Sdk 'cmake\3.22.1\bin\cmake.exe'
# ABI variants replace the same package and must use the same signing identity.
$keystore = Join-Path $root 'build-android-runtime-windows-x86_64\debug.keystore'
function Run([string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed ($LASTEXITCODE)" }
}
$clang = Join-Path $Sdk "ndk\$NdkVersion\toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe"
$target = @{ 'x86_64' = 'x86_64-linux-android29'; 'arm64-v8a' = 'aarch64-linux-android29' }[$Abi]
foreach ($required in @("$bt\aapt2.exe", "$bt\d8.bat", "$bt\zipalign.exe", "$bt\apksigner.bat", "$Jdk\bin\javac.exe", $androidJar, $toolchain, $ninja, $cmake, $clang)) {
    if (!(Test-Path -LiteralPath $required)) { throw "Missing prerequisite: $required" }
}
$oldJavaHome = $env:JAVA_HOME
try {
    $env:JAVA_HOME = $Jdk
    foreach ($dir in @('res', 'gen', 'classes', 'dex', "package\lib\$Abi")) {
        New-Item -ItemType Directory -Force "$build\$dir" | Out-Null
    }
    Run $cmake @('-S', $root, '-B', "$build\runtime", '-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$ninja", "-DCMAKE_TOOLCHAIN_FILE=$toolchain", "-DANDROID_ABI=$Abi", '-DANDROID_PLATFORM=android-29', '-DCMAKE_BUILD_TYPE=Release', '-DREFRACT_BUILD_HOST_BRIDGE=OFF', '-DREFRACT_BUILD_TESTS=OFF')
    Run $cmake @('--build', "$build\runtime")
    Run "$bt\aapt2.exe" @('compile', '--dir', "$PSScriptRoot\res", '-o', "$build\res")
    $resources = @(Get-ChildItem "$build\res" -Filter *.flat | ForEach-Object FullName)
    Run "$bt\aapt2.exe" (@('link', '-I', $androidJar, '--manifest', "$PSScriptRoot\AndroidManifest.xml", '--java', "$build\gen", '-o', "$build\base.apk") + $resources)
    $sources = @(Get-ChildItem "$PSScriptRoot\src", "$build\gen" -Recurse -Filter *.java | ForEach-Object FullName)
    Copy-Item -LiteralPath $androidJar -Destination "$build\android.jar" -Force
    Run "$Jdk\bin\javac.exe" (@('-source', '8', '-target', '8', '-Xlint:-options', '-bootclasspath', "$build\android.jar", '-d', "$build\classes") + $sources)
    $classes = @(Get-ChildItem "$build\classes" -Recurse -Filter *.class | ForEach-Object FullName)
    Run "$bt\d8.bat" (@('--min-api', '29', '--output', "$build\dex") + $classes)
    Copy-Item "$build\base.apk" "$build\unsigned.apk" -Force
    Copy-Item "$build\runtime\android-runtime\libopenxr_runtime.so" "$build\package\lib\$Abi" -Force
    Copy-Item "$build\dex\classes.dex" "$build\package" -Force
    Run "$Jdk\bin\jar.exe" @('uf', "$build\unsigned.apk", '-C', "$build\package", 'classes.dex', '-C', "$build\package", 'lib')
    Run "$bt\zipalign.exe" @('-f', '-p', '4', "$build\unsigned.apk", "$build\aligned.apk")
    if (!(Test-Path $keystore)) {
        New-Item -ItemType Directory -Force (Split-Path $keystore -Parent) | Out-Null
        Run "$Jdk\bin\keytool.exe" @('-genkeypair', '-keystore', $keystore, '-storepass', 'android', '-keypass', 'android', '-alias', 'androiddebugkey', '-keyalg', 'RSA', '-keysize', '2048', '-validity', '10000', '-dname', 'CN=Android Debug,O=Android,C=US')
    }
    Run "$bt\apksigner.bat" @('sign', '--ks', $keystore, '--ks-pass', 'pass:android', '--key-pass', 'pass:android', '--out', "$build\refract-openxr-runtime-debug.apk", "$build\aligned.apk")
    Run "$bt\apksigner.bat" @('verify', "$build\refract-openxr-runtime-debug.apk")
    Write-Host "Built $build\refract-openxr-runtime-debug.apk"

    # The com.oculus.systemdriver stand-in (systemdriver\AndroidManifest.xml): the same runtime library for games
    # whose Meta OpenXR loader looks for it there. Signed with the same key.
    $driver = Join-Path $build 'systemdriver'
    foreach ($dir in @('classes', 'dex', "package\lib\$Abi")) { New-Item -ItemType Directory -Force "$driver\$dir" | Out-Null }
    Run "$bt\aapt2.exe" @('link', '-I', $androidJar, '--manifest', "$PSScriptRoot\systemdriver\AndroidManifest.xml", '-o', "$driver\unsigned.apk")
    $sources = @(Get-ChildItem "$PSScriptRoot\systemdriver\src" -Recurse -Filter *.java | ForEach-Object FullName)
    Run "$Jdk\bin\javac.exe" (@('-source', '8', '-target', '8', '-Xlint:-options', '-bootclasspath', "$build\android.jar", '-d', "$driver\classes") + $sources)
    $classes = @(Get-ChildItem "$driver\classes" -Recurse -Filter *.class | ForEach-Object FullName)
    Run "$bt\d8.bat" (@('--min-api', '29', '--output', "$driver\dex") + $classes)
    Run $clang @("--target=$target", '-shared', '-fPIC', '-O2', '-Wall', '-o', "$driver\package\lib\$Abi\librefract_driver.so", "$PSScriptRoot\systemdriver\refract_driver.c", '-llog', '-ldl')
    Copy-Item "$build\runtime\android-runtime\libopenxr_runtime.so" "$driver\package\lib\$Abi" -Force
    Copy-Item "$driver\dex\classes.dex" "$driver\package" -Force
    Run "$Jdk\bin\jar.exe" @('uf', "$driver\unsigned.apk", '-C', "$driver\package", 'classes.dex', '-C', "$driver\package", 'lib')
    Run "$bt\zipalign.exe" @('-f', '-p', '4', "$driver\unsigned.apk", "$driver\aligned.apk")
    Run "$bt\apksigner.bat" @('sign', '--ks', $keystore, '--ks-pass', 'pass:android', '--key-pass', 'pass:android', '--out', "$build\refract-systemdriver-debug.apk", "$driver\aligned.apk")
    Run "$bt\apksigner.bat" @('verify', "$build\refract-systemdriver-debug.apk")
    Write-Host "Built $build\refract-systemdriver-debug.apk"
} finally { $env:JAVA_HOME = $oldJavaHome }
