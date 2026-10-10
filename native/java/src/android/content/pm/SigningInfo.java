package android.content.pm;

import android.os.Parcel;
import android.os.Parcelable;

/** The current signers only (no key rotation history). */
public final class SigningInfo implements Parcelable {
    private final Signature[] mSigners;

    public SigningInfo() { mSigners = new Signature[0]; }
    public SigningInfo(SigningInfo orig) { mSigners = orig.mSigners.clone(); }
    /** For refract.app.PackageManagerImpl. */
    public SigningInfo(Signature[] signers) { mSigners = signers.clone(); }

    public boolean hasMultipleSigners() { return mSigners.length > 1; }
    public boolean hasPastSigningCertificates() { return false; }
    public Signature[] getSigningCertificateHistory() { return hasMultipleSigners() ? null : mSigners.clone(); }
    public Signature[] getApkContentsSigners() { return mSigners.clone(); }
    public int describeContents() { return 0; }
    public void writeToParcel(Parcel dest, int flags) {}
}
