package android.media;

import android.os.Handler;
import java.nio.ByteBuffer;
import refract.media.HostAudio;

/** Plays to the default Windows playback device (refract.media.HostAudio). */
public class AudioTrack {
    public static final int PLAYSTATE_STOPPED = 1, PLAYSTATE_PAUSED = 2, PLAYSTATE_PLAYING = 3;
    public static final int MODE_STATIC = 0, MODE_STREAM = 1;
    public static final int STATE_UNINITIALIZED = 0, STATE_INITIALIZED = 1, STATE_NO_STATIC_DATA = 2;
    public static final int SUCCESS = 0, ERROR = -1, ERROR_BAD_VALUE = -2, ERROR_INVALID_OPERATION = -3,
            ERROR_DEAD_OBJECT = -6, ERROR_WOULD_BLOCK = -7;
    public static final int WRITE_BLOCKING = 0, WRITE_NON_BLOCKING = 1;
    public static final int PERFORMANCE_MODE_NONE = 0, PERFORMANCE_MODE_LOW_LATENCY = 1,
            PERFORMANCE_MODE_POWER_SAVING = 2;

    private long mStream;
    private int mStreamType = AudioManager.STREAM_MUSIC, mSampleRate, mChannelMask, mChannels, mEncoding,
            mFrameBytes, mSession, mMode = MODE_STREAM, mPerformanceMode;
    private volatile int mPlayState = PLAYSTATE_STOPPED;
    private byte[] mStaticData;  // MODE_STATIC: played from the start on every play()
    private int mStaticBytes;

    public AudioTrack(int streamType, int sampleRateInHz, int channelConfig, int audioFormat, int bufferSizeInBytes,
                      int mode) throws IllegalArgumentException {
        this(streamType, sampleRateInHz, channelConfig, audioFormat, bufferSizeInBytes, mode, 0);
    }

    public AudioTrack(int streamType, int sampleRateInHz, int channelConfig, int audioFormat, int bufferSizeInBytes,
                      int mode, int sessionId) throws IllegalArgumentException {
        mStreamType = streamType;
        init(sampleRateInHz, channelConfig, audioFormat, bufferSizeInBytes, mode, sessionId);
    }

    public AudioTrack(AudioAttributes attributes, AudioFormat format, int bufferSizeInBytes, int mode, int sessionId)
            throws IllegalArgumentException {
        init(format.getSampleRate(), format.getChannelMask() != AudioFormat.CHANNEL_INVALID
                ? format.getChannelMask() : AudioFormat.CHANNEL_OUT_STEREO, format.getEncoding(), bufferSizeInBytes,
                mode, sessionId);
    }

    private void init(int rate, int channelMask, int encoding, int bufferBytes, int mode, int session) {
        mSampleRate = rate > 0 ? rate : HostAudio.mixRate();
        mChannelMask = channelMask;
        mChannels = HostAudio.outChannels(channelMask);
        mEncoding = HostAudio.format(encoding) == HostAudio.FORMAT_I16 ? AudioFormat.ENCODING_PCM_16BIT : encoding;
        mFrameBytes = mChannels * HostAudio.sampleBytes(mEncoding);
        mMode = mode;
        mSession = session > 0 ? session : HostAudio.newSession();
        int format = HostAudio.format(mEncoding);
        if (format == 0 || mFrameBytes == 0) return;
        if (mode == MODE_STATIC) mStaticData = new byte[Math.max(bufferBytes, 0)];
        mStream = HostAudio.open(false, mSampleRate, mChannels, format,
                mode == MODE_STATIC ? 0 : Math.max(bufferBytes, 0) / mFrameBytes);
    }

    public static int getMinBufferSize(int sampleRateInHz, int channelConfig, int audioFormat) {
        int bytes = HostAudio.sampleBytes(audioFormat);
        if (bytes == 0 || sampleRateInHz <= 0) return ERROR_BAD_VALUE;
        return sampleRateInHz / 50 * HostAudio.outChannels(channelConfig) * bytes;  // 20 ms
    }
    public static int getNativeOutputSampleRate(int streamType) { return HostAudio.mixRate(); }
    public static float getMinVolume() { return 0f; }
    public static float getMaxVolume() { return 1f; }

