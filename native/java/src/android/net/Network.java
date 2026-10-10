package android.net;

import java.io.IOException;
import java.net.InetAddress;
import java.net.Socket;
import java.net.URL;
import java.net.URLConnection;
import java.net.UnknownHostException;
import javax.net.SocketFactory;

/** The one network (the host's), reached with plain java.net. */
public class Network implements android.os.Parcelable {
    final int netId;

    public Network() { this(100); }
    Network(int netId) { this.netId = netId; }

    public InetAddress[] getAllByName(String host) throws UnknownHostException { return InetAddress.getAllByName(host); }
    public InetAddress getByName(String host) throws UnknownHostException { return InetAddress.getByName(host); }
    public SocketFactory getSocketFactory() { return SocketFactory.getDefault(); }
    public URLConnection openConnection(URL url) throws IOException { return url.openConnection(); }
    public URLConnection openConnection(URL url, java.net.Proxy proxy) throws IOException { return url.openConnection(proxy); }
    public void bindSocket(java.net.DatagramSocket socket) {}
    public void bindSocket(Socket socket) {}
    public void bindSocket(java.io.FileDescriptor fd) {}
    public long getNetworkHandle() { return ((long) netId << 32) | 0xfacade; }
    public int describeContents() { return 0; }
    public void writeToParcel(android.os.Parcel dest, int flags) {}
    @Override public boolean equals(Object o) { return o instanceof Network n && n.netId == netId; }
    @Override public int hashCode() { return netId * 11; }
    @Override public String toString() { return Integer.toString(netId); }
}
