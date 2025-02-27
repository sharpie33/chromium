// Copyright 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.weblayer_private;

import android.content.Context;
import android.content.res.Configuration;
import android.graphics.Rect;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.util.SparseArray;
import android.view.DragEvent;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.View.OnSystemUiVisibilityChangeListener;
import android.view.ViewGroup.OnHierarchyChangeListener;
import android.view.ViewStructure;
import android.view.accessibility.AccessibilityNodeProvider;
import android.view.autofill.AutofillValue;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.widget.RelativeLayout;

import org.chromium.base.ObserverList;
import org.chromium.base.TraceEvent;
import org.chromium.base.compat.ApiHelperForO;
import org.chromium.components.autofill.AutofillProvider;
import org.chromium.content_public.browser.ImeAdapter;
import org.chromium.content_public.browser.RenderCoordinates;
import org.chromium.content_public.browser.SmartClipProvider;
import org.chromium.content_public.browser.ViewEventSink;
import org.chromium.content_public.browser.WebContents;
import org.chromium.content_public.browser.WebContentsAccessibility;
import org.chromium.ui.base.EventForwarder;
import org.chromium.ui.base.EventOffsetHandler;

/**
 * The containing view for {@link WebContents} that exists in the Android UI hierarchy and exposes
 * the various {@link View} functionality to it.
 */
