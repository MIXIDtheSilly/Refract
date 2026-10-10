package android.media;

import android.content.Context;
import android.os.Handler;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;
import refract.media.HostAudio;

/** Records from the default Windows recording device (refract.media.HostAudio). */
public class AudioRecord {
    public static final int STATE_UNINITIALIZED = 0, STATE_INITIALIZED = 1;
    public static final int RECORDSTATE_STOPPED = 1, RECORDSTATE_RECORDING = 3;
    public static final int SUCCESS = 0, ERROR = -1, ERROR_BAD_VALUE = -2, ERROR_INVALID_OPERATION = -3,
            ERROR_DEAD_OBJECT = -6;
    public static final int READ_BLOCKING = 0, READ_NON_BLOCKING = 1;

    private long mStream;
    private int mSource, mSampleRate, mChannelMask, mChannels, mEncoding, mFrameBytes, mSession;
    private volatile int mRecordingState = RECORDSTATE_STOPPED;

    public AudioRecord(int audioSource, int sampleRateInHz, int channelConfig, int audioFormat, int bufferSizeInBytes)
            throws IllegalArgumentException {
        init(audioSource, sampleRateInHz, channelConfig, audioFormat, bufferSizeInBytes, 0);
    }

    private AudioRecord(int source, AudioFormat format, int bufferSizeInBytes, int session) {
        init(source, format.getSampleRate(), format.getChannelMask() != AudioFormat.CHANNEL_INVALID
                ? format.getChannelMask() : AudioFormat.CHANNEL_IN_MONO, format.getEncoding(), bufferSizeInBytes, session);
    }

    private void init(int source, int rate, int channelMask, int encoding, int bufferBytes, int session) {
        mSource = source;
        mSampleRate = rate > 0 ? rate : HostAudio.mixRate();
        mChannelMask = channelMask;
        mChannels = HostAudio.inChannels(channelMask);
        mEncoding = HostAudio.format(encoding) == HostAudio.FORMAT_I16 ? AudioFormat.ENCODING_PCM_16BIT : encoding;
        mFrameBytes = mChannels * HostAudio.sampleBytes(mEncoding);
        mSession = session > 0 ? session : HostAudio.newSession();
        int format = HostAudio.format(mEncoding);
        if (format != 0 && mFrameBytes > 0)
            mStream = HostAudio.open(true, mSampleRate, mChannels, format, Math.max(bufferBytes, 0) / mFrameBytes);
    }

    public static int getMinBufferSize(int sampleRateInHz, int channelConfig, int audioFormat) {
        int bytes = HostAudio.sampleBytes(audioFormat);
        if (bytes == 0 || sampleRateInHz <= 0) return ERROR_BAD_VALUE;
        return sampleRateInHz / 50 * HostAudio.inChannels(channelConfig) * bytes;  // 20 ms
    }

    public int getState() { return mStream != 0 ? STATE_INITIALIZED : STATE_UNINITIALIZED; }
    public int getRecordingState() { return mRecordingState; }
    public int getSampleRate() { return mSampleRate; }
    public int getChannelCount() { return mChannels; }
    public int getChannelConfiguration() { return mChannelMask; }
    public int getAudioFormat() { return mEncoding; }
    public int getAudioSource() { return mSource; }
    public int getAudioSessionId() { return mSession; }
    public int getBufferSizeInFrames() { return mStream != 0 ? HostAudio.bufferFrames(mStream) : 0; }
    public AudioFormat getFormat() {
        return new AudioFormat.Builder().setSampleRate(mSampleRate).setChannelMask(mChannelMask).setEncoding(mEncoding).build();
    }

    public void startRecording() throws IllegalStateException {
        if (mStream == 0) throw new IllegalStateException("startRecording() called on an uninitialized AudioRecord.");
        if (HostAudio.start(mStream) == 0) mRecordingState = RECORDSTATE_RECORDING;
    }
    public void startRecording(MediaSyncEvent syncEvent) throws IllegalStateException { startRecording(); }

    public void stop() throws IllegalStateException {
        if (mStream == 0) throw new IllegalStateException("stop() called on an uninitialized AudioRecord.");
        mRecordingState = RECORDSTATE_STOPPED;
        HostAudio.stop(mStream);
    }

    public void release() {
        long s = mStream;
        mStream = 0;
        mRecordingState = RECORDSTATE_STOPPED;
        if (s != 0) HostAudio.close(s);
    }

    private int result(int n) { return n >= 0 ? n : n == -899 ? ERROR_DEAD_OBJECT : ERROR_INVALID_OPERATION; }

    private int readBytes(Object array, int byteOffset, int bytes, int mode) {
        if (mStream == 0) return ERROR_INVALID_OPERATION;
        if (array == null || byteOffset < 0 || bytes < 0) return ERROR_BAD_VALUE;
        if (mode != READ_BLOCKING && mode != READ_NON_BLOCKING) return ERROR_BAD_VALUE;
        return result(HostAudio.readArray(mStream, array, byteOffset, bytes, mode == READ_BLOCKING));
    }

