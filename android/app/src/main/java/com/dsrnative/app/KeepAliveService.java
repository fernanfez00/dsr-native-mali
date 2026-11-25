package com.dsrnative.app;

import android.Manifest;
import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.os.Process;

/**
 * Settings "Keep running in the background" (settings.txt keep_alive, on by default): while the game runs, a
 * foreground service shows a "DSR Native is running" notification. An app in the background with no such service is
 * a cached process, and one holding 1-2 GB like the game is among the first Android closes when it needs memory
 * (the game then quietly restarts from the title screen when the player comes back). A foreground service raises the
 * process's priority so Android leaves it alone far more often. The notification only informs; the game itself is
 * still paused while it is in the background.
 */
public class KeepAliveService extends Service {
    private static final String CHANNEL = "dsr_running";
    private static final int NOTIFICATION_ID = 1;
    static final int REQ_NOTIFICATIONS = 4110;

    /** Starts or stops the service (from any thread). */
    static void apply(Activity activity, boolean on) {
        Context c = activity.getApplicationContext();
        Intent i = new Intent(c, KeepAliveService.class);
        try {
            if (on) {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
                    c.startForegroundService(i);
                else
                    c.startService(i);
            } else {
                c.stopService(i);
            }
        } catch (Exception e) {
            android.util.Log.w("DSRNative", "keep_alive: could not " + (on ? "start" : "stop") + " the service", e);
        }
    }

    /**
     * Android 13+: notifications need the player's permission, and without it the notification (and with it, on
     * some phones, the protection) is missing. Asked at each game start while the setting is on and the permission
     * is missing, and when the player turns the setting on. Android itself stops showing the request after the
     * player has refused it twice (the setting's note then applies: allow it in the app's system settings).
     */
    static void askNotificationPermission(Activity activity) {
        if (Build.VERSION.SDK_INT < 33) return;
        if (activity.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED)
            return;
        activity.runOnUiThread(() -> {
            try {
                activity.requestPermissions(new String[] {Manifest.permission.POST_NOTIFICATIONS}, REQ_NOTIFICATIONS);
            } catch (Exception ignored) {
            }
        });
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        Notification.Builder b;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel ch = new NotificationChannel(CHANNEL, Strings.t("keep_channel"),
                    NotificationManager.IMPORTANCE_LOW);
            ch.setShowBadge(false);
            if (nm != null) nm.createNotificationChannel(ch);
            b = new Notification.Builder(this, CHANNEL);
        } else {
            b = new Notification.Builder(this);
        }
        Intent open = new Intent(this, GameActivity.class);
        open.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_REORDER_TO_FRONT);
        PendingIntent pi = PendingIntent.getActivity(this, 0, open,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        Notification n = b.setSmallIcon(R.drawable.ic_notify)
                .setContentTitle(Strings.t("keep_running"))
                .setContentText(Strings.t("keep_text"))
                .setContentIntent(pi)
                .setOngoing(true)
                .setShowWhen(false)
                .build();
        try {
            if (Build.VERSION.SDK_INT >= 34)
                startForeground(NOTIFICATION_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
            else
                startForeground(NOTIFICATION_ID, n);
        } catch (Exception e) {
            android.util.Log.w("DSRNative", "keep_alive: startForeground failed", e);
            stopSelf();
        }
        return START_NOT_STICKY;  // not restarted on its own: it only means something while the game runs
    }

    /**
     * Swiped away from the recent apps: without the service Android ends the process; with it the game would stay
     * alive with no screen, so the process ends here as it would have.
     */
    @Override
    public void onTaskRemoved(Intent rootIntent) {
        super.onTaskRemoved(rootIntent);
        stopSelf();
        Process.killProcess(Process.myPid());
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
