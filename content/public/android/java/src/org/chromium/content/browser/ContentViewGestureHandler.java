// Copyright 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.content.browser;

import android.content.Context;
import android.os.Bundle;
import android.os.SystemClock;
import android.util.Log;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.ViewConfiguration;

import org.chromium.base.CommandLine;
import org.chromium.base.TraceEvent;
import org.chromium.content.browser.LongPressDetector.LongPressDelegate;
import org.chromium.content.browser.third_party.GestureDetector;
import org.chromium.content.browser.third_party.GestureDetector.OnDoubleTapListener;
import org.chromium.content.browser.third_party.GestureDetector.OnGestureListener;
import org.chromium.content.common.ContentSwitches;

import java.util.ArrayDeque;
import java.util.Deque;

/**
 * This class handles all MotionEvent handling done in ContentViewCore including the gesture
 * recognition. It sends all related native calls through the interface MotionEventDelegate.
 */
class ContentViewGestureHandler implements LongPressDelegate {

    private static final String TAG = "ContentViewGestureHandler";
    /**
     * Used for GESTURE_FLING_START x velocity
     */
    static final String VELOCITY_X = "Velocity X";
    /**
     * Used for GESTURE_FLING_START y velocity
     */
    static final String VELOCITY_Y = "Velocity Y";
    /**
     * Used for GESTURE_SCROLL_BY x distance (scroll offset of update)
     */
    static final String DISTANCE_X = "Distance X";
    /**
     * Used for GESTURE_SCROLL_BY y distance (scroll offset of update)
     */
    static final String DISTANCE_Y = "Distance Y";
    /**
     * Used for GESTURE_SCROLL_START delta X hint (movement triggering scroll)
     */
    static final String DELTA_HINT_X = "Delta Hint X";
    /**
     * Used for GESTURE_SCROLL_START delta Y hint (movement triggering scroll)
     */
    static final String DELTA_HINT_Y = "Delta Hint Y";
    /**
     * Used in GESTURE_SINGLE_TAP_CONFIRMED to check whether ShowPress has been called before.
     */
    static final String SHOW_PRESS = "ShowPress";
    /**
     * Used for GESTURE_PINCH_BY delta
     */
    static final String DELTA = "Delta";

    /**
     * Used by UMA stat for tracking accidental double tap navigations. Specifies the amount of
     * time after a double tap within which actions will be recorded to the UMA stat.
     */
    private static final long ACTION_AFTER_DOUBLE_TAP_WINDOW_MS = 5000;

    private final Bundle mExtraParamBundleSingleTap;
    private final Bundle mExtraParamBundleFling;
    private final Bundle mExtraParamBundleScroll;
    private final Bundle mExtraParamBundleScrollStart;
    private final Bundle mExtraParamBundleDoubleTapDragZoom;
    private final Bundle mExtraParamBundlePinchBy;
    private GestureDetector mGestureDetector;
    private final ZoomManager mZoomManager;
    private LongPressDetector mLongPressDetector;
    private OnGestureListener mListener;
    private OnDoubleTapListener mDoubleTapListener;
    private MotionEvent mCurrentDownEvent;
    private final MotionEventDelegate mMotionEventDelegate;

    // Queue of motion events.
    private final Deque<MotionEvent> mPendingMotionEvents = new ArrayDeque<MotionEvent>();

    // All events are forwarded to the GestureDetector, bypassing Javascript.
    private static final int NO_TOUCH_HANDLER = 0;

    // All events are forwarded as normal to Javascript, and if unconsumed to the GestureDetector.
    //     * Activated from the renderer by way of |hasTouchEventHandlers(true)|.
    private static final int HAS_TOUCH_HANDLER = 1;

    // Events in the current gesture are forwarded to the GestureDetector, bypassing Javascript.
    //     * Activated if the touch down for the current gesture had no Javascript consumer.
    private static final int NO_TOUCH_HANDLER_FOR_GESTURE = 2;

    // Events in the current gesture are forwarded to Javascript, and not to the GestureDetector.
    //     * Activated if *any* touch event in the current sequence was consumed by Javascript.
    private static final int JAVASCRIPT_CONSUMING_GESTURE = 3;

    private static final int TOUCH_HANDLING_STATE_DEFAULT = NO_TOUCH_HANDLER;

    private int mTouchHandlingState = TOUCH_HANDLING_STATE_DEFAULT;

    // Remember whether onShowPress() is called. If it is not, in onSingleTapConfirmed()
    // we will first show the press state, then trigger the click.
    private boolean mShowPressIsCalled;

    // Whether a sent GESTURE_TAP_DOWN event has yet to be accompanied by a corresponding
    // GESTURE_SINGLE_TAP_UP, GESTURE_SINGLE_TAP_CONFIRMED, GESTURE_TAP_CANCEL or
    // GESTURE_DOUBLE_TAP.
    private boolean mNeedsTapEndingEvent;

    // This flag is used for ignoring the remaining touch events, i.e., All the events until the
    // next ACTION_DOWN. This is automatically set to false on the next ACTION_DOWN.
    private boolean mIgnoreRemainingTouchEvents;

    // TODO(klobag): this is to avoid a bug in GestureDetector. With multi-touch,
    // mAlwaysInTapRegion is not reset. So when the last finger is up, onSingleTapUp()
    // will be mistakenly fired.
    private boolean mIgnoreSingleTap;

    // True from right before we send the first scroll event until the last finger is raised.
    private boolean mTouchScrolling;

    // TODO(wangxianzhu): For now it is true after a fling is started until the next
    // touch. Should reset it to false on end of fling if the UI is able to know when the
    // fling ends.
    private boolean mFlingMayBeActive;

    // Used to remove the touch slop from the initial scroll event in a scroll gesture.
    private boolean mSeenFirstScrollEvent;

    private boolean mPinchInProgress = false;

    // Guard against nested |trySendPendingEventsToNative()| loops.
    private boolean mSendingPendingEventsToNative = false;

    private static final int DOUBLE_TAP_TIMEOUT = ViewConfiguration.getDoubleTapTimeout();

    //On single tap this will store the x, y coordinates of the touch.
    private int mSingleTapX;
    private int mSingleTapY;

    // Indicate current double tap mode state.
    private int mDoubleTapMode = DOUBLE_TAP_MODE_NONE;

    // x, y coordinates for an Anchor on double tap drag zoom.
    private float mDoubleTapDragZoomAnchorX;
    private float mDoubleTapDragZoomAnchorY;

    // On double tap this will store the y coordinates of the touch.
    private float mDoubleTapY;

    // Double tap drag zoom sensitive (speed).
    private static final float DOUBLE_TAP_DRAG_ZOOM_SPEED = 0.005f;

