package org.umineko_project.onscripter_ru;

import android.os.Handler;
import android.os.Looper;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.VelocityTracker;
import android.view.ViewConfiguration;

import org.libsdl.app.SDLActivity;

/**
 * Translates Android touch gestures into the input the engine actually consumes
 * on this platform.
 *
 * Plain taps already work without this class: the engine groups finger events
 * itself and reads the count as a mouse button, so a two-finger tap is a
 * right-click on its own. What it cannot do under SDL3 is gestures -- the
 * multi-gesture branch of ONScripter::touchEvent opens with
 * "#if defined(ONS_USE_SDL3) return false;", leaving two-finger scrolling and
 * every three-finger swipe as dead code. This class adds those, and a
 * long-press alternative to the two-finger right-click.
 *
 * Which events reach the engine at all is the part that constrains the design.
 * Its dispatch switch in Event.cpp is split:
 *
 *     #if defined(IOS) || defined(DROID)
 *         case SDL_FINGERDOWN / SDL_FINGERUP  -> touchEvent
 *     #else
 *         case SDL_MOUSEBUTTONDOWN / UP       -> mousePressEvent
 *         case SDL_MOUSEWHEEL                 -> mouseScrollEvent
 *     #endif
 *
 * On Android the mouse-button and wheel cases are not compiled. Synthesising
 * mouse buttons therefore does nothing at all, however well-formed they are.
 * SDL_MOUSEMOTION sits outside that #if and is handled on every platform.
 *
 * So this class drives both paths deliberately:
 *
 *   - Buttons go through onNativeTouch, as a count of simultaneous fingers.
 *     The engine decides which button a touch means by grouping finger events
 *     itself: the first sets the id to 1, and any further event within
 *     MAX_TOUCH_TAP_TIMESPAN (80 ms) increments it, giving left, right and
 *     middle. Whatever id SDL supplies is overwritten, so a right-click is two
 *     finger-ups in quick succession rather than a particular id.
 *
 *   - Scrolling goes through onNativeMouse as ACTION_SCROLL, which SDL turns
 *     into a real SDL_MOUSEWHEEL. The engine's wheel case had to be compiled on
 *     Android for that to arrive, and with it comes mouseScrollEvent, which
 *     animates scrollable sprites and opens the backlog. Arrow keys could do
 *     neither: they step the hovered element one whole entry at a time. The
 *     wheel carries a float under SDL3, so a drag sends fractions of a tick and
 *     scrolls by the distance the finger actually moved.
 *
 *   - Position goes through onNativeMouse as ACTION_MOVE. mouseButtonDecision
 *     resolves a left click against `hoveringButton`, which only mouseMoveEvent
 *     updates, so a click without a preceding motion lands on no button at all.
 *
 * Right-click is not optional in this game: it opens the menus, and the
 * file-verification screen accepts nothing else -- its left-click exit is
 * commented out in the script.
 */
final class TouchInput {
    private static final String C = "TouchInput";

    // Mirrors the ACTION_* constants in SDL's Android backend.
    private static final int SDL_ACTION_DOWN = 0;
    private static final int SDL_ACTION_UP = 1;
    private static final int SDL_ACTION_MOVE = 2;
    // SDL's Android backend reads the raw MotionEvent action here, and turns
    // ACTION_SCROLL into SDL_MOUSEWHEEL with the x/y axes as the wheel deltas.
    private static final int SDL_ACTION_SCROLL = MotionEvent.ACTION_SCROLL;

    // Simultaneous fingers the engine must see to read a button. Its grouping
    // window is 80 ms, so these are emitted back to back.
    private static final int FINGERS_LEFT = 1;
    private static final int FINGERS_RIGHT = 2;
    private static final int FINGERS_MIDDLE = 3;

    /** Held still for this long with one finger is a right-click. */
    private static final long LONG_PRESS_MS = 400;

