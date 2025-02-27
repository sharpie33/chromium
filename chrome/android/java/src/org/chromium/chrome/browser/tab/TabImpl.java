// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.tab;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.content.Context;
import android.graphics.Rect;
import android.text.TextUtils;
import android.view.View;
import android.view.View.OnAttachStateChangeListener;
import android.view.accessibility.AccessibilityEvent;

import androidx.annotation.Nullable;

import org.chromium.base.ContextUtils;
import org.chromium.base.Log;
import org.chromium.base.ObserverList;
import org.chromium.base.ObserverList.RewindableIterator;
import org.chromium.base.TraceEvent;
import org.chromium.base.UserDataHost;
import org.chromium.base.annotations.CalledByNative;
import org.chromium.base.annotations.NativeMethods;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.ChromeActivity;
import org.chromium.chrome.browser.ChromeVersionInfo;
import org.chromium.chrome.browser.WarmupManager;
import org.chromium.chrome.browser.WebContentsFactory;
import org.chromium.chrome.browser.content.ContentUtils;
import org.chromium.chrome.browser.contextmenu.ContextMenuPopulator;
import org.chromium.chrome.browser.native_page.NativePageAssassin;
import org.chromium.chrome.browser.native_page.NativePageFactory;
import org.chromium.chrome.browser.night_mode.NightModeUtils;
import org.chromium.chrome.browser.offlinepages.OfflinePageUtils;
import org.chromium.chrome.browser.prerender.ExternalPrerenderHandler;
import org.chromium.chrome.browser.profiles.Profile;
import org.chromium.chrome.browser.rlz.RevenueStats;
import org.chromium.chrome.browser.tab.TabState.WebContentsState;
import org.chromium.chrome.browser.tab.TabUma.TabCreationState;
import org.chromium.chrome.browser.ui.native_page.FrozenNativePage;
import org.chromium.chrome.browser.ui.native_page.NativePage;
import org.chromium.chrome.browser.util.UrlConstants;
import org.chromium.chrome.browser.vr.VrModuleProvider;
import org.chromium.components.dom_distiller.core.DomDistillerUrlUtils;
import org.chromium.components.embedder_support.view.ContentView;
import org.chromium.content_public.browser.ChildProcessImportance;
import org.chromium.content_public.browser.LoadUrlParams;
import org.chromium.content_public.browser.WebContents;
import org.chromium.content_public.browser.WebContentsAccessibility;
import org.chromium.content_public.common.ResourceRequestBody;
import org.chromium.ui.base.PageTransition;
import org.chromium.ui.base.WindowAndroid;

/**
 * Implementation of the interface {@link Tab}. Contains and manages a {@link ContentView}.
 * This class is not intended to be extended.
 */
public class TabImpl implements Tab {
    private static final long INVALID_TIMESTAMP = -1;

    /** Used for logging. */
    private static final String TAG = "Tab";

    private static final String PRODUCT_VERSION = ChromeVersionInfo.getProductVersion();

    private long mNativeTabAndroid;

    /** Unique id of this tab (within its container). */
    private final int mId;

    /** Whether or not this tab is an incognito tab. */
    private final boolean mIncognito;

    /**
     * An Application {@link Context}.  Unlike {@link #mActivity}, this is the only one that is
     * publicly exposed to help prevent leaking the {@link Activity}.
     */
    private final Context mThemedApplicationContext;

    /** Gives {@link Tab} a way to interact with the Android window. */
    private WindowAndroid mWindowAndroid;

    /** The current native page (e.g. chrome-native://newtab), or {@code null} if there is none. */
    private NativePage mNativePage;

    /** {@link WebContents} showing the current page, or {@code null} if the tab is frozen. */
    private WebContents mWebContents;

    /** The parent view of the ContentView and the InfoBarContainer. */
    private ContentView mContentView;

    /** A list of Tab observers.  These are used to broadcast Tab events to listeners. */
    private final ObserverList<TabObserver> mObservers = new ObserverList<>();

    // Content layer Delegates
    private TabWebContentsDelegateAndroid mWebContentsDelegate;

    /**
     * If this tab was opened from another tab, store the id of the tab that
     * caused it to be opened so that we can activate it when this tab gets
     * closed.
     */
    private final int mParentId;

    /**
     * Tab id to be used as a source tab in SyncedTabDelegate.
     */
    private final int mSourceTabId;

    /**
     * By default, this id inherits from the tab that caused it to be opened, or it equals to tab
     * id. This is used to restore the relationship that defined by {@link TabModelFilter} between
     * this tab and other tabs. This id can be re-set whenever is needed.
     */
    private int mRootId;

    private boolean mIsClosing;
    private boolean mIsShowingErrorPage;

    /** Whether or not the TabState has changed. */
    private boolean mIsTabStateDirty = true;

    /**
     * Saves how this tab was launched (from a link, external app, etc) so that
     * we can determine the different circumstances in which it should be
     * closed. For example, a tab opened from an external app should be closed
     * when the back stack is empty and the user uses the back hardware key. A
     * standard tab however should be kept open and the entire activity should
     * be moved to the background.
     */
    private final @Nullable @TabLaunchType Integer mLaunchType;

    /**
     * Saves how this tab was initially launched so that we can record metrics on how the
     * tab was created. This is different than {@code mLaunchType}, since {@code mLaunchType} will
     * be overridden to "FROM_RESTORE" during tab restoration.
     */
    private @Nullable @TabLaunchType Integer mLaunchTypeAtCreation;

    /**
     * Navigation state of the WebContents as returned by nativeGetContentsStateAsByteBuffer(),
     * stored to be inflated on demand using unfreezeContents(). If this is not null, there is no
     * WebContents around. Upon tab switch WebContents will be unfrozen and the variable will be set
     * to null.
     */
    private WebContentsState mFrozenContentsState;

    /**
     * URL load to be performed lazily when the Tab is next shown.
     */
    private LoadUrlParams mPendingLoadParams;

    /**
     * URL of the page currently loading. Used as a fall-back in case tab restore fails.
     */
    private String mUrl;

    /**
     * True while a page load is in progress.
     */
    private boolean mIsLoading;

    /**
     * True while a restore page load is in progress.
     */
    private boolean mIsBeingRestored;

    /**
     * Whether or not the Tab is currently visible to the user.
     */
    private boolean mIsHidden = true;

    /**
     * Importance of the WebContents currently attached to this tab. Note the key difference from
     * |mIsHidden| is that a tab is hidden when the application is hidden, but the importance is
     * not affected by this signal.
     */
    private @ChildProcessImportance int mImportance = ChildProcessImportance.NORMAL;

