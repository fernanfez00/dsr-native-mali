package com.dsrnative.app;

import android.app.Activity;
import android.content.Context;
import android.graphics.Canvas;
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

    private static TouchControls sView;
    /** settings.txt touch_camera_recenter: a thumb resting on the camera stick brings it back to the centre. */
    static volatile boolean sRecenter = true;
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
            sView.setAlpha(Math.max(0.1f, Math.min(1.0f, opacity)));
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
                float k = 1f - (float) Math.exp(-dt * ramp / RECENTER_TIME_S);
                mRBaseX += (mRX - mRBaseX) * k;
                mRBaseY += (mRY - mRBaseY) * k;
                float dx = mRX - mRBaseX, dy = mRY - mRBaseY;
                if (dx * dx + dy * dy < 0.25f) {
                    mRBaseX = mRX;
                    mRBaseY = mRY;
                }
                sendState();
                invalidate();
            }
            android.view.Choreographer.getInstance().postFrameCallback(this);
        }
    };
    private float mCameraSpeed;

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
        setAlpha(Math.max(0.1f, Math.min(1.0f, opacity)));
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
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P)
            lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        try {
            activity.getWindowManager().addView(this, lp);
            if (mEditTemp) return;  // only for the layout editor
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
        mSavedAlpha = getAlpha();
        setAlpha(Math.max(0.85f, mSavedAlpha));
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
        setAlpha(mSavedAlpha);
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
        if (!mShown) {
            if (action == MotionEvent.ACTION_DOWN) checkDoubleTap(e.getX(), e.getY());
            return true;
        }
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int i = e.getActionIndex();
                down(e.getPointerId(i), e.getX(i), e.getY(i));
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++) move(e.getPointerId(i), e.getX(i), e.getY(i));
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                int i = e.getActionIndex();
                up(e.getPointerId(i));
                break;
            }
            case MotionEvent.ACTION_CANCEL:
                releaseAll();
                break;
            default:
                break;
        }
        sendState();
        invalidate();
        return true;
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
        if (mRStickPointer >= 0) {
            // Full tilt at 1 / speed of the ring's radius (speed 1: at the edge).
            float reach = mRRadius / mCameraSpeed;
            rx = Math.max(-1f, Math.min(1f, (mRX - mRBaseX) / reach));
            ry = Math.max(-1f, Math.min(1f, -(mRY - mRBaseY) / reach));
            if (Math.abs(rx) < 0.06f && Math.abs(ry) < 0.06f) rx = ry = 0;
        }
        nativeState(bits, lx, ly, rx, ry);
    }

    // ---- drawing --------------------------------------------------------------------------------------------

    @Override
    protected void onDraw(Canvas c) {
        if (!mShown) return;
        if (mEditing) c.drawColor(0x50000000);
        // Stick (the knob stays inside the ring).
        final float bx = mStickBaseX, by = mStickBaseY;
        mFill.setColor(mEditing && ("stick".equals(mDragGroup) || "stick".equals(mSelGroup)) ? 0xA0C9A55C : 0x40000000);
        c.drawCircle(bx, by, mStickRadius, mFill);
        c.drawCircle(bx, by, mStickRadius, mStroke);
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
        mFill.setColor(mStickPointer >= 0 ? 0xA0FFFFFF : 0x60FFFFFF);
        c.drawCircle(kx, ky, mStickRadius * 0.45f, mFill);
        // Camera stick, only while touched, faint: 10% opaque on screen (whatever touch_opacity is).
        if (mRStickPointer >= 0) {
            final int a = Math.max(1, Math.min(255, Math.round(255 * 0.10f / Math.max(0.1f, getAlpha()))));
            mFill.setColor(a << 24);
            c.drawCircle(mRBaseX, mRBaseY, mRRadius, mFill);
            mStroke.setAlpha(a);
            c.drawCircle(mRBaseX, mRBaseY, mRRadius, mStroke);
            mStroke.setAlpha(0xFF);
            float dx = mRX - mRBaseX, dy = mRY - mRBaseY, len = (float) Math.sqrt(dx * dx + dy * dy);
            if (len > mRRadius) {
                dx = dx / len * mRRadius;
                dy = dy / len * mRRadius;
            }
            mFill.setColor((a << 24) | 0xFFFFFF);
            c.drawCircle(mRBaseX + dx, mRBaseY + dy, mRRadius * 0.45f, mFill);
        }
        // Buttons.
        for (Button b : mButtons) {
            boolean on = b.pressCount > 0;
            mFill.setColor(mEditing && (b.group.equals(mDragGroup) || b.group.equals(mSelGroup)) ? 0xC0C9A55C
                    : on ? 0x90FFFFFF : 0x50000000);
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