    /**
     * The backlog's scrollbar arrows, as fractions of the game canvas.
     *
     * The engine cannot hit-test these for us: it holds a pointer to the
     * scrollbar's thumb sprite and nothing else, so the track and these two
     * arrows -- drawn by the script -- have no rect to ask it for. The positions
     * were measured off the running backlog and converted to canvas fractions,
     * which keeps them right at any screen size because the script's coordinate
     * space is fixed. The mapping back to the surface mirrors the engine's own:
     * largest whole-script scale that fits, centred, letterboxed.
     *
     * Far larger than the arrows they cover -- the drawn glyphs are about
     * eighteen script pixels tall, hopeless to hit with a finger. They can afford
     * to be, because nothing outside the backlog ever consults them: the boxes
     * only exist while the engine reports a scrollbar on screen. The right-hand
     * strip they occupy is clear of the backlog's text and of its button row.
     */
    private static final float ARROW_X_MIN = 0.895f;
    private static final float ARROW_X_MAX = 1.000f;
    private static final float ARROW_UP_Y_MIN = 0.120f;
    private static final float ARROW_UP_Y_MAX = 0.330f;
    private static final float ARROW_DOWN_Y_MIN = 0.670f;
    private static final float ARROW_DOWN_Y_MAX = 0.880f;

    // Mirrors ONScripter::InputContext.
    private static final int CONTEXT_NOVEL = 1 << 0;
    private static final int CONTEXT_BACKLOG = 1 << 1;
    private static final int CONTEXT_PAGED = 1 << 2;

    /** Held this long on an arrow before it starts repeating. */
    private static final long PAGE_REPEAT_DELAY_MS = 450;
    /** And a page every this often after that. */
    private static final long PAGE_REPEAT_INTERVAL_MS = 130;

    /** Aspect the engine letterboxes the script canvas to inside the surface. */
    private static final float CANVAS_ASPECT = 16f / 9f;

    /** A multi-finger touch shorter than this, that did not move, is a tap. */
    private static final long TAP_MS = 300;
    /**
     * Finger travel that equals one wheel tick.
     *
     * The engine multiplies a tick by mouse_scroll_mul, which is -100 on this
     * platform, so a hundred pixels of finger per tick makes the content follow
     * the finger roughly one to one. The sign is what puts the content under the
     * finger rather than against it.
     */
    private static final float SCROLL_PX_PER_TICK = 100f;
    /** Below this there is nothing worth sending; it only inflates the event rate. */
    private static final float SCROLL_MIN_EMIT_PX = 3f;

    /** Lifting slower than this is a stop, not a throw. */
    private static final float FLING_MIN_VELOCITY = 250f;
    /** Below this the glide is over. */
    private static final float FLING_STOP_VELOCITY = 50f;
    /**
     * Velocity retained per frame. 0.94 at 16 ms leaves about a second of glide,
     * and carries roughly a quarter of a second of travel at the speed the finger
     * left at.
     */
    private static final float FLING_FRICTION = 0.94f;
    private static final long FLING_FRAME_MS = 16;

    private enum State {
        IDLE,
        /** One finger down, not yet committed to a tap or a drag. */
        ONE_PENDING,
        /** One finger down and moving. */
        ONE_DRAG,
        TWO,
        THREE,
        /** Gesture resolved; ignore the rest until all fingers lift. */
        SPENT,
    }

    /** Supplies the SDL surface geometry, since events arrive in screen space. */
    interface SurfaceMapper {
        float toSurfaceX(float rawX);

        float toSurfaceY(float rawY);

        int surfaceWidth();

        int surfaceHeight();
    }

    private final SurfaceMapper mapper;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final int touchSlop;
    private final int swipeThreshold;

    private State state = State.IDLE;
    private int touchDeviceId;

    private float startX, startY;
    private long startTime;
    private float lastScrollY;
    private VelocityTracker velocity;
    /** +1 while an up arrow is held, -1 for down, 0 when neither is. */
    private int pageDirection;
    /** Cleared if the engine turns out not to carry the scrollbar query. */
    private static boolean queryAvailable = true;
    /** Pixels per second still to be spent on the glide; positive is downward. */
    private float flingVelocity;
    private boolean multiMoved;
    /** Whether startX/startY have been set by a real touch yet. */
    private boolean hasPoint;

    private final Runnable longPress = () -> {
        if (state != State.ONE_PENDING) {
            return;
        }
        Diag.i(C, "gesture: long press -> right click");
        click(FINGERS_RIGHT, startX, startY);
        state = State.SPENT;
    };

    /** Keeps paging while an arrow stays held. */
    private final Runnable pageRepeat = new Runnable() {
        @Override
        public void run() {
            if (pageDirection == 0) {
                return;
            }
            sendPage(pageDirection);
            handler.postDelayed(this, PAGE_REPEAT_INTERVAL_MS);
        }
    };

