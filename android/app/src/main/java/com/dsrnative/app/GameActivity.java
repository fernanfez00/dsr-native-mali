package com.dsrnative.app;

import android.Manifest;
import android.app.AlertDialog;
import android.app.Dialog;
import android.app.NativeActivity;
import android.content.ClipData;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.os.StatFs;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * The native activity, full screen: the status and navigation bars are hidden while the game runs (immersive
 * mode: a swipe from the screen edge shows them for a moment), and the game may draw under a display cutout.
 */
public class GameActivity extends NativeActivity {
    // Access to Download/DSRNative (game files, saves, settings): 0 = waiting for the player's answer,
    // 1 = granted, 2 = refused (the game then falls back to the app's private folder). Read by native code.
    private static volatile int sStorage = 0;
    private boolean mAsked = false, mPausedSinceAsk = false;

    public static int storageAccess() {
        return sStorage;
    }

    // ---- game folder chosen by the player (when the game is not in Download/DSRNative/game) -----------------
    static final int REQ_GAME_FOLDER = 4103;
    // 0 = not asked, 1 = the player is choosing (dialog or picker up), 2 = chosen (takeGameFolder). Read by native.
    private static volatile int sFolderState = 0;
    private static volatile String sFolder = null;

    public static int gameFolderState() {
        return sFolderState;
    }

    /** The folder the player chose (holding exefs/main), once; null if none yet. */
    public static String takeGameFolder() {
        if (sFolderState != 2) return null;
        String f = sFolder;
        sFolder = null;
        sFolderState = 0;
        return f;
    }

    /** Called by native code when the game files are not found, or found incomplete. */
    public void askGameFolder(final String title, final String message) {
        sFolderState = 1;
        runOnUiThread(() -> showFolderDialog(title, message));
    }

    private void showFolderDialog(String title, String message) {
        AlertDialog d = new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(title)
                .setMessage(message)
                .setCancelable(false)
                .setPositiveButton("Choose folder", (dlg, which) -> pickGameFolder())
                .setNeutralButton("Select nsp/xci", (dlg, which) -> pickPackages())
                .setNegativeButton("Exit", (dlg, which) -> finishAndRemoveTask())
                .create();
        showImmersive(d);
    }

    // ---- installing the game from .nsp / .xci files ---------------------------------------------------------
    static final int REQ_GAME_PACKAGE = 4104, REQ_KEYS = 4105;
    private final List<Uri> mPackages = new ArrayList<>();
    private volatile boolean mCancel;

