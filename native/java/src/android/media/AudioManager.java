package android.media;

public class AudioManager {
    public static final String PROPERTY_OUTPUT_SAMPLE_RATE = "android.media.property.OUTPUT_SAMPLE_RATE";
    public static final String PROPERTY_OUTPUT_FRAMES_PER_BUFFER = "android.media.property.OUTPUT_FRAMES_PER_BUFFER";
    public static final int AUDIOFOCUS_REQUEST_GRANTED = 1;
    public static final int STREAM_MUSIC = 3;
    public static final int MODE_NORMAL = 0;
    public static final int RINGER_MODE_NORMAL = 2;

    private final int[] volumes = {10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10};

    public AudioManager() {}

    public String getProperty(String key) {
        if (PROPERTY_OUTPUT_SAMPLE_RATE.equals(key)) return "48000";
        if (PROPERTY_OUTPUT_FRAMES_PER_BUFFER.equals(key)) return "256";
        return null;
    }

    public int requestAudioFocus(OnAudioFocusChangeListener l, int streamType, int durationHint) { return AUDIOFOCUS_REQUEST_GRANTED; }
    public int requestAudioFocus(AudioFocusRequest request) { return AUDIOFOCUS_REQUEST_GRANTED; }
    public int abandonAudioFocus(OnAudioFocusChangeListener l) { return AUDIOFOCUS_REQUEST_GRANTED; }
    public int abandonAudioFocusRequest(AudioFocusRequest request) { return AUDIOFOCUS_REQUEST_GRANTED; }
    public int getStreamVolume(int streamType) { return streamType >= 0 && streamType < volumes.length ? volumes[streamType] : 10; }
    public int getStreamMaxVolume(int streamType) { return 15; }
    public int getStreamMinVolume(int streamType) { return 0; }
    public void setStreamVolume(int streamType, int index, int flags) {
        if (streamType >= 0 && streamType < volumes.length) volumes[streamType] = index;
    }
    public void adjustStreamVolume(int streamType, int direction, int flags) {}
    public int getMode() { return MODE_NORMAL; }
    public void setMode(int mode) {}
    public int getRingerMode() { return RINGER_MODE_NORMAL; }
    public boolean isMusicActive() { return false; }
    public boolean isWiredHeadsetOn() { return false; }
    public boolean isBluetoothA2dpOn() { return false; }
    public boolean isBluetoothScoOn() { return false; }
    public boolean isSpeakerphoneOn() { return false; }
    public boolean isMicrophoneMute() { return false; }
    public AudioDeviceInfo[] getDevices(int flags) { return new AudioDeviceInfo[0]; }
    public void registerAudioDeviceCallback(AudioDeviceCallback callback, android.os.Handler handler) {}
    public void unregisterAudioDeviceCallback(AudioDeviceCallback callback) {}
    public int generateAudioSessionId() { return 1; }

    public interface OnAudioFocusChangeListener {
        void onAudioFocusChange(int focusChange);
    }
}
