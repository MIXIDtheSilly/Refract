package com.google.android.play.core.assetpacks;

import android.content.Context;
import com.google.android.gms.tasks.OnCompleteListener;
import com.google.android.gms.tasks.OnFailureListener;
import com.google.android.gms.tasks.OnSuccessListener;
import com.google.android.gms.tasks.RuntimeExecutionException;
import com.google.android.gms.tasks.Task;
import java.io.File;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

public final class AssetPackManagerFactory {
    private static final int STATUS_NOT_INSTALLED = 8;
    private static final int ERROR_PACK_UNAVAILABLE = -2;
    private static final String OBB_DIR = "/storage/emulated/0/Android/obb/" + refract.Runtime.PACKAGE;
    private static AssetPackManager manager;

    private AssetPackManagerFactory() {}

    public static synchronized AssetPackManager getInstance(Context context) {
        if (manager == null) {
            Map<String, File> archives = findArchives();
            refract.Runtime.log(4, "refract.AssetPack", "Mounted asset packs: " + archives.keySet());
            manager = new AssetPackManager() {
                @Override public Task<AssetPackStates> fetch(List<String> packNames) {
                    if (containsUnityCorePack(packNames)) return unavailable("fetch", packNames);
                    AssetPackStates result = states(packNames, archives);
                    logStates("fetch(" + packNames + ")", result);
                    return completed(result);
                }
                @Override public AssetPackStates cancel(List<String> packNames) {
                    tracePackCall("cancel", packNames);
                    AssetPackStates result = states(packNames, archives);
                    logStates("cancel(" + packNames + ")", result);
                    return result;
                }
                @Override public void registerListener(AssetPackStateUpdateListener listener) {}
                @Override public void unregisterListener(AssetPackStateUpdateListener listener) {}
                @Override public Task<AssetPackStates> getPackStates(List<String> packNames) {
                    if (containsUnityCorePack(packNames)) return unavailable("getPackStates", packNames);
                    AssetPackStates result = states(packNames, archives);
                    logStates("getPackStates(" + packNames + ")", result);
                    return completed(result);
                }
                @Override public AssetPackLocation getPackLocation(String packName) {
                    if (isUnityCorePack(packName)) {
                        tracePackCall("getPackLocation", Collections.singletonList(packName));
                    }
                    AssetPackLocation location =
                            isUnityCorePack(packName) || findArchive(packName, archives) == null
                                    ? null : new InstalledLocation(OBB_DIR);
                    refract.Runtime.log(4, "refract.AssetPack",
                            "getPackLocation(" + packName + ") -> " +
                                    (location == null ? "null" : location.assetsPath()));
                    return location;
                }
                @Override public Map<String, AssetPackLocation> getPackLocations() {
                    tracePackCall("getPackLocations", null);
                    Map<String, AssetPackLocation> locations = new LinkedHashMap<>();
                    for (String name : archives.keySet()) {
                        if (!isUnityCorePack(name)) locations.put(name, new InstalledLocation(OBB_DIR));
                    }
                    refract.Runtime.log(4, "refract.AssetPack", "getPackLocations() -> " + locations.keySet());
                    return locations;
                }
                @Override public Task<Void> removePack(String packName) {
                    if (isUnityCorePack(packName)) {
                        tracePackCall("removePack", Collections.singletonList(packName));
                    }
                    return completed(null);
                }
                @Override public Task<Integer> showCellularDataConfirmation(android.app.Activity activity) { return completed(0); }
                @Override public void clearListeners() {}
            };
        }
        return manager;
    }

    private static <T> Task<T> completed(T result) {
        return new CompletedTask<>(result);
    }

    private static Task<AssetPackStates> unavailable(String operation, List<String> packNames) {
        tracePackCall(operation, packNames);
        AssetPackException error = new AssetPackException(
                "Unity asset packs are not included in this APK: " + packNames, ERROR_PACK_UNAVAILABLE);
        refract.Runtime.log(5, "refract.AssetPack",
                operation + "(" + packNames + ") -> PACK_UNAVAILABLE (" + ERROR_PACK_UNAVAILABLE + ")");
        return new FailedTask<>(error);
    }

    private static void tracePackCall(String operation, List<String> packNames) {
        if (packNames != null && !containsUnityCorePack(packNames)
                && !"getPackLocations".equals(operation)) return;

        StringBuilder trace = new StringBuilder("TRACE ").append(operation);
        if (packNames != null) trace.append(" packs=").append(packNames);
        StackTraceElement[] frames = Thread.currentThread().getStackTrace();
        int frameCount = 0;
        for (StackTraceElement frame : frames) {
            String className = frame.getClassName();
            if (className.equals(Thread.class.getName())
                    || className.startsWith(AssetPackManagerFactory.class.getName())) continue;
            trace.append(frameCount++ == 0 ? " caller=" : " <- ")
                    .append(className).append('.').append(frame.getMethodName())
                    .append(':').append(frame.getLineNumber());
            if (frameCount == 8) break;
        }
        refract.Runtime.log(4, "refract.AssetPack", trace.toString());
    }

    // The native installer uses one APK; Meta's HDOG archives are not Unity PAD split APKs.
    private static boolean containsUnityCorePack(List<String> packNames) {
        if (packNames == null) return false;
        for (String packName : packNames) if (isUnityCorePack(packName)) return true;
        return false;
    }

