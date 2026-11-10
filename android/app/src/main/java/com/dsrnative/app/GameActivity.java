package com.dsrnative.app;

import android.Manifest;
import android.app.NativeActivity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.graphics.Color;
import android.util.TypedValue;
import android.view.Gravity;
import android.widget.FrameLayout;
import android.widget.TextView;

/**
 * The native activity, full screen: the status and navigation bars are hidden while the game runs (immersive
 * mode: a swipe from the screen edge shows them for a moment), and the game may draw under a display cutout.
 */
public class GameActivity extends NativeActivity {
    // Access to Download/DSRNative (game files, saves, settings): 0 = waiting for the player's answer,
    // 1 = granted, 2 = refused (the game then falls back to the app's private folder). Read by native code.
    private static volatile int sStorage = 0;
    private boolean mAsked = false, mPausedSinceAsk = false;
    private TextView mFps;

    // Called from native code once a second with the frames presented in that second.
    public void setFps(final float fps) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (mFps != null) mFps.setText(String.format(java.util.Locale.US, "%.0f FPS", fps));
            }
        });
    }

    private void addFpsOverlay() {
        TextView t = new TextView(this);
        t.setTextColor(Color.WHITE);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        t.setBackgroundColor(0x88000000);
        t.setPadding(16, 8, 16, 8);
        t.setText("-- FPS");
        addContentView(t, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.END));
        mFps = t;
    }

    public static int storageAccess() {
        return sStorage;
    }

    private boolean hasStorageAccess() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) return Environment.isExternalStorageManager();
        return checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    private void requestStorageAccess() {
        if (hasStorageAccess()) {
            sStorage = 1;
            return;
        }
        mAsked = true;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:" + getPackageName())));
            } catch (Exception e) {
                try {
                    startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                } catch (Exception e2) {
                    sStorage = 2;
                }
            }
        } else {
            requestPermissions(new String[] {Manifest.permission.READ_EXTERNAL_STORAGE,
                    Manifest.permission.WRITE_EXTERNAL_STORAGE}, 1);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(requestCode, permissions, results);
        sStorage = hasStorageAccess() ? 1 : 2;
    }

    @Override
    protected void onPause() {
        super.onPause();
        if (mAsked) mPausedSinceAsk = true;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        requestStorageAccess();
        Window w = getWindow();
        w.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            WindowManager.LayoutParams lp = w.getAttributes();
            lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            w.setAttributes(lp);
        }
        hideSystemBars();
        addFpsOverlay();
    }

    @Override
    protected void onResume() {
        super.onResume();
        // Back from the "All files access" settings screen: the answer is final for this launch.
        if (mAsked && mPausedSinceAsk && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R)
            sStorage = hasStorageAccess() ? 1 : 2;
        hideSystemBars();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }

    @SuppressWarnings("deprecation")
    private void hideSystemBars() {
        Window w = getWindow();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            w.setDecorFitsSystemWindows(false);
            WindowInsetsController c = w.getInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            w.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        }
    }
}
