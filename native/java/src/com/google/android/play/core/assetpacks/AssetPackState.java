package com.google.android.play.core.assetpacks;

public abstract class AssetPackState {
    public abstract String name();
    public abstract int status();
    public abstract int errorCode();
    public abstract long bytesDownloaded();
    public abstract long totalBytesToDownload();
    public abstract int transferProgressPercentage();
}
