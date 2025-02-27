// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.signin;

import static android.support.test.espresso.Espresso.onView;
import static android.support.test.espresso.action.ViewActions.click;
import static android.support.test.espresso.matcher.RootMatchers.isDialog;
import static android.support.test.espresso.matcher.ViewMatchers.withId;

import static org.hamcrest.Matchers.sameInstance;

import android.app.Instrumentation.ActivityMonitor;
import android.content.DialogInterface;
import android.support.test.InstrumentationRegistry;
import android.support.test.filters.MediumTest;
import android.support.v4.app.DialogFragment;
import android.support.v4.app.Fragment;
import android.support.v7.app.AlertDialog;
import android.support.v7.app.AppCompatActivity;
import android.support.v7.preference.Preference;
import android.widget.Button;

import org.junit.After;
import org.junit.Assert;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;

import org.chromium.base.ThreadUtils;
import org.chromium.base.test.util.CommandLineFlags;
import org.chromium.base.test.util.Restriction;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.ChromeSwitches;
import org.chromium.chrome.browser.bookmarks.BookmarkBridge;
import org.chromium.chrome.browser.preferences.Pref;
import org.chromium.chrome.browser.preferences.PrefServiceBridge;
import org.chromium.chrome.browser.profiles.Profile;
import org.chromium.chrome.browser.settings.MainSettings;
import org.chromium.chrome.browser.settings.SettingsActivity;
import org.chromium.chrome.browser.settings.sync.AccountManagementFragment;
import org.chromium.chrome.browser.settings.sync.SignInPreference;
import org.chromium.chrome.browser.tab.TabImpl;
import org.chromium.chrome.test.ChromeJUnit4ClassRunner;
import org.chromium.chrome.test.ChromeTabbedActivityTestRule;
import org.chromium.chrome.test.util.ActivityUtils;
import org.chromium.chrome.test.util.BookmarkTestUtil;
import org.chromium.chrome.test.util.ChromeRestriction;
import org.chromium.chrome.test.util.browser.signin.SigninTestUtil;
import org.chromium.components.bookmarks.BookmarkId;
import org.chromium.components.signin.ChromeSigninController;
import org.chromium.components.signin.metrics.SignoutReason;
import org.chromium.content_public.browser.test.util.TestThreadUtils;

/**
 * Test suite for sign in tests.
 *
 * These tests cover the sign in flow for both consumer and managed accounts. They also verify
 * the state of the browser while signed in, and any changes when signing out.
 *
 * The accounts used to sign in are mocked by a FakeAccountManagerDelegate.
 */
@RunWith(ChromeJUnit4ClassRunner.class)
@CommandLineFlags.Add({ChromeSwitches.DISABLE_FIRST_RUN_EXPERIENCE})
public class SigninTest {
    /**
     * Helper class that observes when signing in becomes allowed.
     */

    @Rule
    public ChromeTabbedActivityTestRule mActivityTestRule = new ChromeTabbedActivityTestRule();

    private static class TestSignInAllowedObserver implements SigninManager.SignInAllowedObserver {
        private final Object mLock = new Object();
        private boolean mIsSignInAllowed;
        private SigninManager mSigninManager;

        public void startObserving(SigninManager signinManager) {
            ThreadUtils.assertOnUiThread();
            mIsSignInAllowed = signinManager.isSignInAllowed();
            if (!mIsSignInAllowed) {
                mSigninManager = signinManager;
                mSigninManager.addSignInAllowedObserver(this);
            }
        }

        @Override
        public void onSignInAllowedChanged() {
            ThreadUtils.assertOnUiThread();
            synchronized (mLock) {
                if (mSigninManager.isSignInAllowed()) {
                    mIsSignInAllowed = true;
                    mSigninManager.removeSignInAllowedObserver(this);
                    mLock.notifyAll();
                }
            }
        }

        public void waitForSignInAllowed() {
            assert !ThreadUtils.runningOnUiThread();
            synchronized (mLock) {
                while (!mIsSignInAllowed) {
                    try {
                        mLock.wait();
                    } catch (InterruptedException exception) {
                        // Ignore.
                    }
                }
            }
        }
    }

    /**
     * Helper class that observes signin state changes.
     */
    private static class TestSignInObserver implements SigninManager.SignInStateObserver {
        private final Object mLock = new Object();
        public int mSignInCount;
        public int mSignOutCount;

        public void waitForSignInEvents(int total) {
            synchronized (mLock) {
                while (mSignInCount + mSignOutCount < total) {
                    try {
                        mLock.wait();
                    } catch (InterruptedException exception) {
                        // Ignore.
                    }
                }
            }
        }

        @Override
        public void onSignedIn() {
            synchronized (mLock) {
                mSignInCount++;
                mLock.notifyAll();
            }
        }

        @Override
        public void onSignedOut() {
            synchronized (mLock) {
                mSignOutCount++;
                mLock.notifyAll();
            }
        }
    }

    /**
     * Helper class that waits for the BookmarkModel to be ready.
     */
    private static class TestBookmarkModelObserver extends BookmarkBridge.BookmarkModelObserver {
        private final Object mLock = new Object();
        private boolean mAdded;

