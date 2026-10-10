package android.app;

import android.content.Context;

/** Dialogs aren't shown; titles and messages go to the log so fatal errors are visible. */
public class AlertDialog extends Dialog {
    protected AlertDialog(Context context) { super(context); }

    public static class Builder {
        public Builder(Context context) {}

        public Builder setTitle(CharSequence title) {
            refract.Runtime.log(6, "AlertDialog", "title: " + title);
            return this;
        }

        public Builder setMessage(CharSequence message) {
            refract.Runtime.log(6, "AlertDialog", "message: " + message);
            return this;
        }
    }
}
