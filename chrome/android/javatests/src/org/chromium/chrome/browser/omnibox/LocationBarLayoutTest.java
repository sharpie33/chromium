// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.omnibox;

import static android.support.test.espresso.Espresso.onView;
import static android.support.test.espresso.matcher.ViewMatchers.withId;
import static android.view.View.GONE;
import static android.view.View.VISIBLE;

import android.support.test.filters.SmallTest;
import android.view.View;
import android.widget.ImageButton;
import android.widget.ImageView;

import org.junit.Assert;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;

import org.chromium.base.ContextUtils;
import org.chromium.base.metrics.RecordHistogram;
import org.chromium.base.test.util.CommandLineFlags;
import org.chromium.base.test.util.Feature;
import org.chromium.base.test.util.Restriction;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.ChromeActivity;
import org.chromium.chrome.browser.ChromeSwitches;
import org.chromium.chrome.browser.flags.ChromeFeatureList;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.toolbar.LocationBarModel;
import org.chromium.chrome.browser.util.UrlConstants;
import org.chromium.chrome.test.ChromeActivityTestRule;
import org.chromium.chrome.test.ChromeJUnit4ClassRunner;
import org.chromium.chrome.test.util.browser.Features.EnableFeatures;
import org.chromium.components.security_state.ConnectionSecurityLevel;
import org.chromium.content_public.browser.test.util.TestThreadUtils;
import org.chromium.ui.test.util.UiRestriction;

import java.util.concurrent.Callable;
import java.util.concurrent.ExecutionException;

/**
 * Unit tests for {@link LocationBarLayout}.
 */
@RunWith(ChromeJUnit4ClassRunner.class)
@CommandLineFlags.Add({ChromeSwitches.DISABLE_FIRST_RUN_EXPERIENCE})
public class LocationBarLayoutTest {
    @Rule
    public ChromeActivityTestRule<ChromeActivity> mActivityTestRule =
            new ChromeActivityTestRule<>(ChromeActivity.class);

    private static final String SEARCH_TERMS = "machine learning";
    private static final String SEARCH_TERMS_URL = "testing.com";
    private static final String GOOGLE_SRP_URL = "https://www.google.com/search?q=machine+learning";

    private static final String VERBOSE_URL = "https://www.suchwowveryyes.edu";
    private static final String TRIMMED_URL = "suchwowveryyes.edu";

    private static final String GOOGLE_URL = "https://www.google.com";
    private static final String YAHOO_URL = "https://www.yahoo.com";

    private TestLocationBarModel mTestLocationBarModel;

    private class TestLocationBarModel extends LocationBarModel {
        private String mCurrentUrl;
        private String mEditingText;
        private String mDisplayText;
        private String mDisplaySearchTerms;
        private Integer mSecurityLevel;

        public TestLocationBarModel() {
            super(ContextUtils.getApplicationContext());
            initializeWithNative();
        }

        void setCurrentUrl(String url) {
            mCurrentUrl = url;
        }

        void setDisplaySearchTerms(String terms) {
            mDisplaySearchTerms = terms;
        }

        void setSecurityLevel(@ConnectionSecurityLevel int securityLevel) {
            mSecurityLevel = securityLevel;
        }

        @Override
        public String getCurrentUrl() {
            if (mCurrentUrl == null) return super.getCurrentUrl();
            return mCurrentUrl;
        }

        @Override
        public String getDisplaySearchTerms() {
            if (mDisplaySearchTerms == null) return super.getDisplaySearchTerms();
            return mDisplaySearchTerms;
        }

        @Override
        @ConnectionSecurityLevel
        public int getSecurityLevel() {
            if (mSecurityLevel == null) return super.getSecurityLevel();
            return mSecurityLevel;
        }

        @Override
        public UrlBarData getUrlBarData() {
            UrlBarData urlBarData = super.getUrlBarData();
            CharSequence displayText = mDisplayText == null ? urlBarData.displayText : mDisplayText;
            String editingText = mEditingText == null ? urlBarData.editingText : mEditingText;
            return UrlBarData.forUrlAndText(getCurrentUrl(), displayText.toString(), editingText);
        }
    }

    @Before
    public void setUp() throws InterruptedException {
        mActivityTestRule.startMainActivityOnBlankPage();
        setupModelsForCurrentTab();
    }