    public int read(byte[] audioData, int offsetInBytes, int sizeInBytes) {
        return read(audioData, offsetInBytes, sizeInBytes, READ_BLOCKING);
    }
    public int read(byte[] audioData, int offsetInBytes, int sizeInBytes, int readMode) {
        if (audioData != null && offsetInBytes + sizeInBytes > audioData.length) return ERROR_BAD_VALUE;
        return readBytes(audioData, offsetInBytes, sizeInBytes, readMode);
    }
    public int read(short[] audioData, int offsetInShorts, int sizeInShorts) {
        return read(audioData, offsetInShorts, sizeInShorts, READ_BLOCKING);
    }
    public int read(short[] audioData, int offsetInShorts, int sizeInShorts, int readMode) {
        if (audioData != null && offsetInShorts + sizeInShorts > audioData.length) return ERROR_BAD_VALUE;
        int n = readBytes(audioData, offsetInShorts * 2, sizeInShorts * 2, readMode);
        return n > 0 ? n / 2 : n;
    }
    public int read(float[] audioData, int offsetInFloats, int sizeInFloats, int readMode) {
        if (audioData != null && offsetInFloats + sizeInFloats > audioData.length) return ERROR_BAD_VALUE;
        int n = readBytes(audioData, offsetInFloats * 4, sizeInFloats * 4, readMode);
        return n > 0 ? n / 4 : n;
    }
    /** Like Android, fills the buffer from index 0 and leaves its position alone. */
    public int read(ByteBuffer audioBuffer, int sizeInBytes) { return read(audioBuffer, sizeInBytes, READ_BLOCKING); }
    public int read(ByteBuffer audioBuffer, int sizeInBytes, int readMode) {
        if (mStream == 0) return ERROR_INVALID_OPERATION;
        if (audioBuffer == null || sizeInBytes < 0) return ERROR_BAD_VALUE;
        int size = Math.min(sizeInBytes, audioBuffer.capacity());
        if (audioBuffer.isDirect())
            return result(HostAudio.readBuffer(mStream, audioBuffer, 0, size, readMode == READ_BLOCKING));
        if (audioBuffer.hasArray()) return readBytes(audioBuffer.array(), audioBuffer.arrayOffset(), size, readMode);
        return ERROR_BAD_VALUE;
    }

    public int setNotificationMarkerPosition(int markerInFrames) { return SUCCESS; }
    public int setPositionNotificationPeriod(int periodInFrames) { return SUCCESS; }
    public int getNotificationMarkerPosition() { return 0; }
    public int getPositionNotificationPeriod() { return 0; }
    public void setRecordPositionUpdateListener(OnRecordPositionUpdateListener listener) {}
    public void setRecordPositionUpdateListener(OnRecordPositionUpdateListener listener, Handler handler) {}
    public AudioDeviceInfo getRoutedDevice() { return AudioDeviceInfo.refractMicrophone(); }
    public AudioDeviceInfo getPreferredDevice() { return null; }
    public boolean setPreferredDevice(AudioDeviceInfo deviceInfo) { return true; }
    public List<MicrophoneInfo> getActiveMicrophones() { return new ArrayList<>(); }
    public boolean setPreferredMicrophoneDirection(int direction) { return true; }
    public boolean setPreferredMicrophoneFieldDimension(float zoom) { return true; }
    public int getTimestamp(AudioTimestamp outTimestamp, int timebase) {
        if (mStream == 0 || mRecordingState != RECORDSTATE_RECORDING) return ERROR_INVALID_OPERATION;
        outTimestamp.framePosition = HostAudio.position(mStream);
        outTimestamp.nanoTime = System.nanoTime();
        return SUCCESS;
    }

    public interface OnRecordPositionUpdateListener {
        void onMarkerReached(AudioRecord recorder);
        void onPeriodicNotification(AudioRecord recorder);
    }

    public static class Builder {
        private int mSource = MediaRecorder.AudioSource.DEFAULT;
        private AudioFormat mFormat;
        private int mBufferBytes, mSession;

        public Builder() {}
        public Builder setAudioSource(int source) { mSource = source; return this; }
        public Builder setAudioFormat(AudioFormat format) { mFormat = format; return this; }
        public Builder setBufferSizeInBytes(int bufferSizeInBytes) { mBufferBytes = bufferSizeInBytes; return this; }
        public Builder setAudioAttributes(AudioAttributes attributes) { return this; }
        public Builder setSessionId(int sessionId) { mSession = sessionId; return this; }
        public Builder setContext(Context context) { return this; }
        public Builder setPrivacySensitive(boolean privacySensitive) { return this; }
        public AudioRecord build() throws UnsupportedOperationException {
            AudioFormat f = mFormat != null ? mFormat : new AudioFormat.Builder().build();
            AudioRecord r = new AudioRecord(mSource, f, mBufferBytes, mSession);
            if (r.getState() != STATE_INITIALIZED) throw new UnsupportedOperationException("Cannot create AudioRecord");
            return r;
        }
    }
}
