package com.dsrnative.app;

import android.app.Activity;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.LightingColorFilter;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.RectF;
import android.hardware.input.InputManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * On-screen controls drawn in a transparent panel window over the game. A fixed stick at the bottom left
 * moves, a smaller stick appears wherever the right side of the screen is touched (not on a button) and turns
 * the camera, and the Switch buttons sit around the edges.
 * They hide when a game controller is connected or used, and come back with a double tap on the screen.
 * State goes to native code (nativeState), which merges it with the controller.
 * The player can move and resize the controls (editLayout, from the settings menu): each group (face buttons, d-pad,
 * stick, each shoulder button, ...) keeps an offset from its default spot in layout units (a tenth of the screen's
 * short side) and a size factor, saved in settings.txt as `touch_layout = group:dx,dy[,size];...` (size 1 = default,
 * left out; layouts from before sizes existed read as size 1).
 */
public final class TouchControls extends View implements InputManager.InputDeviceListener {
    // nn::hid button bits (src/hle/input.h).
    static final int A = 1, B = 1 << 1, X = 1 << 2, Y = 1 << 3, L3 = 1 << 4, R3 = 1 << 5, L = 1 << 6, R = 1 << 7,
            ZL = 1 << 8, ZR = 1 << 9, PLUS = 1 << 10, MINUS = 1 << 11, LEFT = 1 << 12, UP = 1 << 13, RIGHT = 1 << 14,
            DOWN = 1 << 15;

    static native void nativeState(int buttons, float lx, float ly, float rx, float ry);
    static native void nativeVisible(boolean visible);
    static native void nativeLog(String text);  // a line for our log (diagnostics)

    private static TouchControls sView;
    /** settings.txt touch_camera_recenter: a thumb resting on the camera stick brings it back to the centre. */
    static volatile boolean sRecenter = true;
    /**
     * settings.txt touch_camera_mode = swipe: the right side of the screen turns the camera the way a mobile game
     * does, by how fast the thumb moves (no stick: the camera stops when the thumb stops). stick (default): the
     * floating camera stick.
     */
    static volatile boolean sSwipe = true;  // touch_camera_mode: swipe unless set to stick
    /**
     * settings.txt touch_draw = gpu: the controls' window is drawn by Android's OpenGL renderer again (GameActivity
     * itself is not hardware accelerated, see the manifest). Default cpu (an Adreno 630 with Turnip striped the
     * stick and lost its GPU context); a test of whether CPU drawing makes the swipe camera judder on other devices.
     */
    static volatile boolean sGpuDraw = false;
    /** Offsets of the groups from their default spots, in layout units (settings.txt touch_layout). */
    private static volatile Map<String, float[]> sLayout = new HashMap<>();
    /** Groups that can be moved, in settings.txt. */
    static final String[] GROUPS = {"face", "dpad", "stick", "zl", "l", "zr", "r", "minus", "plus", "l3", "lock"};

    /** Size factors a group can have (layout editor: pinch, or - / +). */
    static final float SIZE_MIN = 0.5f, SIZE_MAX = 2.0f, SIZE_STEP = 0.1f;

    /** settings.txt touch_layout -> {dx, dy, size} per group (unknown groups and bad numbers are skipped). */
    static Map<String, float[]> parseLayout(String s) {
        Map<String, float[]> m = new HashMap<>();
        if (s == null) return m;
        for (String part : s.split(";")) {
            int c = part.indexOf(':');
            if (c <= 0) continue;
            String g = part.substring(0, c).trim().toLowerCase(Locale.ROOT);
            boolean known = false;
            for (String x : GROUPS) known |= x.equals(g);
            if (!known) continue;
            String[] n = part.substring(c + 1).split(",");
            if (n.length < 2) continue;
            try {
                float dx = Float.parseFloat(n[0].trim());
                float dy = Float.parseFloat(n[1].trim());
                float size = n.length > 2 ? Float.parseFloat(n[2].trim()) : 1f;
                if (Float.isFinite(dx) && Float.isFinite(dy) && Math.abs(dx) < 50 && Math.abs(dy) < 50
                        && Float.isFinite(size))
                    m.put(g, new float[] {dx, dy, Math.max(SIZE_MIN, Math.min(SIZE_MAX, size))});
            } catch (NumberFormatException ignored) {
            }
        }
        return m;
    }

    static String formatLayout(Map<String, float[]> m) {
        StringBuilder b = new StringBuilder();
        for (String g : GROUPS) {
            float[] o = m.get(g);
            if (o == null) continue;
            float size = o.length > 2 ? o[2] : 1f;
            boolean moved = Math.abs(o[0]) >= 0.005f || Math.abs(o[1]) >= 0.005f, sized = Math.abs(size - 1f) >= 0.005f;
            if (!moved && !sized) continue;
            if (b.length() > 0) b.append(';');
            b.append(String.format(Locale.ROOT, "%s:%.2f,%.2f", g, o[0], o[1]));
            if (sized) b.append(String.format(Locale.ROOT, ",%.2f", size));
        }
        return b.toString();
    }

    /** The saved layout (settings.txt touch_layout): applied to the controls at once. */
    static void setLayout(String s) {
        sLayout = parseLayout(s);
        final TouchControls v = sView;
        if (v != null) v.post(v::relayout);
    }

    /** True while the player moves the controls. */
    static volatile boolean sEditing;

    // ---- skins (settings.txt touch_skin) ---------------------------------------------------------------------------
    // A skin is a set of PNG images drawn in place of the plain shapes: built in (assets/skins/<id>/) or the
    // player's own (Download/DSRNative/skins/<folder>/). Names: a b x y l r zl zr minus plus l3 lock dpad_up dpad_down
    // dpad_left dpad_right stick_base stick_nub ring (the camera stick); <name>_lit is used while pressed (without
    // it, the image is drawn brighter). Each image is fitted into its button, keeping its proportions; a missing
    // image falls back to the plain shape. The Dark Souls skin (by v39192503-ux, issue #33) is the default.
    static final String SKIN_DEFAULT = "dark_souls", SKIN_CLASSIC = "classic";
    static final String[] SKIN_IMAGES = {"a", "b", "x", "y", "l", "r", "zl", "zr", "minus", "plus", "l3", "lock",
            "dpad_up", "dpad_down", "dpad_left", "dpad_right", "stick_base", "stick_nub", "ring"};
    /** The images of the skin in use (name -> bitmap); empty: the plain shapes. */
    private static volatile Map<String, Bitmap> sSkin = new HashMap<>();
    private static String sSkinName = "";

    /** The player's skins folder (next to settings.txt). */
    static File skinsDir(String settingsPath) {
        File parent = new File(settingsPath == null ? "" : settingsPath).getParentFile();
        return new File(parent == null ? new File(".") : parent, "skins");
    }

    /** The player's own skins: folders of the skins folder with at least one known image. */
    static List<String> folderSkins(String settingsPath) {
        List<String> out = new ArrayList<>();
        File[] dirs = skinsDir(settingsPath).listFiles();
        if (dirs == null) return out;
        for (File d : dirs) {
            if (!d.isDirectory() || d.getName().equals(SKIN_DEFAULT) || d.getName().equals(SKIN_CLASSIC)) continue;
            for (String n : SKIN_IMAGES)
                if (new File(d, n + ".png").isFile()) {
                    out.add(d.getName());
                    break;
                }
        }
        java.util.Collections.sort(out, String.CASE_INSENSITIVE_ORDER);
        return out;
    }

    /** Creates the skins folder with a short README the first time (where to put a skin, and its file names). */
    static void prepareSkinsDir(String settingsPath) {
        try {
            File dir = skinsDir(settingsPath);
            File readme = new File(dir, "README.txt");
            if (readme.exists() || (!dir.isDirectory() && !dir.mkdirs())) return;
            StringBuilder b = new StringBuilder();
            b.append("Touch control skins. Make a folder here for each skin (its name is shown in the settings menu,\n")
                    .append("Touch tab, Skin) and put PNG images in it, named:\n\n");
            for (String n : SKIN_IMAGES) b.append("  ").append(n).append(".png\n");
            b.append("\nring.png is the camera stick, which appears where the right side of the screen is touched.\n")
                    .append("<name>_lit.png (for example a_lit.png) is shown while the button is pressed; without it the\n")
                    .append("image is drawn brighter. Images are fitted into their button keeping their proportions,\n")
                    .append("and any image left out is drawn as the plain button. Square images of 160 to 384 pixels\n")
                    .append("work well (larger ones are reduced).\n");
            try (FileOutputStream o = new FileOutputStream(readme)) {
                o.write(b.toString().getBytes("UTF-8"));
            }
        } catch (Exception ignored) {
        }
    }

