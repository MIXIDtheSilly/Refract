package refract.media;

import android.media.AudioFormat;
import java.nio.ByteBuffer;

/** Host audio streams (native/src/audio_hle.cpp: WASAPI, shared with the guest's AAudio). */
public final class HostAudio {
    private HostAudio() {}

    /** AAudio sample formats. */
    public static final int FORMAT_I16 = 1, FORMAT_FLOAT = 2, FORMAT_I24 = 3, FORMAT_I32 = 4;

    /** A playback or recording stream on the default Windows device, or 0. */
    public static native long open(boolean capture, int sampleRate, int channels, int format, int bufferFrames);
    public static native int start(long stream);
    public static native void stop(long stream);
    public static native void pause(long stream);
    public static native void flush(long stream);
    public static native void close(long stream);
    public static native int bufferFrames(long stream);
    /** Frames played (playback) or delivered (recording) so far. */
    public static native long position(long stream);
    public static native int underruns(long stream);
    /** The playback device's mixing rate. */
    public static native int mixRate();
    // Byte counts; a negative result is an AAudio error.
    public static native int writeArray(long stream, Object array, int byteOffset, int bytes, boolean blocking);
    public static native int writeBuffer(long stream, ByteBuffer direct, int byteOffset, int bytes, boolean blocking);
    public static native int readArray(long stream, Object array, int byteOffset, int bytes, boolean blocking);
    public static native int readBuffer(long stream, ByteBuffer direct, int byteOffset, int bytes, boolean blocking);

    /** AAudio format for an AudioFormat encoding, or 0 if unsupported. */
    public static int format(int encoding) {
        switch (encoding) {
            case AudioFormat.ENCODING_DEFAULT:
            case AudioFormat.ENCODING_INVALID:
            case AudioFormat.ENCODING_PCM_16BIT: return FORMAT_I16;
            case AudioFormat.ENCODING_PCM_FLOAT: return FORMAT_FLOAT;
            case AudioFormat.ENCODING_PCM_24BIT_PACKED: return FORMAT_I24;
            case AudioFormat.ENCODING_PCM_32BIT: return FORMAT_I32;
            default: return 0;
        }
    }

    public static int sampleBytes(int encoding) {
        switch (format(encoding)) {
            case FORMAT_I16: return 2;
            case FORMAT_I24: return 3;
            case FORMAT_I32:
            case FORMAT_FLOAT: return 4;
            default: return 0;
        }
    }

    /** Channels of an AudioFormat.CHANNEL_IN_* mask (or a legacy CHANNEL_CONFIGURATION_*). */
    public static int inChannels(int mask) {
        if (mask == AudioFormat.CHANNEL_IN_DEFAULT || mask == AudioFormat.CHANNEL_CONFIGURATION_MONO
                || mask == AudioFormat.CHANNEL_INVALID) return 1;
        if (mask == AudioFormat.CHANNEL_CONFIGURATION_STEREO) return 2;
        return Math.max(1, Integer.bitCount(mask));
    }

    /** Channels of an AudioFormat.CHANNEL_OUT_* mask (or a legacy CHANNEL_CONFIGURATION_*). */
    public static int outChannels(int mask) {
        if (mask == AudioFormat.CHANNEL_OUT_DEFAULT || mask == AudioFormat.CHANNEL_CONFIGURATION_STEREO
                || mask == AudioFormat.CHANNEL_INVALID) return 2;
        if (mask == AudioFormat.CHANNEL_CONFIGURATION_MONO) return 1;
        return Math.max(1, Integer.bitCount(mask));
    }

    private static int sNextSession = 100;

    public static synchronized int newSession() {
        return ++sNextSession;
    }
}
