package com.google.android.play.core.assetpacks;

public class AssetPackException extends RuntimeException {
    private final int errorCode;

    public AssetPackException(int errorCode) {
        this.errorCode = errorCode;
    }

    public AssetPackException(String message, int errorCode) {
        super(message);
        this.errorCode = errorCode;
    }

    public AssetPackException(String message, int errorCode, Throwable cause) {
        super(message, cause);
        this.errorCode = errorCode;
    }

    public int getErrorCode() { return errorCode; }
}