    // Used to track the last rawX/Y coordinates for moves.  This gives absolute scroll distance.
    // Useful for full screen tracking.
    private float mLastRawX = 0;
    private float mLastRawY = 0;

    // Cache of square of the scaled touch slop so we don't have to calculate it on every touch.
    private int mScaledTouchSlopSquare;

    // Object that keeps track of and updates scroll snapping behavior.
    private final SnapScrollController mSnapScrollController;

    // Used to track the accumulated scroll error over time. This is used to remove the
    // rounding error we introduced by passing integers to webkit.
    private float mAccumulatedScrollErrorX = 0;
    private float mAccumulatedScrollErrorY = 0;

    // The page's viewport and scale sometimes allow us to disable double tap gesture detection,
    // according to the logic in ContentViewCore.onRenderCoordinatesUpdated().
    private boolean mShouldDisableDoubleTap;

    // Keeps track of the last long press event, if we end up opening a context menu, we would need
    // to potentially use the event to send GESTURE_TAP_CANCEL to remove ::active styling
    private MotionEvent mLastLongPressEvent;

    // Whether the click delay should always be disabled by sending clicks for double tap gestures.
    private final boolean mDisableClickDelay;

    // Used for tracking UMA ActionAfterDoubleTap to tell user's immediate
    // action after a double tap.
    private long mLastDoubleTapTimeMs;

    // Coordinates of the start of a touch sequence offered to native (i.e. the touchdown).
    private float mTouchDownToNativeX;
    private float mTouchDownToNativeY;

    // True iff a (potential) touchmove offered to native has exceeded the touch slop distance
    // OR it had multiple pointers.
    private boolean mTouchMoveToNativeConfirmed;

    static final int GESTURE_SHOW_PRESSED_STATE = 0;
    static final int GESTURE_DOUBLE_TAP = 1;
    static final int GESTURE_SINGLE_TAP_UP = 2;
    static final int GESTURE_SINGLE_TAP_CONFIRMED = 3;
    static final int GESTURE_SINGLE_TAP_UNCONFIRMED = 4;
    static final int GESTURE_LONG_PRESS = 5;
    static final int GESTURE_SCROLL_START = 6;
    static final int GESTURE_SCROLL_BY = 7;
    static final int GESTURE_SCROLL_END = 8;
    static final int GESTURE_FLING_START = 9;
    static final int GESTURE_FLING_CANCEL = 10;
    static final int GESTURE_PINCH_BEGIN = 11;
    static final int GESTURE_PINCH_BY = 12;
    static final int GESTURE_PINCH_END = 13;
    static final int GESTURE_TAP_CANCEL = 14;
    static final int GESTURE_LONG_TAP = 15;
    static final int GESTURE_TAP_DOWN = 16;

    // These have to be kept in sync with content/port/common/input_event_ack_state.h
    static final int INPUT_EVENT_ACK_STATE_UNKNOWN = 0;
    static final int INPUT_EVENT_ACK_STATE_CONSUMED = 1;
    static final int INPUT_EVENT_ACK_STATE_NOT_CONSUMED = 2;
    static final int INPUT_EVENT_ACK_STATE_NO_CONSUMER_EXISTS = 3;
    static final int INPUT_EVENT_ACK_STATE_IGNORED = 4;

    // Return values of sendPendingEventToNative();
    static final int EVENT_FORWARDED_TO_NATIVE = 0;
    static final int EVENT_DROPPED = 1;
    static final int EVENT_NOT_FORWARDED = 2;

    private final float mPxToDp;

    static final int DOUBLE_TAP_MODE_NONE = 0;
    static final int DOUBLE_TAP_MODE_DRAG_DETECTION_IN_PROGRESS = 1;
    static final int DOUBLE_TAP_MODE_DRAG_ZOOM = 2;
    static final int DOUBLE_TAP_MODE_DISABLED = 3;

    /**
     * This is an interface to handle MotionEvent related communication with the native side also
     * access some ContentView specific parameters.
     */
    public interface MotionEventDelegate {
        /**
         * Send a raw {@link MotionEvent} to the native side
         * @param timeMs Time of the event in ms.
         * @param action The action type for the event.
         * @param pts The TouchPoint array to be sent for the event.
         * @return Whether the event was sent to the native side successfully or not.
         */
        public boolean sendTouchEvent(long timeMs, int action, TouchPoint[] pts);

        /**
         * Send a gesture event to the native side.
         * @param type The type of the gesture event.
         * @param timeMs The time the gesture event occurred at.
         * @param x The x location for the gesture event.
         * @param y The y location for the gesture event.
         * @param extraParams A bundle that holds specific extra parameters for certain gestures.
         *                    This is read-only and should not be modified in this function.
         * Refer to gesture type definition for more information.
         * @return Whether the gesture was sent successfully.
         */
        boolean sendGesture(int type, long timeMs, int x, int y, Bundle extraParams);

        /**
         * Show the zoom picker UI.
         */
        public void invokeZoomPicker();

        /**
         * Send action after dobule tap for UMA stat tracking.
         * @param type The action that occured
         * @param clickDelayEnabled Whether the tap down delay is active
         */
        public void sendActionAfterDoubleTapUMA(int type, boolean clickDelayEnabled);

        /**
         * Send single tap UMA.
         * @param type The tap type: delayed or undelayed
         */
        public void sendSingleTapUMA(int type);
    }

    ContentViewGestureHandler(
            Context context, MotionEventDelegate delegate, ZoomManager zoomManager) {
        mExtraParamBundleSingleTap = new Bundle();
        mExtraParamBundleFling = new Bundle();
        mExtraParamBundleScroll = new Bundle();
        mExtraParamBundleScrollStart = new Bundle();
        mExtraParamBundleDoubleTapDragZoom = new Bundle();
        mExtraParamBundlePinchBy = new Bundle();

        mLongPressDetector = new LongPressDetector(context, this);
        mMotionEventDelegate = delegate;
        mZoomManager = zoomManager;
        mSnapScrollController = new SnapScrollController(context, mZoomManager);
        mPxToDp = 1.0f / context.getResources().getDisplayMetrics().density;

        mDisableClickDelay = CommandLine.isInitialized() &&
                CommandLine.getInstance().hasSwitch(ContentSwitches.DISABLE_CLICK_DELAY);

        initGestureDetectors(context);
    }

    /**
     * Used to override the default long press detector, gesture detector and listener.
     * This is used for testing only.
     * @param longPressDetector The new LongPressDetector to be assigned.
     * @param gestureDetector The new GestureDetector to be assigned.
     * @param listener The new onGestureListener to be assigned.
     */
    void setTestDependencies(
            LongPressDetector longPressDetector, GestureDetector gestureDetector,
            OnGestureListener listener) {
        if (longPressDetector != null) mLongPressDetector = longPressDetector;
        if (gestureDetector != null) mGestureDetector = gestureDetector;
        if (listener != null) mListener = listener;
    }

