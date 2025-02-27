// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.preferences;

import android.text.TextUtils;

import androidx.annotation.VisibleForTesting;

import org.chromium.base.annotations.CheckDiscard;

import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.regex.Pattern;

/**
 * Class that checks if given Strings are valid SharedPreferences keys to use.
 */
@CheckDiscard("Validation is performed in tests and in debug builds.")
class ChromePreferenceKeyChecker extends BaseChromePreferenceKeyChecker {
    private static final ChromePreferenceKeyChecker INSTANCE = new ChromePreferenceKeyChecker();

    private Set<String> mKeysInUse;
    private Set<String> mGrandfatheredFormatKeys;
    private List<KeyPrefix> mGrandfatheredPrefixes;
    private Pattern mDynamicPartPattern;

    /**
     * Constructor called by the singleton, pulls the lists of keys from {@link
     * ChromePreferenceKeys}.
     */
    private ChromePreferenceKeyChecker() {
        this(ChromePreferenceKeys.createKeysInUse(),
                ChromePreferenceKeys.createGrandfatheredKeysInUse(),
                ChromePreferenceKeys.createGrandfatheredPrefixesInUse());
    }

    /**
     * Generic constructor, dependencies are passed in.
     */
    @VisibleForTesting
    ChromePreferenceKeyChecker(List<String> keysInUse, List<String> grandfatheredKeys,
            List<KeyPrefix> grandfatheredPrefixes) {
        mKeysInUse = new HashSet<>(keysInUse);
        mGrandfatheredFormatKeys = new HashSet<>(grandfatheredKeys);
        mGrandfatheredPrefixes = grandfatheredPrefixes;

        // The dynamic part cannot be empty, but otherwise it is anything that does not contain
        // stars.
        mDynamicPartPattern = Pattern.compile("[^\\*]+");
    }

    /**
     * @return The ChromePreferenceKeyChecker singleton.
     */
    public static ChromePreferenceKeyChecker getInstance() {
        return INSTANCE;
    }

    /**
     * Check that the |key| passed is in use.
     * @throws RuntimeException if the key is not in use.
     */
    @Override
    void checkIsKeyInUse(String key) {
        if (!isKeyInUse(key)) {
            throw new RuntimeException("SharedPreferences key \"" + key
                    + "\" is not registered in ChromePreferenceKeys.createKeysInUse()");
        }
    }

    /**
     * @return Whether |key| is in use.
     */
    private boolean isKeyInUse(String key) {
        // For non-dynamic grandfathered keys, a simple map check is enough.
        if (mGrandfatheredFormatKeys.contains(key)) {
            return true;
        }

        // For dynamic grandfathered keys, each grandfathered prefix has to be checked.
        for (KeyPrefix prefix : mGrandfatheredPrefixes) {
            if (prefix.hasGenerated(key)) {
                return true;
            }
        }

        // If not a format-grandfathered key, assume it follows the format and find out if it is
        // a prefixed key.
        String[] parts = key.split("\\.", 4);
        if (parts.length < 3) return false;
        boolean isPrefixed = parts.length >= 4;

        if (isPrefixed) {
            // Key with prefix in format "Chrome.[Feature].[KeyPrefix].[Suffix]".

            // Check if its prefix is whitelisted in |mKeysInUse|.
            String prefixFormat =
                    TextUtils.join(".", Arrays.asList(parts[0], parts[1], parts[2], "*"));
            if (!mKeysInUse.contains(prefixFormat)) return false;

            // Check if the dynamic part is correctly formed.
            String dynamicPart = parts[3];
            return mDynamicPartPattern.matcher(dynamicPart).matches();
        } else {
            // Regular key in format "Chrome.[Feature].[Key]" which was not present in |mKeysInUse|.
            // Just check if it is in [keys in use].
            return mKeysInUse.contains(key);
        }
    }
}