    /** Whether the renderer is currently unresponsive. */
    private boolean mIsRendererUnresponsive;

    /**
     * The last time this tab was shown or the time of its initialization if it wasn't yet shown.
     */
    private long mTimestampMillis = INVALID_TIMESTAMP;

    /**
     * Title of the ContentViews webpage.
     */
    private String mTitle;

    /**
     * Whether didCommitProvisionalLoadForFrame() hasn't yet been called for the current native page
     * (page A). To decrease latency, we show native pages in both loadUrl() and
     * didCommitProvisionalLoadForFrame(). However, we mustn't show a new native page (page B) in
     * loadUrl() if the current native page hasn't yet been committed. Otherwise, we'll show each
     * page twice (A, B, A, B): the first two times in loadUrl(), the second two times in
     * didCommitProvisionalLoadForFrame().
     */
    private boolean mIsNativePageCommitPending;

    private TabDelegateFactory mDelegateFactory;

    /** Listens for views related to the tab to be attached or detached. */
    private OnAttachStateChangeListener mAttachStateChangeListener;

    /** Whether the tab can currently be interacted with. */
    private boolean mInteractableState;

    /** Whether or not the tab's active view is attached to the window. */
    private boolean mIsViewAttachedToWindow;

    private final UserDataHost mUserDataHost = new UserDataHost();

    /**
     * Creates an instance of a {@link TabImpl}.
     *
     * This constructor can be called before the native library has been loaded, so any additions
     * must be vetted for library calls.
     *
     * Package-private. Use {@link TabBuilder} to create an instance.
     *
     * @param id The id this tab should be identified with.
     * @param parent The tab that caused this tab to be opened.
     * @param incognito Whether or not this tab is incognito.
     * @param launchType Type indicating how this tab was launched.
     */
    @SuppressLint("HandlerLeak")
    TabImpl(int id, Tab parent, boolean incognito, @Nullable @TabLaunchType Integer launchType) {
        mId = TabIdManager.getInstance().generateValidId(id);
        mIncognito = incognito;
        if (parent == null) {
            mParentId = INVALID_TAB_ID;
            mSourceTabId = INVALID_TAB_ID;
        } else {
            mParentId = parent.getId();
            mSourceTabId = parent.isIncognito() == incognito ? mParentId : INVALID_TAB_ID;
        }
        mRootId = mId;

        // Override the configuration for night mode to always stay in light mode until all UIs in
        // Tab are inflated from activity context instead of application context. This is to
        // avoid getting the wrong night mode state when application context inherits a system UI
        // mode different from the UI mode we need.
        // TODO(https://crbug.com/938641): Remove this once Tab UIs are all inflated from
        // activity.
        mThemedApplicationContext =
                NightModeUtils.wrapContextWithNightModeConfig(ContextUtils.getApplicationContext(),
                        ChromeActivity.getThemeId(), false /*nightMode*/);

        mLaunchType = launchType;

        mAttachStateChangeListener = new OnAttachStateChangeListener() {
            @Override
            public void onViewAttachedToWindow(View view) {
                mIsViewAttachedToWindow = true;
                updateInteractableState();
            }

            @Override
            public void onViewDetachedFromWindow(View view) {
                mIsViewAttachedToWindow = false;
                updateInteractableState();
            }
        };
    }

    @Override
    public void addObserver(TabObserver observer) {
        mObservers.addObserver(observer);
    }

    @Override
    public void removeObserver(TabObserver observer) {
        mObservers.removeObserver(observer);
    }

    @Override
    public UserDataHost getUserDataHost() {
        return mUserDataHost;
    }

    @Override
    public WebContents getWebContents() {
        return mWebContents;
    }

    @Override
    public Context getContext() {
        if (getWindowAndroid() == null) return mThemedApplicationContext;
        Context context = getWindowAndroid().getContext().get();
        return context == context.getApplicationContext() ? mThemedApplicationContext : context;
    }

    @Override
    public WindowAndroid getWindowAndroid() {
        return mWindowAndroid;
    }

    @Override
    public void updateAttachment(
            @Nullable WindowAndroid window, @Nullable TabDelegateFactory tabDelegateFactory) {
        // Non-null delegate factory while being detached is not valid.
        assert !(window == null && tabDelegateFactory != null);

        boolean attached = window != null;
        if (attached) {
            updateWindowAndroid(window);
            if (tabDelegateFactory != null) setDelegateFactory(tabDelegateFactory);

            // Reload the NativePage (if any), since the old NativePage has a reference to the old
            // activity.
            if (isNativePage()) maybeShowNativePage(getUrl(), true);
        }

        // Notify the event to observers only when we do the reparenting task, not when we simply
        // switch window in which case a new window is non-null but delegate is null.
        boolean notify = (window != null && tabDelegateFactory != null)
                || (window == null && tabDelegateFactory == null);
        if (notify) {
            for (TabObserver observer : mObservers) {
                observer.onActivityAttachmentChanged(this, attached);
            }
        }
    }

    @Override
    public ContentView getContentView() {
        return mContentView;
    }

    @Override
    public View getView() {
        return mNativePage != null ? mNativePage.getView() : mContentView;
    }

    @Override
    @CalledByNative
    public int getId() {
        return mId;
    }

    @Override
    public int getParentId() {
        return mParentId;
    }

    @CalledByNative
    @Override
    public String getUrl() {
        String url = getWebContents() != null ? getWebContents().getVisibleUrl() : "";

        // If we have a ContentView, or a NativePage, or the url is not empty, we have a WebContents
        // so cache the WebContent's url. If not use the cached version.
        if (getWebContents() != null || isNativePage() || !TextUtils.isEmpty(url)) {
            mUrl = url;
        }

        return mUrl != null ? mUrl : "";
    }

    @CalledByNative
    @Override
    public String getTitle() {
        if (mTitle == null) updateTitle();
        return mTitle;
    }

    Context getThemedApplicationContext() {
        return mThemedApplicationContext;
    }

    @Override
    public NativePage getNativePage() {
        return mNativePage;
    }

    @Override
    @CalledByNative
    public boolean isNativePage() {
        return mNativePage != null;
    }

    @Override
    public void freezeNativePage() {
        if (mNativePage == null || mNativePage.isFrozen()
                || mNativePage.getView().getParent() == null) {
            return;
        }
        mNativePage = FrozenNativePage.freeze(mNativePage);
        updateInteractableState();
    }

    @Override
    public @TabLaunchType int getLaunchType() {
        return mLaunchType;
    }

