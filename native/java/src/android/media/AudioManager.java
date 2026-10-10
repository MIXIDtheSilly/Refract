package android.media;

import android.os.Handler;
import android.os.Looper;
import java.util.ArrayList;
import java.util.List;
import refract.media.HostAudio;

/** The PC's default speaker and microphone (refract.media.HostAudio), always routed and unmuted. */
public class AudioManager {
    public static final String PROPERTY_OUTPUT_SAMPLE_RATE = "android.media.property.OUTPUT_SAMPLE_RATE";
    public static final String PROPERTY_OUTPUT_FRAMES_PER_BUFFER = "android.media.property.OUTPUT_FRAMES_PER_BUFFER";
    public static final int AUDIOFOCUS_REQUEST_GRANTED = 1;
    public static final int STREAM_MUSIC = 3;
    public static final int MODE_NORMAL = 0;
    public static final int RINGER_MODE_NORMAL = 2;
    public static final int GET_DEVICES_INPUTS = 1, GET_DEVICES_OUTPUTS = 2, GET_DEVICES_ALL = 3;

    private final int[] volumes = {10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10};

    public AudioManager() {}

    public String getProperty(String key) {
        if (PROPERTY_OUTPUT_SAMPLE_RATE.equals(key)) return Integer.toString(HostAudio.mixRate());
        if (PROPERTY_OUTPUT_FRAMES_PER_BUFFER.equals(key)) return Integer.toString(HostAudio.mixRate() / 100);
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
    public void setSpeakerphoneOn(boolean on) {}
    public void setMicrophoneMute(boolean on) {}
    public void setBluetoothScoOn(boolean on) {}
    public boolean isBluetoothScoAvailableOffCall() { return false; }
    public void startBluetoothSco() {}
    public void stopBluetoothSco() {}

    public AudioDeviceInfo[] getDevices(int flags) {
        List<AudioDeviceInfo> out = new ArrayList<>();
        if ((flags & GET_DEVICES_INPUTS) != 0) out.add(AudioDeviceInfo.refractMicrophone());
        if ((flags & GET_DEVICES_OUTPUTS) != 0) out.add(AudioDeviceInfo.refractSpeaker());
        return out.toArray(new AudioDeviceInfo[0]);
    }
    public List<MicrophoneInfo> getMicrophones() { return new ArrayList<>(); }
    public List<AudioDeviceInfo> getAvailableCommunicationDevices() {
        List<AudioDeviceInfo> out = new ArrayList<>();
        out.add(AudioDeviceInfo.refractSpeaker());
        return out;
    }
    public AudioDeviceInfo getCommunicationDevice() { return AudioDeviceInfo.refractSpeaker(); }
    public boolean setCommunicationDevice(AudioDeviceInfo device) { return true; }
    public void clearCommunicationDevice() {}

    /** As on Android, a new callback hears about the current devices right away. */
    public void registerAudioDeviceCallback(AudioDeviceCallback callback, Handler handler) {
        if (callback == null) return;
        Handler h = handler != null ? handler : new Handler(Looper.getMainLooper());
        h.post(() -> callback.onAudioDevicesAdded(getDevices(GET_DEVICES_ALL)));
    }
    public void unregisterAudioDeviceCallback(AudioDeviceCallback callback) {}
    public int generateAudioSessionId() { return HostAudio.newSession(); }

    public interface OnAudioFocusChangeListener {
        void onAudioFocusChange(int focusChange);
    }
}
