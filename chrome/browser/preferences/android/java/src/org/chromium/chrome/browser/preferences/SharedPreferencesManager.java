// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.preferences;

import android.content.SharedPreferences;
import android.content.SharedPreferences.Editor;

import androidx.annotation.Nullable;
import androidx.annotation.VisibleForTesting;

import org.chromium.base.ContextUtils;
import org.chromium.base.StrictModeContext;
import org.chromium.base.annotations.RemovableInRelease;

import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

/**
 * Layer over android {@link SharedPreferences}.
 */
@SuppressWarnings("UseSharedPreferencesManagerFromChromeCheck")
public class SharedPreferencesManager {
    private static class LazyHolder {
        static final SharedPreferencesManager INSTANCE = new SharedPreferencesManager();
    }

    /**
     * @return The SharedPreferencesManager singleton.
     */
    public static SharedPreferencesManager getInstance() {
        return LazyHolder.INSTANCE;
    }

    private BaseChromePreferenceKeyChecker mKeyChecker;

    private SharedPreferencesManager() {
        maybeInitializeChecker();
        // In production builds, use a dummy key checker.
        if (mKeyChecker == null) {
            mKeyChecker = new BaseChromePreferenceKeyChecker();
        }
    }

    @VisibleForTesting
    SharedPreferencesManager(BaseChromePreferenceKeyChecker keyChecker) {
        mKeyChecker = keyChecker;
    }

    @RemovableInRelease
    private void maybeInitializeChecker() {
        // Create a working key checker, which does not happen in production builds.
        mKeyChecker = ChromePreferenceKeyChecker.getInstance();
    }

    @VisibleForTesting
    BaseChromePreferenceKeyChecker swapKeyCheckerForTesting(
            BaseChromePreferenceKeyChecker newChecker) {
        BaseChromePreferenceKeyChecker swappedOut = mKeyChecker;
        mKeyChecker = newChecker;
        return swappedOut;
    }

    @VisibleForTesting
    public void disableKeyCheckerForTesting() {
        mKeyChecker = new BaseChromePreferenceKeyChecker();
    }

    /**
     * Observes preference changes.
     */
    public interface Observer {
        /**
         * Notifies when a preference maintained by {@link SharedPreferencesManager} is changed.
         * @param key The key of the preference changed.
         */
        void onPreferenceChanged(String key);
    }

    private final Map<Observer, SharedPreferences.OnSharedPreferenceChangeListener> mObservers =
            new HashMap<>();

    /**
     * @param observer The {@link Observer} to be added for observing preference changes.
     */
    public void addObserver(Observer observer) {
        SharedPreferences.OnSharedPreferenceChangeListener listener =
                (SharedPreferences sharedPreferences, String s) -> observer.onPreferenceChanged(s);
        mObservers.put(observer, listener);
        ContextUtils.getAppSharedPreferences().registerOnSharedPreferenceChangeListener(listener);
    }

    /**
     * @param observer The {@link Observer} to be removed from observing preference changes.
     */
    public void removeObserver(Observer observer) {
        SharedPreferences.OnSharedPreferenceChangeListener listener = mObservers.get(observer);
        if (listener == null) return;
        ContextUtils.getAppSharedPreferences().unregisterOnSharedPreferenceChangeListener(listener);
    }

    /**
     * Reads set of String values from preferences.
     *
     * Note that you must not modify the set instance returned by this call.
     */
    public Set<String> readStringSet(String key) {
        return readStringSet(key, Collections.emptySet());
    }

    /**
     * Reads set of String values from preferences.
     *
     * Note that you must not modify the set instance returned by this call.
     */
    public Set<String> readStringSet(String key, Set<String> defaultValue) {
        mKeyChecker.checkIsKeyInUse(key);
        return ContextUtils.getAppSharedPreferences().getStringSet(key, defaultValue);
    }

    /**
     * Adds a value to string set in shared preferences.
     */
    public void addToStringSet(String key, String value) {
        mKeyChecker.checkIsKeyInUse(key);
        // Construct a new set so it can be modified safely. See crbug.com/568369.
        Set<String> values = new HashSet<>(
                ContextUtils.getAppSharedPreferences().getStringSet(key, Collections.emptySet()));
        values.add(value);
        writeStringSetUnchecked(key, values);
    }

    /**
     * Removes value from string set in shared preferences.
     */
    public void removeFromStringSet(String key, String value) {
        mKeyChecker.checkIsKeyInUse(key);
        // Construct a new set so it can be modified safely. See crbug.com/568369.
        Set<String> values = new HashSet<>(
                ContextUtils.getAppSharedPreferences().getStringSet(key, Collections.emptySet()));
        if (values.remove(value)) {
            writeStringSetUnchecked(key, values);
        }
    }

    /**
     * Writes string set to shared preferences.
     */
    public void writeStringSet(String key, Set<String> values) {
        mKeyChecker.checkIsKeyInUse(key);
        writeStringSetUnchecked(key, values);
    }