        TestBookmarkModelObserver() {
            mAdded = false;
        }

        void waitForBookmarkAdded() {
            synchronized (mLock) {
                while (!mAdded) {
                    try {
                        mLock.wait();
                    } catch (InterruptedException exception) {
                        // Ignore.
                    }
                }
            }
        }

        @Override
        public void bookmarkNodeAdded(BookmarkBridge.BookmarkItem parent, int index) {
            synchronized (mLock) {
                mAdded = true;
                mLock.notify();
            }
        }

        @Override
        public void bookmarkAllUserNodesRemoved() {
            synchronized (mLock) {
                mLock.notify();
            }
        }

        @Override
        public void bookmarkModelChanged() {
            // Ignore.
        }
    }

    private SigninManager mSigninManager;
    private PrefServiceBridge mPrefService;
    private BookmarkBridge mBookmarks;
    private TestBookmarkModelObserver mTestBookmarkModelObserver;
    private TestSignInObserver mTestSignInObserver;

    @Before
    public void setUp() throws Exception {
        // Mock out the account manager on the device.
        SigninTestUtil.setUpAuthForTest();

        mActivityTestRule.startMainActivityOnBlankPage();
        final TestSignInAllowedObserver signinAllowedObserver = new TestSignInAllowedObserver();

        TestThreadUtils.runOnUiThreadBlocking(() -> {
            // This call initializes the ChromeSigninController to use our test context.
            ChromeSigninController.get();

            // Start observing the SigninManager.
            mTestSignInObserver = new TestSignInObserver();
            mSigninManager = IdentityServicesProvider.get().getSigninManager();
            mSigninManager.addSignInStateObserver(mTestSignInObserver);

            // Get these handles in the UI thread.
            mPrefService = PrefServiceBridge.getInstance();
            Profile profile =
                    ((TabImpl) mActivityTestRule.getActivity().getActivityTab()).getProfile();
            mBookmarks = new BookmarkBridge(profile);
            mBookmarks.loadFakePartnerBookmarkShimForTesting();
        });
        BookmarkTestUtil.waitForBookmarkModelLoaded();
        TestThreadUtils.runOnUiThreadBlocking(() -> {
            // Add a test bookmark, to verify later if sign out cleared the bookmarks.
            mTestBookmarkModelObserver = new TestBookmarkModelObserver();
            mBookmarks.addObserver(mTestBookmarkModelObserver);
            Assert.assertEquals(0, mBookmarks.getChildCount(mBookmarks.getMobileFolderId()));
            BookmarkId mTestBookmark = mBookmarks.addBookmark(
                    mBookmarks.getMobileFolderId(), 0, "Test Bookmark", "http://google.com");
            mTestBookmarkModelObserver.waitForBookmarkAdded();
            Assert.assertNotNull(mTestBookmark);
            Assert.assertEquals(1, mBookmarks.getChildCount(mBookmarks.getMobileFolderId()));

            // Start observing if signing in is allowed. This observer must be installed on
            // the UI thread, but waiting must be done outside the UI thread (otherwise it
            // won't ever unblock).
            signinAllowedObserver.startObserving(mSigninManager);
        });

        signinAllowedObserver.waitForSignInAllowed();
        Assert.assertTrue(mSigninManager.isSignInAllowed());
    }

    @After
    public void tearDown() {
        TestThreadUtils.runOnUiThreadBlocking(() -> {
            mBookmarks.removeObserver(mTestBookmarkModelObserver);

            mSigninManager.removeSignInStateObserver(mTestSignInObserver);

            if (ChromeSigninController.get().isSignedIn()) {
                mSigninManager.signOut(SignoutReason.SIGNOUT_TEST);
            }

            mBookmarks.destroy();
        });
        SigninTestUtil.tearDownAuthForTest();
    }

    @Test
    @MediumTest
    @Restriction(ChromeRestriction.RESTRICTION_TYPE_GOOGLE_PLAY_SERVICES)
    public void testConsumerSignin() {
        SigninTestUtil.addTestAccount();
        signInToSingleAccount();

        TestThreadUtils.runOnUiThreadBlocking(() -> {
            // Verify that the account isn't managed.
            Assert.assertNull(mSigninManager.getManagementDomain());

            // Verify that the password manager is enabled by default.
            Assert.assertTrue(mPrefService.getBoolean(Pref.REMEMBER_PASSWORDS_ENABLED));
            Assert.assertFalse(mPrefService.isManagedPreference(Pref.REMEMBER_PASSWORDS_ENABLED));
        });

        // Verify that its preference UI is enabled.
        SettingsActivity settingsActivity = mActivityTestRule.startSettingsActivity(null);
        MainSettings mainPrefs = getMainSettings(settingsActivity);
        Preference passwordPref = mainPrefs.findPreference(MainSettings.PREF_PASSWORDS);
        Assert.assertNotNull(passwordPref);
        // This preference opens a new fragment when clicked.
        Assert.assertNotNull(passwordPref.getFragment());
        // There is no icon for this preference by default.
        Assert.assertNull(passwordPref.getIcon());
        settingsActivity.finish();

        // Sign out now.
        signOut();

        TestThreadUtils.runOnUiThreadBlocking(() -> {
            // Verify that the profile data hasn't been wiped when signing out of a normal
            // account. We check that by looking for the test bookmark from setUp().
            Assert.assertEquals(1, mBookmarks.getChildCount(mBookmarks.getMobileFolderId()));
        });
    }