public class ContentView extends RelativeLayout
        implements ViewEventSink.InternalAccessDelegate, SmartClipProvider,
                   OnHierarchyChangeListener, OnSystemUiVisibilityChangeListener {
    private static final String TAG = "ContentView";

    // Default value to signal that the ContentView's size need not be overridden.
    public static final int DEFAULT_MEASURE_SPEC =
            MeasureSpec.makeMeasureSpec(0, MeasureSpec.UNSPECIFIED);

    private WebContents mWebContents;
    private AutofillProvider mAutofillProvider;
    private final ObserverList<OnHierarchyChangeListener> mHierarchyChangeListeners =
            new ObserverList<>();
    private final ObserverList<OnSystemUiVisibilityChangeListener> mSystemUiChangeListeners =
            new ObserverList<>();

    /**
     * The desired size of this view in {@link MeasureSpec}. Set by the host
     * when it should be different from that of the parent.
     */
    private int mDesiredWidthMeasureSpec = DEFAULT_MEASURE_SPEC;
    private int mDesiredHeightMeasureSpec = DEFAULT_MEASURE_SPEC;

    private EventOffsetHandler mEventOffsetHandler;

    /**
     * Constructs a new ContentView for the appropriate Android version.
     * @param context The Context the view is running in, through which it can
     *                access the current theme, resources, etc.
     * @param webContents The WebContents managing this content view.
     * @return an instance of a ContentView.
     */
    public static ContentView createContentView(
            Context context, EventOffsetHandler eventOffsetHandler) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            return new ContentViewApi26(context, eventOffsetHandler);
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            return new ContentViewApi23(context, eventOffsetHandler);
        }
        return new ContentView(context, eventOffsetHandler);
    }

    /**
     * Creates an instance of a ContentView.
     * @param context The Context the view is running in, through which it can
     *                access the current theme, resources, etc.
     * @param webContents A pointer to the WebContents managing this content view.
     */
    ContentView(Context context, EventOffsetHandler eventOffsetHandler) {
        super(context, null, android.R.attr.webViewStyle);

        if (getScrollBarStyle() == View.SCROLLBARS_INSIDE_OVERLAY) {
            setHorizontalScrollBarEnabled(false);
            setVerticalScrollBarEnabled(false);
        }

        mEventOffsetHandler = eventOffsetHandler;

        setFocusable(true);
        setFocusableInTouchMode(true);

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            ApiHelperForO.setDefaultFocusHighlightEnabled(this, false);
        }

        setOnHierarchyChangeListener(this);
        setOnSystemUiVisibilityChangeListener(this);

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            // The Autofill system-level infrastructure has heuristics for which Views it considers
            // important for autofill; only these Views will be queried for their autofill
            // structure on notifications that a new (virtual) View was entered. By default,
            // RelativeLayout is not considered important for autofill. Thus, for ContentView to be
            // queried for its autofill structure, we must explicitly inform the autofill system
            // that this View is important for autofill.
            setImportantForAutofill(View.IMPORTANT_FOR_AUTOFILL_YES);
        }
    }

    protected WebContentsAccessibility getWebContentsAccessibility() {
        return mWebContents != null && !mWebContents.isDestroyed()
                ? WebContentsAccessibility.fromWebContents(mWebContents)
                : null;
    }

    protected AutofillProvider getAutofillProvider() {
        return mAutofillProvider;
    }

    public void setWebContents(WebContents webContents) {
        boolean wasFocused = isFocused();
        boolean wasWindowFocused = hasWindowFocus();
        boolean wasAttached = isAttachedToWindow();
        if (wasFocused) onFocusChanged(false, View.FOCUS_FORWARD, null);
        if (wasWindowFocused) onWindowFocusChanged(false);
        if (wasAttached) onDetachedFromWindow();
        mWebContents = webContents;
        if (wasFocused) onFocusChanged(true, View.FOCUS_FORWARD, null);
        if (wasWindowFocused) onWindowFocusChanged(true);
        if (wasAttached) onAttachedToWindow();
    }

    public void setAutofillProvider(AutofillProvider autofillProvider) {
        mAutofillProvider = autofillProvider;
    }

    @Override
    public boolean performAccessibilityAction(int action, Bundle arguments) {
        WebContentsAccessibility wcax = getWebContentsAccessibility();
        return wcax != null && wcax.supportsAction(action)
                ? wcax.performAction(action, arguments)
                : super.performAccessibilityAction(action, arguments);
    }

    /**
     * Set the desired size of the view. The values are in {@link MeasureSpec}.
     * @param width The width of the content view.
     * @param height The height of the content view.
     */
    public void setDesiredMeasureSpec(int width, int height) {
        mDesiredWidthMeasureSpec = width;
        mDesiredHeightMeasureSpec = height;
    }

    @Override
    public void setOnHierarchyChangeListener(OnHierarchyChangeListener listener) {
        assert listener == this : "Use add/removeOnHierarchyChangeListener instead.";
        super.setOnHierarchyChangeListener(listener);
    }

    /**
     * Registers the given listener to receive state changes for the content view hierarchy.
     * @param listener Listener to receive view hierarchy state changes.
     */
    public void addOnHierarchyChangeListener(OnHierarchyChangeListener listener) {
        mHierarchyChangeListeners.addObserver(listener);
    }

    /**
     * Unregisters the given listener from receiving state changes for the content view hierarchy.
     * @param listener Listener that doesn't want to receive view hierarchy state changes.
     */
    public void removeOnHierarchyChangeListener(OnHierarchyChangeListener listener) {
        mHierarchyChangeListeners.removeObserver(listener);
    }

    @Override
    public void setOnSystemUiVisibilityChangeListener(OnSystemUiVisibilityChangeListener listener) {
        assert listener == this : "Use add/removeOnSystemUiVisibilityChangeListener instead.";
        super.setOnSystemUiVisibilityChangeListener(listener);
    }

    /**
     * Registers the given listener to receive system UI visibility state changes.
     * @param listener Listener to receive system UI visibility state changes.
     */
    public void addOnSystemUiVisibilityChangeListener(OnSystemUiVisibilityChangeListener listener) {
        mSystemUiChangeListeners.addObserver(listener);
    }

    /**
     * Unregisters the given listener from receiving system UI visibility state changes.
     * @param listener Listener that doesn't want to receive state changes.
     */
    public void removeOnSystemUiVisibilityChangeListener(
            OnSystemUiVisibilityChangeListener listener) {
        mSystemUiChangeListeners.removeObserver(listener);
    }

    // View.OnHierarchyChangeListener implementation

    @Override
    public void onChildViewRemoved(View parent, View child) {
        for (OnHierarchyChangeListener listener : mHierarchyChangeListeners) {
            listener.onChildViewRemoved(parent, child);
        }
    }

    @Override
    public void onChildViewAdded(View parent, View child) {
        for (OnHierarchyChangeListener listener : mHierarchyChangeListeners) {
            listener.onChildViewAdded(parent, child);
        }
    }

    // View.OnHierarchyChangeListener implementation

    @Override
    public void onSystemUiVisibilityChange(int visibility) {
        for (OnSystemUiVisibilityChangeListener listener : mSystemUiChangeListeners) {
            listener.onSystemUiVisibilityChange(visibility);
        }
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        if (mDesiredWidthMeasureSpec != DEFAULT_MEASURE_SPEC) {
            widthMeasureSpec = mDesiredWidthMeasureSpec;
        }
        if (mDesiredHeightMeasureSpec != DEFAULT_MEASURE_SPEC) {
            heightMeasureSpec = mDesiredHeightMeasureSpec;
        }
        super.onMeasure(widthMeasureSpec, heightMeasureSpec);
    }

    @Override
    public AccessibilityNodeProvider getAccessibilityNodeProvider() {
        WebContentsAccessibility wcax = getWebContentsAccessibility();
        AccessibilityNodeProvider provider =
                (wcax != null) ? wcax.getAccessibilityNodeProvider() : null;
        return (provider != null) ? provider : super.getAccessibilityNodeProvider();
    }

    // Needed by ViewEventSink.InternalAccessDelegate
    @Override
    public void onScrollChanged(int l, int t, int oldl, int oldt) {
        super.onScrollChanged(l, t, oldl, oldt);
    }

    @Override
    public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
        // Calls may come while/after WebContents is destroyed. See https://crbug.com/821750#c8.
        if (mWebContents != null && mWebContents.isDestroyed()) return null;
        return ImeAdapter.fromWebContents(mWebContents).onCreateInputConnection(outAttrs);
    }

    @Override
    public boolean onCheckIsTextEditor() {
        if (mWebContents != null && mWebContents.isDestroyed()) return false;
        return ImeAdapter.fromWebContents(mWebContents).onCheckIsTextEditor();
    }

    @Override
    protected void onFocusChanged(boolean gainFocus, int direction, Rect previouslyFocusedRect) {
        try {
            TraceEvent.begin("ContentView.onFocusChanged");
            super.onFocusChanged(gainFocus, direction, previouslyFocusedRect);
            if (mWebContents != null) {
                getViewEventSink().setHideKeyboardOnBlur(true);
                getViewEventSink().onViewFocusChanged(gainFocus);
            }
        } finally {
            TraceEvent.end("ContentView.onFocusChanged");
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasWindowFocus) {
        super.onWindowFocusChanged(hasWindowFocus);
        if (mWebContents != null) {
            getViewEventSink().onWindowFocusChanged(hasWindowFocus);
        }
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        return getEventForwarder().onKeyUp(keyCode, event);
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        return isFocused() ? getEventForwarder().dispatchKeyEvent(event)
                           : super.dispatchKeyEvent(event);
    }

    @Override
    public boolean onDragEvent(DragEvent event) {
        return getEventForwarder().onDragEvent(event, this);
    }

    @Override
    public boolean onInterceptTouchEvent(MotionEvent e) {
        boolean ret = super.onInterceptTouchEvent(e);
        mEventOffsetHandler.onInterceptTouchEvent(e);
        return ret;
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        boolean ret = getEventForwarder().onTouchEvent(event);
        mEventOffsetHandler.onTouchEvent(event);
        return ret;
    }

    @Override
    public boolean onInterceptHoverEvent(MotionEvent e) {
        mEventOffsetHandler.onInterceptHoverEvent(e);
        return super.onInterceptHoverEvent(e);
    }

    @Override
    public boolean dispatchDragEvent(DragEvent e) {
        mEventOffsetHandler.onPreDispatchDragEvent(e.getAction());
        boolean ret = super.dispatchDragEvent(e);
        mEventOffsetHandler.onPostDispatchDragEvent(e.getAction());
        return ret;
    }

    /**
     * Mouse move events are sent on hover enter, hover move and hover exit.
     * They are sent on hover exit because sometimes it acts as both a hover
     * move and hover exit.
     */
    @Override
    public boolean onHoverEvent(MotionEvent event) {
        boolean consumed = getEventForwarder().onHoverEvent(event);
        WebContentsAccessibility wcax = getWebContentsAccessibility();
        if (wcax != null && !wcax.isTouchExplorationEnabled()) super.onHoverEvent(event);
        return consumed;
    }

    @Override
    public boolean onGenericMotionEvent(MotionEvent event) {
        return getEventForwarder().onGenericMotionEvent(event);
    }

    private EventForwarder getEventForwarder() {
        return mWebContents != null ? mWebContents.getEventForwarder() : null;
    }

    private ViewEventSink getViewEventSink() {
        return mWebContents != null ? ViewEventSink.from(mWebContents) : null;
    }

    @Override
    public boolean performLongClick() {
        return false;
    }

    @Override
    protected void onConfigurationChanged(Configuration newConfig) {
        if (mWebContents != null) {
            getViewEventSink().onConfigurationChanged(newConfig);
        }
        super.onConfigurationChanged(newConfig);
    }

    /**
     * Currently the ContentView scrolling happens in the native side. In
     * the Java view system, it is always pinned at (0, 0). scrollBy() and scrollTo()
     * are overridden, so that View's mScrollX and mScrollY will be unchanged at
     * (0, 0). This is critical for drawing ContentView correctly.
     */
    @Override
    public void scrollBy(int x, int y) {
        getEventForwarder().scrollBy(x, y);
    }

    @Override
    public void scrollTo(int x, int y) {
        getEventForwarder().scrollTo(x, y);
    }

    @Override
    protected int computeHorizontalScrollExtent() {
        RenderCoordinates rc = getRenderCoordinates();
        return rc != null ? rc.getLastFrameViewportWidthPixInt() : 0;
    }

    @Override
    protected int computeHorizontalScrollOffset() {
        RenderCoordinates rc = getRenderCoordinates();
        return rc != null ? rc.getScrollXPixInt() : 0;
    }

    @Override
    protected int computeHorizontalScrollRange() {
        RenderCoordinates rc = getRenderCoordinates();
        return rc != null ? rc.getContentWidthPixInt() : 0;
    }

    @Override
    protected int computeVerticalScrollExtent() {
        RenderCoordinates rc = getRenderCoordinates();
        return rc != null ? rc.getLastFrameViewportHeightPixInt() : 0;
    }

    @Override
    protected int computeVerticalScrollOffset() {
        RenderCoordinates rc = getRenderCoordinates();
        return rc != null ? rc.getScrollYPixInt() : 0;
    }

    @Override
    protected int computeVerticalScrollRange() {
        RenderCoordinates rc = getRenderCoordinates();
        return rc != null ? rc.getContentHeightPixInt() : 0;
    }

    private RenderCoordinates getRenderCoordinates() {
        return mWebContents != null ? RenderCoordinates.fromWebContents(mWebContents) : null;
    }

    // End RelativeLayout overrides.

    @Override
    public boolean awakenScrollBars(int startDelay, boolean invalidate) {
        // For the default implementation of ContentView which draws the scrollBars on the native
        // side, calling this function may get us into a bad state where we keep drawing the
        // scrollBars, so disable it by always returning false.
        if (getScrollBarStyle() == View.SCROLLBARS_INSIDE_OVERLAY) return false;
        return super.awakenScrollBars(startDelay, invalidate);
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        if (mWebContents != null) {
            getViewEventSink().onAttachedToWindow();
        }
    }

    @Override
    protected void onDetachedFromWindow() {
        super.onDetachedFromWindow();
        if (mWebContents != null) {
            getViewEventSink().onDetachedFromWindow();
        }
    }

    // Implements SmartClipProvider
    @Override
    public void extractSmartClipData(int x, int y, int width, int height) {
        if (mWebContents != null) {
            mWebContents.requestSmartClipExtract(x, y, width, height);
        }
    }

    // Implements SmartClipProvider
    @Override
    public void setSmartClipResultHandler(final Handler resultHandler) {
        if (mWebContents != null) {
            mWebContents.setSmartClipResultHandler(resultHandler);
        }
    }

    ///////////////////////////////////////////////////////////////////////////////////////////////
    //              Start Implementation of ViewEventSink.InternalAccessDelegate                 //
    ///////////////////////////////////////////////////////////////////////////////////////////////

    @Override
    public boolean super_onKeyUp(int keyCode, KeyEvent event) {
        return super.onKeyUp(keyCode, event);
    }

    @Override
    public boolean super_dispatchKeyEvent(KeyEvent event) {
        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean super_onGenericMotionEvent(MotionEvent event) {
        return super.onGenericMotionEvent(event);
    }

    ///////////////////////////////////////////////////////////////////////////////////////////////
    //                End Implementation of ViewEventSink.InternalAccessDelegate                 //
    ///////////////////////////////////////////////////////////////////////////////////////////////

    private static class ContentViewApi23 extends ContentView {
        public ContentViewApi23(Context context, EventOffsetHandler eventOffsetHandler) {
            super(context, eventOffsetHandler);
        }

        @Override
        public void onProvideVirtualStructure(final ViewStructure structure) {
            WebContentsAccessibility wcax = getWebContentsAccessibility();
            if (wcax != null) wcax.onProvideVirtualStructure(structure, false);
        }
    }

    private static class ContentViewApi26 extends ContentViewApi23 {
        public ContentViewApi26(Context context, EventOffsetHandler eventOffsetHandler) {
            super(context, eventOffsetHandler);
        }

        @Override
        public void onProvideAutofillVirtualStructure(ViewStructure structure, int flags) {
            // A new (virtual) View has been entered, and the autofill system-level
            // infrastructure wants us to populate |structure| with the autofill structure of the
            // (virtual) View. Forward this on to AutofillProvider to accomplish.
            AutofillProvider autofillProvider = getAutofillProvider();
            if (autofillProvider != null) {
                autofillProvider.onProvideAutoFillVirtualStructure(structure, flags);
            }
        }

        @Override
        public void autofill(final SparseArray<AutofillValue> values) {
            // The autofill system-level infrastructure has information that we can use to
            // autofill the current (virtual) View. Forward this on to AutofillProvider to
            // accomplish.
            AutofillProvider autofillProvider = getAutofillProvider();
            if (autofillProvider != null) {
                autofillProvider.autofill(values);
            }
        }
    }
}