    /**
     * Writes string set to shared preferences.
     */
    private void writeStringSetUnchecked(String key, Set<String> values) {
        Editor editor = ContextUtils.getAppSharedPreferences().edit().putStringSet(key, values);
        editor.apply();
    }

    /**
     * Writes the given string set to the named shared preference and immediately commit to disk.
     * @param key The name of the preference to modify.
     * @param value The new value for the preference.
     * @return Whether the operation succeeded.
     */
    public boolean writeStringSetSync(String key, Set<String> value) {
        mKeyChecker.checkIsKeyInUse(key);
        Editor editor = ContextUtils.getAppSharedPreferences().edit().putStringSet(key, value);
        return editor.commit();
    }

    /**
     * Writes the given int value to the named shared preference.
     * @param key The name of the preference to modify.
     * @param value The new value for the preference.
     */
    public void writeInt(String key, int value) {
        mKeyChecker.checkIsKeyInUse(key);
        writeIntUnchecked(key, value);
    }

    private void writeIntUnchecked(String key, int value) {
        SharedPreferences.Editor ed = ContextUtils.getAppSharedPreferences().edit();
        ed.putInt(key, value);
        ed.apply();
    }

    /**
     * Writes the given int value to the named shared preference and immediately commit to disk.
     * @param key The name of the preference to modify.
     * @param value The new value for the preference.
     * @return Whether the operation succeeded.
     */
    public boolean writeIntSync(String key, int value) {
        mKeyChecker.checkIsKeyInUse(key);
        SharedPreferences.Editor ed = ContextUtils.getAppSharedPreferences().edit();
        ed.putInt(key, value);

        try (StrictModeContext ignored = StrictModeContext.allowDiskWrites()) {
            return ed.commit();
        }
    }

    /**
     * Reads the given int value from the named shared preference, defaulting to 0 if not found.
     * @param key The name of the preference to return.
     * @return The value of the preference.
     */
    public int readInt(String key) {
        return readInt(key, 0);
    }

    /**
     * Reads the given int value from the named shared preference.
     * @param key The name of the preference to return.
     * @param defaultValue The default value to return if the preference is not set.
     * @return The value of the preference.
     */
    public int readInt(String key, int defaultValue) {
        mKeyChecker.checkIsKeyInUse(key);
        try (StrictModeContext ignored = StrictModeContext.allowDiskReads()) {
            return ContextUtils.getAppSharedPreferences().getInt(key, defaultValue);
        }
    }

    /**
     * Increments the integer value specified by the given key.  If no initial value is present then
     * an initial value of 0 is assumed and incremented, so a new value of 1 is set.
     * @param key The key specifying which integer value to increment.
     * @return The newly incremented value.
     */
    public int incrementInt(String key) {
        mKeyChecker.checkIsKeyInUse(key);
        int value = ContextUtils.getAppSharedPreferences().getInt(key, 0);
        writeIntUnchecked(key, ++value);
        return value;
    }

    /**
     * Writes the given long to the named shared preference.
     *
     * @param key The name of the preference to modify.
     * @param value The new value for the preference.
     */
    public void writeLong(String key, long value) {
        mKeyChecker.checkIsKeyInUse(key);
        SharedPreferences.Editor ed = ContextUtils.getAppSharedPreferences().edit();
        ed.putLong(key, value);
        ed.apply();
    }

    /**
     * Writes the given long value to the named shared preference and immediately commit to disk.
     * @param key The name of the preference to modify.
     * @param value The new value for the preference.
     * @return Whether the operation succeeded.
     */
    public boolean writeLongSync(String key, long value) {
        mKeyChecker.checkIsKeyInUse(key);
        SharedPreferences.Editor ed = ContextUtils.getAppSharedPreferences().edit();
        ed.putLong(key, value);

        try (StrictModeContext ignored = StrictModeContext.allowDiskWrites()) {
            return ed.commit();
        }
    }

    /**
     * Reads the given long value from the named shared preference.
     *
     * @param key The name of the preference to return.
     * @return The value of the preference if stored; defaultValue otherwise.
     */
    public long readLong(String key) {
        return readLong(key, 0);
    }

    /**
     * Reads the given long value from the named shared preference.
     *
     * @param key The name of the preference to return.
     * @param defaultValue The default value to return if there's no value stored.
     * @return The value of the preference if stored; defaultValue otherwise.
     */
    public long readLong(String key, long defaultValue) {
        mKeyChecker.checkIsKeyInUse(key);
        try (StrictModeContext ignored = StrictModeContext.allowDiskReads()) {
            return ContextUtils.getAppSharedPreferences().getLong(key, defaultValue);
        }
    }

    /**
     * Writes the given float to the named shared preference.
     *
     * @param key The name of the preference to modify.
     * @param value The new value for the preference.
     */
    public void writeFloat(String key, float value) {
        mKeyChecker.checkIsKeyInUse(key);
        SharedPreferences.Editor ed = ContextUtils.getAppSharedPreferences().edit();
        ed.putFloat(key, value);
        ed.apply();
    }

