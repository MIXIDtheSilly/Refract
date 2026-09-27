# Removes phone-only Google/AOSP apps from the emulator for user 0 (pm uninstall -k --user 0).
# The APKs stay on /system, so -Restore puts them back (cmd package install-existing).
# Kept on purpose: GMS/GSF/Play Store, WebView, Chrome, launcher, keyboard, Settings,
# SystemUI, DocumentsUI, providers and networking, plus everything Refract installs.
param(
    [string]$Serial = 'emulator-5582',
    [switch]$Restore
)

$adb = 'C:\Users\mixid\Android\Sdk\platform-tools\adb.exe'

$packages = @(
    # Google apps
    'com.google.android.googlequicksearchbox'      # Google app / Assistant
    'com.google.android.as'                        # Android System Intelligence
    'com.google.android.as.oss'
    'com.google.android.tts'
    'com.google.android.apps.messaging'
    'com.google.android.apps.photos'
    'com.google.android.apps.wellbeing'
    'com.google.android.apps.restore'
    'com.google.android.apps.safetyhub'            # boot-time crash dialog that steals focus
    'com.google.android.apps.maps'
    'com.google.android.apps.docs'
    'com.google.android.apps.youtube.music'
    'com.google.android.youtube'
    'com.google.android.gm'
    'com.google.android.calendar'
    'com.google.android.deskclock'
    'com.google.android.dialer'
    'com.google.android.contacts'
    'com.google.android.projection.gearhead'       # Android Auto
    'com.google.android.settings.intelligence'
    'com.google.android.feedback'
    'com.google.android.markup'
    'com.google.android.avatarpicker'
    'com.google.android.odad'
    'com.google.android.gms.supervision'
    'com.google.android.printservice.recommendation'
    # Accessibility services
    'com.google.android.marvin.talkback'
    'com.google.android.marvin.talkbackoverlay'
    'com.google.android.accessibility.switchaccess' # boot-time crash dialog that steals focus
    'com.google.android.apps.accessibility.voiceaccess'
    # AOSP extras
    'com.android.camera2'
    'com.android.DeviceAsWebcam'
    'com.android.devicediagnostics'
    'com.android.imsserviceentitlement'
    'com.android.egg'
    'com.android.dreams.basic'
    'com.android.bips'
    'com.android.virtualization.terminal'
    'com.android.stk'
    'com.android.simappdialog'
)

foreach ($p in $packages) {
    if ($Restore) {
        $out = & $adb -s $Serial shell cmd package install-existing --user 0 $p 2>&1
    } else {
        $out = & $adb -s $Serial shell pm uninstall -k --user 0 $p 2>&1
    }
    '{0,-52} {1}' -f $p, ($out -join ' ')
}