    private static boolean isUnityCorePack(String packName) {
        return "UnityDataAssetPack".equals(packName)
                || "UnityStreamingAssetsPack".equals(packName)
                || "UnityStreamingAssetPack".equals(packName)
                || "UnityTextureCompressionsAssetPack".equals(packName);
    }

    private static Map<String, File> findArchives() {
        String hostDir = refract.Runtime.hostPath(OBB_DIR);
        File[] files = new File(hostDir).listFiles();
        Map<String, File> archives = new LinkedHashMap<>();
        if (files == null) return archives;
        for (File file : files) {
            String filename = file.getName();
            if (!file.isFile() || !filename.toLowerCase(Locale.ROOT).endsWith(".hdog")) continue;
            String stem = filename.substring(0, filename.length() - ".hdog".length());
            int dot = stem.lastIndexOf('.');
            String packName = dot < 0 ? stem : stem.substring(dot + 1);
            if (!packName.isEmpty()) archives.putIfAbsent(packName, file);
        }
        return archives;
    }

    private static File findArchive(String packName, Map<String, File> archives) {
        if (packName == null) return null;
        File archive = archives.get(packName);
        if (archive != null) return archive;
        for (File candidate : archives.values()) {
            String filename = candidate.getName();
            String stem = filename.substring(0, filename.length() - ".hdog".length());
            if (stem.equals(packName) || stem.endsWith("." + packName) || filename.equals(packName)) return candidate;
        }
        return null;
    }

    private static AssetPackStates states(List<String> packNames, Map<String, File> archives) {
        Map<String, AssetPackState> states = new LinkedHashMap<>();
        long totalBytes = 0;
        if (packNames != null) {
            for (String packName : packNames) {
                if (states.containsKey(packName)) continue;
                File archive = findArchive(packName, archives);
                if (archive == null) {
                    states.put(packName, new NotInstalledState(packName));
                    continue;
                }
                long bytes = archive.length();
                states.put(packName, new InstalledState(packName, bytes));
                totalBytes += bytes;
            }
        }
        return new InstalledStates(states, totalBytes);
    }

    private static void logStates(String operation, AssetPackStates states) {
        StringBuilder details = new StringBuilder();
        for (AssetPackState state : states.packStates().values()) {
            if (details.length() > 0) details.append(", ");
            details.append(state.name()).append(" status=").append(state.status());
        }
        refract.Runtime.log(4, "refract.AssetPack",
                operation + "() -> [" + details + "], " + states.totalBytes() + " bytes");
    }

    private static final class InstalledLocation extends AssetPackLocation {
        private final String path;
        InstalledLocation(String path) { this.path = path; }
        @Override public int packStorageMethod() { return 0; }
        @Override public String path() { return path; }
        @Override public String assetsPath() { return path; }
    }

    private static final class InstalledState extends AssetPackState {
        private final String name;
        private final long bytes;
        InstalledState(String name, long bytes) { this.name = name; this.bytes = bytes; }
        @Override public String name() { return name; }
        @Override public int status() { return 4; }
        @Override public int errorCode() { return 0; }
        @Override public long bytesDownloaded() { return bytes; }
        @Override public long totalBytesToDownload() { return bytes; }
        @Override public int transferProgressPercentage() { return 100; }
    }

    private static final class NotInstalledState extends AssetPackState {
        private final String name;
        NotInstalledState(String name) { this.name = name; }
        @Override public String name() { return name; }
        @Override public int status() { return STATUS_NOT_INSTALLED; }
        @Override public int errorCode() { return 0; }
        @Override public long bytesDownloaded() { return 0; }
        @Override public long totalBytesToDownload() { return 0; }
        @Override public int transferProgressPercentage() { return 0; }
    }

    private static final class InstalledStates extends AssetPackStates {
        private final Map<String, AssetPackState> states;
        private final long totalBytes;
        InstalledStates(Map<String, AssetPackState> states, long totalBytes) {
            this.states = Collections.unmodifiableMap(states);
            this.totalBytes = totalBytes;
        }
        @Override public long totalBytes() { return totalBytes; }
        @Override public Map<String, AssetPackState> packStates() { return states; }
    }

    private static final class CompletedTask<T> extends Task<T> {
        private final T result;

        CompletedTask(T result) { this.result = result; }
        @Override public boolean isSuccessful() { return true; }
        @Override public T getResult() { return result; }
        @Override public Task<T> addOnSuccessListener(OnSuccessListener<? super T> listener) {
            listener.onSuccess(result);
            return this;
        }
        @Override public Task<T> addOnCompleteListener(OnCompleteListener<T> listener) {
            listener.onComplete(this);
            return this;
        }
    }

    private static final class FailedTask<T> extends Task<T> {
        private final Exception exception;

        FailedTask(Exception exception) { this.exception = exception; }
        @Override public boolean isSuccessful() { return false; }
        @Override public T getResult() { throw new RuntimeExecutionException(exception); }
        @Override public Exception getException() { return exception; }
        @Override public Task<T> addOnSuccessListener(OnSuccessListener<? super T> listener) { return this; }
        @Override public Task<T> addOnFailureListener(OnFailureListener listener) {
            listener.onFailure(exception);
            return this;
        }
        @Override public Task<T> addOnCompleteListener(OnCompleteListener<T> listener) {
            listener.onComplete(this);
            return this;
        }
    }
}