    @Override
    public @Nullable @TabLaunchType Integer getLaunchTypeAtInitialTabCreation() {
        return mLaunchTypeAtCreation;
    }

    @Override
    public long getTimestampMillis() {
        return mTimestampMillis;
    }

    @Override
    public boolean isIncognito() {
        return mIncognito;
    }

    @Override
    public boolean isShowingErrorPage() {
        return mIsShowingErrorPage;
    }

    /**
     * @return true iff the tab doesn't hold a live page. This happens before initialize() and when
     * the tab holds frozen WebContents state that is yet to be inflated.
     */
    @Override
    public boolean isFrozen() {
        return !isNativePage() && getWebContents() == null;
    }

    @CalledByNative
    @Override
    public boolean isUserInteractable() {
        return mInteractableState;
    }

    @Override
    public int loadUrl(LoadUrlParams params) {
        try {
            TraceEvent.begin("Tab.loadUrl");
            // TODO(tedchoc): When showing the android NTP, delay the call to
            // TabImplJni.get().loadUrl until the android view has entirely rendered.
            if (!mIsNativePageCommitPending) {
                mIsNativePageCommitPending = maybeShowNativePage(params.getUrl(), false);
            }

            if ("chrome://java-crash/".equals(params.getUrl())) {
                return handleJavaCrash();
            }

            if (mNativeTabAndroid == 0) {
                // if mNativeTabAndroid is null then we are going to crash anyways on the
                // native side. Lets crash on the java side so that we can have a better stack
                // trace.
                throw new RuntimeException("Tab.loadUrl called when no native side exists");
            }

            // We load the URL from the tab rather than directly from the ContentView so the tab has
            // a chance of using a prerenderer page is any.
            int loadType = TabImplJni.get().loadUrl(mNativeTabAndroid, TabImpl.this,
                    params.getUrl(), params.getInitiatorOrigin(), params.getVerbatimHeaders(),
                    params.getPostData(), params.getTransitionType(),
                    params.getReferrer() != null ? params.getReferrer().getUrl() : null,
                    // Policy will be ignored for null referrer url, 0 is just a placeholder.
                    // TODO(ppi): Should we pass Referrer jobject and add JNI methods to read it
                    //            from the native?
                    params.getReferrer() != null ? params.getReferrer().getPolicy() : 0,
                    params.getIsRendererInitiated(), params.getShouldReplaceCurrentEntry(),
                    params.getHasUserGesture(), params.getShouldClearHistoryList(),
                    params.getInputStartTimestamp(), params.getIntentReceivedTimestamp());

            for (TabObserver observer : mObservers) {
                observer.onLoadUrl(this, params, loadType);
            }
            return loadType;
        } finally {
            TraceEvent.end("Tab.loadUrl");
        }
    }

    @Override
    public boolean loadIfNeeded() {
        if (getActivity() == null) {
            Log.e(TAG, "Tab couldn't be loaded because Context was null.");
            return false;
        }

        if (mPendingLoadParams != null) {
            assert isFrozen();
            WebContents webContents = WarmupManager.getInstance().takeSpareWebContents(
                    isIncognito(), isHidden(), isCustomTab());
            if (webContents == null) {
                webContents = WebContentsFactory.createWebContents(isIncognito(), isHidden());
            }
            initWebContents(webContents);
            loadUrl(mPendingLoadParams);
            mPendingLoadParams = null;
            return true;
        }

        restoreIfNeeded();
        return true;
    }

    @Override
    public void reload() {
        // TODO(dtrainor): Should we try to rebuild the ContentView if it's frozen?
        if (OfflinePageUtils.isOfflinePage(this)) {
            // If current page is an offline page, reload it with custom behavior defined in extra
            // header respected.
            OfflinePageUtils.reload(getWebContents(),
                    /*loadUrlDelegate=*/new OfflinePageUtils.TabOfflinePageLoadUrlDelegate(this));
        } else {
            if (getWebContents() != null) getWebContents().getNavigationController().reload(true);
        }
    }

    @Override
    public void reloadIgnoringCache() {
        if (getWebContents() != null) {
            getWebContents().getNavigationController().reloadBypassingCache(true);
        }
    }

    @Override
    public void stopLoading() {
        if (isLoading()) {
            RewindableIterator<TabObserver> observers = getTabObservers();
            while (observers.hasNext()) {
                observers.next().onPageLoadFinished(this, getUrl());
            }
        }
        if (getWebContents() != null) getWebContents().stop();
    }

    @Override
    public boolean needsReload() {
        return getWebContents() != null && getWebContents().getNavigationController().needsReload();
    }

    @Override
    public boolean isLoading() {
        return mIsLoading && !isShowingInterstitialPage();
    }

    @Override
    public boolean isBeingRestored() {
        return mIsBeingRestored;
    }

    @Override
    public float getProgress() {
        return !isLoading() ? 1 : (int) mWebContents.getLoadProgress();
    }

    @Override
    public boolean canGoBack() {
        return getWebContents() != null && getWebContents().getNavigationController().canGoBack();
    }

    @Override
    public boolean canGoForward() {
        return getWebContents() != null
                && getWebContents().getNavigationController().canGoForward();
    }

    @Override
    public void goBack() {
        if (getWebContents() != null) getWebContents().getNavigationController().goBack();
    }

    @Override
    public void goForward() {
        if (getWebContents() != null) getWebContents().getNavigationController().goForward();
    }

    // TabLifecycle implementation.

    @Override
    public boolean isInitialized() {
        return mNativeTabAndroid != 0;
    }

    @Override
    public final void show(@TabSelectionType int type) {
        try {
            TraceEvent.begin("Tab.show");
            if (!isHidden()) return;
            // Keep unsetting mIsHidden above loadIfNeeded(), so that we pass correct visibility
            // when spawning WebContents in loadIfNeeded().
            mIsHidden = false;
            updateInteractableState();

            loadIfNeeded();
            assert !isFrozen();

            if (getWebContents() != null) getWebContents().onShow();

            // If the NativePage was frozen while in the background (see NativePageAssassin),
            // recreate the NativePage now.
            NativePage nativePage = getNativePage();
            if (nativePage != null && nativePage.isFrozen()) {
                maybeShowNativePage(nativePage.getUrl(), true);
            }
            NativePageAssassin.getInstance().tabShown(this);
            TabImportanceManager.tabShown(this);

            // If the page is still loading, update the progress bar (otherwise it would not show
            // until the renderer notifies of new progress being made).
            if (getProgress() < 100 && !isShowingInterstitialPage()) {
                notifyLoadProgress(getProgress());
            }

            for (TabObserver observer : mObservers) observer.onShown(this, type);

            // Updating the timestamp has to happen after the showInternal() call since subclasses
            // may use it for logging.
            mTimestampMillis = System.currentTimeMillis();
        } finally {
            TraceEvent.end("Tab.show");
        }
    }