    private void setupModelsForCurrentTab() {
        mTestLocationBarModel = new TestLocationBarModel();
        Tab tab = mActivityTestRule.getActivity().getActivityTab();
        mTestLocationBarModel.setTab(tab, tab.isIncognito());

        TestThreadUtils.runOnUiThreadBlocking(
                () -> getLocationBar().setToolbarDataProvider(mTestLocationBarModel));
    }

    private void setUrlToPageUrl(LocationBarLayout locationBar) {
        TestThreadUtils.runOnUiThreadBlocking(() -> { getLocationBar().updateLoadingState(true); });
    }

    private String getUrlText(UrlBar urlBar) {
        try {
            return TestThreadUtils.runOnUiThreadBlocking(() -> urlBar.getText().toString());
        } catch (ExecutionException ex) {
            throw new RuntimeException(ex);
        }
    }

    private UrlBar getUrlBar() {
        return (UrlBar) mActivityTestRule.getActivity().findViewById(R.id.url_bar);
    }

    private LocationBarLayout getLocationBar() {
        return (LocationBarLayout) mActivityTestRule.getActivity().findViewById(R.id.location_bar);
    }

    private ImageButton getDeleteButton() {
        return mActivityTestRule.getActivity().findViewById(R.id.delete_button);
    }

    private ImageButton getMicButton() {
        return mActivityTestRule.getActivity().findViewById(R.id.mic_button);
    }

    private ImageButton getSecurityButton() {
        return mActivityTestRule.getActivity().findViewById(R.id.security_button);
    }

    private void setUrlBarTextAndFocus(String text) throws ExecutionException {
        TestThreadUtils.runOnUiThreadBlocking(new Callable<Void>() {
            @Override
            public Void call() throws InterruptedException {
                getLocationBar().onUrlFocusChange(true);
                mActivityTestRule.typeInOmnibox(text, false);
                return null;
            }
        });
    }

    @Test
    @SmallTest
    @Restriction(UiRestriction.RESTRICTION_TYPE_PHONE)
    public void testNotShowingVoiceSearchButtonIfUrlBarContainsText() throws ExecutionException {
        setUrlBarTextAndFocus("testing");

        Assert.assertEquals(getDeleteButton().getVisibility(), VISIBLE);
        Assert.assertNotEquals(getMicButton().getVisibility(), VISIBLE);
    }

    @Test
    @SmallTest
    @Restriction(UiRestriction.RESTRICTION_TYPE_PHONE)
    public void testShowingVoiceSearchButtonIfUrlBarIsEmpty() throws ExecutionException {
        setUrlBarTextAndFocus("");

        Assert.assertNotEquals(getDeleteButton().getVisibility(), VISIBLE);
        Assert.assertEquals(getMicButton().getVisibility(), VISIBLE);
    }

    @Test
    @SmallTest
    @EnableFeatures(ChromeFeatureList.QUERY_IN_OMNIBOX)
    @Feature({"QueryInOmnibox"})
    public void testIsViewShowingModelSearchTerms() {
        final UrlBar urlBar = getUrlBar();
        final LocationBarLayout locationBar = getLocationBar();

        mTestLocationBarModel.setCurrentUrl(GOOGLE_SRP_URL);
        mTestLocationBarModel.setSecurityLevel(ConnectionSecurityLevel.SECURE);
        mTestLocationBarModel.setDisplaySearchTerms(null);
        setUrlToPageUrl(locationBar);
        Assert.assertNotEquals(SEARCH_TERMS, getUrlText(urlBar));

        mTestLocationBarModel.setDisplaySearchTerms(SEARCH_TERMS);
        setUrlToPageUrl(locationBar);
        Assert.assertEquals(SEARCH_TERMS, getUrlText(urlBar));
    }

    @Test
    @SmallTest
    public void testEditingTextShownOnFocus() {
        final UrlBar urlBar = getUrlBar();
        final LocationBarLayout locationBar = getLocationBar();

        mTestLocationBarModel.setCurrentUrl(VERBOSE_URL);
        mTestLocationBarModel.setSecurityLevel(ConnectionSecurityLevel.SECURE);
        mTestLocationBarModel.mDisplayText = TRIMMED_URL;
        mTestLocationBarModel.mEditingText = VERBOSE_URL;
        setUrlToPageUrl(locationBar);

        Assert.assertEquals(TRIMMED_URL, getUrlText(urlBar));

        TestThreadUtils.runOnUiThreadBlocking(() -> { urlBar.requestFocus(); });

        Assert.assertEquals(VERBOSE_URL, getUrlText(urlBar));

        TestThreadUtils.runOnUiThreadBlocking(() -> {
            Assert.assertEquals(0, urlBar.getSelectionStart());
            Assert.assertEquals(VERBOSE_URL.length(), urlBar.getSelectionEnd());
        });
    }