    /**
     * Writes the given float value to the named shared preference and immediately commit to disk.
     * @param key The name of the preference to modify.
     * @param value The new value for the preference.
     * @return Whether the operation succeeded.
     */
    public boolean writeFloatSync(String key, float value) {
        mKeyChecker.checkIsKeyInUse(key);
        SharedPreferences.Editor ed = ContextUtils.getAppSharedPreferences().edit();
        ed.putFloat(key, value);

        try (StrictModeContext ignored = StrictModeContext.allowDiskWrites()) {
            return ed.commit();
        }
    }

    /**
     * Reads the given float value from the named shared preference.
     *
     * @param key The name of the preference to return.
     * @param defaultValue The default value to return if there's no value stored.
     * @return The value of the preference if stored; defaultValue otherwise.
     */
    public float readFloat(String key, float defaultValue) {
        mKeyChecker.checkIsKeyInUse(key);
        try (StrictModeContext ignored = StrictModeContext.allowDiskReads()) {
            return ContextUtils.getAppSharedPreferences().getFloat(key, defaultValue);
        }
    }

    /**
     * Writes the given boolean to the named shared preference.
     *
     * @param key The name of the preference to modify.
     * @param value The new value for the preference.
     */
    public void writeBoolean(String key, boolean value) {
        mKeyChecker.checkIsKeyInUse(key);
        SharedPreferences.Editor ed = ContextUtils.getAppSharedPreferences().edit();
        ed.putBoolean(key, value);
        ed.apply();
    }

    /**
     * Writes the given boolean value to the named shared preference and immediately commit to disk.
     * @param key The name of the preference to modify.
     * @param value The new value for the preference.
     * @return Whether the operation succeeded.
     */
    public boolean writeBooleanSync(String key, boolean value) {
        mKeyChecker.checkIsKeyInUse(key);
        SharedPreferences.Editor ed = ContextUtils.getAppSharedPreferences().edit();
        ed.putBoolean(key, value);

        try (StrictModeContext ignored = StrictModeContext.allowDiskWrites()) {
            return ed.commit();
        }
    }

    /**
     * Reads the given boolean value from the named shared preference.
     *
     * @param key The name of the preference to return.
     * @param defaultValue The default value to return if there's no value stored.
     * @return The value of the preference if stored; defaultValue otherwise.
     */
    public boolean readBoolean(String key, boolean defaultValue) {
        mKeyChecker.checkIsKeyInUse(key);
        try (StrictModeContext ignored = StrictModeContext.allowDiskReads()) {
            return ContextUtils.getAppSharedPreferences().getBoolean(key, defaultValue);
        }
    }

    /**
     * Writes the given string to the named shared preference.
     *
     * @param key The name of the preference to modify.
     * @param value The new value for the preference.
     */
    public void writeString(String key, String value) {
        mKeyChecker.checkIsKeyInUse(key);
        SharedPreferences.Editor ed = ContextUtils.getAppSharedPreferences().edit();
        ed.putString(key, value);
        ed.apply();
    }

    /**
     * Writes the given string value to the named shared preference and immediately commit to disk.
     * @param key The name of the preference to modify.
     * @param value The new value for the preference.
     * @return Whether the operation succeeded.
     */
    public boolean writeStringSync(String key, String value) {
        mKeyChecker.checkIsKeyInUse(key);
        SharedPreferences.Editor ed = ContextUtils.getAppSharedPreferences().edit();
        ed.putString(key, value);

        try (StrictModeContext ignored = StrictModeContext.allowDiskWrites()) {
            return ed.commit();
        }
    }

    /**
     * Reads the given String value from the named shared preference.
     *
     * @param key The name of the preference to return.
     * @param defaultValue The default value to return if there's no value stored.
     * @return The value of the preference if stored; defaultValue otherwise.
     */
    public String readString(String key, @Nullable String defaultValue) {
        mKeyChecker.checkIsKeyInUse(key);
        try (StrictModeContext ignored = StrictModeContext.allowDiskReads()) {
            return ContextUtils.getAppSharedPreferences().getString(key, defaultValue);
        }
    }

    /**
     * Removes the shared preference entry.
     *
     * @param key The key of the preference to remove.
     */
    public void removeKey(String key) {
        mKeyChecker.checkIsKeyInUse(key);
        SharedPreferences.Editor ed = ContextUtils.getAppSharedPreferences().edit();
        ed.remove(key);
        ed.apply();
    }

    public boolean removeKeySync(String key) {
        mKeyChecker.checkIsKeyInUse(key);
        SharedPreferences.Editor ed = ContextUtils.getAppSharedPreferences().edit();
        ed.remove(key);
        return ed.commit();
    }

    /**
     * Checks if any value was written associated to a key in shared preferences.
     *
     * @param key The key of the preference to check.
     * @return Whether any value was written for that key.
     */
    public boolean contains(String key) {
        mKeyChecker.checkIsKeyInUse(key);
        return ContextUtils.getAppSharedPreferences().contains(key);
    }
}
