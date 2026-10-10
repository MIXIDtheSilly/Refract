package android.net;

import android.os.Handler;
import android.os.Looper;
import java.util.ArrayList;

/** One always-connected default network: the host network, reported as Wi-Fi. */
public class ConnectivityManager {
    public static final int TYPE_MOBILE = 0, TYPE_WIFI = 1;
    public static final int RESTRICT_BACKGROUND_STATUS_DISABLED = 1;
    public static final int MULTIPATH_PREFERENCE_HANDOVER = 1;

    private static final Network sNetwork = new Network();
    private final ArrayList<NetworkCallback> mCallbacks = new ArrayList<>();

    public ConnectivityManager() {}

    public NetworkInfo getActiveNetworkInfo() { return NetworkInfo.refractWifi(); }
    public Network getActiveNetwork() { return sNetwork; }
    public NetworkInfo getNetworkInfo(int networkType) { return networkType == TYPE_WIFI ? NetworkInfo.refractWifi() : null; }
    public NetworkInfo getNetworkInfo(Network network) { return sNetwork.equals(network) ? NetworkInfo.refractWifi() : null; }
    public NetworkInfo[] getAllNetworkInfo() { return new NetworkInfo[] {NetworkInfo.refractWifi()}; }
    public Network[] getAllNetworks() { return new Network[] {sNetwork}; }
    public LinkProperties getLinkProperties(Network network) { return sNetwork.equals(network) ? new LinkProperties() : null; }
    public NetworkCapabilities getNetworkCapabilities(Network network) {
        return sNetwork.equals(network) ? NetworkCapabilities.refractWifi() : null;
    }
    public boolean isActiveNetworkMetered() { return false; }
    public boolean isDefaultNetworkActive() { return true; }
    public boolean bindProcessToNetwork(Network network) { return true; }
    public static boolean setProcessDefaultNetwork(Network network) { return true; }
    public Network getBoundNetworkForProcess() { return null; }
    public static Network getProcessDefaultNetwork() { return null; }
    public int getRestrictBackgroundStatus() { return RESTRICT_BACKGROUND_STATUS_DISABLED; }
    public int getMultipathPreference(Network network) { return MULTIPATH_PREFERENCE_HANDOVER; }
    public boolean requestBandwidthUpdate(Network network) { return true; }
    public void reportNetworkConnectivity(Network network, boolean hasConnectivity) {}
    public void reportBadNetwork(Network network) {}

    public void registerDefaultNetworkCallback(NetworkCallback cb) { register(cb, null); }
    public void registerDefaultNetworkCallback(NetworkCallback cb, Handler handler) { register(cb, handler); }
    public void registerNetworkCallback(NetworkRequest request, NetworkCallback cb) { register(cb, null); }
    public void registerNetworkCallback(NetworkRequest request, NetworkCallback cb, Handler handler) { register(cb, handler); }
    public void registerBestMatchingNetworkCallback(NetworkRequest request, NetworkCallback cb, Handler handler) {
        register(cb, handler);
    }
    public void requestNetwork(NetworkRequest request, NetworkCallback cb) { register(cb, null); }
    public void requestNetwork(NetworkRequest request, NetworkCallback cb, Handler handler) { register(cb, handler); }
    public void requestNetwork(NetworkRequest request, NetworkCallback cb, int timeoutMs) { register(cb, null); }
    public void requestNetwork(NetworkRequest request, NetworkCallback cb, Handler handler, int timeoutMs) {
        register(cb, handler);
    }
    public void unregisterNetworkCallback(NetworkCallback cb) {
        synchronized (mCallbacks) {
            mCallbacks.remove(cb);
        }
    }

    /** As on Android, a callback hears about the available network right after registering. */
    private void register(NetworkCallback cb, Handler handler) {
        synchronized (mCallbacks) {
            mCallbacks.add(cb);
        }
        Handler h = handler != null ? handler : new Handler(Looper.getMainLooper());
        h.post(() -> {
            synchronized (mCallbacks) {
                if (!mCallbacks.contains(cb)) return;
            }
            cb.onAvailable(sNetwork);
            cb.onCapabilitiesChanged(sNetwork, NetworkCapabilities.refractWifi());
            cb.onLinkPropertiesChanged(sNetwork, new LinkProperties());
            cb.onBlockedStatusChanged(sNetwork, false);
        });
    }

    public static class NetworkCallback {
        public NetworkCallback() {}
        public NetworkCallback(int flags) {}
        public void onAvailable(Network network) {}
        public void onLosing(Network network, int maxMsToLive) {}
        public void onLost(Network network) {}
        public void onUnavailable() {}
        public void onCapabilitiesChanged(Network network, NetworkCapabilities networkCapabilities) {}
        public void onLinkPropertiesChanged(Network network, LinkProperties linkProperties) {}
        public void onBlockedStatusChanged(Network network, boolean blocked) {}
    }
}
