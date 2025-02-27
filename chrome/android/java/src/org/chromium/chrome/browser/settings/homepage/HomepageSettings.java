// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.settings.homepage;

import android.os.Bundle;
import android.support.v7.preference.Preference;
import android.support.v7.preference.PreferenceFragmentCompat;

import androidx.annotation.VisibleForTesting;

import org.chromium.chrome.R;
import org.chromium.chrome.browser.flags.CachedFeatureFlags;
import org.chromium.chrome.browser.homepage.HomepagePolicyManager;
import org.chromium.chrome.browser.partnercustomizations.HomepageManager;
import org.chromium.chrome.browser.settings.ChromeSwitchPreference;
import org.chromium.chrome.browser.settings.ManagedPreferenceDelegate;
import org.chromium.chrome.browser.settings.SettingsUtils;

/**
 * Fragment that allows the user to configure homepage related preferences.
 */
public class HomepageSettings extends PreferenceFragmentCompat {
    @VisibleForTesting
    public static final String PREF_HOMEPAGE_SWITCH = "homepage_switch";
    @VisibleForTesting
    public static final String PREF_HOMEPAGE_EDIT = "homepage_edit";

    /**
     * Delegate used to mark that the homepage is being managed.
     * Created for {@link org.chromium.chrome.browser.settings.HomepagePreferences}
     */
    private static class HomepageManagedPreferenceDelegate implements ManagedPreferenceDelegate {
        @Override
        public boolean isPreferenceControlledByPolicy(Preference preference) {
            return HomepagePolicyManager.isHomepageManagedByPolicy();
        }
    }

    private HomepageManager mHomepageManager;
    private Preference mHomepageEdit;

    @Override
    public void onCreatePreferences(Bundle savedInstanceState, String rootKey) {
        mHomepageManager = HomepageManager.getInstance();

        getActivity().setTitle(R.string.options_homepage_title);
        SettingsUtils.addPreferencesFromResource(this, R.xml.homepage_preferences);

        ChromeSwitchPreference homepageSwitch =
                (ChromeSwitchPreference) findPreference(PREF_HOMEPAGE_SWITCH);
        homepageSwitch.setManagedPreferenceDelegate(new HomepageManagedPreferenceDelegate());

        if (CachedFeatureFlags.isBottomToolbarEnabled()) {
            homepageSwitch.setVisible(false);
        } else {
            boolean isHomepageEnabled = HomepageManager.isHomepageEnabled();
            homepageSwitch.setChecked(isHomepageEnabled);
            homepageSwitch.setOnPreferenceChangeListener((preference, newValue) -> {
                mHomepageManager.setPrefHomepageEnabled((boolean) newValue);
                return true;
            });
        }

        mHomepageEdit = findPreference(PREF_HOMEPAGE_EDIT);
        updateCurrentHomepageUrl();
    }

    private void updateCurrentHomepageUrl() {
        if (HomepagePolicyManager.isHomepageManagedByPolicy()) mHomepageEdit.setEnabled(false);

        mHomepageEdit.setSummary(HomepageManager.getHomepageUri());
    }

    @Override
    public void onResume() {
        super.onResume();
        updateCurrentHomepageUrl();
    }
}
