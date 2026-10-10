package android.net;

/** Capabilities of the host network: Wi-Fi with validated, unmetered internet. */
public final class NetworkCapabilities implements android.os.Parcelable {
    public static final int TRANSPORT_CELLULAR = 0, TRANSPORT_WIFI = 1;
    public static final int NET_CAPABILITY_NOT_METERED = 11, NET_CAPABILITY_INTERNET = 12,
            NET_CAPABILITY_NOT_RESTRICTED = 13, NET_CAPABILITY_TRUSTED = 14, NET_CAPABILITY_NOT_VPN = 15,
            NET_CAPABILITY_VALIDATED = 16, NET_CAPABILITY_NOT_ROAMING = 18, NET_CAPABILITY_FOREGROUND = 19,
            NET_CAPABILITY_NOT_CONGESTED = 20, NET_CAPABILITY_NOT_SUSPENDED = 21;

    private long mTransports;
    private long mCapabilities;

    public NetworkCapabilities() {}
    public NetworkCapabilities(NetworkCapabilities nc) {
        if (nc != null) {
            mTransports = nc.mTransports;
            mCapabilities = nc.mCapabilities;
        }
    }

    /** The host network as Android reports a connected Wi-Fi network. */
    public static NetworkCapabilities refractWifi() {
        NetworkCapabilities nc = new NetworkCapabilities();
        nc.mTransports = 1L << TRANSPORT_WIFI;
        for (int c : new int[] {NET_CAPABILITY_NOT_METERED, NET_CAPABILITY_INTERNET, NET_CAPABILITY_NOT_RESTRICTED,
                NET_CAPABILITY_TRUSTED, NET_CAPABILITY_NOT_VPN, NET_CAPABILITY_VALIDATED, NET_CAPABILITY_NOT_ROAMING,
                NET_CAPABILITY_FOREGROUND, NET_CAPABILITY_NOT_CONGESTED, NET_CAPABILITY_NOT_SUSPENDED})
            nc.mCapabilities |= 1L << c;
        return nc;
    }

    public boolean hasTransport(int transportType) { return transportType >= 0 && (mTransports & (1L << transportType)) != 0; }
    public boolean hasCapability(int capability) { return capability >= 0 && (mCapabilities & (1L << capability)) != 0; }
    public int[] getCapabilities() { return bits(mCapabilities); }
    public int[] getTransportTypes() { return bits(mTransports); }
    public int[] getEnterpriseIds() { return new int[0]; }
    public boolean hasEnterpriseId(int enterpriseId) { return false; }
    public int getLinkUpstreamBandwidthKbps() { return 100_000; }
    public int getLinkDownstreamBandwidthKbps() { return 100_000; }
    public int getSignalStrength() { return -50; }
    public int getOwnerUid() { return -1; }
    public NetworkSpecifier getNetworkSpecifier() { return null; }
    public TransportInfo getTransportInfo() { return null; }
    public int describeContents() { return 0; }
    public void writeToParcel(android.os.Parcel dest, int flags) {}

    private static int[] bits(long mask) {
        int[] out = new int[Long.bitCount(mask)];
        for (int i = 0, n = 0; i < 64; ++i)
            if ((mask & (1L << i)) != 0) out[n++] = i;
        return out;
    }

    @Override public boolean equals(Object o) {
        return o instanceof NetworkCapabilities n && n.mTransports == mTransports && n.mCapabilities == mCapabilities;
    }
    @Override public int hashCode() { return Long.hashCode(mTransports) * 31 + Long.hashCode(mCapabilities); }
    @Override public String toString() {
        return "[ Transports: " + java.util.Arrays.toString(getTransportTypes()) + " Capabilities: "
                + java.util.Arrays.toString(getCapabilities()) + "]";
    }
}