    public int getState() {
        if (mStream == 0) return STATE_UNINITIALIZED;
        return mMode == MODE_STATIC && mStaticBytes == 0 ? STATE_NO_STATIC_DATA : STATE_INITIALIZED;
    }
    public int getPlayState() { return mPlayState; }
    public int getSampleRate() { return mSampleRate; }
    public int getPlaybackRate() { return mSampleRate; }
    public int setPlaybackRate(int sampleRateInHz) { return SUCCESS; }
    public int getChannelCount() { return mChannels; }
    public int getChannelConfiguration() { return mChannelMask; }
    public int getAudioFormat() { return mEncoding; }
    public int getStreamType() { return mStreamType; }
    public int getAudioSessionId() { return mSession; }
    public int getPerformanceMode() { return mPerformanceMode; }
    public AudioFormat getFormat() {
        return new AudioFormat.Builder().setSampleRate(mSampleRate).setChannelMask(mChannelMask).setEncoding(mEncoding).build();
    }
    public int getBufferSizeInFrames() { return mStream != 0 ? HostAudio.bufferFrames(mStream) : 0; }
    public int getBufferCapacityInFrames() { return getBufferSizeInFrames(); }
    public int setBufferSizeInFrames(int bufferSizeInFrames) { return getBufferSizeInFrames(); }
    public int getUnderrunCount() { return mStream != 0 ? HostAudio.underruns(mStream) : 0; }
    public int getPlaybackHeadPosition() { return mStream != 0 ? (int) HostAudio.position(mStream) : 0; }
    public boolean getTimestamp(AudioTimestamp timestamp) {
        if (mStream == 0 || mPlayState != PLAYSTATE_PLAYING) return false;
        timestamp.framePosition = HostAudio.position(mStream);
        timestamp.nanoTime = System.nanoTime();
        return true;
    }
    public int setVolume(float gain) { return SUCCESS; }
    public int setStereoVolume(float leftGain, float rightGain) { return SUCCESS; }
    public int setAuxEffectSendLevel(float level) { return SUCCESS; }
    public int attachAuxEffect(int effectId) { return SUCCESS; }
    public int setNotificationMarkerPosition(int markerInFrames) { return SUCCESS; }
    public int setPositionNotificationPeriod(int periodInFrames) { return SUCCESS; }
    public int setPlaybackHeadPosition(int positionInFrames) { return SUCCESS; }
    public int setLoopPoints(int startInFrames, int endInFrames, int loopCount) { return SUCCESS; }
    public int reloadStaticData() { return SUCCESS; }
    public void setPlaybackPositionUpdateListener(OnPlaybackPositionUpdateListener listener) {}
    public void setPlaybackPositionUpdateListener(OnPlaybackPositionUpdateListener listener, Handler handler) {}
    public AudioDeviceInfo getRoutedDevice() { return AudioDeviceInfo.refractSpeaker(); }
    public AudioDeviceInfo getPreferredDevice() { return null; }
    public boolean setPreferredDevice(AudioDeviceInfo deviceInfo) { return true; }

    public void play() throws IllegalStateException {
        if (mStream == 0) throw new IllegalStateException("play() called on uninitialized AudioTrack.");
        if (HostAudio.start(mStream) != 0) return;
        mPlayState = PLAYSTATE_PLAYING;
        if (mMode == MODE_STATIC && mStaticBytes > 0) {
            final long stream = mStream;
            final byte[] data = mStaticData;
            final int bytes = mStaticBytes;
            Thread t = new Thread(() -> HostAudio.writeArray(stream, data, 0, bytes, true), "AudioTrack static");
            t.setDaemon(true);
            t.start();
        }
    }
    public void pause() throws IllegalStateException {
        if (mStream == 0) throw new IllegalStateException("pause() called on uninitialized AudioTrack.");
        mPlayState = PLAYSTATE_PAUSED;
        HostAudio.pause(mStream);
    }
    public void stop() throws IllegalStateException {
        if (mStream == 0) throw new IllegalStateException("stop() called on uninitialized AudioTrack.");
        mPlayState = PLAYSTATE_STOPPED;
        HostAudio.stop(mStream);
    }
    public void flush() {
        if (mStream != 0 && mPlayState != PLAYSTATE_PLAYING) HostAudio.flush(mStream);
    }
    public void release() {
        long s = mStream;
        mStream = 0;
        mPlayState = PLAYSTATE_STOPPED;
        if (s != 0) HostAudio.close(s);
    }

    private int result(int n) { return n >= 0 ? n : n == -899 ? ERROR_DEAD_OBJECT : ERROR_INVALID_OPERATION; }

    private int writeBytes(Object array, int byteOffset, int bytes, int mode) {
        if (mStream == 0) return ERROR_INVALID_OPERATION;
        if (array == null || byteOffset < 0 || bytes < 0) return ERROR_BAD_VALUE;
        if (mode != WRITE_BLOCKING && mode != WRITE_NON_BLOCKING) return ERROR_BAD_VALUE;
        if (mMode == MODE_STATIC) {
            int n = Math.min(bytes, mStaticData.length - mStaticBytes);
            if (array instanceof byte[] b) System.arraycopy(b, byteOffset, mStaticData, mStaticBytes, n);
            else if (array instanceof short[] s)
                ByteBuffer.wrap(mStaticData, mStaticBytes, n).order(java.nio.ByteOrder.LITTLE_ENDIAN).asShortBuffer()
                        .put(s, byteOffset / 2, n / 2);
            else if (array instanceof float[] f)
                ByteBuffer.wrap(mStaticData, mStaticBytes, n).order(java.nio.ByteOrder.LITTLE_ENDIAN).asFloatBuffer()
                        .put(f, byteOffset / 4, n / 4);
            mStaticBytes += n;
            return n;
        }
        return result(HostAudio.writeArray(mStream, array, byteOffset, bytes, mode == WRITE_BLOCKING));
    }