    /**
     * Keeps scrolling after the fingers leave, losing speed each frame.
     *
     * The engine does not do this for us. mouseScrollEvent animates each scroll
     * over 100 ms, which smooths one wheel notch but ends the moment the events
     * stop, so a flick would otherwise halt dead under the finger.
     *
     * Overriding a scroll still in flight loses nothing: addSpriteProperty folds
     * the unspent remainder of the previous change into the new one when the
     * value is relative, so a 16 ms cadence accumulates exactly.
     */
    private final Runnable fling = new Runnable() {
        @Override
        public void run() {
            if (SDLActivity.mBrokenLibraries
                    || SDLActivity.mCurrentNativeState != SDLActivity.NativeState.RESUMED) {
                flingVelocity = 0;
                return;
            }
            wheel(flingVelocity * FLING_FRAME_MS / 1000f / SCROLL_PX_PER_TICK);
            flingVelocity *= FLING_FRICTION;
            if (Math.abs(flingVelocity) >= FLING_STOP_VELOCITY) {
                handler.postDelayed(this, FLING_FRAME_MS);
            }
        }
    };

    TouchInput(ViewConfiguration config, SurfaceMapper mapper) {
        this.mapper = mapper;
        this.touchSlop = config.getScaledTouchSlop();
        // Deliberately larger than the tap slop: three fingers drift more than
        // one, and a swipe should not fire while the user is still deciding.
        this.swipeThreshold = config.getScaledTouchSlop() * 6;
    }