    @Test
    @SmallTest
    @Restriction(UiRestriction.RESTRICTION_TYPE_PHONE)
    @EnableFeatures(ChromeFeatureList.OMNIBOX_SEARCH_ENGINE_LOGO)
    @Feature({"OmniboxSearchEngineLogo"})
    public void testOmniboxSearchEngineLogo_unfocusedOnNTP_whenIncognito() {
        final LocationBarLayout locationBar = getLocationBar();
        final View iconView = locationBar.getSecurityIconView();
        updateSearchEngineLogoWithGoogle(locationBar);
        loadUrlInNewTabAndUpdateModels(UrlConstants.NTP_URL, true);

        onView(withId(R.id.location_bar_status))
                .check((view, e) -> Assert.assertEquals(iconView.getVisibility(), GONE));
    }

    @Test
    @SmallTest
    @Restriction(UiRestriction.RESTRICTION_TYPE_PHONE)
    @EnableFeatures(ChromeFeatureList.OMNIBOX_SEARCH_ENGINE_LOGO)
    @Feature({"OmniboxSearchEngineLogo"})
    public void testOmniboxSearchEngineLogo_unfocusedOnNTP() {
        final LocationBarLayout locationBar = getLocationBar();
        final View iconView = locationBar.getSecurityIconView();
        updateSearchEngineLogoWithGoogle(locationBar);
        loadUrlInNewTabAndUpdateModels(UrlConstants.NTP_URL, false);
        TestThreadUtils.runOnUiThreadBlocking(() -> { locationBar.updateVisualsForState(); });

        onView(withId(R.id.location_bar_status))
                .check((view, e) -> Assert.assertEquals(GONE, iconView.getVisibility()));
    }

    @Test
    @SmallTest
    @EnableFeatures(ChromeFeatureList.OMNIBOX_SEARCH_ENGINE_LOGO)
    @Feature({"OmniboxSearchEngineLogo"})
    public void testOmniboxSearchEngineLogo_focusedOnNTP() throws ExecutionException {
        final LocationBarLayout locationBar = getLocationBar();
        final ImageView iconView = (ImageView) locationBar.getSecurityIconView();
        updateSearchEngineLogoWithGoogle(locationBar);
        loadUrlInNewTabAndUpdateModels(UrlConstants.NTP_URL, false);
        setUrlBarTextAndFocus("");

        onView(withId(R.id.location_bar_status)).check((view, e) -> {
            Assert.assertEquals(VISIBLE, iconView.getVisibility());
            Assert.assertEquals(R.drawable.ic_logo_googleg_20dp,
                    locationBar.getStatusViewCoordinatorForTesting()
                            .getSecurityIconResourceIdForTesting());
        });
    }

    @Test
    @SmallTest
    @EnableFeatures(ChromeFeatureList.OMNIBOX_SEARCH_ENGINE_LOGO)
    @Feature({"OmniboxSearchEngineLogo"})
    public void testOmniboxSearchEngineLogo_focusedOnNTP_nonGoogleEngine()
            throws ExecutionException {
        final LocationBarLayout locationBar = getLocationBar();
        final ImageView iconView = (ImageView) locationBar.getSecurityIconView();
        updateSearchEngineLogoWithYahoo(locationBar);
        loadUrlInNewTabAndUpdateModels(UrlConstants.NTP_URL, false);
        setUrlBarTextAndFocus("");

        onView(withId(R.id.location_bar_status)).check((view, e) -> {
            Assert.assertEquals(VISIBLE, iconView.getVisibility());
            Assert.assertEquals(R.drawable.ic_search,
                    locationBar.getStatusViewCoordinatorForTesting()
                            .getSecurityIconResourceIdForTesting());
        });
    }

