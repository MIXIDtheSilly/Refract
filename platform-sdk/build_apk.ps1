param(
    [string]$Sdk = "$env:LOCALAPPDATA\Android\Sdk",
    [string]$Jdk = "$env:ProgramFiles\Android\Android Studio\jbr",
    [string]$NdkVersion = '27.3.13750724',
    [string]$BuildToolsVersion = '36.1.0'
)
# Builds the Refract Platform SDK stand-in (package com.oculus.horizon, arm64 only):
# build-platform-sdk\refract-platform-debug.apk. Install with adb install --force-queryable -r.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'build-platform-sdk'
$bt = Join-Path $Sdk "build-tools\$BuildToolsVersion"
$androidJar = Join-Path $Sdk 'platforms\android-29\android.jar'
$toolchain = Join-Path $Sdk "ndk\$NdkVersion\build\cmake\android.toolchain.cmake"
$ninja = Join-Path $Sdk 'cmake\3.22.1\bin\ninja.exe'
$cmake = Join-Path $Sdk 'cmake\3.22.1\bin\cmake.exe'
$keystore = Join-Path $root 'build-android-runtime-windows-x86_64\debug.keystore'
function Run([string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed ($LASTEXITCODE)" }
}
foreach ($required in @("$bt\aapt2.exe", "$bt\d8.bat", "$bt\zipalign.exe", "$bt\apksigner.bat", "$Jdk\bin\javac.exe", $androidJar, $toolchain, $ninja, $cmake, $keystore)) {
    if (!(Test-Path -LiteralPath $required)) { throw "Missing prerequisite: $required" }
}
$oldJavaHome = $env:JAVA_HOME
try {
    $env:JAVA_HOME = $Jdk
    foreach ($dir in @('classes', 'dex', 'package\lib\arm64-v8a')) {
        New-Item -ItemType Directory -Force "$build\$dir" | Out-Null
    }
    Run $cmake @('-S', "$PSScriptRoot\native", '-B', "$build\native", '-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$ninja", "-DCMAKE_TOOLCHAIN_FILE=$toolchain", '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-29', '-DCMAKE_BUILD_TYPE=Release')
    Run $cmake @('--build', "$build\native")
    Run "$bt\aapt2.exe" @('link', '-I', $androidJar, '--manifest', "$PSScriptRoot\apk\AndroidManifest.xml", '-o', "$build\base.apk")
    $sources = @(Get-ChildItem "$PSScriptRoot\apk\src" -Recurse -Filter *.java | ForEach-Object FullName)
    Run "$Jdk\bin\javac.exe" (@('-source', '8', '-target', '8', '-Xlint:-options', '-bootclasspath', $androidJar, '-d', "$build\classes") + $sources)
    $classes = @(Get-ChildItem "$build\classes" -Recurse -Filter *.class | ForEach-Object FullName)
    Run "$bt\d8.bat" (@('--min-api', '29', '--output', "$build\dex") + $classes)
    Copy-Item "$build\base.apk" "$build\unsigned.apk" -Force
    Copy-Item "$build\native\librefract_ovrplatform.so" "$build\package\lib\arm64-v8a" -Force
    Copy-Item "$build\dex\classes.dex" "$build\package" -Force
    Run "$Jdk\bin\jar.exe" @('uf', "$build\unsigned.apk", '-C', "$build\package", 'classes.dex', '-C', "$build\package", 'lib')
    Run "$bt\zipalign.exe" @('-f', '-p', '4', "$build\unsigned.apk", "$build\aligned.apk")
    Run "$bt\apksigner.bat" @('sign', '--ks', $keystore, '--ks-pass', 'pass:android', '--key-pass', 'pass:android', '--out', "$build\refract-platform-debug.apk", "$build\aligned.apk")
    Run "$bt\apksigner.bat" @('verify', "$build\refract-platform-debug.apk")
    Write-Host "Built $build\refract-platform-debug.apk"
} finally { $env:JAVA_HOME = $oldJavaHome }
