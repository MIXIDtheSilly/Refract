package android.media;

/** The two devices audio goes through: the PC's default speaker and microphone (see HostAudio). */
public final class AudioDeviceInfo {
    public static final int TYPE_UNKNOWN = 0, TYPE_BUILTIN_EARPIECE = 1, TYPE_BUILTIN_SPEAKER = 2,
            TYPE_WIRED_HEADSET = 3, TYPE_WIRED_HEADPHONES = 4, TYPE_BLUETOOTH_SCO = 7, TYPE_BLUETOOTH_A2DP = 8,
            TYPE_BUILTIN_MIC = 15, TYPE_TELEPHONY = 18;

    // Ids match native/src/audio_hle.cpp (AAudioStream_getDeviceId).
    private static final AudioDeviceInfo SPEAKER = new AudioDeviceInfo(2, TYPE_BUILTIN_SPEAKER, false);
    private static final AudioDeviceInfo MICROPHONE = new AudioDeviceInfo(1, TYPE_BUILTIN_MIC, true);

    private final int mId, mType;
    private final boolean mSource;

    private AudioDeviceInfo(int id, int type, boolean source) {
        mId = id;
        mType = type;
        mSource = source;
    }

    public static AudioDeviceInfo refractSpeaker() { return SPEAKER; }
    public static AudioDeviceInfo refractMicrophone() { return MICROPHONE; }

    public int getId() { return mId; }
    public int getType() { return mType; }
    public CharSequence getProductName() { return "Refract"; }
    public String getAddress() { return ""; }
    public boolean isSource() { return mSource; }
    public boolean isSink() { return !mSource; }
    public int[] getSampleRates() { return new int[] {8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000}; }
    public int[] getChannelCounts() { return new int[] {1, 2}; }
    public int[] getChannelMasks() {
        return mSource ? new int[] {AudioFormat.CHANNEL_IN_MONO, AudioFormat.CHANNEL_IN_STEREO}
                : new int[] {AudioFormat.CHANNEL_OUT_MONO, AudioFormat.CHANNEL_OUT_STEREO};
    }
    public int[] getChannelIndexMasks() { return new int[] {1, 3}; }
    public int[] getEncodings() { return new int[] {AudioFormat.ENCODING_PCM_16BIT, AudioFormat.ENCODING_PCM_FLOAT}; }
    public int[] getEncapsulationModes() { return new int[0]; }
    public int[] getEncapsulationMetadataTypes() { return new int[0]; }
    @Override public String toString() { return "AudioDeviceInfo{id=" + mId + ", type=" + mType + "}"; }
}
