package android.media;

public class Spatializer {
    public static final int SPATIALIZER_IMMERSIVE_LEVEL_NONE = 0;

    Spatializer() {}

    public int getImmersiveAudioLevel() { return SPATIALIZER_IMMERSIVE_LEVEL_NONE; }
    public boolean isAvailable() { return false; }
    public boolean isEnabled() { return false; }
}