    private void initGestureDetectors(final Context context) {
        final int scaledTouchSlop = ViewConfiguration.get(context).getScaledTouchSlop();
        mScaledTouchSlopSquare = scaledTouchSlop * scaledTouchSlop;
        try {
            TraceEvent.begin();
            GestureDetector.SimpleOnGestureListener listener =
                new GestureDetector.SimpleOnGestureListener() {
                    @Override
                    public boolean onDown(MotionEvent e) {
                        mShowPressIsCalled = false;
                        mIgnoreSingleTap = false;
                        mTouchScrolling = false;
                        mSeenFirstScrollEvent = false;
                        mLastRawX = e.getRawX();
                        mLastRawY = e.getRawY();
                        mAccumulatedScrollErrorX = 0;
                        mAccumulatedScrollErrorY = 0;
                        mNeedsTapEndingEvent = false;
                        if (sendMotionEventAsGesture(GESTURE_TAP_DOWN, e, null)) {
                            mNeedsTapEndingEvent = true;
                        }
                        // Return true to indicate that we want to handle touch
                        return true;
                    }

                    @Override
                    public boolean onScroll(MotionEvent e1, MotionEvent e2,
                            float rawDistanceX, float rawDistanceY) {
                        assert e1.getEventTime() <= e2.getEventTime();
                        float distanceX = rawDistanceX;
                        float distanceY = rawDistanceY;
                        if (!mSeenFirstScrollEvent) {
                            // Remove the touch slop region from the first scroll event to avoid a
                            // jump.
                            mSeenFirstScrollEvent = true;
                            double distance = Math.sqrt(
                                    distanceX * distanceX + distanceY * distanceY);
                            double epsilon = 1e-3;
                            if (distance > epsilon) {
                                double ratio = Math.max(0, distance - scaledTouchSlop) / distance;
                                distanceX *= ratio;
                                distanceY *= ratio;
                            }
                        }
                        mSnapScrollController.updateSnapScrollMode(distanceX, distanceY);
                        if (mSnapScrollController.isSnappingScrolls()) {
                            if (mSnapScrollController.isSnapHorizontal()) {
                                distanceY = 0;
                            } else {
                                distanceX = 0;
                            }
                        }

                        mLastRawX = e2.getRawX();
                        mLastRawY = e2.getRawY();
                        if (!mTouchScrolling) {
                            sendTapCancelIfNecessary(e1);
                            endFlingIfNecessary(e2.getEventTime());
                            // Note that scroll start hints are in distance traveled, where
                            // scroll deltas are in the opposite direction.
                            mExtraParamBundleScrollStart.putInt(DELTA_HINT_X, (int) -rawDistanceX);
                            mExtraParamBundleScrollStart.putInt(DELTA_HINT_Y, (int) -rawDistanceY);
                            assert mExtraParamBundleScrollStart.size() == 2;
                            if (sendGesture(GESTURE_SCROLL_START, e2.getEventTime(),
                                        (int) e1.getX(), (int) e1.getY(),
                                        mExtraParamBundleScrollStart)) {
                                mTouchScrolling = true;
                            }
                        }

                        // distanceX and distanceY is the scrolling offset since last onScroll.
                        // Because we are passing integers to webkit, this could introduce
                        // rounding errors. The rounding errors will accumulate overtime.
                        // To solve this, we should be adding back the rounding errors each time
                        // when we calculate the new offset.
                        int x = (int) e2.getX();
                        int y = (int) e2.getY();
                        int dx = (int) (distanceX + mAccumulatedScrollErrorX);
                        int dy = (int) (distanceY + mAccumulatedScrollErrorY);
                        mAccumulatedScrollErrorX = distanceX + mAccumulatedScrollErrorX - dx;
                        mAccumulatedScrollErrorY = distanceY + mAccumulatedScrollErrorY - dy;

                        mExtraParamBundleScroll.putInt(DISTANCE_X, dx);
                        mExtraParamBundleScroll.putInt(DISTANCE_Y, dy);
                        assert mExtraParamBundleScroll.size() == 2;

                        if ((dx | dy) != 0) {
                            sendGesture(GESTURE_SCROLL_BY,
                                    e2.getEventTime(), x, y, mExtraParamBundleScroll);
                        }

                        mMotionEventDelegate.invokeZoomPicker();

                        return true;
                    }

                    @Override
                    public boolean onFling(MotionEvent e1, MotionEvent e2,
                            float velocityX, float velocityY) {
                        assert e1.getEventTime() <= e2.getEventTime();
                        if (mSnapScrollController.isSnappingScrolls()) {
                            if (mSnapScrollController.isSnapHorizontal()) {
                                velocityY = 0;
                            } else {
                                velocityX = 0;
                            }
                        }

                        fling(e2.getEventTime(), (int) e1.getX(0), (int) e1.getY(0),
                                        (int) velocityX, (int) velocityY);
                        return true;
                    }

                    @Override
                    public void onShowPress(MotionEvent e) {
                        mShowPressIsCalled = true;
                        sendMotionEventAsGesture(GESTURE_SHOW_PRESSED_STATE, e, null);
                    }

                    @Override
                    public boolean onSingleTapUp(MotionEvent e) {
                        if (isDistanceBetweenDownAndUpTooLong(e.getRawX(), e.getRawY())) {
                            sendTapCancelIfNecessary(e);
                            mIgnoreSingleTap = true;
                            return true;
                        }
                        // This is a hack to address the issue where user hovers
                        // over a link for longer than DOUBLE_TAP_TIMEOUT, then
                        // onSingleTapConfirmed() is not triggered. But we still
                        // want to trigger the tap event at UP. So we override
                        // onSingleTapUp() in this case. This assumes singleTapUp
                        // gets always called before singleTapConfirmed.
                        if (!mIgnoreSingleTap && !mLongPressDetector.isInLongPress()) {
                            if (e.getEventTime() - e.getDownTime() > DOUBLE_TAP_TIMEOUT) {
                                float x = e.getX();
                                float y = e.getY();
                                if (sendTapEndingEventAsGesture(GESTURE_SINGLE_TAP_UP, e, null)) {
                                    mIgnoreSingleTap = true;
                                }
                                setClickXAndY((int) x, (int) y);

                                mMotionEventDelegate.sendSingleTapUMA(
                                        isDoubleTapDisabled() ?
                                        ContentViewCore.UMASingleTapType.UNDELAYED_TAP :
                                        ContentViewCore.UMASingleTapType.DELAYED_TAP);

                                return true;
                            } else if (isDoubleTapDisabled() || mDisableClickDelay) {
                                // If double tap has been disabled, there is no need to wait
                                // for the double tap timeout.
                                return onSingleTapConfirmed(e);
                            } else {
                                // Notify Blink about this tapUp event anyway,
                                // when none of the above conditions applied.
                                sendMotionEventAsGesture(GESTURE_SINGLE_TAP_UNCONFIRMED, e, null);
                            }
                        }

                        return triggerLongTapIfNeeded(e);
                    }

                    @Override
                    public boolean onSingleTapConfirmed(MotionEvent e) {
                        // Long taps in the edges of the screen have their events delayed by
                        // ContentViewHolder for tab swipe operations. As a consequence of the delay
                        // this method might be called after receiving the up event.
                        // These corner cases should be ignored.
                        if (mLongPressDetector.isInLongPress() || mIgnoreSingleTap) return true;

                        mMotionEventDelegate.sendSingleTapUMA(
                                isDoubleTapDisabled() ?
                                ContentViewCore.UMASingleTapType.UNDELAYED_TAP :
                                ContentViewCore.UMASingleTapType.DELAYED_TAP);

                        int x = (int) e.getX();
                        int y = (int) e.getY();
                        mExtraParamBundleSingleTap.putBoolean(SHOW_PRESS, mShowPressIsCalled);
                        assert mExtraParamBundleSingleTap.size() == 1;
                        if (sendTapEndingEventAsGesture(GESTURE_SINGLE_TAP_CONFIRMED, e,
                                mExtraParamBundleSingleTap)) {
                            mIgnoreSingleTap = true;
                        }

                        setClickXAndY(x, y);
                        return true;
                    }

                    @Override
                    public boolean onDoubleTapEvent(MotionEvent e) {
                        switch (e.getActionMasked()) {
                            case MotionEvent.ACTION_DOWN:
                                sendTapCancelIfNecessary(e);
                                mDoubleTapDragZoomAnchorX = e.getX();
                                mDoubleTapDragZoomAnchorY = e.getY();
                                mDoubleTapMode = DOUBLE_TAP_MODE_DRAG_DETECTION_IN_PROGRESS;
                                break;
                            case MotionEvent.ACTION_MOVE:
                                if (mDoubleTapMode
                                        == DOUBLE_TAP_MODE_DRAG_DETECTION_IN_PROGRESS) {
                                    float distanceX = mDoubleTapDragZoomAnchorX - e.getX();
                                    float distanceY = mDoubleTapDragZoomAnchorY - e.getY();

                                    // Begin double tap drag zoom mode if the move distance is
                                    // further than the threshold.
                                    if (isDistanceGreaterThanTouchSlop(distanceX, distanceY)) {
                                        sendTapCancelIfNecessary(e);
                                        mExtraParamBundleScrollStart.putInt(DELTA_HINT_X,
                                                (int) -distanceX);
                                        mExtraParamBundleScrollStart.putInt(DELTA_HINT_Y,
                                                (int) -distanceY);
                                        assert mExtraParamBundleScrollStart.size() == 2;
                                        sendGesture(GESTURE_SCROLL_START, e.getEventTime(),
                                                (int) e.getX(), (int) e.getY(),
                                                mExtraParamBundleScrollStart);
                                        pinchBegin(e.getEventTime(),
                                                Math.round(mDoubleTapDragZoomAnchorX),
                                                Math.round(mDoubleTapDragZoomAnchorY));
                                        mDoubleTapMode = DOUBLE_TAP_MODE_DRAG_ZOOM;
                                    }
                                } else if (mDoubleTapMode == DOUBLE_TAP_MODE_DRAG_ZOOM) {
                                    assert mExtraParamBundleDoubleTapDragZoom.isEmpty();
                                    sendGesture(GESTURE_SCROLL_BY, e.getEventTime(),
                                            (int) e.getX(), (int) e.getY(),
                                            mExtraParamBundleDoubleTapDragZoom);

                                    float dy = mDoubleTapY - e.getY();
                                    pinchBy(e.getEventTime(),
                                            Math.round(mDoubleTapDragZoomAnchorX),
                                            Math.round(mDoubleTapDragZoomAnchorY),
                                            (float) Math.pow(dy > 0 ?
                                                    1.0f - DOUBLE_TAP_DRAG_ZOOM_SPEED :
                                                    1.0f + DOUBLE_TAP_DRAG_ZOOM_SPEED,
                                                    Math.abs(dy * mPxToDp)));
                                }
                                break;
                            case MotionEvent.ACTION_UP:
                                if (mDoubleTapMode != DOUBLE_TAP_MODE_DRAG_ZOOM) {
                                    // Normal double tap gesture.
                                    sendTapEndingEventAsGesture(GESTURE_DOUBLE_TAP, e, null);
                                }
                                endDoubleTapDragIfNecessary(e);
                                break;
                            case MotionEvent.ACTION_CANCEL:
                                sendTapCancelIfNecessary(e);
                                endDoubleTapDragIfNecessary(e);
                                break;
                            default:
                                break;
                        }
                        mDoubleTapY = e.getY();
                        return true;
                    }

                    @Override
                    public void onLongPress(MotionEvent e) {
                        if (!mZoomManager.isScaleGestureDetectionInProgress() &&
                                (mDoubleTapMode == DOUBLE_TAP_MODE_NONE ||
                                 isDoubleTapDisabled())) {
                            mLastLongPressEvent = e;
                            sendMotionEventAsGesture(GESTURE_LONG_PRESS, e, null);
                        }
                    }

                    /**
                     * This method inspects the distance between where the user started touching
                     * the surface, and where she released. If the points are too far apart, we
                     * should assume that the web page has consumed the scroll-events in-between,
                     * and as such, this should not be considered a single-tap.
                     *
                     * We use the Android frameworks notion of how far a touch can wander before
                     * we think the user is scrolling.
                     *
                     * @param x the new x coordinate
                     * @param y the new y coordinate
                     * @return true if the distance is too long to be considered a single tap
                     */
                    private boolean isDistanceBetweenDownAndUpTooLong(float x, float y) {
                        return isDistanceGreaterThanTouchSlop(mLastRawX - x, mLastRawY - y);
                    }
                };
            mListener = listener;
            mDoubleTapListener = listener;
            mGestureDetector = new GestureDetector(context, listener);
            mGestureDetector.setIsLongpressEnabled(false);
        } finally {
            TraceEvent.end();
        }
    }

