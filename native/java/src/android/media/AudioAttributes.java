package android.media;

/** Plain value class (the framework's needs the native AudioSystem); constants not listed here come
 *  from android.jar. */
public final class AudioAttributes {
    public static final int USAGE_UNKNOWN = 0, USAGE_MEDIA = 1, USAGE_VOICE_COMMUNICATION = 2, USAGE_ALARM = 4,
            USAGE_NOTIFICATION = 5, USAGE_GAME = 14;
    public static final int CONTENT_TYPE_UNKNOWN = 0, CONTENT_TYPE_SPEECH = 1, CONTENT_TYPE_MUSIC = 2;
    public static final int ALLOW_CAPTURE_BY_ALL = 1;
    public static final int SPATIALIZATION_BEHAVIOR_AUTO = 0;

    private int mUsage = USAGE_UNKNOWN, mContentType = CONTENT_TYPE_UNKNOWN, mFlags, mCapturePolicy = ALLOW_CAPTURE_BY_ALL,
            mSpatializationBehavior = SPATIALIZATION_BEHAVIOR_AUTO;
    private boolean mSpatialized, mHapticsMuted = true;

    public AudioAttributes() {}

    public int getUsage() { return mUsage; }
    public int getContentType() { return mContentType; }
    public int getFlags() { return mFlags; }
    public int getAllowedCapturePolicy() { return mCapturePolicy; }
    public boolean isContentSpatialized() { return mSpatialized; }
    public int getSpatializationBehavior() { return mSpatializationBehavior; }
    public boolean areHapticChannelsMuted() { return mHapticsMuted; }
    public int getVolumeControlStream() {
        switch (mUsage) {
            case USAGE_VOICE_COMMUNICATION: return 0;  // STREAM_VOICE_CALL
            case USAGE_ALARM: return 4;
            case USAGE_NOTIFICATION: return 5;
            default: return AudioManager.STREAM_MUSIC;
        }
    }

    @Override public boolean equals(Object o) {
        return o instanceof AudioAttributes a && a.mUsage == mUsage && a.mContentType == mContentType && a.mFlags == mFlags;
    }
    @Override public int hashCode() { return (mUsage * 31 + mContentType) * 31 + mFlags; }
    @Override public String toString() {
        return "AudioAttributes: usage=" + mUsage + " content=" + mContentType + " flags=0x" + Integer.toHexString(mFlags);
    }

    public static class Builder {
        private final AudioAttributes mA = new AudioAttributes();

        public Builder() {}
        public Builder(AudioAttributes aa) {
            mA.mUsage = aa.mUsage;
            mA.mContentType = aa.mContentType;
            mA.mFlags = aa.mFlags;
            mA.mCapturePolicy = aa.mCapturePolicy;
            mA.mSpatialized = aa.mSpatialized;
            mA.mSpatializationBehavior = aa.mSpatializationBehavior;
            mA.mHapticsMuted = aa.mHapticsMuted;
        }
        public Builder setUsage(int usage) { mA.mUsage = usage; return this; }
        public Builder setContentType(int contentType) { mA.mContentType = contentType; return this; }
        public Builder setFlags(int flags) { mA.mFlags |= flags; return this; }
        public Builder setAllowedCapturePolicy(int capturePolicy) { mA.mCapturePolicy = capturePolicy; return this; }
        public Builder setIsContentSpatialized(boolean isSpatialized) { mA.mSpatialized = isSpatialized; return this; }
        public Builder setSpatializationBehavior(int sb) { mA.mSpatializationBehavior = sb; return this; }
        public Builder setHapticChannelsMuted(boolean muted) { mA.mHapticsMuted = muted; return this; }
        public Builder setLegacyStreamType(int streamType) {
            mA.mUsage = streamType == 0 ? USAGE_VOICE_COMMUNICATION : streamType == 4 ? USAGE_ALARM
                    : streamType == 5 ? USAGE_NOTIFICATION : USAGE_MEDIA;
            return this;
        }
        public AudioAttributes build() {
            AudioAttributes a = new AudioAttributes();
            a.mUsage = mA.mUsage;
            a.mContentType = mA.mContentType;
            a.mFlags = mA.mFlags;
            a.mCapturePolicy = mA.mCapturePolicy;
            a.mSpatialized = mA.mSpatialized;
            a.mSpatializationBehavior = mA.mSpatializationBehavior;
            a.mHapticsMuted = mA.mHapticsMuted;
            return a;
        }
    }
}
