package com.google.android.play.core.assetpacks;

import java.util.Map;

public abstract class AssetPackStates {
    public abstract long totalBytes();
    public abstract Map<String, AssetPackState> packStates();
}
