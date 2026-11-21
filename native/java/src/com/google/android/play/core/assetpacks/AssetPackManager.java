package com.google.android.play.core.assetpacks;

import android.app.Activity;
import com.google.android.gms.tasks.Task;
import java.util.List;
import java.util.Map;

public interface AssetPackManager {
    Task<AssetPackStates> fetch(List<String> packNames);
    AssetPackStates cancel(List<String> packNames);
    void registerListener(AssetPackStateUpdateListener listener);
    void unregisterListener(AssetPackStateUpdateListener listener);
    Task<AssetPackStates> getPackStates(List<String> packNames);
    AssetPackLocation getPackLocation(String packName);
    Map<String, AssetPackLocation> getPackLocations();
    Task<Void> removePack(String packName);
    Task<Integer> showCellularDataConfirmation(Activity activity);
    void clearListeners();
}