    /**
     * Feeds one event through the recogniser.
     *
     * @return true when the event was handled and must not reach SDL's own
     * touch listener, which would otherwise deliver the same finger twice.
     */
    boolean onTouch(MotionEvent event) {
        if (SDLActivity.mBrokenLibraries
                || SDLActivity.mCurrentNativeState != SDLActivity.NativeState.RESUMED) {
            // The engine is not listening yet; leave the event alone.
            return false;
        }

        touchDeviceId = event.getDeviceId();
        float x = mapper.toSurfaceX(event.getRawX());
        float y = mapper.toSurfaceY(event.getRawY());

        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                beginSingle(x, y, event.getEventTime());
                break;
            case MotionEvent.ACTION_POINTER_DOWN:
                beginMulti(event);
                break;
            case MotionEvent.ACTION_MOVE:
                move(event, x, y);
                break;
            case MotionEvent.ACTION_POINTER_UP:
                resolveMulti(event);
                break;
            case MotionEvent.ACTION_UP:
                endSingle(x, y);
                break;
            case MotionEvent.ACTION_CANCEL:
                cancel();
                break;
            default:
                break;
        }
        return true;
    }

    /** Drops any gesture in progress. Call when the activity loses the surface. */
    void reset() {
        cancel();
        // VelocityTracker instances come from a shared pool; hand this one back
        // rather than holding it across a surface loss. beginMulti re-obtains.
        if (velocity != null) {
            velocity.recycle();
            velocity = null;
        }
    }

    /**
     * Plays a system Back press into the game as a right-click.
     *
     * Right-click is what the script already treats as "back", and it is
     * context-sensitive without anyone having to ask what screen is showing.
     * mouseButtonDecision only acts on one when
     * `(rmode_flag && WAIT_TEXT_MODE) || WAIT_BUTTON_MODE | WAIT_RCLICK_MODE`,
     * so the same press opens the menu during the novel, steps out of a
     * submenu, and does nothing at the title screen or mid-effect.
     *
     * Escape is not a substitute. It reaches the same buttonState of -1 but by
     * a different route, bypassing that gate, and from a scene it jumps
     * straight to the title screen.
     *
     * @return true when the press was delivered to the engine. False means the
     * engine was not listening; the caller decides what Back should mean then.
     */
    boolean systemBack() {
        if (SDLActivity.mBrokenLibraries
                || SDLActivity.mCurrentNativeState != SDLActivity.NativeState.RESUMED) {
            return false;
        }

        // A Back press arriving mid-gesture would otherwise interleave with it.
        cancel();

        float x = hasPoint ? startX : mapper.surfaceWidth() / 2f;
        float y = hasPoint ? startY : mapper.surfaceHeight() / 2f;
        Diag.i(C, "system back -> right click");
        click(FINGERS_RIGHT, x, y);
        return true;
    }

    // --- gesture phases -----------------------------------------------------

    private void beginSingle(float x, float y, long eventTime) {
        cancelLongPress();
        // Touching the screen stops a glide, the way it does everywhere else.
        stopFling();
        state = State.ONE_PENDING;
        startX = x;
        startY = y;
        hasPoint = true;
        startTime = eventTime;
        multiMoved = false;

        if (inBacklog()) {
            int arrow = arrowAt(x, y);
            if (arrow != 0) {
                // One page now, then repeat while the finger stays down. Marking
                // the gesture spent stops the release firing a left click and
                // stops the long press firing a right click, which would close
                // the backlog out from under the repeat.
                //
                // Safe to take the press over only because the engine confirmed
                // the backlog is what is waiting for input. Everywhere else this
                // branch is skipped and the gesture runs as it always did.
                //
                // A whole page, rather than the single line the script's own arrow
                // buttons scroll -- those still work, and are still a line a tap.
                Diag.i(C, "gesture: scrollbar arrow -> page " + (arrow > 0 ? "up" : "down"));
                pageDirection = arrow;
                sendPage(arrow);
                handler.postDelayed(pageRepeat, PAGE_REPEAT_DELAY_MS);
                state = State.SPENT;
                return;
            }
        }

        // Move the cursor immediately so anything under the finger is hovered
        // before a tap resolves.
        motion(x, y);
        handler.postDelayed(longPress, LONG_PRESS_MS);
    }

    private void beginMulti(MotionEvent event) {
        cancelLongPress();
        stopFling();

        if (velocity == null) {
            velocity = VelocityTracker.obtain();
        }
        velocity.clear();
        velocity.addMovement(event);

        int count = event.getPointerCount();
        state = count >= 3 ? State.THREE : State.TWO;
        float[] centre = centroid(event);
        startX = centre[0];
        startY = centre[1];
        hasPoint = true;
        startTime = event.getEventTime();
        lastScrollY = centre[1];
        multiMoved = false;
    }

    private void move(MotionEvent event, float x, float y) {
        switch (state) {
            case ONE_PENDING:
                if (Math.hypot(x - startX, y - startY) <= touchSlop) {
                    return;
                }
                cancelLongPress();
                state = State.ONE_DRAG;
                motion(x, y);
                break;

            case ONE_DRAG:
                motion(x, y);
                break;

            case TWO: {
                if (velocity != null) {
                    velocity.addMovement(event);
                }
                float[] centre = centroid(event);
                if (Math.abs(centre[1] - startY) > touchSlop) {
                    multiMoved = true;
                }
                float dy = centre[1] - lastScrollY;
                if (Math.abs(dy) >= SCROLL_MIN_EMIT_PX) {
                    // Dragging down scrolls the content down, which is a wheel
                    // *up* -- the direction every touchscreen uses, and the one
                    // the engine reads as "open the backlog" in text mode.
                    wheel(dy / SCROLL_PX_PER_TICK);
                    lastScrollY = centre[1];
                }
                break;
            }

            case THREE: {
                float[] centre = centroid(event);
                if (Math.abs(centre[0] - startX) > touchSlop
                        || Math.abs(centre[1] - startY) > touchSlop) {
                    multiMoved = true;
                }
                break;
            }

            default:
                break;
        }
    }

    private void resolveMulti(MotionEvent event) {
        long held = event.getEventTime() - startTime;

        if (state == State.TWO && !multiMoved && held < TAP_MS) {
            Diag.i(C, "gesture: two-finger tap -> right click");
            click(FINGERS_RIGHT, startX, startY);
        } else if (state == State.THREE && !multiMoved && held < TAP_MS) {
            // Three fingers mean different things in different places, which is
            // only expressible now that the engine can say where we are. In the
            // backlog the script wants a middle click, which jumps the story to
            // the line under the finger -- its own help text says as much.
            if (inBacklog()) {
                Diag.i(C, "gesture: three-finger tap -> jump to line");
                click(FINGERS_MIDDLE, startX, startY);
            } else {
                Diag.i(C, "gesture: three-finger tap -> skip");
                key(KeyEvent.KEYCODE_S, true);
            }
        } else if (state == State.THREE && multiMoved) {
            float[] centre = centroid(event);
            swipe(centre[0] - startX, centre[1] - startY);
        } else if (state == State.TWO && multiMoved) {
            startFling(event);
        }

        if (state != State.IDLE) {
            // One gesture per touch sequence. Without this, lifting fingers one
            // at a time re-enters TWO and fires a spurious right click.
            state = State.SPENT;
        }
    }

    private void endSingle(float x, float y) {
        cancelLongPress();
        stopFling();
        stopPaging();
        if (state == State.ONE_PENDING) {
            Diag.i(C, "gesture: tap -> left click");
            click(FINGERS_LEFT, x, y);
        }
        state = State.IDLE;
    }

    private void cancel() {
        cancelLongPress();
        stopFling();
        stopPaging();
        state = State.IDLE;
    }

    /**
     * Throws the scroll at the speed the fingers were moving when they left.
     *
     * Averaged over the pointers that are staying down, skipping the one being
     * lifted -- that one is already decelerating against the glass and reports a
     * speed the user did not intend.
     */
    private void startFling(MotionEvent event) {
        if (velocity == null) {
            return;
        }
        velocity.addMovement(event);
        velocity.computeCurrentVelocity(1000);

        int skip = event.getActionMasked() == MotionEvent.ACTION_POINTER_UP
                ? event.getActionIndex() : -1;
        float sum = 0;
        int n = 0;
        for (int i = 0; i < event.getPointerCount(); i++) {
            if (i == skip) {
                continue;
            }
            sum += velocity.getYVelocity(event.getPointerId(i));
            n++;
        }
        if (n == 0) {
            return;
        }

        float v = sum / n;
        if (Math.abs(v) < FLING_MIN_VELOCITY) {
            return;
        }

        Diag.i(C, "gesture: two-finger fling -> scroll at " + (int) v + " px/s");
        flingVelocity = v;
        handler.postDelayed(fling, FLING_FRAME_MS);
    }

    private void stopFling() {
        handler.removeCallbacks(fling);
        flingVelocity = 0;
    }

    private void stopPaging() {
        handler.removeCallbacks(pageRepeat);
        pageDirection = 0;
    }

    /**
     * Which input context the script is waiting in.
     *
     * The only thing the engine tells Java about what is on screen, and it has to
     * exist: every other JNI entry point runs the other way, Java calling in.
     * Without it a gesture bound to a screen region would have to fire
     * everywhere, which is wrong outside the one screen it means something on.
     *
     * Costs the engine three loads and a store per frame, because it reads the
     * get*_flag set the script had already raised rather than inspecting the
     * scene. Each of those is raised in exactly one place in wh.txt, which is
     * what makes them an identity rather than a hint.
     */
    private int inputContext() {
        if (!queryAvailable
                || SDLActivity.mBrokenLibraries
                || SDLActivity.mCurrentNativeState != SDLActivity.NativeState.RESUMED) {
            return 0;
        }
        try {
            return nativeInputContext();
        } catch (UnsatisfiedLinkError e) {
            // An engine built without the query. Stop asking and leave input alone.
            Diag.w(C, "engine has no input-context query; region gestures disabled", e);
            queryAvailable = false;
            return 0;
        }
    }

    /** The backlog: getmclick, which wh.txt raises only in *log_button_loop. */
    private boolean inBacklog() {
        return (inputContext() & CONTEXT_BACKLOG) != 0;
    }

    /** The novel's own click-wait: gettab, raised only in *text_cwlp. */
    private boolean inNovel() {
        return (inputContext() & CONTEXT_NOVEL) != 0;
    }

    private static native int nativeInputContext();

    /**
     * Which scrollbar arrow, if any, sits under this point.
     *
     * @return +1 for the up arrow, -1 for the down arrow, 0 for anywhere else.
     */
    private int arrowAt(float x, float y) {
        int sw = mapper.surfaceWidth();
        int sh = mapper.surfaceHeight();
        if (sw <= 0 || sh <= 0) {
            return 0;
        }

        // The engine fits the canvas inside the surface and letterboxes the rest,
        // so the game does not start at the surface's left edge on a tall screen.
        float canvasW = sw;
        float canvasH = sh;
        if (sw > sh * CANVAS_ASPECT) {
            canvasW = sh * CANVAS_ASPECT;
        } else {
            canvasH = sw / CANVAS_ASPECT;
        }
        float originX = (sw - canvasW) / 2f;
        float originY = (sh - canvasH) / 2f;

        float fx = (x - originX) / canvasW;
        float fy = (y - originY) / canvasH;

        if (fx < ARROW_X_MIN || fx > ARROW_X_MAX) {
            return 0;
        }
        if (fy >= ARROW_UP_Y_MIN && fy <= ARROW_UP_Y_MAX) {
            return 1;
        }
        if (fy >= ARROW_DOWN_Y_MIN && fy <= ARROW_DOWN_Y_MAX) {
            return -1;
        }
        return 0;
    }

    /**
     * Pages the backlog.
     *
     * Page Up and Page Down are what the backlog's own help text tells the reader
     * to use, and the engine routes them through getpageup_flag and
     * getpagedown_flag. Those are only set while a screen wants paging, so a
     * press that lands here outside the backlog reaches an engine that ignores it.
     */
    private void sendPage(int direction) {
        key(direction > 0 ? KeyEvent.KEYCODE_PAGE_UP : KeyEvent.KEYCODE_PAGE_DOWN, false);
    }


    private void swipe(float dx, float dy) {
        if (Math.abs(dx) < swipeThreshold && Math.abs(dy) < swipeThreshold) {
            return;
        }

        // The directions the SDL2 handler used, expressed with real keys.
        // ONS_SCANCODE_SKIP and ONS_SCANCODE_MUTE are synthetic values above
        // SDL_NUM_SCANCODES that no keyboard can produce; the engine accepts
        // Alt+S and Alt+M for the same actions.
        if (Math.abs(dx) > Math.abs(dy)) {
            if (dx > 0) {
                Diag.i(C, "gesture: three-finger swipe right -> skip");
                key(KeyEvent.KEYCODE_S, true);
            } else {
                Diag.i(C, "gesture: three-finger swipe left -> auto");
                key(KeyEvent.KEYCODE_A, false);
            }
        } else {
            if (dy > 0) {
                Diag.i(C, "gesture: three-finger swipe down -> backlog");
                key(KeyEvent.KEYCODE_TAB, false);
            } else {
                Diag.i(C, "gesture: three-finger swipe up -> mute");
                key(KeyEvent.KEYCODE_M, true);
            }
        }
    }

    // --- event emission -----------------------------------------------------

    /**
     * A click is `fingers` simultaneous touches down and up at the same spot,
     * preceded by a motion so the engine knows what is under the cursor.
     *
     * The releases are what carry the button: the engine only acts on a
     * right-click for a finger-up, and its grouping counts the events that
     * arrive inside one 80 ms window.
     */
    private void click(int fingers, float x, float y) {
        motion(x, y);
        for (int i = 0; i < fingers; i++) {
            finger(i, SDL_ACTION_DOWN, x, y);
        }
        for (int i = 0; i < fingers; i++) {
            finger(i, SDL_ACTION_UP, x, y);
        }
    }

    private void finger(int pointerId, int action, float x, float y) {
        int w = mapper.surfaceWidth();
        int h = mapper.surfaceHeight();
        if (w <= 0 || h <= 0) {
            return;
        }
        // SDL expects normalised coordinates for touch, unlike mouse events.
        SDLActivity.onNativeTouch(touchDeviceId, pointerId, action, x / w, y / h, 1.0f);
    }

    private void motion(float x, float y) {
        SDLActivity.onNativeMouse(0, SDL_ACTION_MOVE, x, y, false);
    }

    /** Sends a wheel movement, in ticks, fractions included. */
    private void wheel(float ticks) {
        SDLActivity.onNativeMouse(0, SDL_ACTION_SCROLL, 0f, ticks, false);
    }

    private void key(int keyCode, boolean withAlt) {
        if (withAlt) {
            SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_ALT_LEFT);
        }
        SDLActivity.onNativeKeyDown(keyCode);
        SDLActivity.onNativeKeyUp(keyCode);
        if (withAlt) {
            SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_ALT_LEFT);
        }
    }

    // --- helpers ------------------------------------------------------------

    private void cancelLongPress() {
        handler.removeCallbacks(longPress);
    }

    /**
     * Average of the active pointers, skipping the one being lifted so a release
     * does not drag the centre sideways just as the gesture resolves.
     */
    private float[] centroid(MotionEvent event) {
        int skip = event.getActionMasked() == MotionEvent.ACTION_POINTER_UP
                ? event.getActionIndex() : -1;
        float sx = 0, sy = 0;
        int n = 0;
        for (int i = 0; i < event.getPointerCount(); i++) {
            if (i == skip) {
                continue;
            }
            sx += mapper.toSurfaceX(event.getRawX(i));
            sy += mapper.toSurfaceY(event.getRawY(i));
            n++;
        }
        if (n == 0) {
            return new float[] { startX, startY };
        }
        return new float[] { sx / n, sy / n };
    }
}