    /** Loads a skin (on a worker thread) and redraws the controls with it. */
    static void setSkin(final Context context, final String name, final String settingsPath) {
        final String n = name == null || name.isEmpty() ? SKIN_DEFAULT : name;
        if (n.equals(sSkinName)) return;
        sSkinName = n;
        final Context app = context.getApplicationContext();
        new Thread(() -> {
            Map<String, Bitmap> m = new HashMap<>();
            if (!n.equals(SKIN_CLASSIC)) {
                File dir = n.equals(SKIN_DEFAULT) ? null : new File(skinsDir(settingsPath), n);
                for (String base : SKIN_IMAGES)
                    for (String img : new String[] {base, base + "_lit"}) {
                        Bitmap bm = loadSkinImage(app, n, dir, img);
                        if (bm != null) m.put(img, bm);
                    }
            }
            android.util.Log.i("DSRNative", "touch controls: skin " + n + " (" + m.size() + " images)");
            synchronized (TouchControls.class) {
                if (!n.equals(sSkinName)) return;  // another skin was picked meanwhile
                sSkin = m;
            }
            final TouchControls v = sView;
            if (v != null) v.postInvalidate();
        }, "dsr-skin").start();
    }

    /** One image of a skin, or null: built in (dir null) from the APK's assets, else from the folder. */
    private static Bitmap loadSkinImage(Context app, String skin, File dir, String img) {
        try {
            BitmapFactory.Options o = new BitmapFactory.Options();
            if (dir == null) {
                try (InputStream in = app.getAssets().open("skins/" + skin + "/" + img + ".png")) {
                    return BitmapFactory.decodeStream(in);
                }
            }
            File f = new File(dir, img + ".png");
            if (!f.isFile()) return null;
            o.inJustDecodeBounds = true;
            BitmapFactory.decodeFile(f.getPath(), o);
            o.inJustDecodeBounds = false;
            o.inSampleSize = 1;
            while (Math.max(o.outWidth, o.outHeight) / o.inSampleSize > 512) o.inSampleSize *= 2;  // memory bound
            return BitmapFactory.decodeFile(f.getPath(), o);
        } catch (Exception | OutOfMemoryError e) {
            return null;  // missing or unreadable: the plain shape
        }
    }

    /** The skin image name of a button. */
    private static String skinName(int bit) {
        switch (bit) {
            case A: return "a";
            case B: return "b";
            case X: return "x";
            case Y: return "y";
            case L: return "l";
            case R: return "r";
            case ZL: return "zl";
            case ZR: return "zr";
            case MINUS: return "minus";
            case PLUS: return "plus";
            case L3: return "l3";
            case R3: return "lock";
            case UP: return "dpad_up";
            case DOWN: return "dpad_down";
            case LEFT: return "dpad_left";
            case RIGHT: return "dpad_right";
            default: return "";
        }
    }

    /**
     * Lets the player move the controls (shown for it even when they are off or hidden). Save writes the layout to
     * settings.txt; `done` runs on the UI thread when the editor closes (saved or not).
     */
    static void editLayout(final Activity activity, final Runnable done) {
        activity.runOnUiThread(() -> {
            TouchControls v = sView;
            boolean temp = v == null;
            if (temp) v = new TouchControls(activity, 1f, 1f);
            sEditing = true;
            v.startEdit(temp, done);
            if (temp) v.addTo(activity, 0);
        });
    }

    /** Called by native code once the game starts. */
    public static void attach(final Activity activity, final float opacity, final float cameraSpeed,
            final boolean enabled) {
        activity.runOnUiThread(() -> {
            if (sView != null || !enabled || sEditing) return;
            final TouchControls v = new TouchControls(activity, opacity, cameraSpeed);
            v.addTo(activity, 0);
        });
    }

    /**
     * Settings menu: turns the controls on or off and changes their opacity and camera stick speed, at once.
     */
    public static void applySettings(final Activity activity, final boolean enabled, final float opacity,
            final float cameraSpeed) {
        activity.runOnUiThread(() -> {
            if (!enabled) {
                if (sView != null) {
                    TouchControls v = sView;
                    sView = null;
                    v.releaseAll();
                    nativeVisible(false);
                    InputManager im = (InputManager) activity.getSystemService(Context.INPUT_SERVICE);
                    if (im != null) im.unregisterInputDeviceListener(v);
                    try {
                        activity.getWindowManager().removeView(v);
                    } catch (Exception e) {
                        android.util.Log.w("DSRNative", "touch controls: could not remove the overlay", e);
                    }
                }
                return;
            }
            if (sView == null) {
                if (sEditing) return;
                final TouchControls v = new TouchControls(activity, opacity, cameraSpeed);
                v.mShown = true;  // turned on by the player: shown even with a controller connected
                v.addTo(activity, 0);
                return;
            }
            sView.mOpacity = Math.max(0.1f, Math.min(1.0f, opacity));
            sView.mCameraSpeed = Math.max(0.1f, Math.min(10f, cameraSpeed));
            sView.invalidate();
        });
    }

    /** The settings menu opened: everything held is let go (its releases go to the menu). */
    public static void releaseForMenu() {
        if (sView != null) {
            sView.releaseAll();
            sView.invalidate();
        }
    }

    /** Called by native code when a controller is used while the controls show. */
    public static void controllerUsed(final Activity activity) {
        activity.runOnUiThread(() -> {
            if (sView != null && !sView.mEditing) sView.setShown(false);
        });
    }

    private final Activity mActivity;
    private final float mDensity;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mShown = true;

    private static final int K_BUTTON = 0, K_STICK = 1, K_RSTICK = 2, K_NONE = 3;

    private static final class Button {
        final String label;
        final int bit;
        final boolean round;
        final String group;  // moved together in the layout editor
        float cx, cy, w, h;  // centre and size (px)
        int pressCount;
        Button(String label, int bit, boolean round, String group) {
            this.label = label;
            this.bit = bit;
            this.round = round;
            this.group = group;
        }
        boolean hit(float x, float y, float slop) {
            if (round) {
                float r = w / 2 + slop, dx = x - cx, dy = y - cy;
                return dx * dx + dy * dy <= r * r;
            }
            return Math.abs(x - cx) <= w / 2 + slop && Math.abs(y - cy) <= h / 2 + slop;
        }
    }

    private final List<Button> mButtons = new ArrayList<>();

    // Per finger (pointer id 0..15): what it does.
    private final int[] mKind = new int[16];
    private final Button[] mOn = new Button[16];
    private final float[] mLastX = new float[16], mLastY = new float[16];
    // Second fingers placed while others are down (ACTION_POINTER_DOWN) arrived at wrong places on an OPPO Pad Air
    // (Android 13): their position in that one event was the panel's own, not turned with the screen (finger on
    // the A button at 91%, 70% reported at 18%, 152%), while every later event had it right, so the second button
    // never registered. A new finger's first move is compared with where it came down: when it is far off and one
    // of the three turns of the screen puts the down where the move is, the finger is placed again where it really
    // is, and from then on that turn is applied to every such down at once (sPointerTurn: 1 = 90, 2 = 180, 3 = 270).
    private static int sPointerTurn = 0;
    private final boolean[] mFresh = new boolean[16];
    private final float[] mFreshX = new float[16], mFreshY = new float[16];
    private final long[] mFreshAt = new long[16];

    private float[] turned(int turn, float x, float y) {
        final float w = getWidth(), h = getHeight();
        switch (turn) {
            case 1: return new float[] {y, h - x};
            case 2: return new float[] {w - x, h - y};
            case 3: return new float[] {w - y, x};
            default: return new float[] {x, y};
        }
    }

    // A finger's first move (see sPointerTurn). True when it was placed again.
    private boolean checkFreshPointer(int id, float x, float y) {
        if (id < 0 || id >= 16 || !mFresh[id]) return false;
        mFresh[id] = false;
        if (android.os.SystemClock.uptimeMillis() - mFreshAt[id] > 250) return false;
        final float diag = (float) Math.hypot(getWidth(), getHeight());
        float[] placed = turned(sPointerTurn, mFreshX[id], mFreshY[id]);
        if (Math.hypot(placed[0] - x, placed[1] - y) < 0.12f * diag) return false;
        for (int turn = 1; turn <= 3; turn++) {
            float[] t = turned(turn, mFreshX[id], mFreshY[id]);
            if (Math.hypot(t[0] - x, t[1] - y) < 0.06f * diag) {
                if (turn != sPointerTurn)
                    touchLog("finger " + id + " came down at the wrong place (" + Math.round(100f * mFreshX[id] / getWidth())
                            + "%, " + Math.round(100f * mFreshY[id] / getHeight()) + "%; Android's position turned "
                            + (90 * turn) + " degrees): placed where it is, and such positions are turned from now on");
                sPointerTurn = turn;
                up(id);
                down(id, x, y);
                return true;
            }
        }
        return false;
    }

