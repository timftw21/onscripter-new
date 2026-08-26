package org.umineko_project.onscripter_ru;

import android.annotation.SuppressLint;
import android.content.pm.ActivityInfo;
import android.os.Build;
import android.os.Bundle;
import android.view.Display;
import android.view.MotionEvent;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;

import androidx.annotation.RequiresApi;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.util.Arrays;

public class ONSActivity extends SDLActivity implements TouchInput.SurfaceMapper {
    private static final String C = "ONSActivity";

    /** The rate the engine also caps itself at; see DROID_MAX_AUTO_FPS. */
    private static final float TARGET_REFRESH_RATE_HZ = 60.0f;
    /** Never drop the panel below this chasing 60 on an odd set of modes. */
    private static final float MIN_REFRESH_RATE_HZ = 50.0f;

    private TouchInput touchInput;

    /** Screen position of the SDL surface, refreshed lazily. */
    private final int[] surfaceOrigin = new int[2];
    private SurfaceView surfaceView;

    /**
     * Created only on API 33+. Keep the field descriptor itself API-neutral:
     * OnBackInvokedCallback does not exist below 33, and ONSActivity must still
     * be loadable by those runtimes before the guarded methods are called.
     */
    private Object backCallback;

    @Override
    protected String[] getLibraries() {
        // SDL3 is statically linked into libmain.so, so there is no
        // libSDL3.so to dlopen. Loading the SDL default list would throw
        // UnsatisfiedLinkError before "main" is ever loaded, leaving every
        // native method unresolved.
        String[] libraries = new String[] { "main" };
        Diag.i(C, "getLibraries -> " + Arrays.toString(libraries));
        return libraries;
    }

    @Override
    protected String getMainFunction() {
        // The engine declares a plain `int main(...)` in Engine/Core/Loader.cpp
        // and never includes SDL_main.h, so SDL2's `#define main SDL_main`
        // shim is not in effect. libmain.so exports "main", not "SDL_main".
        Diag.i(C, "getMainFunction -> main");
        return "main";
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        Diag.i(C, "onCreate");
        super.onCreate(savedInstanceState);
        touchInput = new TouchInput(ViewConfiguration.get(this), this);
        configureScopedStorage();

        // The manifest locks landscape so a phone is never briefly portrait
        // before this runs; large screens relax it here. Doing it in code
        // rather than trusting the manifest alone is deliberate: Android 16
        // ignores a manifest-declared fixed orientation on large screens, so
        // the manifest by itself gives the right answer for the wrong reason
        // and only on that one OS version.
        if (!getResources().getBoolean(R.bool.lock_landscape)) {
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED);
        }

