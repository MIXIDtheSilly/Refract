package android.content.pm;

import android.os.Parcel;
import android.os.Parcelable;

public final class InstallSourceInfo implements Parcelable {
    private final String mInitiatingPackageName;
    private final String mOriginatingPackageName;
    private final String mInstallingPackageName;

    InstallSourceInfo() { this(null, null, null); }

    /** For refract.app.PackageManagerImpl. */
    public InstallSourceInfo(String initiatingPackageName, String originatingPackageName, String installingPackageName) {
        mInitiatingPackageName = initiatingPackageName;
        mOriginatingPackageName = originatingPackageName;
        mInstallingPackageName = installingPackageName;
    }

    public int describeContents() { return 0; }
    public void writeToParcel(Parcel dest, int flags) {}
    public String getInitiatingPackageName() { return mInitiatingPackageName; }
    public SigningInfo getInitiatingPackageSigningInfo() { return null; }
    public String getOriginatingPackageName() { return mOriginatingPackageName; }
    public String getInstallingPackageName() { return mInstallingPackageName; }
    public String getUpdateOwnerPackageName() { return null; }
    public int getPackageSource() { return PackageInstaller.PACKAGE_SOURCE_STORE; }
}
