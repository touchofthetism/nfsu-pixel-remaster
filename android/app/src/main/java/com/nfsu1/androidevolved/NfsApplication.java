package com.nfsu1.androidevolved;

import android.app.Application;
import android.os.Build;
import android.system.ErrnoException;
import android.system.Os;

/**
 * Process-wide setup that must happen before the game library loads. DXVK reads DXVK_LOG_LEVEL when
 * libnfsu1_android.so is loaded (GameActivity's static initializer), so the launcher option can only
 * reach it through the environment of the fresh :game process, set here.
 */
public final class NfsApplication extends Application {
    @Override public void onCreate() {
        super.onCreate();
        String process = Build.VERSION.SDK_INT >= 28 ? getProcessName() : "";
        if (process == null || !process.endsWith(":game")) return;
        String level = GameOptions.get(this, GameOptions.GFXLOG).equals("debug") ? "debug" : "info";
        try { Os.setenv("DXVK_LOG_LEVEL", level, true); }
        catch (ErrnoException error) { android.util.Log.w("NFSU1", "DXVK_LOG_LEVEL not set: " + error); }
    }
}
