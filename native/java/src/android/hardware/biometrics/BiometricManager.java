package android.hardware.biometrics;

/** Quest headsets have no biometric hardware. */
public class BiometricManager {
    public static final int BIOMETRIC_SUCCESS = 0, BIOMETRIC_ERROR_HW_UNAVAILABLE = 1, BIOMETRIC_ERROR_NONE_ENROLLED = 11,
            BIOMETRIC_ERROR_NO_HARDWARE = 12, BIOMETRIC_ERROR_SECURITY_UPDATE_REQUIRED = 15;

    public BiometricManager() {}

    public int canAuthenticate() { return BIOMETRIC_ERROR_NO_HARDWARE; }
    public int canAuthenticate(int authenticators) { return BIOMETRIC_ERROR_NO_HARDWARE; }
}