    @Override
    public final void hide(@TabHidingType int type) {
        try {
            TraceEvent.begin("Tab.hide");
            if (isHidden()) return;
            mIsHidden = true;
            updateInteractableState();

            if (getWebContents() != null) getWebContents().onHide();

            // Allow this tab's NativePage to be frozen if it stays hidden for a while.
            NativePageAssassin.getInstance().tabHidden(this);

            for (TabObserver observer : mObservers) observer.onHidden(this, type);
        } finally {
            TraceEvent.end("Tab.hide");
        }
    }

    @Override
    public boolean isClosing() {
        return mIsClosing;
    }

    @Override
    public void setClosing(boolean closing) {
        mIsClosing = closing;
        for (TabObserver observer : mObservers) observer.onClosingStateChanged(this, closing);
    }

    @Override
    public boolean isHidden() {
        return mIsHidden;
    }

    @Override
    public void destroy() {
        // Update the title before destroying the tab. http://b/5783092
        updateTitle();

        for (TabObserver observer : mObservers) observer.onDestroyed(this);
        mObservers.clear();

        mUserDataHost.destroy();
        hideNativePage(false, null);
        destroyWebContents(true);

        TabImportanceManager.tabDestroyed(this);

        // Destroys the native tab after destroying the ContentView but before destroying the
        // InfoBarContainer. The native tab should be destroyed before the infobar container as
        // destroying the native tab cleanups up any remaining infobars. The infobar container
        // expects all infobars to be cleaned up before its own destruction.
        if (mNativeTabAndroid != 0) {
            TabImplJni.get().destroy(mNativeTabAndroid, TabImpl.this);
            assert mNativeTabAndroid == 0;
        }
    }

    /**
     * @return {@link ChromeActivity} that currently contains this {@link Tab} in its
     *         {@link TabModel}.
     */
    public ChromeActivity<?> getActivity() {
        if (getWindowAndroid() == null) return null;
        Activity activity = ContextUtils.activityFromContext(getWindowAndroid().getContext().get());
        if (activity instanceof ChromeActivity) return (ChromeActivity<?>) activity;
        return null;
    }

    /**
     * @return The profile associated with this tab.
     */
    public Profile getProfile() {
        return Profile.fromWebContents(getWebContents());
    }

    /**
     * This is used to change how this tab related to other tabs.
     * @param rootId New relationship id to be set.
     */
    public void setRootId(int rootId) {
        if (rootId == mRootId) return;
        mRootId = rootId;
        mIsTabStateDirty = true;
        for (TabObserver observer : mObservers) {
            observer.onRootIdChanged(this, rootId);
        }
    }

    /**
     * @return Tab's relationship id.
     */
    public int getRootId() {
        return mRootId;
    }

    /**
     * @return Original url of the tab, which is the original url from DOMDistiller.
     */
    public String getOriginalUrl() {
        return DomDistillerUrlUtils.getOriginalUrlFromDistillerUrl(getUrl());
    }

    /**
     * @return Whether or not the {@link Tab} is currently showing an interstitial page, such as
     *         a bad HTTPS page.
     */
    public boolean isShowingInterstitialPage() {
        return getWebContents() != null && getWebContents().isShowingInterstitialPage();
    }

    /**
     * @param tab {@link Tab} instance being checked.
     * @return Whether the tab is detached from any Activity and its {@link WindowAndroid}.
     * Certain functionalities will not work until it is attached to an activity
     * with {@link ReparentingTask#finish}.
     */
    public static boolean isDetached(Tab tab) {
        if (tab.getWebContents() == null) return true;
        // Should get WindowAndroid from WebContents since the one from |getWindowAndroid()|
        // is always non-null even when the tab is in detached state. See the comment in |detach()|.
        WindowAndroid window = tab.getWebContents().getTopLevelNativeWindow();
        if (window == null) return true;
        Activity activity = ContextUtils.activityFromContext(window.getContext().get());
        return !(activity instanceof ChromeActivity);
    }

    /**
     * @return Whether the TabState representing this Tab has been updated.
     */
    public boolean isTabStateDirty() {
        return mIsTabStateDirty;
    }

    /**
     * Set whether the TabState representing this Tab has been updated.
     * @param isDirty Whether the Tab's state has changed.
     */
    public void setIsTabStateDirty(boolean isDirty) {
        mIsTabStateDirty = isDirty;
    }

    /**
     * @return Whether there are pending {@link LoadUrlParams} associated with the tab.  This
     *         indicates the tab was created for lazy load.
     */
    public boolean hasPendingLoadParams() {
        return mPendingLoadParams != null;
    }

    /**
     * Update whether or not the current native tab and/or web contents are
     * currently visible (from an accessibility perspective), or whether
     * they're obscured by another view.
     */
    public void updateAccessibilityVisibility() {
        View view = getView();
        if (view != null) {
            int importantForAccessibility = isObscuredByAnotherViewForAccessibility()
                    ? View.IMPORTANT_FOR_ACCESSIBILITY_NO_HIDE_DESCENDANTS
                    : View.IMPORTANT_FOR_ACCESSIBILITY_YES;
            if (view.getImportantForAccessibility() != importantForAccessibility) {
                view.setImportantForAccessibility(importantForAccessibility);
                view.sendAccessibilityEvent(AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED);
            }
        }

        WebContentsAccessibility wcax = getWebContentsAccessibility(getWebContents());
        if (wcax != null) {
            boolean isWebContentObscured =
                    isObscuredByAnotherViewForAccessibility() || SadTab.isShowing(this);
            wcax.setObscuredByAnotherView(isWebContentObscured);
        }
    }

    /**
     * Called when the contextual ActionBar is shown or hidden.
     * @param show {@code true} when the ActionBar is shown; {@code false} otherwise.
     */
    public void notifyContextualActionBarVisibilityChanged(boolean show) {
        for (TabObserver observer : mObservers) {
            observer.onContextualActionBarVisibilityChanged(this, show);
        }
    }