    /**
     * @return LongPressDetector handling setting up timers for and canceling LongPress gestures.
     */
    LongPressDetector getLongPressDetector() {
        return mLongPressDetector;
    }

    /**
     * @param event Start a LongPress gesture event from the listener.
     */
    @Override
    public void onLongPress(MotionEvent event) {
        mListener.onLongPress(event);
    }

    /**
     * Cancels any ongoing LongPress timers.
     */
    void cancelLongPress() {
        mLongPressDetector.cancelLongPress();
    }

    /**
     * Fling the ContentView from the current position.
     * @param x Fling touch starting position
     * @param y Fling touch starting position
     * @param velocityX Initial velocity of the fling (X) measured in pixels per second.
     * @param velocityY Initial velocity of the fling (Y) measured in pixels per second.
     */
    void fling(long timeMs, int x, int y, int velocityX, int velocityY) {
        endFlingIfNecessary(timeMs);

        if (velocityX == 0 && velocityY == 0) {
            endTouchScrollIfNecessary(timeMs, true);
            return;
        }

        if (!mTouchScrolling) {
            // The native side needs a GESTURE_SCROLL_BEGIN before GESTURE_FLING_START
            // to send the fling to the correct target. Send if it has not sent.
            // The distance traveled in one second is a reasonable scroll start hint.
            mExtraParamBundleScrollStart.putInt(DELTA_HINT_X, velocityX);
            mExtraParamBundleScrollStart.putInt(DELTA_HINT_Y, velocityY);
            assert mExtraParamBundleScrollStart.size() == 2;
            sendGesture(GESTURE_SCROLL_START, timeMs, x, y, mExtraParamBundleScrollStart);
        }
        endTouchScrollIfNecessary(timeMs, false);

        mFlingMayBeActive = true;

        mExtraParamBundleFling.putInt(VELOCITY_X, velocityX);
        mExtraParamBundleFling.putInt(VELOCITY_Y, velocityY);
        assert mExtraParamBundleFling.size() == 2;
        sendGesture(GESTURE_FLING_START, timeMs, x, y, mExtraParamBundleFling);
    }

