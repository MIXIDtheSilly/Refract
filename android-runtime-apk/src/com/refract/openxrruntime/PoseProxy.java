package com.refract.openxrruntime;

import android.os.Build;
import android.os.ParcelFileDescriptor;
import android.util.Log;

import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;

// Relays the host pose stream (TCP 38490) to an app that may not open sockets itself (no INTERNET
// permission, e.g. AC Nexus). The runtime in the app reads the provider FD like its own TCP socket.
final class PoseProxy {
    private static final String TAG = "Refract.PoseProxy";
    private static final int BRIDGE_PORT = 38490;
    private static final boolean EMULATOR = "ranchu".equals(Build.HARDWARE) || "goldfish".equals(Build.HARDWARE);
    // The emulator reaches the PC directly at its host alias, as the runtime's own pose client does; through
    // adb reverse, poses arrived in bursts every ~45 ms, so frame-synced games (SteamVR) waited between them
    // (AC Nexus: ~67 fps of 90). No adb fallback there: adb reverse accepts even before the bridge listens, so
    // a relay started early stayed on it for the whole session. Phones only have adb reverse.
    static final String[] HOSTS = EMULATOR ? new String[] {"10.0.2.2"} : new String[] {"127.0.0.1", "10.0.2.2"};
    // A refused connection through the emulator's NAT takes ~2 s (Windows retries refused loopback connects).
    private static final int CONNECT_TIMEOUT_MS = EMULATOR ? 3000 : 1000;

    private PoseProxy() {
    }

    // The bridge at the first of HOSTS that accepts, with Nagle off; null if none does.
    static Socket connectBridge(int port, String tag, String what) {
        for (String host : HOSTS) {
            try {
                Socket socket = new Socket();
                socket.connect(new InetSocketAddress(host, port), CONNECT_TIMEOUT_MS);
                socket.setTcpNoDelay(true);
                Log.i(tag, "relaying " + what + " from host=" + host + " through provider FD");
                return socket;
            } catch (IOException ex) {
                Log.i(tag, "connect failed host=" + host + ": " + ex);
            }
        }
        return null;
    }

    static void relayFileDescriptor(final ParcelFileDescriptor descriptor) {
        Thread thread = new Thread(new Runnable() {
            @Override
            public void run() {
                forward(descriptor);
            }
        }, "Refract-PoseFdRelay");
        thread.setDaemon(true);
        thread.start();
    }

    private static void forward(ParcelFileDescriptor descriptor) {
        // Poses are read once per frame; a late one is a late head turn.
        android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_DISPLAY);
        final Socket host = connectBridge(BRIDGE_PORT, TAG, "pose stream");
        try (ParcelFileDescriptor app = descriptor) {
            if (host == null) return;  // Closing the FD tells the runtime to retry later.
            try (Socket hostSocket = host) {
                Thread upstream = new Thread(new Runnable() {
                    @Override
                    public void run() {
                        try {
                            copy(new FileInputStream(app.getFileDescriptor()), hostSocket.getOutputStream());
                        } catch (IOException ignored) {
                        } finally {
                            try { hostSocket.close(); } catch (IOException ignored) {}
                        }
                    }
                }, "Refract-PoseFdUpstream");
                upstream.setDaemon(true);
                upstream.start();
                copy(hostSocket.getInputStream(), new FileOutputStream(app.getFileDescriptor()));
            }
        } catch (IOException ex) {
            Log.i(TAG, "pose FD relay ended: " + ex);
        }
    }

    private static void copy(InputStream source, OutputStream destination) throws IOException {
        byte[] buffer = new byte[16 * 1024];
        int size;
        while ((size = source.read(buffer)) != -1) {
            destination.write(buffer, 0, size);
        }
    }
}
