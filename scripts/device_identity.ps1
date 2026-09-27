param([Parameter(Mandatory)][ValidateSet('quest', 'google')][string]$Identity, [string]$Serial = 'emulator-5582')
# Switches the emulator's brand/manufacturer between a Quest ('oculus'/'Oculus') and 'google'/'Google'.
# The model stays "Quest 3" either way. Reboots the guest; run ensure_tsc.ps1 afterwards.
#   quest  - unmodified Quest APKs: Unity's OculusUnity.getIsOnOculusHardware() needs "oculus" in
#            Build.MANUFACTURER, and the Platform SDK then comes from the Refract platform package.
#   google - OVRPort-patched APKs that must pick OVRPort's generic OpenXR loader.
# The first switch saves each build.prop as <file>.before-identity next to it.
$ErrorActionPreference = 'Stop'
$adb = 'C:\Users\mixid\Android\Sdk\platform-tools\adb.exe'
$brand, $manufacturer = if ($Identity -eq 'quest') { 'oculus', 'Oculus' } else { 'google', 'Google' }
$files = '/system/build.prop /vendor/build.prop /product/etc/build.prop /system_ext/etc/build.prop /odm/etc/build.prop'

& $adb -s $Serial root | Out-Null
& $adb -s $Serial wait-for-device
& $adb -s $Serial remount | Out-Null
$script = "for f in $files; do [ -f `$f.before-identity ] || cp -p `$f `$f.before-identity; " +
          "sed -i -E 's/^(ro\.product\.[a-z_]+\.brand)=.*/\1=$brand/; s/^(ro\.product\.[a-z_]+\.manufacturer)=.*/\1=$manufacturer/' `$f; done; " +
          "grep -hE '^ro\.product\.[a-z_]+\.(brand|manufacturer)=' $files"
& $adb -s $Serial shell $script
& $adb -s $Serial reboot
& $adb -s $Serial wait-for-device
& $adb -s $Serial shell 'while [ "$(getprop sys.boot_completed)" != 1 ]; do sleep 1; done; getprop ro.product.brand; getprop ro.product.manufacturer'