    @Test
    @SmallTest
    @EnableFeatures({ChromeFeatureList.OMNIBOX_SEARCH_ENGINE_LOGO})
    @Feature({"OmniboxSearchEngineLogo"})
    public void testOmniboxSearchEngineLogo_unfocusedOnSRP_withQIO() {
        final LocationBarLayout locationBar = getLocationBar();
        final ImageView iconView = (ImageView) locationBar.getSecurityIconView();
        updateSearchEngineLogoWithGoogle(locationBar);

        mTestLocationBarModel.setCurrentUrl(GOOGLE_SRP_URL);
        mTestLocationBarModel.setSecurityLevel(ConnectionSecurityLevel.SECURE);
        mTestLocationBarModel.setDisplaySearchTerms(SEARCH_TERMS);
        setUrlToPageUrl(locationBar);

        onView(withId(R.id.location_bar_status)).check((view, e) -> {
            Assert.assertEquals(iconView.getVisibility(), VISIBLE);
            Assert.assertEquals(R.drawable.ic_logo_googleg_20dp,
                    locationBar.getStatusViewCoordinatorForTesting()
                            .getSecurityIconResourceIdForTesting());
        });
    }

    @Test
    @SmallTest
    @EnableFeatures({ChromeFeatureList.OMNIBOX_SEARCH_ENGINE_LOGO})
    @Feature({"OmniboxSearchEngineLogo"})
    public void testOmniboxSearchEngineLogo_unfocusedOnSRP() {
        final LocationBarLayout locationBar = getLocationBar();
        final ImageView iconView = (ImageView) locationBar.getSecurityIconView();
        updateSearchEngineLogoWithGoogle(locationBar);
        mTestLocationBarModel.setCurrentUrl(GOOGLE_SRP_URL);
        mTestLocationBarModel.setSecurityLevel(ConnectionSecurityLevel.SECURE);
        setUrlToPageUrl(locationBar);

        onView(withId(R.id.location_bar_status)).check((view, e) -> {
            Assert.assertEquals(iconView.getVisibility(), VISIBLE);
            Assert.assertEquals(R.drawable.omnibox_https_valid,
                    locationBar.getStatusViewCoordinatorForTesting()
                            .getSecurityIconResourceIdForTesting());
        });
    }

    @Test
    @SmallTest
    @EnableFeatures(
            {ChromeFeatureList.OMNIBOX_SEARCH_ENGINE_LOGO, ChromeFeatureList.QUERY_IN_OMNIBOX})
    @Feature({"OmniboxSearchEngineLogo"})
    public void
    testOmniboxSearchEngineLogo_focusedOnSRP() throws ExecutionException {
        final LocationBarLayout locationBar = getLocationBar();
        final ImageView iconView = (ImageView) locationBar.getSecurityIconView();
        updateSearchEngineLogoWithGoogle(locationBar);
        mTestLocationBarModel.setCurrentUrl(GOOGLE_SRP_URL);
        mTestLocationBarModel.setSecurityLevel(ConnectionSecurityLevel.SECURE);
        mTestLocationBarModel.setDisplaySearchTerms(null);
        setUrlToPageUrl(locationBar);
        setUrlBarTextAndFocus("");

        onView(withId(R.id.location_bar_status)).check((view, e) -> {
            Assert.assertEquals(iconView.getVisibility(), VISIBLE);
            Assert.assertEquals(R.drawable.ic_logo_googleg_20dp,
                    locationBar.getStatusViewCoordinatorForTesting()
                            .getSecurityIconResourceIdForTesting());
        });
    }

    @Test
    @SmallTest
    @EnableFeatures({ChromeFeatureList.OMNIBOX_SEARCH_ENGINE_LOGO})
    @Feature({"OmniboxSearchEngineLogo"})
    public void testOmniboxSearchEngineLogo_unfocusedOnChromeVersion() throws ExecutionException {
        final LocationBarLayout locationBar = getLocationBar();
        final ImageView iconView = (ImageView) locationBar.getSecurityIconView();
        updateSearchEngineLogoWithGoogle(locationBar);
        mTestLocationBarModel.setCurrentUrl("chrome://version");
        mTestLocationBarModel.setSecurityLevel(ConnectionSecurityLevel.NONE);
        mTestLocationBarModel.setDisplaySearchTerms(null);
        setUrlToPageUrl(locationBar);

        onView(withId(R.id.location_bar_status)).check((view, e) -> {
            Assert.assertEquals(iconView.getVisibility(), VISIBLE);
            Assert.assertEquals(R.drawable.omnibox_info,
                    locationBar.getStatusViewCoordinatorForTesting()
                            .getSecurityIconResourceIdForTesting());
        });
    }