    // Stick: fixed in place; a finger that comes down on it (or near it) drives it.
    private int mStickPointer = -1;
    private float mStickBaseX, mStickBaseY, mStickX, mStickY, mStickRadius;
    // Camera stick: shown faintly only while touched; it appears under a finger that touches the right side and
    // its base follows a finger that goes past its edge. Smaller than the left stick; `camera speed` = deflection
    // per distance.
    private int mRStickPointer = -1;
    private float mRBaseX, mRBaseY, mRX, mRY, mRRadius;
    // A thumb resting on it: the base slides under the thumb, so the camera eases to a stop instead of turning
    // on and on. The slide fades in over the first moments of stillness (no jolt when it starts) and runs every
    // display frame (Choreographer), with a short time constant.
    private long mRLastMove;
    private long mRLastFrameNs;
    private static final float REST_START_S = 0.012f, REST_RAMP_S = 0.045f;  // stillness before / to full slide
    private static final float RECENTER_TIME_S = 0.06f;  // time constant of the slide at full strength
    private final android.view.Choreographer.FrameCallback mRecenter = new android.view.Choreographer.FrameCallback() {
        @Override
        public void doFrame(long frameNs) {
            if (mRStickPointer < 0) return;
            float dt = mRLastFrameNs == 0 ? 0.016f : Math.min(0.05f, (frameNs - mRLastFrameNs) / 1e9f);
            mRLastFrameNs = frameNs;
            float still = (android.os.SystemClock.uptimeMillis() - mRLastMove) / 1000f;
            float ramp = sRecenter ? Math.max(0f, Math.min(1f, (still - REST_START_S) / REST_RAMP_S)) : 0f;
            ramp = ramp * ramp * (3f - 2f * ramp);  // smoothstep
            if (ramp > 0f && (mRX != mRBaseX || mRY != mRBaseY)) {
                snapshot();
                float k = 1f - (float) Math.exp(-dt * ramp / RECENTER_TIME_S);
                mRBaseX += (mRX - mRBaseX) * k;
                mRBaseY += (mRY - mRBaseY) * k;
                float dx = mRX - mRBaseX, dy = mRY - mRBaseY;
                if (dx * dx + dy * dy < 0.25f) {
                    mRBaseX = mRX;
                    mRBaseY = mRY;
                }
                sendState();
                invalidateChanges();
            }
            android.view.Choreographer.getInstance().postFrameCallback(this);
        }
    };
    private float mCameraSpeed;

    // Swipe camera (sSwipe): the game only has a stick, so the thumb's speed becomes the stick's tilt. Movement is
    // gathered between display frames, turned into a speed (smoothed over a few frames: touch events and display
    // frames do not line up), and full tilt is reached at SWIPE_FULL_DP_S dp per second (divided by the camera
    // speed setting), through a curve that favours the lower half (SWIPE_CURVE). The game ignores small tilts
    // (its own dead zone, and its camera turns slowly near it), so any movement starts at SWIPE_MIN_TILT: a slow
    // drag still turns the camera. When the thumb stops the tilt falls to zero within a few frames.
    // A swipe faster than full tilt can follow would be cut short (the game caps its turn rate): what the cap cut
    // off is kept as a debt and paid back at full tilt once the thumb slows or lifts (at most SWIPE_DEBT_MAX_S), so
    // a fast flick still turns the camera about as far as the thumb went.
    private static final float STICK_DEAD_ZONE = 0.37f, STICK_SCALE = 1f / 1.7241380f;  // the stick vector's: full at 0.95
    // While swiping, the camera turns by the tilt times its slowest turn speed (native code skips the game's turn ramp),
    // so the tilt follows the thumb's speed linearly: the camera turns the same angle for the same thumb travel at any
    // speed. Curve and minimum tilt are off (they made the turn per distance change with speed); smoothing is a little
    // longer (thumb speed measured per display frame jitters, and that now shows directly as camera judder).
    private static final float SWIPE_FULL_DP_S = 220f, SWIPE_MIN_TILT = 0f, SWIPE_CURVE = 1f,
            SWIPE_SMOOTH_S = 0.04f, SWIPE_DEBT_MAX_S = 0f, SWIPE_MAX_TILT = 8f;  // carry-over off: it kept turning after the thumb lifted
    private float mSwipeDebtX, mSwipeDebtY;  // full-tilt seconds the cap cut off, per axis (signed)
    private float mSwipeAccX, mSwipeAccY;  // pixels moved since the last display frame
    private float mSwipeVelX, mSwipeVelY;  // smoothed, pixels per second
    private float mSwipeRX, mSwipeRY;      // the tilt sent to the game
    private float mSwipeDirX, mSwipeDirY;  // the last swipe direction (unit), for a held thumb
    private static final float SWIPE_HOLD_TILT = 1e-3f;  // clear of the pad value rounding (1/32767)
    private long mSwipeLastNs;
    private final android.view.Choreographer.FrameCallback mSwipeFrame = new android.view.Choreographer.FrameCallback() {
        @Override
        public void doFrame(long frameNs) {
            float dt = mSwipeLastNs == 0 ? 0.016f : Math.max(0.004f, Math.min(0.05f, (frameNs - mSwipeLastNs) / 1e9f));
            mSwipeLastNs = frameNs;
            float k = 1f - (float) Math.exp(-dt / SWIPE_SMOOTH_S);
            mSwipeVelX += (mSwipeAccX / dt - mSwipeVelX) * k;
            mSwipeVelY += (mSwipeAccY / dt - mSwipeVelY) * k;
            mSwipeAccX = mSwipeAccY = 0;
            float full = SWIPE_FULL_DP_S * mDensity / mCameraSpeed;
            float wx = mSwipeVelX / full, wy = -mSwipeVelY / full;  // wanted tilt, may be past 1
            float len = (float) Math.sqrt(wx * wx + wy * wy);
            float rx = 0, ry = 0;
            if (len < 0.01f) {
                mSwipeVelX = mSwipeVelY = 0;
            } else {
                // Lift past the game's dead zone and keep the direction. Past full tilt the value goes on rising
                // with the thumb's speed (up to SWIPE_MAX_TILT): the game's pad code does not cap it, so a fast
                // flick may turn the camera faster than a stick can (native code lets touch go to 6x, the camera 8x).
                // The camera reads the right stick through actions with no dead zone in the input mapping (18 / 19,
                // logged), then the stick vector's radial dead zone of 0.37: length (len - 0.37) * 1.724, capped at 1
                // (lifted for the swipe camera past full tilt). Sending 0.37 + 0.58 * d makes the camera see d.
                float want = len > 1f ? Math.min(len, SWIPE_MAX_TILT)
                        : SWIPE_MIN_TILT + (1f - SWIPE_MIN_TILT) * (SWIPE_CURVE == 1f ? len : (float) Math.pow(len, SWIPE_CURVE));
                float t = STICK_DEAD_ZONE + STICK_SCALE * want;
                rx = wx / len * t;
                ry = wy / len * t;
                if (len > SWIPE_MAX_TILT) {  // past the most we send: keep what is cut off
                    mSwipeDebtX += (wx - wx / len * SWIPE_MAX_TILT) * dt;
                    mSwipeDebtY += (wy - wy / len * SWIPE_MAX_TILT) * dt;
                    float d = (float) Math.sqrt(mSwipeDebtX * mSwipeDebtX + mSwipeDebtY * mSwipeDebtY);
                    if (d > SWIPE_DEBT_MAX_S) {
                        mSwipeDebtX *= SWIPE_DEBT_MAX_S / d;
                        mSwipeDebtY *= SWIPE_DEBT_MAX_S / d;
                    }
                }
            }
            // Pay the debt back at full tilt while the thumb is slower than that (or lifted). A swipe the other way
            // cancels it.
            if (mSwipeDebtX != 0 || mSwipeDebtY != 0) {
                if (rx * mSwipeDebtX + ry * mSwipeDebtY < 0) {
                    mSwipeDebtX = mSwipeDebtY = 0;
                } else if (len <= SWIPE_MAX_TILT) {
                    float d = (float) Math.sqrt(mSwipeDebtX * mSwipeDebtX + mSwipeDebtY * mSwipeDebtY);
                    float pay = Math.min(d, dt);
                    rx = mSwipeDebtX / d;
                    ry = mSwipeDebtY / d;
                    mSwipeDebtX -= rx * pay;
                    mSwipeDebtY -= ry * pay;
                    if (pay >= d) mSwipeDebtX = mSwipeDebtY = 0;
                }
            }
            // While the thumb is down the game must keep seeing camera input, as with a stick held: when the camera
            // input is exactly zero the game lets its camera follow the character again (it swung back and forth
            // between swipes while walking). A held, still thumb sends the smallest tilt past the game's dead zone in
            // the last direction (the camera sees 1/1000 of full tilt: no visible turn).
            if (rx != 0 || ry != 0) {
                float l = (float) Math.sqrt(rx * rx + ry * ry);
                mSwipeDirX = rx / l;
                mSwipeDirY = ry / l;
            } else if (mRStickPointer >= 0 && (mSwipeDirX != 0 || mSwipeDirY != 0)) {
                float t = STICK_DEAD_ZONE + STICK_SCALE * SWIPE_HOLD_TILT;
                rx = mSwipeDirX * t;
                ry = mSwipeDirY * t;
            }
            if (rx != mSwipeRX || ry != mSwipeRY) {
                mSwipeRX = rx;
                mSwipeRY = ry;
                sendState();
            }
            // Runs while the thumb is down and until the camera has stopped.
            if (mRStickPointer >= 0 || rx != 0 || ry != 0) {
                android.view.Choreographer.getInstance().postFrameCallback(this);
            } else {
                mSwipeLastNs = 0;
            }
        }
    };