    /**
     * Send a GESTURE_FLING_CANCEL event if necessary.
     * @param timeMs The time in ms for the event initiating this gesture.
     */
    void endFlingIfNecessary(long timeMs) {
        if (!mFlingMayBeActive) return;
        mFlingMayBeActive = false;
        sendGesture(GESTURE_FLING_CANCEL, timeMs, 0, 0, null);
    }

    /**
     * End DOUBLE_TAP_MODE_DRAG_ZOOM by sending GESTURE_SCROLL_END and GESTURE_PINCH_END events.
     * @param event A hint event that its x, y, and eventTime will be used for the ending events
     *              to send. This argument is an optional and can be null.
     */
    void endDoubleTapDragIfNecessary(MotionEvent event) {
        assert event != null;
        if (!isDoubleTapActive()) return;
        if (mDoubleTapMode == DOUBLE_TAP_MODE_DRAG_ZOOM) {
            pinchEnd(event.getEventTime());
            sendGesture(GESTURE_SCROLL_END, event.getEventTime(),
                    (int) event.getX(), (int) event.getY(), null);
        }
        mDoubleTapMode = DOUBLE_TAP_MODE_NONE;
        updateDoubleTapListener();
    }

    /**
     * Reset touch scroll flag and optionally send a GESTURE_SCROLL_END event if necessary.
     * @param timeMs The time in ms for the event initiating this gesture.
     * @param sendScrollEndEvent Whether to send GESTURE_SCROLL_END event.
     */
    private void endTouchScrollIfNecessary(long timeMs, boolean sendScrollEndEvent) {
        if (!mTouchScrolling) return;
        mTouchScrolling = false;
        if (sendScrollEndEvent) {
            sendGesture(GESTURE_SCROLL_END, timeMs, 0, 0, null);
        }
    }

    /**
     * @return Whether native is tracking a scroll.
     */
    boolean isNativeScrolling() {
        // TODO(wangxianzhu): Also return true when fling is active once the UI knows exactly when
        // the fling ends.
        return mTouchScrolling;
    }

    /**
     * @return Whether native is tracking a pinch (i.e. between sending GESTURE_PINCH_BEGIN and
     *         GESTURE_PINCH_END).
     */
    boolean isNativePinching() {
        return mPinchInProgress;
    }

    /**
     * Starts a pinch gesture.
     * @param timeMs The time in ms for the event initiating this gesture.
     * @param x The x coordinate for the event initiating this gesture.
     * @param y The x coordinate for the event initiating this gesture.
     */
    void pinchBegin(long timeMs, int x, int y) {
        sendGesture(GESTURE_PINCH_BEGIN, timeMs, x, y, null);
    }

    /**
     * Pinch by a given percentage.
     * @param timeMs The time in ms for the event initiating this gesture.
     * @param anchorX The x coordinate for the anchor point to be used in pinch.
     * @param anchorY The y coordinate for the anchor point to be used in pinch.
     * @param delta The percentage to pinch by.
     */
    void pinchBy(long timeMs, int anchorX, int anchorY, float delta) {
        mExtraParamBundlePinchBy.putFloat(DELTA, delta);
        assert mExtraParamBundlePinchBy.size() == 1;
        sendGesture(GESTURE_PINCH_BY, timeMs, anchorX, anchorY, mExtraParamBundlePinchBy);
        mPinchInProgress = true;
    }

    /**
     * End a pinch gesture.
     * @param timeMs The time in ms for the event initiating this gesture.
     */
    void pinchEnd(long timeMs) {
        sendGesture(GESTURE_PINCH_END, timeMs, 0, 0, null);
        mPinchInProgress = false;
    }

    /**
     * Ignore singleTap gestures.
     */
    void setIgnoreSingleTap(boolean value) {
        mIgnoreSingleTap = value;
    }

    private void setClickXAndY(int x, int y) {
        mSingleTapX = x;
        mSingleTapY = y;
    }

    /**
     * @return The x coordinate for the last point that a singleTap gesture was initiated from.
     */
    public int getSingleTapX()  {
        return mSingleTapX;
    }

    /**
     * @return The y coordinate for the last point that a singleTap gesture was initiated from.
     */
    public int getSingleTapY()  {
        return mSingleTapY;
    }

    /**
     * Cancel the current touch event sequence by sending ACTION_CANCEL and ignore all the
     * subsequent events until the next ACTION_DOWN.
     *
     * One example usecase is stop processing the touch events when showing context popup menu.
     */
    public void setIgnoreRemainingTouchEvents() {
        onTouchEvent(obtainActionCancelMotionEvent());
        mIgnoreRemainingTouchEvents = true;
    }