    /**
     * Initializes {@link Tab} with {@code webContents}.  If {@code webContents} is {@code null}
     * a new {@link WebContents} will be created for this {@link Tab}.
     * @param parent The tab that caused this tab to be opened.
     * @param creationState State in which the tab is created.
     * @param loadUrlParams Parameters used for a lazily loaded Tab.
     * @param webContents A {@link WebContents} object or {@code null} if one should be created.
     * @param delegateFactory The {@link TabDelegateFactory} to be used for delegate creation.
     * @param initiallyHidden Only used if {@code webContents} is {@code null}.  Determines
     *        whether or not the newly created {@link WebContents} will be hidden or not.
     * @param tabState State containing information about this Tab, if it was persisted.
     * @param unfreeze Whether there should be an attempt to restore state at the end of
     *        the initialization.
     */
    void initialize(Tab parent, @Nullable @TabCreationState Integer creationState,
            LoadUrlParams loadUrlParams, WebContents webContents,
            @Nullable TabDelegateFactory delegateFactory, boolean initiallyHidden,
            TabState tabState, boolean unfreeze) {
        try {
            TraceEvent.begin("Tab.initialize");

            mLaunchTypeAtCreation = mLaunchType;
            mPendingLoadParams = loadUrlParams;
            if (loadUrlParams != null) mUrl = loadUrlParams.getUrl();

            TabHelpers.initTabHelpers(this, parent, creationState);

            if (tabState != null) restoreFieldsFromState(tabState);

            initializeNative();

            mDelegateFactory = delegateFactory;
            RevenueStats.getInstance().tabCreated(this);

            // If there is a frozen WebContents state or a pending lazy load, don't create a new
            // WebContents.
            if (getFrozenContentsState() != null || getPendingLoadParams() != null) {
                if (unfreeze) unfreezeContents();
                return;
            }

            boolean creatingWebContents = webContents == null;
            if (creatingWebContents) {
                webContents = WarmupManager.getInstance().takeSpareWebContents(
                        isIncognito(), initiallyHidden, isCustomTab());
                if (webContents == null) {
                    webContents =
                            WebContentsFactory.createWebContents(isIncognito(), initiallyHidden);
                }
            }

            initWebContents(webContents);

            if (!creatingWebContents && webContents.isLoadingToDifferentDocument()) {
                didStartPageLoad(webContents.getVisibleUrl());
            }

        } finally {
            if (mTimestampMillis == INVALID_TIMESTAMP) {
                mTimestampMillis = System.currentTimeMillis();
            }
            for (TabObserver observer : mObservers) observer.onInitialized(this, tabState);
            TraceEvent.end("Tab.initialize");
        }
    }

    /**
     * Restores member fields from the given TabState.
     * @param state TabState containing information about this Tab.
     */
    void restoreFieldsFromState(TabState state) {
        assert state != null;
        mFrozenContentsState = state.contentsState;
        mTimestampMillis = state.timestampMillis;
        mUrl = state.getVirtualUrlFromState();
        mTitle = state.getDisplayTitleFromState();
        mLaunchTypeAtCreation = state.tabLaunchTypeAtCreation;
        mRootId = state.rootId == Tab.INVALID_TAB_ID ? mId : state.rootId;
    }

    /**
     * @return An {@link ObserverList.RewindableIterator} instance that points to all of
     *         the current {@link TabObserver}s on this class.  Note that calling
     *         {@link java.util.Iterator#remove()} will throw an
     *         {@link UnsupportedOperationException}.
     */
    ObserverList.RewindableIterator<TabObserver> getTabObservers() {
        return mObservers.rewindableIterator();
    }

    final void setImportance(@ChildProcessImportance int importance) {
        if (mImportance == importance) return;
        mImportance = importance;
        WebContents webContents = getWebContents();
        if (webContents == null) return;
        webContents.setImportance(mImportance);
    }

    /**
     * Hides the current {@link NativePage}, if any, and shows the {@link WebContents}'s view.
     */
    void showRenderedPage() {
        updateTitle();
        if (mNativePage != null) hideNativePage(true, null);
    }

    void updateWindowAndroid(WindowAndroid windowAndroid) {
        // TODO(yusufo): mWindowAndroid can never be null until crbug.com/657007 is fixed.
        assert windowAndroid != null;
        mWindowAndroid = windowAndroid;
        WebContents webContents = getWebContents();
        if (webContents != null) {
            webContents.setTopLevelNativeWindow(mWindowAndroid);
            webContents.notifyRendererPreferenceUpdate();
        }
    }

    TabDelegateFactory getDelegateFactory() {
        return mDelegateFactory;
    }

    TabWebContentsDelegateAndroid getTabWebContentsDelegateAndroid() {
        return mWebContentsDelegate;
    }

    // Forwarded from TabWebContentsDelegateAndroid.

    /**
     * Called when a navigation begins and no navigation was in progress
     * @param toDifferentDocument Whether this navigation will transition between
     * documents (i.e., not a fragment navigation or JS History API call).
     */
    void onLoadStarted(boolean toDifferentDocument) {
        if (toDifferentDocument) mIsLoading = true;
        for (TabObserver observer : mObservers) observer.onLoadStarted(this, toDifferentDocument);
    }

    /**
     * Called when a navigation completes and no other navigation is in progress.
     */
    void onLoadStopped() {
        // mIsLoading should only be false if this is a same-document navigation.
        boolean toDifferentDocument = mIsLoading;
        mIsLoading = false;
        for (TabObserver observer : mObservers) observer.onLoadStopped(this, toDifferentDocument);
    }

    void handleRendererResponsiveStateChanged(boolean isResponsive) {
        mIsRendererUnresponsive = !isResponsive;
        for (TabObserver observer : mObservers) {
            observer.onRendererResponsiveStateChanged(this, isResponsive);
        }
    }

    // Forwarded from TabWebContentsObserver.

    /**
     * Called when a page has started loading.
     * @param validatedUrl URL being loaded.
     */
    void didStartPageLoad(String validatedUrl) {
        updateTitle();
        if (mIsRendererUnresponsive) handleRendererResponsiveStateChanged(true);
        for (TabObserver observer : mObservers) observer.onPageLoadStarted(this, validatedUrl);
    }

    /**
     * Called when a page has finished loading.
     * @param url URL that was loaded.
     */
    void didFinishPageLoad(String url) {
        mIsTabStateDirty = true;
        updateTitle();

        for (TabObserver observer : mObservers) observer.onPageLoadFinished(this, url);
        mIsBeingRestored = false;
    }

    /**
     * Called when a page has failed loading.
     * @param errorCode The error code causing the page to fail loading.
     */
    void didFailPageLoad(int errorCode) {
        for (TabObserver observer : mObservers) {
            observer.onPageLoadFailed(this, errorCode);
        }
        mIsBeingRestored = false;
    }