    // Double tap (to show the controls again).
    private long mLastTapTime;
    private float mLastTapX, mLastTapY;

    private final Paint mFill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mStroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mText = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF mRect = new RectF();

    private TouchControls(Activity activity, float opacity, float cameraSpeed) {
        super(activity);
        mActivity = activity;
        mCameraSpeed = Math.max(0.1f, Math.min(10f, cameraSpeed));
        mDensity = activity.getResources().getDisplayMetrics().density;
        mOpacity = Math.max(0.1f, Math.min(1.0f, opacity));
        mStroke.setStyle(Paint.Style.STROKE);
        mStroke.setColor(0xFFFFFFFF);
        mText.setColor(0xFFFFFFFF);
        mText.setTextAlign(Paint.Align.CENTER);
        mText.setFakeBoldText(true);
        mButtons.add(new Button("A", A, true, "face"));
        mButtons.add(new Button("B", B, true, "face"));
        mButtons.add(new Button("X", X, true, "face"));
        mButtons.add(new Button("Y", Y, true, "face"));
        mButtons.add(new Button("L", L, false, "l"));
        mButtons.add(new Button("ZL", ZL, false, "zl"));
        mButtons.add(new Button("R", R, false, "r"));
        mButtons.add(new Button("ZR", ZR, false, "zr"));
        mButtons.add(new Button("−", MINUS, true, "minus"));
        mButtons.add(new Button("+", PLUS, true, "plus"));
        mButtons.add(new Button("▲", UP, false, "dpad"));
        mButtons.add(new Button("▼", DOWN, false, "dpad"));
        mButtons.add(new Button("◀", LEFT, false, "dpad"));
        mButtons.add(new Button("▶", RIGHT, false, "dpad"));
        mButtons.add(new Button("L3", L3, true, "l3"));
        mButtons.add(new Button("LOCK", R3, true, "lock"));
        for (int i = 0; i < 16; i++) mKind[i] = K_NONE;
        // A controller already connected (or built in, as on handhelds): start hidden.
        if (controllerConnected()) mShown = false;
        InputManager im = (InputManager) activity.getSystemService(Context.INPUT_SERVICE);
        if (im != null) im.registerInputDeviceListener(this, mHandler);
    }