    /** Download/DSRNative (or the app's own folder without storage access), as native code chooses it. */
    private File appDir() {
        if (hasStorageAccess())
            return new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), "DSRNative");
        File f = getExternalFilesDir(null);
        return f != null ? f : getFilesDir();
    }

    private void pickPackages() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        try {
            startActivityForResult(i, REQ_GAME_PACKAGE);
        } catch (Exception e) {
            showFolderDialog("No file picker", "No app is available to choose files. Copy the extracted game to "
                    + "Download/DSRNative/game instead, then start the app again.");
        }
    }

    private File keysFile() {
        File d = appDir();
        for (File f : new File[] {new File(d, "prod.keys"), new File(d, "keys/prod.keys"), new File(d, "keys.txt")})
            if (f.isFile()) return f;
        return null;
    }

    private void askKeys(String why) {
        AlertDialog d = new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("prod.keys needed")
                .setMessage(why + "\n\nThe game's files are encrypted. To install them, the app needs the keys of "
                        + "your own Switch: a prod.keys file dumped from it (e.g. with Lockpick_RCM). It is copied to "
                        + appDir() + "/prod.keys and only used to unpack the game on this device.")
                .setCancelable(false)
                .setPositiveButton("Select prod.keys", (dlg, which) -> {
                    Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                    i.addCategory(Intent.CATEGORY_OPENABLE);
                    i.setType("*/*");
                    try {
                        startActivityForResult(i, REQ_KEYS);
                    } catch (Exception e) {
                        showFolderDialog("No file picker", "Copy prod.keys to " + appDir() + " instead, then try again.");
                    }
                })
                .setNegativeButton("Back", (dlg, which) -> showFolderDialog("Game files not found",
                        "Choose the folder that holds the game files, or select the game's .nsp / .xci file."))
                .create();
        showImmersive(d);
    }

    private String displayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null,
                null)) {
            if (c != null && c.moveToFirst() && c.getString(0) != null) return c.getString(0);
        } catch (Exception ignored) {
        }
        String n = uri.getLastPathSegment();
        return n == null ? "file" : n.substring(n.lastIndexOf('/') + 1);
    }

    private void importKeys(Uri uri) {
        File out = new File(appDir(), "prod.keys");
        out.getParentFile().mkdirs();
        try (InputStream in = getContentResolver().openInputStream(uri); OutputStream o = new FileOutputStream(out)) {
            if (in == null) throw new IOException("cannot open the file");
            byte[] buf = new byte[1 << 16];
            int n;
            long total = 0;
            while ((n = in.read(buf)) > 0) {
                o.write(buf, 0, n);
                if ((total += n) > (1 << 20)) throw new IOException("too large for a key file");
            }
        } catch (IOException e) {
            out.delete();
            askKeys("The file could not be copied: " + e.getMessage());
            return;
        }
        try (FileReader r = new FileReader(out)) {
            if (!SwitchPackage.Keys.parse(r).map.containsKey("header_key")) {
                out.delete();
                askKeys(displayName(uri) + " is not a prod.keys file (it has no header_key).");
                return;
            }
        } catch (IOException e) {
            askKeys("The file could not be read: " + e.getMessage());
            return;
        }
        startInstall();
    }

    /** Unpacks the chosen packages into the game folder on a background thread, with a progress dialog. */
    private void startInstall() {
        final File keyFile = keysFile();
        if (keyFile == null) {
            askKeys("No prod.keys was found in " + appDir() + ".");
            return;
        }
        final File dest = new File(appDir(), "game");
        final List<Uri> uris = new ArrayList<>(mPackages);
        final float dp = getResources().getDisplayMetrics().density;
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        int p = Math.round(20 * dp);
        box.setPadding(p, p / 2, p, 0);
        final TextView label = new TextView(this);
        label.setText("Reading the game files…");
        final ProgressBar bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        bar.setIndeterminate(true);
        box.addView(label);
        box.addView(bar);
        final TextView file = new TextView(this);
        file.setGravity(Gravity.START);
        file.setAlpha(0.7f);
        box.addView(file);
        mCancel = false;
        final AlertDialog dlg = new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Installing Dark Souls Remastered")
                .setView(box)
                .setCancelable(false)
                .setNegativeButton("Cancel", null)
                .create();
        showImmersive(dlg);
        dlg.getButton(AlertDialog.BUTTON_NEGATIVE).setOnClickListener(v -> {
            mCancel = true;
            label.setText("Cancelling…");
        });
        new Thread(() -> {
            List<SwitchPackage.FileSource> sources = new ArrayList<>();
            String error = null, errorTitle = null;
            SwitchPackage.Result res = null;
            try {
                for (Uri u : uris) {
                    ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(u, "r");
                    if (pfd == null) throw new IOException("cannot open " + displayName(u));
                    FileInputStream in = new FileInputStream(pfd.getFileDescriptor());
                    sources.add(new SwitchPackage.FileSource(displayName(u), in.getChannel(), () -> {
                        in.close();
                        pfd.close();
                    }));
                }
                SwitchPackage.Keys keys;
                try (FileReader r = new FileReader(keyFile)) {
                    keys = SwitchPackage.Keys.parse(r);
                }
                if (!dest.isDirectory()) dest.mkdirs();
                long free = new StatFs(dest.getPath()).getAvailableBytes();
                final long[] last = {0};
                res = SwitchPackage.install(sources, keys, dest, new SwitchPackage.Progress() {
                    @Override
                    public void update(long done, long total, String name) {
                        long now = android.os.SystemClock.uptimeMillis();
                        if (now - last[0] < 200 && done < total) return;
                        last[0] = now;
                        final int permille = total > 0 ? (int) (done * 1000 / total) : 0;
                        final String text = String.format(Locale.ROOT, "Unpacking: %.2f of %.2f GB", done / 1e9,
                                total / 1e9);
                        runOnUiThread(() -> {
                            if (mCancel) return;
                            bar.setIndeterminate(false);
                            bar.setProgress(permille);
                            label.setText(text);
                            file.setText(name);
                        });
                    }

                    @Override
                    public boolean cancelled() {
                        return mCancel;
                    }
                }, free);
            } catch (SwitchPackage.InstallException e) {
                errorTitle = e.title;
                error = e.getMessage();
            } catch (Exception e) {
                android.util.Log.w("DSRNative", "install from package failed", e);
                errorTitle = "Installation failed";
                error = "The game could not be unpacked: " + e.getMessage();
            } finally {
                for (SwitchPackage.FileSource s : sources) {
                    try {
                        s.close();
                    } catch (IOException ignored) {
                    }
                }
            }
            final String fe = error, ft = errorTitle;
            final SwitchPackage.Result fr = res;
            runOnUiThread(() -> {
                dlg.dismiss();
                if (fr == null) {
                    if ("Cancelled".equals(ft))
                        showFolderDialog("Game files not found", "The installation was cancelled. Choose the game "
                                + "folder, or select the .nsp / .xci file again.");
                    else if ("Wrong keys".equals(ft) || "Key missing".equals(ft))
                        askKeys(fe);
                    else
                        showFolderDialog(ft, fe);
                    return;
                }
                installed(dest, fr);
            });
        }, "dsr-install").start();
    }

    private void installed(File dest, SwitchPackage.Result r) {
        android.util.Log.i("DSRNative", "installed the game to " + dest + " (build " + r.buildId + ", update "
                + r.withUpdate + ", " + r.bytes + " bytes)");
        StringBuilder m = new StringBuilder("The game was unpacked to " + dest + (r.withUpdate ? " (with its update)." : ".")
                + " The .nsp / .xci files are no longer needed by the app.");
        if (r.updateIgnored)
            m.append("\n\nThe update was not used: this version of the app is made for the base game's version.");
        if (!r.supportedBuild)
            m.append("\n\nThis version of the game (build ").append(r.buildId.substring(0, 16)).append("…) is not "
                    + "the one this app is made for. It may not run correctly.");
        AlertDialog d = new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Game installed")
                .setMessage(m.toString())
                .setCancelable(false)
                .setPositiveButton("Start", (dlg, which) -> {
                    sFolder = dest.getPath();
                    sFolderState = 2;  // native code starts the game
                })
                .create();
        showImmersive(d);
    }

    private void pickGameFolder() {
        try {
            startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQ_GAME_FOLDER);
        } catch (Exception e) {
            showFolderDialog("Game files not found", "No file manager is available to choose a folder. Copy the "
                    + "complete game folder to Download/DSRNative/game instead, then start the app again.");
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (SettingsMenu.onActivityResult(this, requestCode, resultCode, data)) return;
        if (requestCode == REQ_GAME_PACKAGE) {
            mPackages.clear();
            if (resultCode == RESULT_OK && data != null) {
                ClipData clip = data.getClipData();
                if (clip != null)
                    for (int i = 0; i < clip.getItemCount(); i++) mPackages.add(clip.getItemAt(i).getUri());
                else if (data.getData() != null)
                    mPackages.add(data.getData());
            }
            if (mPackages.isEmpty())
                showFolderDialog("Game files not found", "No file was selected. Select the game's .nsp or .xci file "
                        + "(and its update, if you have one: select both at once), or choose the game folder.");
            else
                startInstall();
            return;
        }
        if (requestCode == REQ_KEYS) {
            Uri uri = resultCode == RESULT_OK && data != null ? data.getData() : null;
            if (uri == null)
                askKeys("No file was selected.");
            else
                importKeys(uri);
            return;
        }
        if (requestCode != REQ_GAME_FOLDER) return;
        Uri uri = resultCode == RESULT_OK && data != null ? data.getData() : null;
        if (uri == null) {
            showFolderDialog("Game files not found", "No folder was chosen. Choose the folder that holds the game "
                    + "files: the one that contains the exefs folder, dvdbnd0.bdt and dvdbnd1.bdt.");
            return;
        }
        String path = SettingsMenu.treeToPath(uri);
        String root = path == null ? null : SettingsMenu.findGameRoot(path);
        if (root == null) {
            String partial = path == null ? null : SettingsMenu.findPartialGameRoot(path);
            if (partial != null)
                showFolderDialog("Game files incomplete", partial + " is missing:\n"
                        + SettingsMenu.missingGameFiles(partial) + "\n\nThese files belong next to the exefs folder. "
                        + "Copy the complete game folder, choose a folder that holds all of it, or select the "
                        + "game's .nsp / .xci file.");
            else
                showFolderDialog("Game files not found", (path == null ? "That folder" : path)
                        + " does not hold the game files.\n\nChoose the folder that contains the exefs folder, "
                        + "dvdbnd0.bdt and dvdbnd1.bdt.");
            return;
        }
        sFolder = root;
        sFolderState = 2;  // native code picks it up when the activity resumes
    }

    // ---- dialogs over the full-screen game ------------------------------------------------------------------
    /** Shows a dialog without bringing back the status and navigation bars. */
    static void showImmersive(Dialog d) {
        Window w = d.getWindow();
        if (w == null) {
            d.show();
            return;
        }
        // Not focusable while it appears (the bars would show), then focusable for touch and controller input.
        w.addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE);
        d.show();
        hideBars(w, false);
        w.clearFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE);
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

    private void hideSystemBars() {
        hideBars(getWindow(), true);
    }

    @SuppressWarnings("deprecation")
    static void hideBars(Window w, boolean underBars) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            if (underBars) w.setDecorFitsSystemWindows(false);
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