    /**
     * Handle the incoming MotionEvent.
     * @return Whether the event was handled.
     */
    boolean onTouchEvent(MotionEvent event) {
        try {
            TraceEvent.begin("onTouchEvent");

            if (mIgnoreRemainingTouchEvents) {
                if (event.getActionMasked() == MotionEvent.ACTION_DOWN) {
                    mIgnoreRemainingTouchEvents = false;
                } else {
                    return false;
                }
            }

            mLongPressDetector.cancelLongPressIfNeeded(event);
            // Notify native that scrolling has stopped whenever a down action is processed prior to
            // passing the event to native as it will drop them as an optimization if scrolling is
            // enabled.  Ending the fling ensures scrolling has stopped as well as terminating the
            // current fling if applicable.
            if (event.getActionMasked() == MotionEvent.ACTION_DOWN) {
                endFlingIfNecessary(event.getEventTime());
            }

            return queueEvent(event);
        } finally {
            TraceEvent.end("onTouchEvent");
        }
    }

    /**
     * Handle content view losing focus -- ensure that any remaining active state is removed.
     */
    void onWindowFocusLost() {
        if (mLongPressDetector.isInLongPress() && mLastLongPressEvent != null) {
            sendTapCancelIfNecessary(mLastLongPressEvent);
        }
    }

    private MotionEvent obtainActionCancelMotionEvent() {
        MotionEvent me = MotionEvent.obtain(
                SystemClock.uptimeMillis(),
                SystemClock.uptimeMillis(),
                MotionEvent.ACTION_CANCEL, 0.0f,  0.0f,  0);
        me.setSource(mCurrentDownEvent != null ?
                mCurrentDownEvent.getSource() : InputDevice.SOURCE_CLASS_POINTER);
        return me;
    }

    /**
     * Resets gesture handlers state; called on didStartLoading().
     * Note that this does NOT clear the pending motion events queue;
     * it gets cleared in hasTouchEventHandlers() called from WebKit
     * FrameLoader::transitionToCommitted iff the page ever had touch handlers.
     */
    void resetGestureHandlers() {
        MotionEvent me = obtainActionCancelMotionEvent();
        mGestureDetector.onTouchEvent(me);
        mZoomManager.processTouchEvent(me);
        me.recycle();
        mLongPressDetector.cancelLongPress();
    }

    /**
     * Sets the flag indicating that the content has registered listeners for touch events.
     */
    void hasTouchEventHandlers(boolean hasTouchHandlers) {
        if (hasTouchHandlers) {
            // If no touch handler was previously registered, ensure that we
            // don't send a partial gesture to Javascript.
            if (mTouchHandlingState == NO_TOUCH_HANDLER)
                mTouchHandlingState = NO_TOUCH_HANDLER_FOR_GESTURE;
        } else {
            // When mainframe is loading, FrameLoader::transitionToCommitted will
            // call this method with |hasTouchHandlers| of false. We use this as
            // an indicator to clear the pending motion events so that events from
            // the previous page will not be carried over to the new page.
            mTouchHandlingState = NO_TOUCH_HANDLER;
            mPendingMotionEvents.clear();
        }
    }

    /**
     * Queues or coalesces |event| into the pending queue.
     *   - If there are no touch handlers, |event| will skip the queue and be processed immediately.
     *   - If there are no pending events, |event| will be sent to native or processed immediately,
     *     depending on the disposition of the current gesture sequence.
     * @return Whether the event was queued OR processed.
     */
    private boolean queueEvent(MotionEvent event) {
        if (mTouchHandlingState == NO_TOUCH_HANDLER) {
            assert mPendingMotionEvents.isEmpty();
            return processTouchEvent(event);
        }

        if (event.getActionMasked() == MotionEvent.ACTION_MOVE) {
            // Avoid flooding the renderer process with move events: if the previous pending
            // command is also a move (common case) that has not yet been forwarded, skip sending
            //  this event to the webkit side and collapse it into the pending event.
            MotionEvent previousEvent = mPendingMotionEvents.peekLast();
            if (previousEvent != null
                    && previousEvent != mPendingMotionEvents.peekFirst()
                    && previousEvent.getActionMasked() == MotionEvent.ACTION_MOVE
                    && previousEvent.getPointerCount() == event.getPointerCount()) {
                TraceEvent.instant("queueEvent:EventCoalesced",
                                   "QueueSize = " + mPendingMotionEvents.size());
                MotionEvent.PointerCoords[] coords =
                        new MotionEvent.PointerCoords[event.getPointerCount()];
                for (int i = 0; i < coords.length; ++i) {
                    coords[i] = new MotionEvent.PointerCoords();
                    event.getPointerCoords(i, coords[i]);
                }
                previousEvent.addBatch(event.getEventTime(), coords, event.getMetaState());
                return true;
            }
        }

        // Copy the event, as the original may get mutated after this method returns.
        MotionEvent clone = MotionEvent.obtain(event);
        mPendingMotionEvents.add(clone);
        if (mPendingMotionEvents.size() == 1) {
            trySendPendingEventsToNative();
        } else {
            TraceEvent.instant("queueEvent:EventQueued",
                               "QueueSize = " + mPendingMotionEvents.size());
        }
        return true;
    }

    private int sendPendingEventToNative() {
        MotionEvent event = mPendingMotionEvents.peekFirst();
        if (event == null) {
            assert false : "Cannot send from an empty pending event queue";
            return EVENT_NOT_FORWARDED;
        }
        assert mTouchHandlingState != NO_TOUCH_HANDLER;

        // The start of a new (multi)touch sequence will reset the touch handling state, and
        // should always be offered to Javascript (when there is any touch handler).
        if (event.getActionMasked() == MotionEvent.ACTION_DOWN) {
            mTouchHandlingState = HAS_TOUCH_HANDLER;
            mTouchMoveToNativeConfirmed = false;
            mTouchDownToNativeX = event.getX();
            mTouchDownToNativeY = event.getY();
            if (mCurrentDownEvent != null) recycleEvent(mCurrentDownEvent);
            mCurrentDownEvent = null;
        }

        if (mTouchHandlingState == NO_TOUCH_HANDLER_FOR_GESTURE) return EVENT_NOT_FORWARDED;

        if (event.getActionMasked() == MotionEvent.ACTION_MOVE) {
            if (!mTouchMoveToNativeConfirmed) {
                if (event.getPointerCount() > 1) {
                    mTouchMoveToNativeConfirmed = true;
                } else {
                    final float distanceX = event.getX() - mTouchDownToNativeX;
                    final float distanceY = event.getY() - mTouchDownToNativeY;
                    if (isDistanceGreaterThanTouchSlop(distanceX, distanceY)) {
                        mTouchMoveToNativeConfirmed = true;
                    }
                }
            }
            // If javascript has not yet prevent-defaulted the touch sequence, only send move events
            // if the move has exceeded the slop threshold OR there are multiple pointers.
            if (mTouchHandlingState != JAVASCRIPT_CONSUMING_GESTURE
                    && !mTouchMoveToNativeConfirmed) {
                return EVENT_DROPPED;
            }
        }

        TouchPoint[] pts = new TouchPoint[event.getPointerCount()];
        int type = TouchPoint.createTouchPoints(event, pts);

        if (type == TouchPoint.CONVERSION_ERROR) return EVENT_NOT_FORWARDED;

        if (mMotionEventDelegate.sendTouchEvent(event.getEventTime(), type, pts)) {
            return EVENT_FORWARDED_TO_NATIVE;
        }
        return EVENT_NOT_FORWARDED;
    }

