package android.net;

/** Connection state of a network (deprecated API, still widely used). */
public class NetworkInfo implements android.os.Parcelable {
    public enum State { CONNECTING, CONNECTED, SUSPENDED, DISCONNECTING, DISCONNECTED, UNKNOWN }

    public enum DetailedState {
        IDLE, SCANNING, CONNECTING, AUTHENTICATING, OBTAINING_IPADDR, CONNECTED, SUSPENDED, DISCONNECTING,
        DISCONNECTED, FAILED, BLOCKED, VERIFYING_POOR_LINK, CAPTIVE_PORTAL_CHECK
    }

    private final int mType, mSubtype;
    private final String mTypeName, mSubtypeName;
    private DetailedState mDetailedState = DetailedState.IDLE;
    private String mReason, mExtraInfo;

    public NetworkInfo(int type, int subtype, String typeName, String subtypeName) {
        mType = type;
        mSubtype = subtype;
        mTypeName = typeName;
        mSubtypeName = subtypeName;
    }

    /** A connected Wi-Fi network: the host network as apps see it. */
    public static NetworkInfo refractWifi() {
        NetworkInfo ni = new NetworkInfo(ConnectivityManager.TYPE_WIFI, 0, "WIFI", "");
        ni.setDetailedState(DetailedState.CONNECTED, null, "\"Refract\"");
        return ni;
    }

    public int getType() { return mType; }
    public int getSubtype() { return mSubtype; }
    public String getTypeName() { return mTypeName; }
    public String getSubtypeName() { return mSubtypeName; }
    public boolean isConnectedOrConnecting() { return getState() == State.CONNECTED || getState() == State.CONNECTING; }
    public boolean isConnected() { return getState() == State.CONNECTED; }
    public boolean isAvailable() { return true; }
    public boolean isFailover() { return false; }
    public boolean isRoaming() { return false; }
    public State getState() {
        return switch (mDetailedState) {
            case CONNECTED, VERIFYING_POOR_LINK, CAPTIVE_PORTAL_CHECK -> State.CONNECTED;
            case SCANNING, CONNECTING, AUTHENTICATING, OBTAINING_IPADDR -> State.CONNECTING;
            case SUSPENDED -> State.SUSPENDED;
            case DISCONNECTING -> State.DISCONNECTING;
            case IDLE, DISCONNECTED, FAILED, BLOCKED -> State.DISCONNECTED;
        };
    }
    public DetailedState getDetailedState() { return mDetailedState; }
    public void setDetailedState(DetailedState detailedState, String reason, String extraInfo) {
        mDetailedState = detailedState;
        mReason = reason;
        mExtraInfo = extraInfo;
    }
    public String getReason() { return mReason; }
    public String getExtraInfo() { return mExtraInfo; }
    public int describeContents() { return 0; }
    public void writeToParcel(android.os.Parcel dest, int flags) {}
    @Override public String toString() {
        return "[type: " + mTypeName + "[" + mSubtypeName + "], state: " + getState() + "/" + mDetailedState + "]";
    }
}