    public int write(byte[] audioData, int offsetInBytes, int sizeInBytes) {
        return write(audioData, offsetInBytes, sizeInBytes, WRITE_BLOCKING);
    }
    public int write(byte[] audioData, int offsetInBytes, int sizeInBytes, int writeMode) {
        if (audioData != null && offsetInBytes + sizeInBytes > audioData.length) return ERROR_BAD_VALUE;
        return writeBytes(audioData, offsetInBytes, sizeInBytes, writeMode);
    }
    public int write(short[] audioData, int offsetInShorts, int sizeInShorts) {
        return write(audioData, offsetInShorts, sizeInShorts, WRITE_BLOCKING);
    }
    public int write(short[] audioData, int offsetInShorts, int sizeInShorts, int writeMode) {
        if (audioData != null && offsetInShorts + sizeInShorts > audioData.length) return ERROR_BAD_VALUE;
        int n = writeBytes(audioData, offsetInShorts * 2, sizeInShorts * 2, writeMode);
        return n > 0 ? n / 2 : n;
    }
    public int write(float[] audioData, int offsetInFloats, int sizeInFloats, int writeMode) {
        if (audioData != null && offsetInFloats + sizeInFloats > audioData.length) return ERROR_BAD_VALUE;
        int n = writeBytes(audioData, offsetInFloats * 4, sizeInFloats * 4, writeMode);
        return n > 0 ? n / 4 : n;
    }
    /** Writes from the buffer's position and advances it, like Android. */
    public int write(ByteBuffer audioData, int sizeInBytes, int writeMode) {
        if (mStream == 0) return ERROR_INVALID_OPERATION;
        if (audioData == null || sizeInBytes < 0) return ERROR_BAD_VALUE;
        int size = Math.min(sizeInBytes, audioData.remaining());
        int n;
        if (audioData.isDirect() && mMode == MODE_STREAM)
            n = result(HostAudio.writeBuffer(mStream, audioData, audioData.position(), size, writeMode == WRITE_BLOCKING));
        else if (audioData.hasArray())
            n = writeBytes(audioData.array(), audioData.arrayOffset() + audioData.position(), size, writeMode);
        else {
            byte[] tmp = new byte[size];
            audioData.duplicate().get(tmp);
            n = writeBytes(tmp, 0, size, writeMode);
        }
        if (n > 0) audioData.position(audioData.position() + n);
        return n;
    }
    public int write(ByteBuffer audioData, int sizeInBytes, int writeMode, long timestamp) {
        return write(audioData, sizeInBytes, writeMode);
    }

    public interface OnPlaybackPositionUpdateListener {
        void onMarkerReached(AudioTrack track);
        void onPeriodicNotification(AudioTrack track);
    }

    public static class Builder {
        private AudioAttributes mAttributes;
        private AudioFormat mFormat;
        private int mBufferBytes, mMode = MODE_STREAM, mSession, mPerformanceMode;

        public Builder() {}
        public Builder setAudioAttributes(AudioAttributes attributes) { mAttributes = attributes; return this; }
        public Builder setAudioFormat(AudioFormat format) { mFormat = format; return this; }
        public Builder setBufferSizeInBytes(int bufferSizeInBytes) { mBufferBytes = bufferSizeInBytes; return this; }
        public Builder setTransferMode(int mode) { mMode = mode; return this; }
        public Builder setSessionId(int sessionId) { mSession = sessionId; return this; }
        public Builder setPerformanceMode(int performanceMode) { mPerformanceMode = performanceMode; return this; }
        public Builder setOffloadedPlayback(boolean offload) { return this; }
        public Builder setEncapsulationMode(int encapsulationMode) { return this; }
        public AudioTrack build() throws UnsupportedOperationException {
            AudioFormat f = mFormat != null ? mFormat : new AudioFormat.Builder().build();
            AudioTrack t = new AudioTrack(mAttributes, f, mBufferBytes, mMode, mSession);
            t.mPerformanceMode = mPerformanceMode;
            if (t.mStream == 0) throw new UnsupportedOperationException("Cannot create AudioTrack");
            return t;
        }
    }
}