    /**
     * Update internal Tab state when provisional load gets committed.
     * @param url The URL that was loaded.
     * @param transitionType The transition type to the current URL.
     */
    void handleDidFinishNavigation(String url, Integer transitionType) {
        mIsNativePageCommitPending = false;
        boolean isReload = (transitionType != null
                && (transitionType & PageTransition.CORE_MASK) == PageTransition.RELOAD);
        if (!maybeShowNativePage(url, isReload)) {
            showRenderedPage();
        }
    }

    /**
     * Notify the observers that the load progress has changed.
     * @param progress The current percentage of progress.
     */
    void notifyLoadProgress(float progress) {
        for (TabObserver observer : mObservers) observer.onLoadProgressChanged(this, progress);
    }

    /**
     * Add a new navigation entry for the current URL and page title.
     */
    void pushNativePageStateToNavigationEntry() {
        assert mNativeTabAndroid != 0 && getNativePage() != null;
        TabImplJni.get().setActiveNavigationEntryTitleForUrl(mNativeTabAndroid, TabImpl.this,
                getNativePage().getUrl(), getNativePage().getTitle());
    }

    /**
     * Set whether the Tab needs to be reloaded.
     */
    void setNeedsReload() {
        assert getWebContents() != null;
        getWebContents().getNavigationController().setNeedsReload();
    }

    /**
     * Called when navigation entries were removed.
     */
    void notifyNavigationEntriesDeleted() {
        mIsTabStateDirty = true;
        for (TabObserver observer : mObservers) observer.onNavigationEntriesDeleted(this);
    }

    //////////////

    /**
     * @return Whether the renderer is currently unresponsive.
     */
    boolean isRendererUnresponsive() {
        return mIsRendererUnresponsive;
    }

    /**
     * Load the original image (uncompressed by spdy proxy) in this tab.
     */
    void loadOriginalImage() {
        if (mNativeTabAndroid != 0) {
            TabImplJni.get().loadOriginalImage(mNativeTabAndroid, TabImpl.this);
        }
    }

    /**
     * Sets whether the tab is showing an error page.  This is reset whenever the tab finishes a
     * navigation.
     * Note: This is kept here to keep the build green. Remove from interface as soon as
     *       the downstream patch lands.
     * @param isShowingErrorPage Whether the tab shows an error page.
     */
    void setIsShowingErrorPage(boolean isShowingErrorPage) {
        mIsShowingErrorPage = isShowingErrorPage;
    }

    /** @return WebContentsState representing the state of the WebContents (navigations, etc.) */
    WebContentsState getFrozenContentsState() {
        return mFrozenContentsState;
    }

    /**
     * Shows a native page for url if it's a valid chrome-native URL. Otherwise, does nothing.
     * @param url The url of the current navigation.
     * @param forceReload If true, the current native page (if any) will not be reused, even if it
     *                    matches the URL.
     * @return True, if a native page was displayed for url.
     */
    boolean maybeShowNativePage(String url, boolean forceReload) {
        // While detached for reparenting we don't have an owning Activity, or TabModelSelector,
        // so we can't create the native page. The native page will be created once reparenting is
        // completed.
        if (isDetached(this)) return false;
        NativePage candidateForReuse = forceReload ? null : getNativePage();
        NativePage nativePage = NativePageFactory.createNativePageForURL(
                url, candidateForReuse, this, getActivity());
        if (nativePage != null) {
            showNativePage(nativePage);
            notifyPageTitleChanged();
            notifyFaviconChanged();
            return true;
        }
        return false;
    }

    /**
     * Calls onContentChanged on all TabObservers and updates accessibility visibility.
     */
    void notifyContentChanged() {
        for (TabObserver observer : mObservers) observer.onContentChanged(this);
        updateAccessibilityVisibility();
    }

    void notifyThemeColorChanged(int themeColor) {
        RewindableIterator<TabObserver> observers = getTabObservers();
        while (observers.hasNext()) {
            observers.next().onDidChangeThemeColor(this, themeColor);
        }
    }

    void updateTitle() {
        if (isFrozen()) return;

        // When restoring the tabs, the title will no longer be populated, so request it from the
        // WebContents or NativePage (if present).
        String title = "";
        if (isNativePage()) {
            title = mNativePage.getTitle();
        } else if (getWebContents() != null) {
            title = getWebContents().getTitle();
        }
        updateTitle(title);
    }

    /**
     * Cache the title for the current page.
     *
     * {@link ContentViewClient#onUpdateTitle} is unreliable, particularly for navigating backwards
     * and forwards in the history stack, so pull the correct title whenever the page changes.
     * onUpdateTitle is only called when the title of a navigation entry changes. When the user goes
     * back a page the navigation entry exists with the correct title, thus the title is not
     * actually changed, and no notification is sent.
     * @param title Title of the page.
     */
    void updateTitle(String title) {
        if (TextUtils.equals(mTitle, title)) return;

        mIsTabStateDirty = true;
        mTitle = title;
        notifyPageTitleChanged();
    }

    /**
     * @return Parameters that should be used for a lazily loaded Tab.  May be null.
     */
    LoadUrlParams getPendingLoadParams() {
        return mPendingLoadParams;
    }

    /**
     * Performs any subclass-specific tasks when the Tab crashes.
     */
    void handleTabCrash() {
        mIsLoading = false;

        RewindableIterator<TabObserver> observers = getTabObservers();
        while (observers.hasNext()) observers.next().onCrash(this);
        mIsBeingRestored = false;
    }

    /**
     * Called when the background color for the content changes.
     * @param color The current for the background.
     */
    void onBackgroundColorChanged(int color) {
        for (TabObserver observer : mObservers) observer.onBackgroundColorChanged(this, color);
    }

    /** This is currently called when committing a pre-rendered page. */
    void swapWebContents(WebContents webContents, boolean didStartLoad, boolean didFinishLoad) {
        boolean hasWebContents = mContentView != null && mWebContents != null;
        Rect original = hasWebContents
                ? new Rect(0, 0, mContentView.getWidth(), mContentView.getHeight())
                : new Rect();
        if (hasWebContents) mWebContents.onHide();
        Context appContext = ContextUtils.getApplicationContext();
        Rect bounds = original.isEmpty()
                ? ExternalPrerenderHandler.estimateContentSize(appContext, false)
                : null;
        if (bounds != null) original.set(bounds);

        mWebContents.setFocus(false);
        destroyWebContents(false /* do not delete native web contents */);
        hideNativePage(false, () -> {
            // Size of the new content is zero at this point. Set the view size in advance
            // so that next onShow() call won't send a resize message with zero size
            // to the renderer process. This prevents the size fluttering that may confuse
            // Blink and break rendered result (see http://crbug.com/340987).
            webContents.setSize(original.width(), original.height());

            if (bounds != null) {
                assert mNativeTabAndroid != 0;
                TabImplJni.get().onPhysicalBackingSizeChanged(
                        mNativeTabAndroid, TabImpl.this, webContents, bounds.right, bounds.bottom);
            }
            webContents.onShow();
            initWebContents(webContents);
        });

        String url = getUrl();

        if (didStartLoad) {
            // Simulate the PAGE_LOAD_STARTED notification that we did not get.
            didStartPageLoad(url);

            // Simulate the PAGE_LOAD_FINISHED notification that we did not get.
            if (didFinishLoad) didFinishPageLoad(url);
        }

        for (TabObserver observer : mObservers) {
            observer.onWebContentsSwapped(this, didStartLoad, didFinishLoad);
        }
    }