    private boolean processTouchEvent(MotionEvent event) {
        final boolean wasTouchScrolling = mTouchScrolling;

        mSnapScrollController.setSnapScrollingMode(event);

        if (event.getActionMasked() == MotionEvent.ACTION_POINTER_DOWN) {
            endDoubleTapDragIfNecessary(event);
        }

        mLongPressDetector.cancelLongPressIfNeeded(event);
        mLongPressDetector.startLongPressTimerIfNeeded(event);

        // Use the framework's GestureDetector to detect pans and zooms not already
        // handled by the WebKit touch events gesture manager.
        boolean handled = false;
        if (canHandle(event)) {
            handled |= mGestureDetector.onTouchEvent(event);
            if (event.getAction() == MotionEvent.ACTION_DOWN) {
                mCurrentDownEvent = MotionEvent.obtain(event);
            }
        }

        handled |= mZoomManager.processTouchEvent(event);

        if (event.getAction() == MotionEvent.ACTION_UP
                || event.getAction() == MotionEvent.ACTION_CANCEL) {
            if (mCurrentDownEvent != null) recycleEvent(mCurrentDownEvent);
            mCurrentDownEvent = null;

            // "Last finger raised" could be an end to movement, but it should
            // only terminate scrolling if the event did not cause a fling.
            if (wasTouchScrolling && !handled) {
                endTouchScrollIfNecessary(event.getEventTime(), true);
            }
        }

        return handled;
    }

    /**
     * Respond to a MotionEvent being returned from the native side.
     * @param ackResult The status acknowledgment code.
     */
    void confirmTouchEvent(int ackResult) {
        try {
            TraceEvent.begin("confirmTouchEvent");

            if (mPendingMotionEvents.isEmpty()) {
                Log.w(TAG, "confirmTouchEvent with Empty pending list!");
                return;
            }
            assert mTouchHandlingState != NO_TOUCH_HANDLER;
            assert mTouchHandlingState != NO_TOUCH_HANDLER_FOR_GESTURE;

            MotionEvent ackedEvent = mPendingMotionEvents.removeFirst();
            switch (ackResult) {
                case INPUT_EVENT_ACK_STATE_UNKNOWN:
                    // This should never get sent.
                    assert (false);
                    break;
                case INPUT_EVENT_ACK_STATE_CONSUMED:
                case INPUT_EVENT_ACK_STATE_IGNORED:
                    if (mTouchHandlingState != JAVASCRIPT_CONSUMING_GESTURE
                            && ackedEvent.getActionMasked() != MotionEvent.ACTION_DOWN) {
                        sendTapCancelIfNecessary(ackedEvent);
                        resetGestureHandlers();
                    } else {
                        mZoomManager.passTouchEventThrough(ackedEvent);
                    }
                    mTouchHandlingState = JAVASCRIPT_CONSUMING_GESTURE;
                    trySendPendingEventsToNative();
                    break;
                case INPUT_EVENT_ACK_STATE_NOT_CONSUMED:
                    if (mTouchHandlingState != JAVASCRIPT_CONSUMING_GESTURE) {
                        processTouchEvent(ackedEvent);
                    }
                    trySendPendingEventsToNative();
                    break;
                case INPUT_EVENT_ACK_STATE_NO_CONSUMER_EXISTS:
                    if (mTouchHandlingState != JAVASCRIPT_CONSUMING_GESTURE) {
                        processTouchEvent(ackedEvent);
                    }
                    if (ackedEvent.getActionMasked() == MotionEvent.ACTION_DOWN) {
                        drainAllPendingEventsUntilNextDown();
                    } else {
                        trySendPendingEventsToNative();
                    }
                    break;
                default:
                    break;
            }

            mLongPressDetector.cancelLongPressIfNeeded(mPendingMotionEvents.iterator());
            recycleEvent(ackedEvent);
        } finally {
            TraceEvent.end("confirmTouchEvent");
        }
    }

    private void trySendPendingEventsToNative() {
        // Avoid nested send loops (possible when acks are synchronous), instead
        // relying on the top-most call to dispatch any queued events.
        if (mSendingPendingEventsToNative) return;
        try {
            mSendingPendingEventsToNative = true;
            while (!mPendingMotionEvents.isEmpty()) {
                int forward = sendPendingEventToNative();
                if (forward == EVENT_FORWARDED_TO_NATIVE) break;

                // Even though we missed sending one event to native, as long as we haven't
                // received INPUT_EVENT_ACK_STATE_NO_CONSUMER_EXISTS, we should keep sending
                // events on the queue to native.
                MotionEvent event = mPendingMotionEvents.removeFirst();
                if (mTouchHandlingState != JAVASCRIPT_CONSUMING_GESTURE
                        && forward != EVENT_DROPPED) {
                    processTouchEvent(event);
                }
                recycleEvent(event);
            }
        } finally {
            mSendingPendingEventsToNative = false;
        }
    }

    private void drainAllPendingEventsUntilNextDown() {
        assert mTouchHandlingState == HAS_TOUCH_HANDLER;
        mTouchHandlingState = NO_TOUCH_HANDLER_FOR_GESTURE;

        // Now process all events that are in the queue until the next down event.
        MotionEvent nextEvent = mPendingMotionEvents.peekFirst();
        while (nextEvent != null && nextEvent.getActionMasked() != MotionEvent.ACTION_DOWN) {
            processTouchEvent(nextEvent);
            mPendingMotionEvents.removeFirst();
            recycleEvent(nextEvent);
            nextEvent = mPendingMotionEvents.peekFirst();
        }

        trySendPendingEventsToNative();
    }

    private void recycleEvent(MotionEvent event) {
        event.recycle();
    }

    private boolean sendMotionEventAsGesture(
            int type, MotionEvent event, Bundle extraParams) {
        return sendGesture(type, event.getEventTime(),
            (int) event.getX(), (int) event.getY(), extraParams);
    }

    private boolean sendGesture(
            int type, long timeMs, int x, int y, Bundle extraParams) {
        assert timeMs != 0;
        updateDoubleTapUmaTimer();

        if (type == GESTURE_DOUBLE_TAP) reportDoubleTap();

        return mMotionEventDelegate.sendGesture(type, timeMs, x, y, extraParams);
    }

