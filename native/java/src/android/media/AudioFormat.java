package android.media;

/** Plain value class (the framework's needs the native AudioSystem); constants not listed here come
 *  from android.jar. */
public final class AudioFormat {
    public static final int ENCODING_INVALID = 0, ENCODING_DEFAULT = 1, ENCODING_PCM_16BIT = 2, ENCODING_PCM_8BIT = 3,
            ENCODING_PCM_FLOAT = 4, ENCODING_PCM_24BIT_PACKED = 21, ENCODING_PCM_32BIT = 22;
    public static final int SAMPLE_RATE_UNSPECIFIED = 0;
    public static final int CHANNEL_INVALID = 0, CHANNEL_IN_DEFAULT = 1, CHANNEL_OUT_DEFAULT = 1,
            CHANNEL_CONFIGURATION_MONO = 2, CHANNEL_CONFIGURATION_STEREO = 3;
    public static final int CHANNEL_IN_LEFT = 0x4, CHANNEL_IN_RIGHT = 0x8, CHANNEL_IN_FRONT = 0x10,
            CHANNEL_IN_MONO = CHANNEL_IN_FRONT, CHANNEL_IN_STEREO = CHANNEL_IN_LEFT | CHANNEL_IN_RIGHT;
    public static final int CHANNEL_OUT_FRONT_LEFT = 0x4, CHANNEL_OUT_FRONT_RIGHT = 0x8,
            CHANNEL_OUT_MONO = CHANNEL_OUT_FRONT_LEFT, CHANNEL_OUT_STEREO = CHANNEL_OUT_FRONT_LEFT | CHANNEL_OUT_FRONT_RIGHT;

    private final int mEncoding, mSampleRate, mChannelMask, mChannelIndexMask;

    private AudioFormat(int encoding, int sampleRate, int channelMask, int channelIndexMask) {
        mEncoding = encoding;
        mSampleRate = sampleRate;
        mChannelMask = channelMask;
        mChannelIndexMask = channelIndexMask;
    }

    public int getEncoding() { return mEncoding; }
    public int getSampleRate() { return mSampleRate; }
    public int getChannelMask() { return mChannelMask; }
    public int getChannelIndexMask() { return mChannelIndexMask; }
    public int getChannelCount() {
        int n = Integer.bitCount(mChannelMask);
        return n != 0 ? n : Integer.bitCount(mChannelIndexMask);
    }
    public int getFrameSizeInBytes() {
        int bytes;
        switch (mEncoding) {
            case ENCODING_PCM_8BIT: bytes = 1; break;
            case ENCODING_PCM_24BIT_PACKED: bytes = 3; break;
            case ENCODING_PCM_FLOAT:
            case ENCODING_PCM_32BIT: bytes = 4; break;
            default: bytes = 2;
        }
        return bytes * Math.max(1, getChannelCount());
    }

    @Override public boolean equals(Object o) {
        return o instanceof AudioFormat f && f.mEncoding == mEncoding && f.mSampleRate == mSampleRate
                && f.mChannelMask == mChannelMask && f.mChannelIndexMask == mChannelIndexMask;
    }
    @Override public int hashCode() { return ((mEncoding * 31 + mSampleRate) * 31 + mChannelMask) * 31 + mChannelIndexMask; }
    @Override public String toString() {
        return "AudioFormat: encoding=" + mEncoding + " rate=" + mSampleRate + " channelMask=0x"
                + Integer.toHexString(mChannelMask) + " channelIndexMask=0x" + Integer.toHexString(mChannelIndexMask);
    }

    public static class Builder {
        private int mEncoding = ENCODING_DEFAULT, mSampleRate = SAMPLE_RATE_UNSPECIFIED, mChannelMask = CHANNEL_INVALID,
                mChannelIndexMask;

        public Builder() {}
        public Builder(AudioFormat af) {
            mEncoding = af.mEncoding;
            mSampleRate = af.mSampleRate;
            mChannelMask = af.mChannelMask;
            mChannelIndexMask = af.mChannelIndexMask;
        }
        public Builder setEncoding(int encoding) throws IllegalArgumentException { mEncoding = encoding; return this; }
        public Builder setSampleRate(int sampleRate) throws IllegalArgumentException { mSampleRate = sampleRate; return this; }
        public Builder setChannelMask(int channelMask) { mChannelMask = channelMask; return this; }
        public Builder setChannelIndexMask(int channelIndexMask) { mChannelIndexMask = channelIndexMask; return this; }
        public AudioFormat build() { return new AudioFormat(mEncoding, mSampleRate, mChannelMask, mChannelIndexMask); }
    }
}