    @Test
    @SmallTest
    @EnableFeatures({ChromeFeatureList.OMNIBOX_SEARCH_ENGINE_LOGO})
    @Feature({"OmniboxSearchEngineLogo"})
    public void testOmniboxSearchEngineLogo_focusedOnChromeVersion() throws ExecutionException {
        final LocationBarLayout locationBar = getLocationBar();
        final ImageView iconView = (ImageView) locationBar.getSecurityIconView();
        updateSearchEngineLogoWithGoogle(locationBar);
        mTestLocationBarModel.setCurrentUrl("chrome://version");
        mTestLocationBarModel.setSecurityLevel(ConnectionSecurityLevel.NONE);
        mTestLocationBarModel.setDisplaySearchTerms(null);
        setUrlToPageUrl(locationBar);
        setUrlBarTextAndFocus("");

        onView(withId(R.id.location_bar_status)).check((view, e) -> {
            Assert.assertEquals(iconView.getVisibility(), VISIBLE);
            Assert.assertEquals(R.drawable.ic_logo_googleg_20dp,
                    locationBar.getStatusViewCoordinatorForTesting()
                            .getSecurityIconResourceIdForTesting());
        });
    }

    @Test
    @SmallTest
    public void testSetUrlBarFocus() {
        final LocationBarLayout locationBar = getLocationBar();

        Assert.assertEquals(
                0, RecordHistogram.getHistogramTotalCountForTesting("Android.OmniboxFocusReason"));
        TestThreadUtils.runOnUiThreadBlocking(() -> {
            locationBar.setUrlBarFocus(
                    true, SEARCH_TERMS_URL, LocationBar.OmniboxFocusReason.FAKE_BOX_LONG_PRESS);
        });
        Assert.assertTrue(locationBar.isUrlBarFocused());
        Assert.assertTrue(locationBar.didFocusUrlFromFakebox());
        Assert.assertEquals(SEARCH_TERMS_URL, getUrlText(getUrlBar()));
        Assert.assertEquals(
                1, RecordHistogram.getHistogramTotalCountForTesting("Android.OmniboxFocusReason"));

        TestThreadUtils.runOnUiThreadBlocking(() -> {
            locationBar.setUrlBarFocus(
                    true, SEARCH_TERMS, LocationBar.OmniboxFocusReason.SEARCH_QUERY);
        });
        Assert.assertTrue(locationBar.isUrlBarFocused());
        Assert.assertTrue(locationBar.didFocusUrlFromFakebox());
        Assert.assertEquals(SEARCH_TERMS, getUrlText(getUrlBar()));
        Assert.assertEquals(
                1, RecordHistogram.getHistogramTotalCountForTesting("Android.OmniboxFocusReason"));

        TestThreadUtils.runOnUiThreadBlocking(() -> {
            locationBar.setUrlBarFocus(false, null, LocationBar.OmniboxFocusReason.UNFOCUS);
        });
        Assert.assertFalse(locationBar.isUrlBarFocused());
        Assert.assertFalse(locationBar.didFocusUrlFromFakebox());
        Assert.assertEquals(
                1, RecordHistogram.getHistogramTotalCountForTesting("Android.OmniboxFocusReason"));

        TestThreadUtils.runOnUiThreadBlocking(() -> {
            locationBar.setUrlBarFocus(true, null, LocationBar.OmniboxFocusReason.OMNIBOX_TAP);
        });
        Assert.assertTrue(locationBar.isUrlBarFocused());
        Assert.assertFalse(locationBar.didFocusUrlFromFakebox());
        Assert.assertEquals(
                2, RecordHistogram.getHistogramTotalCountForTesting("Android.OmniboxFocusReason"));
    }

    private void loadUrlInNewTabAndUpdateModels(String url, boolean incognito) {
        mActivityTestRule.loadUrlInNewTab(url, incognito);
        setupModelsForCurrentTab();
    }

    /** Performs an update on {@link LocationBar} to show the Google logo. */
    private void updateSearchEngineLogoWithGoogle(LocationBar locationBar) {
        TestThreadUtils.runOnUiThreadBlocking(
                () -> { locationBar.updateSearchEngineStatusIcon(true, true, GOOGLE_URL); });
    }

    /** Performs an update on {@link LocationBar} to show the Yahoo logo. */
    private void updateSearchEngineLogoWithYahoo(LocationBar locationBar) {
        TestThreadUtils.runOnUiThreadBlocking(
                () -> { locationBar.updateSearchEngineStatusIcon(true, false, YAHOO_URL); });
    }
}