    private void addTo(final Activity activity, final int attempt) {
        final View decor = activity.getWindow().getDecorView();
        if (decor.getWindowToken() == null) {  // the game's window is not attached yet
            if (attempt < 50) mHandler.postDelayed(() -> addTo(activity, attempt + 1), 100);
            return;
        }
        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                WindowManager.LayoutParams.MATCH_PARENT, WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.TYPE_APPLICATION_PANEL,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                        | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
                PixelFormat.TRANSLUCENT);
        lp.token = decor.getWindowToken();
        if (sGpuDraw) lp.flags |= WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P)
            lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        try {
            activity.getWindowManager().addView(this, lp);
            if (mEditTemp) return;  // only for the layout editor
            post(() -> touchLog("touch controls drawn on the " + (isHardwareAccelerated() ? "GPU" : "CPU")));
            sView = this;
            nativeVisible(mShown);
        } catch (Exception e) {
            android.util.Log.w("DSRNative", "touch controls: could not add the overlay", e);
        }
    }

    private boolean controllerConnected() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d == null || d.isVirtual()) continue;
            int s = d.getSources();
            if ((s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                    || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK)
                return true;
        }
        return false;
    }

    @Override
    public void onInputDeviceAdded(int deviceId) {
        InputDevice d = InputDevice.getDevice(deviceId);
        if (d == null) return;
        int s = d.getSources();
        if ((s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK)
            setShown(false);
    }

    @Override
    public void onInputDeviceRemoved(int deviceId) {}

    @Override
    public void onInputDeviceChanged(int deviceId) {}

    void setShown(boolean shown) {
        if (mEditing) {  // shown while the player moves the controls; restored to `shown` after
            mSavedShown = shown;
            return;
        }
        if (mShown == shown) return;
        mShown = shown;
        releaseAll();
        nativeVisible(shown);
        invalidate();
    }

    private void releaseAll() {
        for (int i = 0; i < 16; i++) {
            mKind[i] = K_NONE;
            mOn[i] = null;
        }
        for (Button b : mButtons) b.pressCount = 0;
        mStickPointer = -1;
        mRStickPointer = -1;
        mSwipeAccX = mSwipeAccY = mSwipeVelX = mSwipeVelY = mSwipeRX = mSwipeRY = 0;
        mSwipeDebtX = mSwipeDebtY = 0;
        sendState();
    }

    // ---- layout ---------------------------------------------------------------------------------------------

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        super.onSizeChanged(w, h, ow, oh);
        layout(w, h);
    }

    private Button find(int bit) {
        for (Button b : mButtons)
            if (b.bit == bit) return b;
        return null;
    }

    private void place(int bit, float cx, float cy, float w, float h) {
        Button b = find(bit);
        b.cx = cx;
        b.cy = cy;
        b.w = w;
        b.h = h;
    }

    private void relayout() {
        if (getWidth() > 0 && getHeight() > 0) layout(getWidth(), getHeight());
        invalidate();
    }

    private float mU, mW, mH;
    private final Map<String, float[]> mDefault = new HashMap<>();  // default spot of each group (px)
    private final Map<String, float[]> mPlaced = new HashMap<>();   // where it is (px)

    /** A group's size factor (1 = default). */
    private float sz(String group) {
        float[] o = (mEditing ? mEditLayout : sLayout).get(group);
        return o != null && o.length > 2 ? o[2] : 1f;
    }

    /** A group's spot: its default plus the player's offset, kept on screen. */
    private float[] at(String group, float x, float y) {
        mDefault.put(group, new float[] {x, y});
        float[] o = (mEditing ? mEditLayout : sLayout).get(group);
        float px = x, py = y;
        if (o != null) {
            px += o[0] * mU;
            py += o[1] * mU;
        }
        float m = 0.6f * mU;
        px = Math.max(m, Math.min(mW - m, px));
        py = Math.max(m, Math.min(mH - m, py));
        float[] p = {px, py};
        mPlaced.put(group, p);
        return p;
    }

    private void layout(int w, int h) {
        final float u = Math.min(w, h) / 10f;  // layout unit: a tenth of the short side
        mU = u;
        mW = w;
        mH = h;
        // Face buttons, Switch layout (X top, A right, B bottom, Y left).
        float[] f = at("face", w - 3.0f * u, h - 3.0f * u);
        final float fs = sz("face"), fcx = f[0], fcy = f[1], d = 1.45f * u * fs, fb = 1.3f * u * fs;
        place(A, fcx + d, fcy, fb, fb);
        place(B, fcx, fcy + d, fb, fb);
        place(X, fcx, fcy - d, fb, fb);
        place(Y, fcx - d, fcy, fb, fb);
        float[] p = at("lock", w - 5.6f * u, h - 1.2f * u);
        place(R3, p[0], p[1], 1.3f * u * sz("lock"), 1.3f * u * sz("lock"));
        // Shoulders: ZL / ZR in the top corners, L / R below them.
        final float sw = 2.4f * u, sh = 1.1f * u;
        p = at("zl", 1.7f * u, 1.0f * u);
        place(ZL, p[0], p[1], sw * sz("zl"), sh * sz("zl"));
        p = at("l", 1.7f * u, 2.5f * u);
        place(L, p[0], p[1], sw * sz("l"), sh * sz("l"));
        p = at("zr", w - 1.7f * u, 1.0f * u);
        place(ZR, p[0], p[1], sw * sz("zr"), sh * sz("zr"));
        p = at("r", w - 1.7f * u, 2.5f * u);
        place(R, p[0], p[1], sw * sz("r"), sh * sz("r"));
        // Minus / plus at the top centre.
        p = at("minus", w / 2f - 1.3f * u, 0.9f * u);
        place(MINUS, p[0], p[1], 1.0f * u * sz("minus"), 1.0f * u * sz("minus"));
        p = at("plus", w / 2f + 1.3f * u, 0.9f * u);
        place(PLUS, p[0], p[1], 1.0f * u * sz("plus"), 1.0f * u * sz("plus"));
        // D-pad, bottom, right of the stick.
        p = at("dpad", 6.9f * u, h - 2.0f * u);
        final float dps = sz("dpad"), dcx = p[0], dcy = p[1], ds = 0.95f * u * dps, db = 0.9f * u * dps;
        place(UP, dcx, dcy - ds, db, db);
        place(DOWN, dcx, dcy + ds, db, db);
        place(LEFT, dcx - ds, dcy, db, db);
        place(RIGHT, dcx + ds, dcy, db, db);
        p = at("l3", 4.7f * u, h - 0.9f * u);
        place(L3, p[0], p[1], 1.0f * u * sz("l3"), 1.0f * u * sz("l3"));
        // Stick: resting spot, and the area where a finger grabs it.
        mStickRadius = 1.6f * u * sz("stick");
        mRRadius = 1.05f * u;
        p = at("stick", 2.8f * u, h - 3.0f * u);
        mStickBaseX = p[0];
        mStickBaseY = p[1];
        mText.setTextSize(0.45f * u);
        mStroke.setStrokeWidth(Math.max(2f, 0.05f * u));
        // Layout editor: Save / Reset / Cancel in the middle of the screen, the size row (- size +) below them.
        final float bw = 3.0f * u, bh = 1.0f * u, gap = 0.4f * u, ty = h * 0.5f;
        for (int i = 0; i < 3; i++) {
            float cx = w / 2f + (i - 1) * (bw + gap);
            mTool[i].set(cx - bw / 2, ty - bh / 2, cx + bw / 2, ty + bh / 2);
        }
        final float sb = 1.0f * u, sy = ty + bh / 2 + 0.35f * u + sb / 2, sx = 2.6f * u;
        mSizeBtn[0].set(w / 2f - sx - sb / 2, sy - sb / 2, w / 2f - sx + sb / 2, sy + sb / 2);
        mSizeBtn[1].set(w / 2f + sx - sb / 2, sy - sb / 2, w / 2f + sx + sb / 2, sy + sb / 2);
    }

    // ---- layout editor --------------------------------------------------------------------------------------

    private boolean mEditing, mEditTemp, mSavedShown;
    private float mSavedAlpha;
    private Runnable mEditDone;
    private Map<String, float[]> mEditLayout = new HashMap<>();
    private int mDragPointer = -1;
    private String mDragGroup;
    private float mDragX, mDragY;
    private final RectF[] mTool = {new RectF(), new RectF(), new RectF()};  // Save, Reset, Cancel
    private final RectF[] mSizeBtn = {new RectF(), new RectF()};  // smaller, bigger (the selected group)
    private String mSelGroup;  // the group last touched: what - / + resize
    private int mPinchPointer = -1;  // a second finger while a group is held: pinch to resize it
    private float mPinchDist, mPinchSize;

    /** Sets a group's offset in the editor, keeping its size. */
    private void putOffset(String g, float dx, float dy) {
        float[] o = mEditLayout.get(g);
        mEditLayout.put(g, new float[] {dx, dy, o != null && o.length > 2 ? o[2] : 1f});
    }

    /** Sets a group's size in the editor, keeping its offset. */
    private void putSize(String g, float size) {
        float[] o = mEditLayout.get(g);
        size = Math.max(SIZE_MIN, Math.min(SIZE_MAX, Math.round(size * 100f) / 100f));
        mEditLayout.put(g, new float[] {o != null ? o[0] : 0f, o != null ? o[1] : 0f, size});
        relayout();
    }

    private static float distance(MotionEvent e, int a, int b) {
        float dx = e.getX(a) - e.getX(b), dy = e.getY(a) - e.getY(b);
        return (float) Math.sqrt(dx * dx + dy * dy);
    }

    private void startEdit(boolean temp, Runnable done) {
        mEditTemp = temp;
        mEditDone = done;
        releaseAll();
        mEditing = true;
        mSavedShown = mShown;
        mShown = true;
        mSavedAlpha = mOpacity;
        mOpacity = Math.max(0.85f, mSavedAlpha);
        mEditLayout = new HashMap<>();
        for (Map.Entry<String, float[]> e : sLayout.entrySet())
            mEditLayout.put(e.getKey(), e.getValue().clone());
        mDragPointer = -1;
        mDragGroup = null;
        mSelGroup = null;
        mPinchPointer = -1;
        relayout();
    }

    private void endEdit(boolean save) {
        if (!mEditing) return;
        if (save) {
            sLayout = mEditLayout;
            if (!SettingsMenu.writeSetting("touch_layout", formatLayout(mEditLayout)))
                android.util.Log.w("DSRNative", "touch controls: could not save the layout");
        }
        mEditing = false;
        sEditing = false;
        mDragPointer = -1;
        mDragGroup = null;
        mOpacity = mSavedAlpha;
        mShown = mSavedShown;
        if (mEditTemp) {
            InputManager im = (InputManager) mActivity.getSystemService(Context.INPUT_SERVICE);
            if (im != null) im.unregisterInputDeviceListener(this);
            try {
                mActivity.getWindowManager().removeView(this);
            } catch (Exception e) {
                android.util.Log.w("DSRNative", "touch controls: could not remove the editor", e);
            }
        } else {
            relayout();
        }
        Runnable done = mEditDone;
        mEditDone = null;
        if (done != null) done.run();
    }

    private String groupAt(float x, float y) {
        Button b = hit(x, y);
        if (b != null) return b.group;
        float dx = x - mStickBaseX, dy = y - mStickBaseY, r = mStickRadius * 1.1f;
        return dx * dx + dy * dy <= r * r ? "stick" : null;
    }

    private void editTouch(MotionEvent e) {
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int i = e.getActionIndex();
                float x = e.getX(i), y = e.getY(i);
                if (mDragPointer >= 0) {  // a second finger while a group is held: pinch resizes that group
                    int d = e.findPointerIndex(mDragPointer);
                    if (mPinchPointer < 0 && d >= 0 && mDragGroup != null) {
                        mPinchPointer = e.getPointerId(i);
                        mPinchDist = Math.max(1f, distance(e, d, i));
                        mPinchSize = sz(mDragGroup);
                    }
                    break;
                }
                if (mSelGroup != null && mSizeBtn[0].contains(x, y)) {
                    putSize(mSelGroup, sz(mSelGroup) - SIZE_STEP);
                    break;
                }
                if (mSelGroup != null && mSizeBtn[1].contains(x, y)) {
                    putSize(mSelGroup, sz(mSelGroup) + SIZE_STEP);
                    break;
                }
                if (mTool[0].contains(x, y)) {
                    endEdit(true);
                    return;
                }
                if (mTool[1].contains(x, y)) {
                    mEditLayout = new HashMap<>();
                    relayout();
                    break;
                }
                if (mTool[2].contains(x, y)) {
                    endEdit(false);
                    return;
                }
                String g = groupAt(x, y);
                if (g != null) {
                    mDragPointer = e.getPointerId(i);
                    mDragGroup = g;
                    mSelGroup = g;
                    mDragX = x;
                    mDragY = y;
                }
                break;
            }
            case MotionEvent.ACTION_MOVE: {
                if (mDragPointer < 0) break;
                int i = e.findPointerIndex(mDragPointer);
                if (i < 0) break;
                float x = e.getX(i), y = e.getY(i);
                if (mPinchPointer >= 0) {  // pinching: resize, no move
                    int j = e.findPointerIndex(mPinchPointer);
                    if (j >= 0) putSize(mDragGroup, mPinchSize * distance(e, i, j) / mPinchDist);
                    mDragX = x;
                    mDragY = y;
                    break;
                }
                float[] def = mDefault.get(mDragGroup), cur = mPlaced.get(mDragGroup);
                if (def == null || cur == null || mU <= 0) break;
                // New spot = where it is + the finger's move; kept on screen by layout().
                float nx = cur[0] + (x - mDragX), ny = cur[1] + (y - mDragY);
                putOffset(mDragGroup, (nx - def[0]) / mU, (ny - def[1]) / mU);
                mDragX = x;
                mDragY = y;
                relayout();
                float[] placed = mPlaced.get(mDragGroup);  // clamped: keep the offset where it stopped
                if (placed != null)
                    putOffset(mDragGroup, (placed[0] - def[0]) / mU, (placed[1] - def[1]) / mU);
                break;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                int id = e.getPointerId(e.getActionIndex());
                if (id == mPinchPointer) {
                    mPinchPointer = -1;
                    int i = e.findPointerIndex(mDragPointer);  // moving again from where the held finger is
                    if (i >= 0) {
                        mDragX = e.getX(i);
                        mDragY = e.getY(i);
                    }
                }
                if (id == mDragPointer) {
                    mDragPointer = -1;
                    mDragGroup = null;
                    mPinchPointer = -1;
                }
                break;
            }
            case MotionEvent.ACTION_CANCEL:
                mDragPointer = -1;
                mDragGroup = null;
                mPinchPointer = -1;
                break;
            default:
                break;
        }
        invalidate();
    }

    private void drawEditor(Canvas c) {
        final float u = mU;
        String[] labels = {Strings.t("save"), Strings.t("reset"), Strings.t("cancel")};
        float size = mText.getTextSize();
        mText.setTextSize(0.4f * u);
        for (int i = 0; i < 3; i++) {
            RectF r = mTool[i];
            mFill.setColor(i == 0 ? 0xFFC9A55C : 0xF0242424);
            c.drawRoundRect(r, 0.25f * u, 0.25f * u, mFill);
            c.drawRoundRect(r, 0.25f * u, 0.25f * u, mStroke);
            mText.setColor(i == 0 ? 0xFF151515 : 0xFFFFFFFF);
            c.drawText(labels[i], r.centerX(), r.centerY() - (mText.descent() + mText.ascent()) / 2, mText);
        }
        // Size row: - / + for the selected group, its size between them.
        for (int i = 0; i < 2; i++) {
            RectF r = mSizeBtn[i];
            mFill.setColor(mSelGroup != null ? 0xF0242424 : 0x90242424);
            c.drawRoundRect(r, 0.25f * u, 0.25f * u, mFill);
            c.drawRoundRect(r, 0.25f * u, 0.25f * u, mStroke);
            mText.setColor(mSelGroup != null ? 0xFFFFFFFF : 0x80FFFFFF);
            mText.setTextSize(0.6f * u);
            c.drawText(i == 0 ? "\u2212" : "+", r.centerX(), r.centerY() - (mText.descent() + mText.ascent()) / 2, mText);
        }
        mText.setTextSize(0.38f * u);
        String sizeText = mSelGroup == null ? Strings.t("size_pick")
                : Strings.t("size_of", Math.round(sz(mSelGroup) * 100f));
        float sw2 = mText.measureText(sizeText), scy = mSizeBtn[0].centerY();
        mRect.set(mW / 2f - sw2 / 2 - 0.3f * u, scy - 0.4f * u, mW / 2f + sw2 / 2 + 0.3f * u, scy + 0.4f * u);
        mFill.setColor(0xD0000000);
        c.drawRoundRect(mRect, 0.2f * u, 0.2f * u, mFill);
        mText.setColor(0xFFFFFFFF);
        c.drawText(sizeText, mW / 2f, scy - (mText.descent() + mText.ascent()) / 2, mText);
        String help = Strings.t("edit_help");
        float tw = mText.measureText(help);
        float ty = mTool[0].top - 0.75f * u;
        mRect.set(mW / 2f - tw / 2 - 0.4f * u, ty - 0.45f * u, mW / 2f + tw / 2 + 0.4f * u, ty + 0.45f * u);
        mFill.setColor(0xD0000000);
        c.drawRoundRect(mRect, 0.2f * u, 0.2f * u, mFill);
        mText.setColor(0xFFFFFFFF);
        c.drawText(help, mW / 2f, ty - (mText.descent() + mText.ascent()) / 2, mText);
        mText.setTextSize(size);
    }

    // ---- touch ----------------------------------------------------------------------------------------------

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (mEditing) {
            editTouch(e);
            return true;
        }
        final int action = e.getActionMasked();
        if (edgeSwipe(e)) return true;
        if (!mShown) {
            if (action == MotionEvent.ACTION_DOWN) checkDoubleTap(e.getX(), e.getY());
            return true;
        }
        snapshot();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int i = e.getActionIndex(), id = e.getPointerId(i);
                float x = e.getX(i), y = e.getY(i);
                if (action == MotionEvent.ACTION_POINTER_DOWN && id >= 0 && id < 16) {
                    mFresh[id] = true;
                    mFreshX[id] = x;
                    mFreshY[id] = y;
                    mFreshAt[id] = android.os.SystemClock.uptimeMillis();
                    float[] t = turned(sPointerTurn, x, y);
                    x = t[0];
                    y = t[1];
                }
                down(id, x, y);
                logTouch(e, i, "down");
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++) {
                    int id = e.getPointerId(i);
                    if (checkFreshPointer(id, e.getX(i), e.getY(i))) continue;
                    move(id, e.getX(i), e.getY(i));
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                int i = e.getActionIndex();
                logTouch(e, i, "up");
                if (e.getPointerId(i) >= 0 && e.getPointerId(i) < 16) mFresh[e.getPointerId(i)] = false;
                up(e.getPointerId(i));
                break;
            }
            case MotionEvent.ACTION_CANCEL:
                logTouch(e, -1, "cancel");
                releaseAll();
                break;
            default:
                break;
        }
        sendState();
        logButtons();
        invalidateChanges();
        return true;
    }

    // ---- diagnostics: multi-touch (an OPPO tablet could not hold two buttons at once) -----------------------------
    // Each finger down / up with what it landed on and how many fingers Android reports, touches Android cancelled
    // (the whole gesture, or one finger: FLAG_CANCELED), and every change of the buttons held. The first 400 lines.
    private int mTouchLogs = 0, mLoggedBits = 0;

    private static void touchLog(String text) {
        try {
            nativeLog(text);
        } catch (UnsatisfiedLinkError err) {
            android.util.Log.i("DSRNative", "touch: " + text);
        }
    }

    private String targetOf(int id) {
        if (id < 0 || id >= 16) return "?";
        switch (mKind[id]) {
            case K_BUTTON: return mOn[id] != null ? "button " + mOn[id].label : "button (slid off)";
            case K_STICK: return "move stick";
            case K_RSTICK: return sSwipe ? "camera (swipe)" : "camera stick";
            default: return "nothing";
        }
    }

    private void logTouch(MotionEvent e, int i, String what) {
        if (mTouchLogs >= 400) return;
        mTouchLogs++;
        if (i < 0) {
            touchLog("Android cancelled the touches (" + e.getPointerCount()
                    + " fingers were down): everything released");
            return;
        }
        int id = e.getPointerId(i);
        boolean cancelled = android.os.Build.VERSION.SDK_INT >= 33 && (e.getFlags() & 0x20) != 0;  // FLAG_CANCELED
        touchLog(String.format(java.util.Locale.ROOT,
                "finger %d %s at %.0f%%, %.0f%% of the screen: %s; Android reports %d finger(s) down%s", id, what,
                100f * e.getX(i) / Math.max(1, getWidth()), 100f * e.getY(i) / Math.max(1, getHeight()), targetOf(id),
                e.getPointerCount() - ("up".equals(what) ? 1 : 0), cancelled ? " (Android cancelled this finger)" : ""));
    }

    private void logButtons() {
        int bits = 0;
        StringBuilder sb = new StringBuilder();
        for (Button b : mButtons)
            if (b.pressCount > 0) {
                if ((bits & b.bit) == 0) sb.append(sb.length() > 0 ? " + " : "").append(b.label);
                bits |= b.bit;
            }
        if (bits == mLoggedBits || mTouchLogs >= 400) return;
        mLoggedBits = bits;
        mTouchLogs++;
        touchLog("buttons held: " + (sb.length() > 0 ? sb : "none"));
    }

    // ---- swipe in from the left edge: opens the settings menu ------------------------------------------------------
    // A finger that comes down within EDGE_DP of the left edge and moves right by a sixth of the screen's width
    // (mostly sideways) opens the menu, whatever it was pressing (let go first). Without the touch controls, the
    // same gesture is caught by native code (android_main.cpp, OnInput).
    static final float EDGE_DP = 24f;
    private final boolean[] mEdge = new boolean[16];
    private final float[] mEdgeX = new float[16], mEdgeY = new float[16];

    private boolean edgeSwipe(MotionEvent e) {
        final int action = e.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            int i = e.getActionIndex(), id = e.getPointerId(i);
            if (id >= 0 && id < 16) {
                mEdge[id] = e.getX(i) <= EDGE_DP * mDensity;
                mEdgeX[id] = e.getX(i);
                mEdgeY[id] = e.getY(i);
            }
        } else if (action == MotionEvent.ACTION_MOVE) {
            for (int i = 0; i < e.getPointerCount(); i++) {
                int id = e.getPointerId(i);
                if (id < 0 || id >= 16 || !mEdge[id]) continue;
                float dx = e.getX(i) - mEdgeX[id], dy = Math.abs(e.getY(i) - mEdgeY[id]);
                if (dy > 0.5f * Math.max(dx, 1f) && dy > 24 * mDensity) {
                    mEdge[id] = false;  // mostly up or down: not the gesture
                } else if (dx >= getWidth() / 6f) {
                    mEdge[id] = false;
                    android.util.Log.i("DSRNative", "swipe from the left edge: opening the settings menu");
                    releaseAll();
                    sendState();
                    invalidate();
                    SettingsMenu.show(mActivity);
                    return true;
                }
            }
        } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            int id = e.getPointerId(e.getActionIndex());
            if (id >= 0 && id < 16) mEdge[id] = false;
        } else if (action == MotionEvent.ACTION_CANCEL) {
            java.util.Arrays.fill(mEdge, false);
        }
        return false;
    }

    private void checkDoubleTap(float x, float y) {
        long now = android.os.SystemClock.uptimeMillis();
        float slop = 60 * mDensity;
        if (now - mLastTapTime < 350 && Math.abs(x - mLastTapX) < slop && Math.abs(y - mLastTapY) < slop) {
            mLastTapTime = 0;
            setShown(true);
        } else {
            mLastTapTime = now;
            mLastTapX = x;
            mLastTapY = y;
        }
    }

    private Button hit(float x, float y) {
        float slop = 0.15f * Math.min(getWidth(), getHeight()) / 10f;
        Button best = null;
        float bestDist = Float.MAX_VALUE;
        for (Button b : mButtons)
            if (b.hit(x, y, slop)) {
                float dx = x - b.cx, dy = y - b.cy, dist = dx * dx + dy * dy;
                if (dist < bestDist) {
                    bestDist = dist;
                    best = b;
                }
            }
        return best;
    }

    private void down(int id, float x, float y) {
        if (id < 0 || id >= 16) return;
        mLastX[id] = x;
        mLastY[id] = y;
        Button b = hit(x, y);
        if (b != null) {
            mKind[id] = K_BUTTON;
            mOn[id] = b;
            b.pressCount++;
        } else if (mStickPointer < 0 && onStick(x, y)) {
            mKind[id] = K_STICK;
            mStickPointer = id;
            mStickX = x;
            mStickY = y;
        } else if (mRStickPointer < 0 && x > getWidth() * 0.5f) {
            mKind[id] = K_RSTICK;
            mRStickPointer = id;
            mRBaseX = mRX = x;
            mRBaseY = mRY = y;
            if (sSwipe) {
                mSwipeAccX = mSwipeAccY = 0;
                mSwipeDebtX = mSwipeDebtY = 0;
                mSwipeDirX = mSwipeDirY = 0;
                android.view.Choreographer.getInstance().removeFrameCallback(mSwipeFrame);
                android.view.Choreographer.getInstance().postFrameCallback(mSwipeFrame);
                return;
            }
            mRLastMove = android.os.SystemClock.uptimeMillis();
            mRLastFrameNs = 0;
            android.view.Choreographer.getInstance().removeFrameCallback(mRecenter);
            android.view.Choreographer.getInstance().postFrameCallback(mRecenter);
        } else {
            mKind[id] = K_NONE;
        }
    }

    private boolean onStick(float x, float y) {
        float dx = x - mStickBaseX, dy = y - mStickBaseY, r = mStickRadius * 1.5f;
        return dx * dx + dy * dy <= r * r;
    }

    private void move(int id, float x, float y) {
        if (id < 0 || id >= 16) return;
        switch (mKind[id]) {
            case K_BUTTON: {  // slide between buttons (e.g. across the face buttons)
                Button b = hit(x, y);
                if (b != mOn[id]) {
                    if (mOn[id] != null) mOn[id].pressCount--;
                    mOn[id] = b;
                    if (b != null) b.pressCount++;
                }
                break;
            }
            case K_STICK:
                mStickX = x;
                mStickY = y;
                break;
            case K_RSTICK: {
                if (sSwipe) {
                    mSwipeAccX += x - mRX;
                    mSwipeAccY += y - mRY;
                    mRX = x;
                    mRY = y;
                    break;
                }
                if (x != mRX || y != mRY) mRLastMove = android.os.SystemClock.uptimeMillis();
                mRX = x;
                mRY = y;
                float dx = x - mRBaseX, dy = y - mRBaseY, len = (float) Math.sqrt(dx * dx + dy * dy);
                if (len > mRRadius) {  // the base follows the finger
                    mRBaseX = x - dx / len * mRRadius;
                    mRBaseY = y - dy / len * mRRadius;
                }
                break;
            }
            default:
                break;
        }
        mLastX[id] = x;
        mLastY[id] = y;
    }

    private void up(int id) {
        if (id < 0 || id >= 16) return;
        if (mKind[id] == K_BUTTON && mOn[id] != null) mOn[id].pressCount--;
        if (mKind[id] == K_STICK) mStickPointer = -1;
        if (mKind[id] == K_RSTICK) mRStickPointer = -1;
        mKind[id] = K_NONE;
        mOn[id] = null;
    }

    private void sendState() {
        int bits = 0;
        for (Button b : mButtons)
            if (b.pressCount > 0) bits |= b.bit;
        float lx = 0, ly = 0;
        if (mStickPointer >= 0) {
            float dx = mStickX - mStickBaseX, dy = mStickY - mStickBaseY;
            float len = (float) Math.sqrt(dx * dx + dy * dy);
            if (len > mStickRadius) {  // past the edge: full tilt in that direction
                dx = dx / len * mStickRadius;
                dy = dy / len * mStickRadius;
            }
            lx = dx / mStickRadius;
            ly = -dy / mStickRadius;
            if (Math.abs(lx) < 0.08f && Math.abs(ly) < 0.08f) lx = ly = 0;
        }
        float rx = 0, ry = 0;
        if (sSwipe) {
            rx = mSwipeRX;
            ry = mSwipeRY;
        } else if (mRStickPointer >= 0) {
            // Full tilt at 1 / speed of the ring's radius (speed 1: at the edge).
            float reach = mRRadius / mCameraSpeed;
            rx = Math.max(-1f, Math.min(1f, (mRX - mRBaseX) / reach));
            ry = Math.max(-1f, Math.min(1f, -(mRY - mRBaseY) / reach));
            float rl = (float) Math.sqrt(rx * rx + ry * ry);
            if (rl > 1f) {  // at most full tilt (past 1 lifts the game's camera cap: swipe camera only)
                rx /= rl;
                ry /= rl;
            }
            if (Math.abs(rx) < 0.06f && Math.abs(ry) < 0.06f) rx = ry = 0;
        }
        nativeState(bits, lx, ly, rx, ry);
    }

    // ---- drawing --------------------------------------------------------------------------------------------

    private final Paint mBitmapPaint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    private final LightingColorFilter mPressedFilter = new LightingColorFilter(0xFFFFFFFF, 0x00383838);
    private final RectF mDst = new RectF();

    /**
     * Draws a skin image fitted into a w x h box at (cx, cy), keeping its proportions; brighter when pressed and the
     * skin has no _lit image. False if the skin has no such image (the plain shape is drawn instead).
     */
    private boolean drawSkin(Canvas c, Map<String, Bitmap> skin, String name, boolean on, float cx, float cy, float w,
            float h, int alpha) {
        Bitmap bm = on ? skin.get(name + "_lit") : null;
        boolean brighten = false;
        if (bm == null) {
            bm = skin.get(name);
            brighten = on;
        }
        if (bm == null) return false;
        float s = Math.min(w / bm.getWidth(), h / bm.getHeight());
        int dw = Math.max(1, Math.round(bm.getWidth() * s)), dh = Math.max(1, Math.round(bm.getHeight() * s));
        mBitmapPaint.setAlpha(alpha);
        mBitmapPaint.setColorFilter(brighten ? mPressedFilter : null);
        c.drawBitmap(scaled(skin, bm, dw, dh), Math.round(cx - dw / 2f), Math.round(cy - dh / 2f), mBitmapPaint);
        return true;
    }

    // Drawn on the CPU (GameActivity is not hardware accelerated: on an Adreno 630, Android's OpenGL drawing of this
    // window next to our Vulkan driver came out striped and its GPU context failed, killing the game). So each
    // skin image is scaled once to its size on screen and then only copied, a touch redraws only what it changed
    // (invalidateChanges), and the opacity is applied to the redrawn area alone (View.setAlpha would blend the
    // whole screen-sized window on every redraw).
    private float mOpacity = 1f;
    private final Map<Bitmap, Bitmap> mScaled = new HashMap<>();
    private Map<String, Bitmap> mScaledFor;

    /** The skin image at w x h pixels, made once (bitmap scaling every frame is slow on the CPU). */
    private Bitmap scaled(Map<String, Bitmap> skin, Bitmap bm, int w, int h) {
        if (skin != mScaledFor) {  // another skin: its scaled images are made again
            mScaled.clear();
            mScaledFor = skin;
        }
        if (bm.getWidth() == w && bm.getHeight() == h) return bm;
        Bitmap sc = mScaled.get(bm);
        if (sc == null || sc.getWidth() != w || sc.getHeight() != h) {
            sc = Bitmap.createScaledBitmap(bm, w, h, true);
            mScaled.put(bm, sc);
        }
        return sc;
    }

    // State before a touch event, to redraw only the parts it changed.
    private int mSnapStick, mSnapR;
    private float mSnapSX, mSnapSY, mSnapRX, mSnapRY, mSnapRBX, mSnapRBY;
    private long mSnapPressed;
    private final android.graphics.Rect mDirty = new android.graphics.Rect();

    private long pressedMask() {
        long m = 0;
        for (int i = 0; i < mButtons.size(); i++)
            if (mButtons.get(i).pressCount > 0) m |= 1L << i;
        return m;
    }

    private void snapshot() {
        mSnapStick = mStickPointer;
        mSnapSX = mStickX;
        mSnapSY = mStickY;
        mSnapR = mRStickPointer;
        mSnapRX = mRX;
        mSnapRY = mRY;
        mSnapRBX = mRBaseX;
        mSnapRBY = mRBaseY;
        mSnapPressed = pressedMask();
    }

    private void addDirty(float cx, float cy, float halfW, float halfH) {
        int pad = 3;
        mDirty.union((int) Math.floor(cx - halfW) - pad, (int) Math.floor(cy - halfH) - pad,
                (int) Math.ceil(cx + halfW) + pad, (int) Math.ceil(cy + halfH) + pad);
    }

    /** Redraws what changed since snapshot(): the stick, the camera stick and buttons pressed or let go. */
    private void invalidateChanges() {
        if (!mShown) return;
        mDirty.setEmpty();
        if (mStickPointer != mSnapStick || (mStickPointer >= 0 && (mStickX != mSnapSX || mStickY != mSnapSY)))
            addDirty(mStickBaseX, mStickBaseY, 1.5f * mStickRadius, 1.5f * mStickRadius);  // base and knob travel
        if (!sSwipe && (mRStickPointer != mSnapR || mRX != mSnapRX || mRY != mSnapRY || mRBaseX != mSnapRBX
                || mRBaseY != mSnapRBY)) {
            if (mSnapR >= 0) addDirty(mSnapRBX, mSnapRBY, 1.5f * mRRadius, 1.5f * mRRadius);  // where it was
            if (mRStickPointer >= 0) addDirty(mRBaseX, mRBaseY, 1.5f * mRRadius, 1.5f * mRRadius);  // where it is
        }
        long changed = pressedMask() ^ mSnapPressed;
        for (int i = 0; i < mButtons.size(); i++) {
            if ((changed & (1L << i)) == 0) continue;
            Button b = mButtons.get(i);
            addDirty(b.cx, b.cy, b.w / 2, (b.round ? b.w : b.h) / 2);
        }
        if (!mDirty.isEmpty()) invalidate(mDirty.left, mDirty.top, mDirty.right, mDirty.bottom);
    }

    @Override
    protected void onDraw(Canvas c) {
        if (!mShown) return;
        // The opacity, over the redrawn area only.
        final int sc = mOpacity < 0.999f
                ? c.saveLayerAlpha(new RectF(c.getClipBounds()), Math.round(255 * mOpacity))
                : -1;
        drawControls(c);
        if (sc >= 0) c.restoreToCount(sc);
    }

    private void drawControls(Canvas c) {
        if (mEditing) c.drawColor(0x50000000);
        final Map<String, Bitmap> skin = sSkin;
        // Stick (the knob stays inside the ring).
        final float bx = mStickBaseX, by = mStickBaseY;
        final boolean stickSel = mEditing && ("stick".equals(mDragGroup) || "stick".equals(mSelGroup));
        if (stickSel || !drawSkin(c, skin, "stick_base", mStickPointer >= 0, bx, by, 2 * mStickRadius,
                2 * mStickRadius, 255)) {
            mFill.setColor(stickSel ? 0xA0C9A55C : 0x40000000);
            c.drawCircle(bx, by, mStickRadius, mFill);
            c.drawCircle(bx, by, mStickRadius, mStroke);
        }
        float kx = bx, ky = by;
        if (mStickPointer >= 0) {
            float dx = mStickX - bx, dy = mStickY - by, len = (float) Math.sqrt(dx * dx + dy * dy);
            if (len > mStickRadius) {
                dx = dx / len * mStickRadius;
                dy = dy / len * mStickRadius;
            }
            kx = bx + dx;
            ky = by + dy;
        }
        if (!drawSkin(c, skin, "stick_nub", mStickPointer >= 0, kx, ky, mStickRadius, mStickRadius, 255)) {
            mFill.setColor(mStickPointer >= 0 ? 0xA0FFFFFF : 0x60FFFFFF);
            c.drawCircle(kx, ky, mStickRadius * 0.45f, mFill);
        }
        // Camera stick, only while touched, faint: 10% opaque on screen (whatever touch_opacity is). Nothing is
        // drawn for the swipe camera.
        if (mRStickPointer >= 0 && !sSwipe) {
            final int a = Math.max(1, Math.min(255, Math.round(255 * 0.10f / Math.max(0.1f, mOpacity))));
            // A skin's camera ring is an image of its own: shown at 40% (the plain one, a faint outline, at 10%).
            final int sa = Math.max(1, Math.min(255, Math.round(255 * 0.40f / Math.max(0.1f, mOpacity))));
            if (!drawSkin(c, skin, "ring", false, mRBaseX, mRBaseY, 2 * mRRadius, 2 * mRRadius, sa)) {
                mFill.setColor(a << 24);
                c.drawCircle(mRBaseX, mRBaseY, mRRadius, mFill);
                mStroke.setAlpha(a);
                c.drawCircle(mRBaseX, mRBaseY, mRRadius, mStroke);
                mStroke.setAlpha(0xFF);
            }
            float dx = mRX - mRBaseX, dy = mRY - mRBaseY, len = (float) Math.sqrt(dx * dx + dy * dy);
            if (len > mRRadius) {
                dx = dx / len * mRRadius;
                dy = dy / len * mRRadius;
            }
            if (!drawSkin(c, skin, "stick_nub", true, mRBaseX + dx, mRBaseY + dy, mRRadius, mRRadius, sa)) {
                mFill.setColor((a << 24) | 0xFFFFFF);
                c.drawCircle(mRBaseX + dx, mRBaseY + dy, mRRadius * 0.45f, mFill);
            }
        }
        // Buttons.
        for (Button b : mButtons) {
            boolean on = b.pressCount > 0;
            boolean sel = mEditing && (b.group.equals(mDragGroup) || b.group.equals(mSelGroup));
            if (!sel && drawSkin(c, skin, skinName(b.bit), on, b.cx, b.cy, b.w, b.round ? b.w : b.h, 255)) continue;
            mFill.setColor(sel ? 0xC0C9A55C : on ? 0x90FFFFFF : 0x50000000);
            if (b.round) {
                c.drawCircle(b.cx, b.cy, b.w / 2, mFill);
                c.drawCircle(b.cx, b.cy, b.w / 2, mStroke);
            } else {
                mRect.set(b.cx - b.w / 2, b.cy - b.h / 2, b.cx + b.w / 2, b.cy + b.h / 2);
                float r = Math.min(b.w, b.h) * 0.3f;
                c.drawRoundRect(mRect, r, r, mFill);
                c.drawRoundRect(mRect, r, r, mStroke);
            }
            mText.setColor(on ? 0xFF000000 : 0xFFFFFFFF);
            float size = mText.getTextSize();
            if (b.label.length() > 2) mText.setTextSize(size * 0.7f);
            c.drawText(b.label, b.cx, b.cy - (mText.descent() + mText.ascent()) / 2, mText);
            mText.setTextSize(size);
        }
        if (mEditing) drawEditor(c);
    }
}
