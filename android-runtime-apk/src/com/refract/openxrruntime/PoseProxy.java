package com.refract.openxrruntime;

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
    // adb reverse first (emulator and phones), then the emulator's host alias.
    private static final String[] HOSTS = {"127.0.0.1", "10.0.2.2"};

    private PoseProxy() {
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

    private static Socket connectHost() {
        for (String host : HOSTS) {
            try {
                Socket socket = new Socket();
                socket.connect(new InetSocketAddress(host, BRIDGE_PORT), 1000);
                socket.setTcpNoDelay(true);
                Log.i(TAG, "relaying pose stream from host=" + host + " through provider FD");
                return socket;
            } catch (IOException ex) {
                Log.i(TAG, "connect failed host=" + host + ": " + ex);
            }
        }
        return null;
    }

    private static void forward(ParcelFileDescriptor descriptor) {
        // Poses are read once per frame; a late one is a late head turn.
        android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_DISPLAY);
        final Socket host = connectHost();
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
