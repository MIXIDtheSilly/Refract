package refract.app;

import android.content.Context;

/** getSystemService(): the services an app can get, by name and by class. */
final class SystemServices {
    private SystemServices() {}

    static Object create(String name, Context ctx) {
        return switch (name) {
            case Context.WINDOW_SERVICE -> refract.view.WindowManagerImpl.get();
            case Context.ACTIVITY_SERVICE -> new android.app.ActivityManager();
            case Context.AUDIO_SERVICE -> new android.media.AudioManager();
            case Context.POWER_SERVICE -> new android.os.PowerManager();
            case Context.DISPLAY_SERVICE -> new android.hardware.display.DisplayManager();
            case Context.INPUT_METHOD_SERVICE -> new android.view.inputmethod.InputMethodManager();
            case Context.SENSOR_SERVICE -> new android.hardware.SensorManager() {};
            case Context.VIBRATOR_SERVICE -> new android.os.Vibrator() {};
            case Context.CONNECTIVITY_SERVICE -> new android.net.ConnectivityManager();
            case Context.TELEPHONY_SERVICE -> new android.telephony.TelephonyManager();
            case Context.UI_MODE_SERVICE -> new android.app.UiModeManager();
            case Context.CLIPBOARD_SERVICE -> new android.content.ClipboardManager();
            case Context.STORAGE_SERVICE -> new android.os.storage.StorageManager();
            case Context.BATTERY_SERVICE -> new android.os.BatteryManager();
            case Context.KEYGUARD_SERVICE -> new android.app.KeyguardManager();
            case Context.LOCATION_SERVICE -> new android.location.LocationManager();
            case Context.NOTIFICATION_SERVICE -> new android.app.NotificationManager();
            case Context.CAMERA_SERVICE -> new android.hardware.camera2.CameraManager();
            case Context.WIFI_SERVICE -> new android.net.wifi.WifiManager();
            default -> null;
        };
    }

    static String nameOf(Class<?> c) {
        String n = c.getName();
        return switch (n) {
            case "android.view.WindowManager" -> Context.WINDOW_SERVICE;
            case "android.app.ActivityManager" -> Context.ACTIVITY_SERVICE;
            case "android.media.AudioManager" -> Context.AUDIO_SERVICE;
            case "android.os.PowerManager" -> Context.POWER_SERVICE;
            case "android.hardware.display.DisplayManager" -> Context.DISPLAY_SERVICE;
            case "android.view.inputmethod.InputMethodManager" -> Context.INPUT_METHOD_SERVICE;
            case "android.hardware.SensorManager" -> Context.SENSOR_SERVICE;
            case "android.os.Vibrator" -> Context.VIBRATOR_SERVICE;
            case "android.net.ConnectivityManager" -> Context.CONNECTIVITY_SERVICE;
            case "android.telephony.TelephonyManager" -> Context.TELEPHONY_SERVICE;
            case "android.app.UiModeManager" -> Context.UI_MODE_SERVICE;
            case "android.content.ClipboardManager" -> Context.CLIPBOARD_SERVICE;
            case "android.os.storage.StorageManager" -> Context.STORAGE_SERVICE;
            case "android.os.BatteryManager" -> Context.BATTERY_SERVICE;
            default -> null;
        };
    }
}