    /**
     * Builds the native counterpart to this class.
     */
    private void initializeNative() {
        if (mNativeTabAndroid == 0) TabImplJni.get().init(TabImpl.this);
        assert mNativeTabAndroid != 0;
    }

    /**
     * @return The native pointer representing the native side of this {@link TabImpl} object.
     */
    @CalledByNative
    private long getNativePtr() {
        return mNativeTabAndroid;
    }

    @CalledByNative
    private void clearNativePtr() {
        assert mNativeTabAndroid != 0;
        mNativeTabAndroid = 0;
    }

    @CalledByNative
    private void setNativePtr(long nativePtr) {
        assert nativePtr != 0;
        assert mNativeTabAndroid == 0;
        mNativeTabAndroid = nativePtr;
    }

    /**
     * Initializes the {@link WebContents}. Completes the browser content components initialization
     * around a native WebContents pointer.
     * <p>
     * {@link #getNativePage()} will still return the {@link NativePage} if there is one.
     * All initialization that needs to reoccur after a web contents swap should be added here.
     * <p />
     * NOTE: If you attempt to pass a native WebContents that does not have the same incognito
     * state as this tab this call will fail.
     *
     * @param webContents The WebContents object that will initialize all the browser components.
     */
    private void initWebContents(WebContents webContents) {
        try {
            TraceEvent.begin("ChromeTab.initWebContents");
            WebContents oldWebContents = mWebContents;
            mWebContents = webContents;

            ContentView cv = ContentView.createContentView(mThemedApplicationContext, webContents);
            cv.setContentDescription(mThemedApplicationContext.getResources().getString(
                    R.string.accessibility_content_view));
            mContentView = cv;
            webContents.initialize(PRODUCT_VERSION, new TabViewAndroidDelegate(this, cv), cv,
                    getWindowAndroid(), WebContents.createDefaultInternalsHolder());
            hideNativePage(false, null);

            if (oldWebContents != null) {
                oldWebContents.setImportance(ChildProcessImportance.NORMAL);
                getWebContentsAccessibility(oldWebContents).setObscuredByAnotherView(false);
            }

            mWebContents.setImportance(mImportance);
            ContentUtils.setUserAgentOverride(mWebContents);

            mContentView.addOnAttachStateChangeListener(mAttachStateChangeListener);
            updateInteractableState();

            mWebContentsDelegate = mDelegateFactory.createWebContentsDelegate(this);

            assert mNativeTabAndroid != 0;
            TabImplJni.get().initWebContents(mNativeTabAndroid, TabImpl.this, mIncognito,
                    isDetached(this), webContents, mSourceTabId, mWebContentsDelegate,
                    new TabContextMenuPopulator(
                            mDelegateFactory.createContextMenuPopulator(this), this));

            mWebContents.notifyRendererPreferenceUpdate();
            TabHelpers.initWebContentsHelpers(this);
            notifyContentChanged();
        } finally {
            TraceEvent.end("ChromeTab.initWebContents");
        }
    }

    /**
     * Shows the given {@code nativePage} if it's not already showing.
     * @param nativePage The {@link NativePage} to show.
     */
    private void showNativePage(NativePage nativePage) {
        assert nativePage != null;
        if (mNativePage == nativePage) return;
        hideNativePage(true, () -> {
            mNativePage = nativePage;
            if (!mNativePage.isFrozen()) {
                mNativePage.getView().addOnAttachStateChangeListener(mAttachStateChangeListener);
            }
            pushNativePageStateToNavigationEntry();
        });
    }

    /**
     * Hide and destroy the native page if it was being shown.
     * @param notify {@code true} to trigger {@link #onContentChanged} event.
     * @param postHideTask {@link Runnable} task to run before actually destroying the
     *        native page. This is necessary to keep the tasks to perform in order.
     */
    private void hideNativePage(boolean notify, Runnable postHideTask) {
        NativePage previousNativePage = mNativePage;
        if (mNativePage != null) {
            if (!mNativePage.isFrozen()) {
                mNativePage.getView().removeOnAttachStateChangeListener(mAttachStateChangeListener);
            }
            mNativePage = null;
        }
        if (postHideTask != null) postHideTask.run();
        if (notify) notifyContentChanged();
        destroyNativePageInternal(previousNativePage);
    }

    /**
     * Set {@link TabDelegateFactory} instance and updates the references.
     * @param factory TabDelegateFactory instance.
     */
    private void setDelegateFactory(TabDelegateFactory factory) {
        // Update the delegate factory, then recreate and propagate all delegates.
        mDelegateFactory = factory;

        mWebContentsDelegate = mDelegateFactory.createWebContentsDelegate(this);

        if (getWebContents() != null) {
            TabImplJni.get().updateDelegates(mNativeTabAndroid, TabImpl.this, mWebContentsDelegate,
                    new TabContextMenuPopulator(
                            mDelegateFactory.createContextMenuPopulator(this), this));
        }
    }

    private void notifyPageTitleChanged() {
        RewindableIterator<TabObserver> observers = getTabObservers();
        while (observers.hasNext()) {
            observers.next().onTitleUpdated(this);
        }
    }

    private void notifyFaviconChanged() {
        RewindableIterator<TabObserver> observers = getTabObservers();
        while (observers.hasNext()) {
            observers.next().onFaviconUpdated(this, null);
        }
    }

    /**
     * Update the interactable state of the tab. If the state has changed, it will call the
     * {@link #onInteractableStateChanged(boolean)} method.
     */
    private void updateInteractableState() {
        boolean currentState = !mIsHidden && !isFrozen()
                && (mIsViewAttachedToWindow || VrModuleProvider.getDelegate().isInVr());

        if (currentState == mInteractableState) return;

        mInteractableState = currentState;
        for (TabObserver observer : mObservers) {
            observer.onInteractabilityChanged(this, currentState);
        }
    }

