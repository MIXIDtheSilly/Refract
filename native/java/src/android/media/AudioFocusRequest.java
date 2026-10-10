package android.media;

import android.os.Handler;

/** Plain value class; focus is always granted (AudioManager). */
public final class AudioFocusRequest {
    private final int mFocusGain;
    private final AudioAttributes mAttributes;
    private final boolean mPauseWhenDucked, mAcceptsDelayedFocusGain;
    private final AudioManager.OnAudioFocusChangeListener mListener;

    private AudioFocusRequest(Builder b) {
        mFocusGain = b.mFocusGain;
        mAttributes = b.mAttributes != null ? b.mAttributes : new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).build();
        mPauseWhenDucked = b.mPauseWhenDucked;
        mAcceptsDelayedFocusGain = b.mAcceptsDelayedFocusGain;
        mListener = b.mListener;
    }

    public AudioAttributes getAudioAttributes() { return mAttributes; }
    public int getFocusGain() { return mFocusGain; }
    public boolean willPauseWhenDucked() { return mPauseWhenDucked; }
    public boolean acceptsDelayedFocusGain() { return mAcceptsDelayedFocusGain; }

    public static final class Builder {
        private int mFocusGain;
        private AudioAttributes mAttributes;
        private boolean mPauseWhenDucked, mAcceptsDelayedFocusGain;
        private AudioManager.OnAudioFocusChangeListener mListener;

        public Builder(int focusGain) { mFocusGain = focusGain; }
        public Builder(AudioFocusRequest r) {
            mFocusGain = r.mFocusGain;
            mAttributes = r.mAttributes;
            mPauseWhenDucked = r.mPauseWhenDucked;
            mAcceptsDelayedFocusGain = r.mAcceptsDelayedFocusGain;
            mListener = r.mListener;
        }
        public Builder setFocusGain(int focusGain) { mFocusGain = focusGain; return this; }
        public Builder setOnAudioFocusChangeListener(AudioManager.OnAudioFocusChangeListener listener) {
            mListener = listener;
            return this;
        }
        public Builder setOnAudioFocusChangeListener(AudioManager.OnAudioFocusChangeListener listener, Handler handler) {
            mListener = listener;
            return this;
        }
        public Builder setAudioAttributes(AudioAttributes attributes) { mAttributes = attributes; return this; }
        public Builder setWillPauseWhenDucked(boolean pauseOnDuck) { mPauseWhenDucked = pauseOnDuck; return this; }
        public Builder setAcceptsDelayedFocusGain(boolean acceptsDelayedFocusGain) {
            mAcceptsDelayedFocusGain = acceptsDelayedFocusGain;
            return this;
        }
        public Builder setForceDucking(boolean forceDucking) { return this; }
        public AudioFocusRequest build() { return new AudioFocusRequest(this); }
    }
}
