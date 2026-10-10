package android.text;

import java.util.Locale;

/** Overlay: the firmware version needs android.icu (not available), everything else stays real. */
public class TextUtils {
    public static int getLayoutDirectionFromLocale(Locale locale) {
        if (locale == null || locale.equals(Locale.ROOT)) return 0;
        switch (locale.getLanguage()) {
            case "ar": case "fa": case "he": case "iw": case "ur": case "yi": case "ps": case "sd": case "ug": case "dv":
                return 1;
            default:
                return 0;
        }
    }
}
