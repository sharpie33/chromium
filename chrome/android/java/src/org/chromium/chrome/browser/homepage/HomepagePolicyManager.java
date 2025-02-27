// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.homepage;

import android.text.TextUtils;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.annotation.VisibleForTesting;

import org.chromium.base.ObserverList;
import org.chromium.chrome.browser.flags.CachedFeatureFlags;
import org.chromium.chrome.browser.flags.ChromeFeatureList;
import org.chromium.chrome.browser.init.ChromeBrowserInitializer;
import org.chromium.chrome.browser.preferences.ChromePreferenceKeys;
import org.chromium.chrome.browser.preferences.Pref;
import org.chromium.chrome.browser.preferences.PrefChangeRegistrar;
import org.chromium.chrome.browser.preferences.PrefChangeRegistrar.PrefObserver;
import org.chromium.chrome.browser.preferences.PrefServiceBridge;
import org.chromium.chrome.browser.preferences.SharedPreferencesManager;

/**
 * Provides information for the home page related policies.
 * Monitors changes for the homepage preference.
 */
public class HomepagePolicyManager implements PrefObserver {
    /**
     * An interface to receive updates from {@link HomepagePolicyManager}.
     */
    public interface HomepagePolicyStateListener {
        /**
         * Will be called when homepage policy status change. Though cases are rare, when homepage
         * policy has changed during runtime, listeners will receive updates.
         */
        void onHomepagePolicyUpdate();
    }

    private static HomepagePolicyManager sInstance;

    private boolean mIsHomepageLocationPolicyEnabled;
    private String mHomepage;

    private boolean mIsInitializedWithNative;
    private PrefChangeRegistrar mPrefChangeRegistrar;
    private SharedPreferencesManager mSharedPreferenceManager;

    private final ObserverList<HomepagePolicyStateListener> mListeners = new ObserverList<>();

    /**
     * @return The singleton instance of {@link HomepagePolicyManager}.
     */
    public static HomepagePolicyManager getInstance() {
        if (sInstance == null) {
            sInstance = new HomepagePolicyManager();
        }
        return sInstance;
    }

    /**
     * If policies such as HomepageLocation are enabled on this device, the home page will be marked
     * as managed.
     * @return True if the current home page is managed by enterprise policy.
     */
    public static boolean isHomepageManagedByPolicy() {
        return isFeatureFlagEnabled() && getInstance().isHomepageLocationPolicyEnabled();
    }

    /**
     * @return The homepage URL from the homepage preference.
     */
    @NonNull
    public static String getHomepageUrl() {
        return getInstance().getHomepagePreference();
    }

    /**
     * Adds a HomepagePolicyStateListener to receive updates when the homepage policy changes.
     * @param listener Object that would like to listen to changes from homepage policy.
     */
    public void addListener(HomepagePolicyStateListener listener) {
        mListeners.addObserver(listener);
    }

    /**
     * Stop observing pref changes and destroy the singleton instance.
     * Will be called from {@link org.chromium.chrome.browser.ChromeActivitySessionTracker}.
     */
    public static void destroy() {
        sInstance.destroyInternal();
        sInstance = null;
    }

    @VisibleForTesting
    static void setInstanceForTests(HomepagePolicyManager instance) {
        assert instance != null;
        sInstance = instance;
    }

    @VisibleForTesting
    HomepagePolicyManager() {
        mIsInitializedWithNative = false;
        mPrefChangeRegistrar = null;

        // Update feature flag related setting
        mSharedPreferenceManager = SharedPreferencesManager.getInstance();
        mHomepage = mSharedPreferenceManager.readString(
                ChromePreferenceKeys.HOMEPAGE_LOCATION_POLICY, "");
        mIsHomepageLocationPolicyEnabled = !TextUtils.isEmpty(mHomepage);

        if (isFeatureFlagEnabled()) {
            ChromeBrowserInitializer.getInstance().runNowOrAfterFullBrowserStarted(
                    this::onFinishNativeInitialization);
        }
    }

    /**
     * Constructor for unit tests.
     * @param prefChangeRegistrar Instance of {@link PrefChangeRegistrar} or test mocking.
     * @param listener Object extends {@link HomepagePolicyStateListener}. Will be added between
     *         singleton {@link HomepagePolicyManager} created, and have it initialized with {@link
     *         #initializeWithNative(PrefChangeRegistrar)} so that it will get the update from
     *         {@link HomepagePolicyStateListener#onHomepagePolicyUpdate()}.
     */
    @VisibleForTesting
    HomepagePolicyManager(@NonNull PrefChangeRegistrar prefChangeRegistrar,
            @Nullable HomepagePolicyStateListener listener) {
        this();

        if (listener != null) addListener(listener);
        if (isFeatureFlagEnabled()) initializeWithNative(prefChangeRegistrar);
    }

    /**
     * Initialize the instance with preference registrar, and start listen to changes for homepage
     * preference.
     * @param prefChangeRegistrar Instance of {@link PrefChangeRegistrar} or test mocking.
     */
    @VisibleForTesting
    void initializeWithNative(PrefChangeRegistrar prefChangeRegistrar) {
        assert isFeatureFlagEnabled();

        mPrefChangeRegistrar = prefChangeRegistrar;
        mPrefChangeRegistrar.addObserver(Pref.HOME_PAGE, this);

        mIsInitializedWithNative = true;
        refresh();
    }

    @Override
    public void onPreferenceChange() {
        assert isFeatureFlagEnabled();
        refresh();
    }

    private void destroyInternal() {
        if (mPrefChangeRegistrar != null) mPrefChangeRegistrar.destroy();
        mListeners.clear();
    }

    private void refresh() {
        assert mIsInitializedWithNative;
        PrefServiceBridge prefServiceBridge = PrefServiceBridge.getInstance();
        boolean isEnabled = prefServiceBridge.isManagedPreference(Pref.HOME_PAGE);
        String homepage = "";
        if (isEnabled) {
            homepage = prefServiceBridge.getString(Pref.HOME_PAGE);
            assert homepage != null;
        }

        // Early return when nothing changes
        if (isEnabled == mIsHomepageLocationPolicyEnabled && homepage.equals(mHomepage)) return;

        mIsHomepageLocationPolicyEnabled = isEnabled;
        mHomepage = homepage;

        // Update shared preference
        mSharedPreferenceManager.writeString(
                ChromePreferenceKeys.HOMEPAGE_LOCATION_POLICY, mHomepage);

        // Update the listeners about the status
        for (HomepagePolicyStateListener listener : mListeners) {
            listener.onHomepagePolicyUpdate();
        }
    }

    /**
     * Called when the native library has finished loading.
     */
    private void onFinishNativeInitialization() {
        if (!mIsInitializedWithNative) initializeWithNative(new PrefChangeRegistrar());
    }

    private static boolean isFeatureFlagEnabled() {
        return CachedFeatureFlags.isEnabled(ChromeFeatureList.HOMEPAGE_LOCATION_POLICY);
    }

    @VisibleForTesting
    boolean isHomepageLocationPolicyEnabled() {
        return mIsHomepageLocationPolicyEnabled;
    }

    @VisibleForTesting
    @NonNull
    String getHomepagePreference() {
        assert mIsHomepageLocationPolicyEnabled;
        return mHomepage;
    }

    @VisibleForTesting
    boolean isInitialized() {
        return mIsInitializedWithNative;
    }

    @VisibleForTesting
    ObserverList<HomepagePolicyStateListener> getListenersForTesting() {
        return mListeners;
    }
}