    private void signInToSingleAccount() {
        // Verify that we aren't signed in yet.
        Assert.assertFalse(ChromeSigninController.get().isSignedIn());

        // Open the settings UI.
        final SettingsActivity settingsActivity = mActivityTestRule.startSettingsActivity(null);
        InstrumentationRegistry.getInstrumentation().waitForIdleSync();

        // Create a monitor to catch the SigninActivity when it is created.
        ActivityMonitor monitor = InstrumentationRegistry.getInstrumentation().addMonitor(
                SigninActivity.class.getName(), null, false);

        // Click sign in.
        TestThreadUtils.runOnUiThreadBlocking(() -> clickSigninPreference(settingsActivity));

        // Pick the mock account.
        SigninActivity signinActivity =
                (SigninActivity) InstrumentationRegistry.getInstrumentation().waitForMonitor(
                        monitor);
        onView(withId(R.id.positive_button)).perform(click());
        InstrumentationRegistry.getInstrumentation().waitForIdleSync();
        settingsActivity.finish();

        // Verify that signin succeeded.
        mTestSignInObserver.waitForSignInEvents(1);
        Assert.assertEquals(1, mTestSignInObserver.mSignInCount);
        Assert.assertEquals(0, mTestSignInObserver.mSignOutCount);
        Assert.assertTrue(ChromeSigninController.get().isSignedIn());
    }

    private void signOut() {
        // Verify that we are currently signed in.
        Assert.assertTrue(ChromeSigninController.get().isSignedIn());

        // Open the account settings.
        final SettingsActivity settingsActivity =
                mActivityTestRule.startSettingsActivity(AccountManagementFragment.class.getName());
        InstrumentationRegistry.getInstrumentation().waitForIdleSync();

        // Click on the signout button.
        TestThreadUtils.runOnUiThreadBlocking(() -> clickSignOut(settingsActivity));

        // Accept the warning dialog.
        acceptAlertDialogWithTag(settingsActivity, AccountManagementFragment.SIGN_OUT_DIALOG_TAG);

        // Verify that signout succeeded.
        mTestSignInObserver.waitForSignInEvents(2);
        Assert.assertEquals(1, mTestSignInObserver.mSignInCount);
        Assert.assertEquals(1, mTestSignInObserver.mSignOutCount);
        Assert.assertFalse(ChromeSigninController.get().isSignedIn());

        if (!settingsActivity.isFinishing()) settingsActivity.finish();
        InstrumentationRegistry.getInstrumentation().waitForIdleSync();
    }

    private static MainSettings getMainSettings(SettingsActivity settingsActivity) {
        Fragment fragment = settingsActivity.getMainFragment();
        Assert.assertNotNull(fragment);
        Assert.assertTrue(fragment instanceof MainSettings);
        return (MainSettings) fragment;
    }

    private static void clickSigninPreference(SettingsActivity settingsActivity) {
        MainSettings mainPrefs = getMainSettings(settingsActivity);
        Preference signinPref = mainPrefs.findPreference(MainSettings.PREF_SIGN_IN);
        Assert.assertNotNull(signinPref);
        Assert.assertTrue(signinPref instanceof SignInPreference);
        Assert.assertNotNull(signinPref.getOnPreferenceClickListener());
        signinPref.getOnPreferenceClickListener().onPreferenceClick(signinPref);
    }

    private static void clickSignOut(SettingsActivity settingsActivity) {
        Fragment fragment = settingsActivity.getMainFragment();
        Assert.assertNotNull(fragment);
        Assert.assertTrue(fragment instanceof AccountManagementFragment);
        AccountManagementFragment managementFragment = (AccountManagementFragment) fragment;
        Preference signOutPref = managementFragment.findPreference(
                AccountManagementFragment.PREF_SIGN_OUT);
        Assert.assertNotNull(signOutPref);
        Assert.assertNotNull(signOutPref.getOnPreferenceClickListener());
        signOutPref.getOnPreferenceClickListener().onPreferenceClick(signOutPref);
    }

    private void acceptAlertDialogWithTag(AppCompatActivity activity, String tag) {
        InstrumentationRegistry.getInstrumentation().waitForIdleSync();
        DialogFragment fragment = ActivityUtils.waitForFragment(activity, tag);
        AlertDialog dialog = (AlertDialog) fragment.getDialog();
        Assert.assertNotNull(dialog);
        Assert.assertTrue(dialog.isShowing());
        Button button = dialog.getButton(DialogInterface.BUTTON_POSITIVE);
        Assert.assertNotNull("Could not find the accept button.", button);
        onView(sameInstance(button)).inRoot(isDialog()).perform(click());
    }
}