        // Back has to be claimed, not merely observed. The manifest sets
        // enableOnBackInvokedCallback, so on API 33+ onBackPressed is never
        // called and SDL's own SDL_ANDROID_TRAP_BACK_BUTTON handling is dead
        // code; without a callback here the system finishes the activity
        // itself. Below 33 the attribute is ignored and onBackPressed is the
        // only route, so exactly one of the two paths is live on any device.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerBackCallback();
        }

        preferSixtyHertz();
    }

    /**
     * Ask the panel to run at 60Hz while the game is on screen.
     *
     * The engine paces the scene to whatever refresh rate it detects, and a
     * visual novel has nothing to show for the 120 or 144Hz a modern panel
     * offers -- it just costs battery, in the engine, the GPU and the display
     * alike. The engine caps itself at 60 as well; this makes the panel agree,
     * so every frame is shown for a whole number of refreshes instead of
     * juddering against a rate it does not divide.
     *
     * Only the refresh rate is chosen here. Picking a mode with a different
     * resolution would move the surface out from under the canvas geometry, so
     * candidates are restricted to the size already in use.
     */
    private void preferSixtyHertz() {
        Display display = getDisplay();
        if (display == null) {
            Diag.i(C, "not attached to a display yet; leaving refresh rate alone");
            return;
        }
        Display.Mode current = display.getMode();

        Display.Mode best = null;
        for (Display.Mode mode : display.getSupportedModes()) {
            if (mode.getPhysicalWidth() != current.getPhysicalWidth()
                    || mode.getPhysicalHeight() != current.getPhysicalHeight()) {
                continue;
            }
            if (mode.getRefreshRate() < MIN_REFRESH_RATE_HZ) {
                continue;
            }
            if (best == null || isCloserToSixty(mode.getRefreshRate(), best.getRefreshRate())) {
                best = mode;
            }
        }

        if (best == null) {
            return;
        }

        WindowManager.LayoutParams lp = getWindow().getAttributes();
        lp.preferredDisplayModeId = best.getModeId();
        getWindow().setAttributes(lp);
        Diag.i(C, "requested " + best.getRefreshRate() + "Hz (was " + current.getRefreshRate() + "Hz)");
    }

    /** Prefers the rate nearer 60, and the lower one when a tie splits it. */
    private static boolean isCloserToSixty(float candidate, float incumbent) {
        float candidateDelta = Math.abs(candidate - TARGET_REFRESH_RATE_HZ);
        float incumbentDelta = Math.abs(incumbent - TARGET_REFRESH_RATE_HZ);
        if (candidateDelta != incumbentDelta) {
            return candidateDelta < incumbentDelta;
        }
        return candidate < incumbent;
    }

    @RequiresApi(api = Build.VERSION_CODES.TIRAMISU)
    private void registerBackCallback() {
        OnBackInvokedCallback callback = this::onSystemBack;
        backCallback = callback;
        getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                OnBackInvokedDispatcher.PRIORITY_DEFAULT, callback);
    }

    @RequiresApi(api = Build.VERSION_CODES.TIRAMISU)
    private void unregisterBackCallback() {
        if (backCallback != null) {
            getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(
                    (OnBackInvokedCallback) backCallback);
            backCallback = null;
        }
    }

    /**
     * The pre-33 half of the same gesture.
     *
     * Deliberately does not call super: that is what finishes the activity, and
     * the whole point is that Back is a game action rather than an exit. The raw
     * KEYCODE_BACK still reaches SDL as SDL_SCANCODE_AC_BACK, which the engine
     * has no handler for, so nothing acts on it twice.
     */
    @Override
    @SuppressLint("GestureBackNavigation")
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        onSystemBack();
    }

    /**
     * What the Back gesture means inside the game.
     *
     * Today it is always handed to the engine as a right-click, so Back never
     * leaves the app: the way out is Exit in the game's own menu, or Home.
     *
     * Letting Back exit at the "root" (the title screen) would go here, on the
     * false branch. It needs care rather than just a finish() call. The engine
     * gives no signal about whether it consumed a click, so knowing we are at
     * the root would take a native callback; and quitting this way runs into a
     * separate teardown bug -- SDL parks the engine thread in nativePause
     * before onDestroy sends the quit, so mSDLThread.join(1000) expires and the
     * engine's shutdown runs on after the window is gone, which is what aborts
     * in hwuiTask1.
     */
    private void onSystemBack() {
        if (touchInput != null && touchInput.systemBack()) {
            return;
        }
        // The engine is not listening yet -- during startup, or once it has
        // begun shutting down. Swallow the press rather than tearing the
        // activity down underneath it.
        Diag.i(C, "back pressed while engine not ready; ignoring");
    }

    @Override
    protected void onPause() {
        // Losing the window mid-drag would otherwise leave SDL holding a button
        // that never receives its release.
        if (touchInput != null) {
            touchInput.reset();
        }
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        // The engine calls exit() on a fatal error, so this is often the last
        // Java line before the process goes away.
        Diag.i(C, "onDestroy");
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            unregisterBackCallback();
        }
        super.onDestroy();
    }

    /**
     * Takes touch input before SDLSurface sees it.
     *
     * SDL's own listener stays installed for mice, styluses and everything else
     * it handles; fingers are intercepted here and re-emitted as mouse events
     * because the engine's SDL3 touch path cannot produce a right-click. See
     * TouchInput for why. Returning true consumes the event, so a finger is
     * never delivered twice.
     */
    @Override
    public boolean dispatchTouchEvent(MotionEvent event) {
        if (touchInput != null
                && event.getToolType(0) == MotionEvent.TOOL_TYPE_FINGER
                && touchInput.onTouch(event)) {
            return true;
        }
        return super.dispatchTouchEvent(event);
    }

    @Override
    public float toSurfaceX(float rawX) {
        refreshSurfaceOrigin();
        return rawX - surfaceOrigin[0];
    }

    @Override
    public float toSurfaceY(float rawY) {
        refreshSurfaceOrigin();
        return rawY - surfaceOrigin[1];
    }

    @Override
    public int surfaceWidth() {
        refreshSurfaceOrigin();
        return surfaceView != null ? surfaceView.getWidth() : 0;
    }

    @Override
    public int surfaceHeight() {
        refreshSurfaceOrigin();
        return surfaceView != null ? surfaceView.getHeight() : 0;
    }

    /**
     * Events arrive in screen coordinates, but the engine expects them relative
     * to the SDL surface. The surface normally fills the window, so the offset
     * is usually zero -- but not under a display cutout or split screen.
     */
    private void refreshSurfaceOrigin() {
        if (surfaceView == null) {
            surfaceView = findSurfaceView(findViewById(android.R.id.content));
            if (surfaceView == null) {
                surfaceOrigin[0] = surfaceOrigin[1] = 0;
                return;
            }
        }
        surfaceView.getLocationOnScreen(surfaceOrigin);
    }

    private static SurfaceView findSurfaceView(View root) {
        if (root instanceof SurfaceView) {
            return (SurfaceView) root;
        }
        if (root instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) root;
            for (int i = 0; i < group.getChildCount(); i++) {
                SurfaceView found = findSurfaceView(group.getChildAt(i));
                if (found != null) {
                    return found;
                }
            }
        }
        return null;
    }

    /**
     * Directory holding the game data: the folder chosen in SetupActivity when one
     * is set and still readable, otherwise the app-scoped location.
     */
    private File resolveGameDir() {
        return GameStorage.resolveRoot(this);
    }

    @Override
    protected String[] getArguments() {
        File dir = resolveGameDir();
        if (dir == null) {
            Diag.e(C, "no game directory could be resolved, launching with no arguments");
            return new String[0];
        }

        Diag.logDirectory(C, "launch dir", dir);

        // --root is authoritative: the engine refuses to let a later path
        // override it, so the game folder cannot be second-guessed by a config
        // file or by whatever getLaunchDir() derives from the environment.
        //
        // Saves are deliberately not passed with --save. Overriding save_path
        // suppresses lookupSavePath(), which is what applies the engine's own
        // layout, and left two SaveData directories in the game folder. Setting
        // EXTERNAL_STORAGE below is enough to keep the engine's structure inside
        // the chosen folder.
        // Software video decoding, deliberately.
        //
        // Android hands out hardware MediaCodec instances from a small global
        // pool and takes them back from apps that are in the background. The
        // engine has no way to survive that: it never releases the codec on
        // pause and cannot rebuild a decoder mid-playback -- looping only seeks
        // the demuxer and keeps the codec alive. So every call into the dead
        // codec returned AVERROR_EXTERNAL, which the decode loop treats as
        // recoverable, leaving the app on a black screen at ~140% CPU emitting
        // roughly fifteen thousand log lines a second until it was killed.
        //
        // With no MediaCodec there is nothing to reclaim, and the decoder is
        // ordinary CPU state that survives backgrounding untouched.
        //
        // The cost is smaller than it sounds. The videos in constant use are
        // graphics/../video/masked/*.m2v -- MPEG-2, which is close to free to
        // decode in software. Only the 35 H.264 movies in video/1080p are
        // expensive, they play rarely, and a 720p set ships alongside them.
        String[] arguments = new String[] { "--root", dir.getAbsolutePath(), "--hwdecoder", "off" };
        Diag.i(C, "handing off to engine, argv " + Arrays.toString(arguments));
        return arguments;
    }

    private void configureScopedStorage() {
        File launchDir = resolveGameDir();
        if (launchDir == null) {
            Diag.e(C, "unable to resolve any storage directory");
            return;
        }
        // Only ever create the app-scoped fallback. A folder the user picked is
        // theirs; if it has gone away, resolveRoot has already stopped returning it.
        if (!launchDir.isDirectory() && launchDir.equals(GameStorage.getScopedRoot(this))) {
            boolean created = launchDir.mkdirs();
            Diag.i(C, "created app-scoped launch dir: " + created);
            if (!created) {
                Diag.e(C, "unable to create launch directory: " + launchDir.getAbsolutePath());
            }
        }

        // SDL3's nativeSetenv calls POSIX setenv() directly -- deliberately, not
        // SDL_setenv -- so these do reach the engine's std::getenv.
        //
        // getLaunchDir() appends "ONScripter-RU" to EXTERNAL_STORAGE, and
        // getStorageDir() hangs "SaveData" off that. A selected game folder is
        // therefore the base itself. The app-scoped fallback already has the
        // provider suffix, so GameStorage deliberately returns its parent to
        // preserve the save location used by previous releases.
        File storageBase = GameStorage.getNativeStorageBase(this, launchDir);
        if (storageBase == null) {
            Diag.e(C, "unable to resolve native storage base for " + launchDir);
            return;
        }
        String basePath = storageBase.getAbsolutePath();
        Diag.i(C, "nativeSetenv EXTERNAL_STORAGE=" + basePath);
        nativeSetenv("EXTERNAL_STORAGE", basePath);
        nativeSetenv("SECONDARY_STORAGE", basePath);
        nativeSetenv("EXTERNAL_SDCARD_STORAGE", basePath);
        nativeSetenv("HOME", basePath);
        nativeSetenv("ONS_SCOPED_STORAGE", launchDir.getAbsolutePath());
    }
}