    /**
     * Loads a tab that was already loaded but since then was lost. This happens either when we
     * unfreeze the tab from serialized state or when we reload a tab that crashed. In both cases
     * the load codepath is the same (run in loadIfNecessary()) and the same caching policies of
     * history load are used.
     */
    private final void restoreIfNeeded() {
        try {
            TraceEvent.begin("Tab.restoreIfNeeded");
            if (isFrozen() && mFrozenContentsState != null) {
                // Restore is needed for a tab that is loaded for the first time. WebContents will
                // be restored from a saved state.
                unfreezeContents();
            } else if (!needsReload()) {
                return;
            }

            if (mWebContents != null) mWebContents.getNavigationController().loadIfNecessary();
            mIsBeingRestored = true;
            for (TabObserver observer : mObservers) observer.onRestoreStarted(this);
        } finally {
            TraceEvent.end("Tab.restoreIfNeeded");
        }
    }

    /**
     * Restores the WebContents from its saved state.  This should only be called if the tab is
     * frozen with a saved TabState, and NOT if it was frozen for a lazy load.
     * @return Whether or not the restoration was successful.
     */
    private void unfreezeContents() {
        try {
            TraceEvent.begin("Tab.unfreezeContents");
            assert mFrozenContentsState != null;

            WebContents webContents =
                    mFrozenContentsState.restoreContentsFromByteBuffer(isHidden());
            boolean failedToRestore = false;
            if (webContents == null) {
                // State restore failed, just create a new empty web contents as that is the best
                // that can be done at this point. TODO(jcivelli) http://b/5910521 - we should show
                // an error page instead of a blank page in that case (and the last loaded URL).
                webContents = WebContentsFactory.createWebContents(isIncognito(), isHidden());
                TabUma.create(this, TabCreationState.FROZEN_ON_RESTORE_FAILED);
                failedToRestore = true;
            }
            View compositorView = getActivity().getCompositorViewHolder();
            webContents.setSize(compositorView.getWidth(), compositorView.getHeight());

            mFrozenContentsState = null;
            initWebContents(webContents);

            if (failedToRestore) {
                String url = TextUtils.isEmpty(mUrl) ? UrlConstants.NTP_URL : mUrl;
                loadUrl(new LoadUrlParams(url, PageTransition.GENERATED));
            }
        } finally {
            TraceEvent.end("Tab.unfreezeContents");
        }
    }

    private boolean isCustomTab() {
        ChromeActivity activity = getActivity();
        return activity != null && activity.isCustomTab();
    }

    /**
     * Throws a RuntimeException. Useful for testing crash reports with obfuscated Java stacktraces.
     */
    private int handleJavaCrash() {
        throw new RuntimeException("Intentional Java Crash");
    }

    /**
     * Delete navigation entries from frozen state matching the predicate.
     * @param predicate Handle for a deletion predicate interpreted by native code.
     *                  Only valid during this call frame.
     */
    @CalledByNative
    private void deleteNavigationEntriesFromFrozenState(long predicate) {
        if (mFrozenContentsState == null) return;
        WebContentsState newState = mFrozenContentsState.deleteNavigationEntries(predicate);
        if (newState != null) {
            mFrozenContentsState = newState;
            notifyNavigationEntriesDeleted();
        }
    }

    private boolean isObscuredByAnotherViewForAccessibility() {
        ChromeActivity activity = getActivity();
        return activity != null && activity.isViewObscuringAllTabs();
    }

    private static WebContentsAccessibility getWebContentsAccessibility(WebContents webContents) {
        return webContents != null ? WebContentsAccessibility.fromWebContents(webContents) : null;
    }

    private void destroyNativePageInternal(NativePage nativePage) {
        if (nativePage == null) return;
        assert nativePage != mNativePage : "Attempting to destroy active page.";

        nativePage.destroy();
    }

    /**
     * Destroys the current {@link WebContents}.
     * @param deleteNativeWebContents Whether or not to delete the native WebContents pointer.
     */
    private final void destroyWebContents(boolean deleteNativeWebContents) {
        if (mWebContents == null) return;

        mContentView.removeOnAttachStateChangeListener(mAttachStateChangeListener);
        mContentView = null;
        updateInteractableState();

        WebContents contentsToDestroy = mWebContents;
        mWebContents = null;
        mWebContentsDelegate = null;

        assert mNativeTabAndroid != 0;
        if (deleteNativeWebContents) {
            // Destruction of the native WebContents will call back into Java to destroy the Java
            // WebContents.
            TabImplJni.get().destroyWebContents(mNativeTabAndroid, TabImpl.this);
        } else {
            TabImplJni.get().releaseWebContents(mNativeTabAndroid, TabImpl.this);
            // Since the native WebContents is still alive, we need to clear its reference to the
            // Java WebContents. While doing so, it will also call back into Java to destroy the
            // Java WebContents.
            contentsToDestroy.clearNativeReference();
        }
    }

    @NativeMethods
    interface Natives {
        void init(TabImpl caller);
        void destroy(long nativeTabAndroid, TabImpl caller);
        void initWebContents(long nativeTabAndroid, TabImpl caller, boolean incognito,
                boolean isBackgroundTab, WebContents webContents, int parentTabId,
                TabWebContentsDelegateAndroid delegate, ContextMenuPopulator contextMenuPopulator);
        void updateDelegates(long nativeTabAndroid, TabImpl caller,
                TabWebContentsDelegateAndroid delegate, ContextMenuPopulator contextMenuPopulator);
        void destroyWebContents(long nativeTabAndroid, TabImpl caller);
        void releaseWebContents(long nativeTabAndroid, TabImpl caller);
        void onPhysicalBackingSizeChanged(long nativeTabAndroid, TabImpl caller,
                WebContents webContents, int width, int height);
        int loadUrl(long nativeTabAndroid, TabImpl caller, String url, String initiatorOrigin,
                String extraHeaders, ResourceRequestBody postData, int transition,
                String referrerUrl, int referrerPolicy, boolean isRendererInitiated,
                boolean shoulReplaceCurrentEntry, boolean hasUserGesture,
                boolean shouldClearHistoryList, long inputStartTimestamp,
                long intentReceivedTimestamp);
        void setActiveNavigationEntryTitleForUrl(
                long nativeTabAndroid, TabImpl caller, String url, String title);
        void loadOriginalImage(long nativeTabAndroid, TabImpl caller);
    }
}
