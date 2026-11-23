package com.dsrnative.app;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Process;

/**
 * Closes the game and opens it again, so settings that are read when the game starts (render scale, aspect ratio,
 * shadows, effects ...) take effect without the player closing the app by hand.
 *
 * It runs in its own process (":restart", see the manifest): the game's process is ended from here, and this
 * activity then starts the game again, so the new run is a fresh process with a fresh native runtime.
 */
public final class RestartActivity extends Activity {
    private static final String EXTRA_PID = "restart_main_pid";
    private static final String EXTRA_INTENT = "restart_launch_intent";

    /** Starts the restart; false if the app's launch intent is not known (nothing was closed). */
    static boolean restart(Activity a) {
        Intent launch = a.getPackageManager().getLaunchIntentForPackage(a.getPackageName());
        if (launch == null) return false;
        launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        Intent i = new Intent(a, RestartActivity.class);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        i.putExtra(EXTRA_PID, Process.myPid());
        i.putExtra(EXTRA_INTENT, launch);
        a.startActivity(i);
        a.finish();
        Runtime.getRuntime().exit(0);  // the game's process ends; RestartActivity (other process) starts the game
        return true;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Intent in = getIntent();
        int pid = in.getIntExtra(EXTRA_PID, -1);
        if (pid > 0 && pid != Process.myPid()) Process.killProcess(pid);  // no-op if it already exited
        Intent launch = in.getParcelableExtra(EXTRA_INTENT);
        if (launch != null) startActivity(launch);
        finish();
        Runtime.getRuntime().exit(0);
    }
}
