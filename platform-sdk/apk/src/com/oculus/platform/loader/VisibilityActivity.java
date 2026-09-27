package com.oculus.platform.loader;

import android.app.Activity;
import android.os.Bundle;

/**
 * Never shown. Games that declare {@code <queries><intent action="MAIN"/></queries>} (as Quest
 * games do to see Horizon) can only see this package if it has an activity handling MAIN.
 */
public final class VisibilityActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        finish();
    }
}