    private boolean sendTapEndingEventAsGesture(int type, MotionEvent e, Bundle extraParams) {
        if (!sendMotionEventAsGesture(type, e, extraParams)) return false;
        mNeedsTapEndingEvent = false;
        return true;
    }

    private void sendTapCancelIfNecessary(MotionEvent e) {
        if (!mNeedsTapEndingEvent) return;
        if (!sendTapEndingEventAsGesture(GESTURE_TAP_CANCEL, e, null)) return;
        mLastLongPressEvent = null;
    }

    /**
     * @return Whether the ContentViewGestureHandler can handle a MotionEvent right now. True only
     * if it's the start of a new stream (ACTION_DOWN), or a continuation of the current stream.
     */
    boolean canHandle(MotionEvent ev) {
        return ev.getAction() == MotionEvent.ACTION_DOWN ||
                (mCurrentDownEvent != null && mCurrentDownEvent.getDownTime() == ev.getDownTime());
    }

    /**
     * @return Whether the event can trigger a LONG_TAP gesture. True when it can and the event
     * will be consumed.
     */
    boolean triggerLongTapIfNeeded(MotionEvent ev) {
        if (mLongPressDetector.isInLongPress() && ev.getAction() == MotionEvent.ACTION_UP &&
                !mZoomManager.isScaleGestureDetectionInProgress()) {
            sendTapCancelIfNecessary(ev);
            sendMotionEventAsGesture(GESTURE_LONG_TAP, ev, null);
            return true;
        }
        return false;
    }

    /**
     * This is for testing only.
     * @return The first motion event on the pending motion events queue.
     */
    MotionEvent peekFirstInPendingMotionEventsForTesting() {
        return mPendingMotionEvents.peekFirst();
    }

    /**
     * This is for testing only.
     * @return The number of motion events on the pending motion events queue.
     */
    int getNumberOfPendingMotionEventsForTesting() {
        return mPendingMotionEvents.size();
    }

    /**
     * This is for testing only.
     * Sends a show pressed state gesture through mListener. This should always be called after
     * a down event;
     */
    void sendShowPressedStateGestureForTesting() {
        if (mCurrentDownEvent == null) return;
        mListener.onShowPress(mCurrentDownEvent);
    }

    /**
     * This is for testing only.
     * @return Whether a sent TapDown event has been accompanied by a tap-ending event.
     */
    boolean needsTapEndingEventForTesting() {
        return mNeedsTapEndingEvent;
    }

    /**
     * Update whether double-tap gestures are supported. This allows
     * double-tap gesture suppression independent of whether or not the page's
     * viewport and scale would normally prevent double-tap.
     * Note: This should never be called while a double-tap gesture is in progress.
     * @param supportDoubleTap Whether double-tap gestures are supported.
     */
    public void updateDoubleTapSupport(boolean supportDoubleTap) {
        assert !isDoubleTapActive();
        int doubleTapMode = supportDoubleTap ?
                DOUBLE_TAP_MODE_NONE : DOUBLE_TAP_MODE_DISABLED;
        if (mDoubleTapMode == doubleTapMode) return;
        mDoubleTapMode = doubleTapMode;
        updateDoubleTapListener();
    }

    /**
     * Update whether double-tap gesture detection should be suppressed due to
     * the viewport or scale of the current page. Suppressing double-tap gesture
     * detection allows for rapid and responsive single-tap gestures.
     * @param shouldDisableDoubleTap Whether double-tap should be suppressed.
     */
    public void updateShouldDisableDoubleTap(boolean shouldDisableDoubleTap) {
        if (mShouldDisableDoubleTap == shouldDisableDoubleTap) return;
        mShouldDisableDoubleTap = shouldDisableDoubleTap;
        updateDoubleTapListener();
    }

    private boolean isDoubleTapDisabled() {
        return mDoubleTapMode == DOUBLE_TAP_MODE_DISABLED || mShouldDisableDoubleTap;
    }

    private boolean isDoubleTapActive() {
        return mDoubleTapMode != DOUBLE_TAP_MODE_DISABLED &&
               mDoubleTapMode != DOUBLE_TAP_MODE_NONE;
    }

    private void updateDoubleTapListener() {
        if (isDoubleTapDisabled()) {
            // Defer nulling the DoubleTapListener until the double tap gesture is complete.
            if (isDoubleTapActive()) return;
            mGestureDetector.setOnDoubleTapListener(null);
        } else {
            mGestureDetector.setOnDoubleTapListener(mDoubleTapListener);
        }
    }

    private void reportDoubleTap() {
        // Make sure repeated double taps don't get silently dropped from
        // the statistics.
        if (mLastDoubleTapTimeMs > 0) {
            mMotionEventDelegate.sendActionAfterDoubleTapUMA(
                    ContentViewCore.UMAActionAfterDoubleTap.NO_ACTION, !mDisableClickDelay);
        }

        mLastDoubleTapTimeMs = SystemClock.uptimeMillis();
    }

    /**
     * Update the UMA stat tracking accidental double tap navigations with a user action.
     * @param type The action the user performed, one of the UMAActionAfterDoubleTap values
     *             defined in ContentViewCore.
     */
    public void reportActionAfterDoubleTapUMA(int type) {
        updateDoubleTapUmaTimer();

        if (mLastDoubleTapTimeMs == 0) return;

        long nowMs = SystemClock.uptimeMillis();
        if ((nowMs - mLastDoubleTapTimeMs) < ACTION_AFTER_DOUBLE_TAP_WINDOW_MS) {
            mMotionEventDelegate.sendActionAfterDoubleTapUMA(type, !mDisableClickDelay);
            mLastDoubleTapTimeMs = 0;
        }
    }

    // Watch for the UMA "action after double tap" timer expiring and reset
    // the timer if necessary.
    private void updateDoubleTapUmaTimer() {
        if (mLastDoubleTapTimeMs == 0) return;

        long nowMs = SystemClock.uptimeMillis();
        if ((nowMs - mLastDoubleTapTimeMs) >= ACTION_AFTER_DOUBLE_TAP_WINDOW_MS) {
            // Time expired, user took no action (that we care about).
            mMotionEventDelegate.sendActionAfterDoubleTapUMA(
                    ContentViewCore.UMAActionAfterDoubleTap.NO_ACTION, !mDisableClickDelay);
            mLastDoubleTapTimeMs = 0;
        }
    }

    private boolean isDistanceGreaterThanTouchSlop(float distanceX, float distanceY) {
        return distanceX * distanceX + distanceY * distanceY > mScaledTouchSlopSquare;
    }
}
