// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.preferences;

import org.chromium.base.annotations.CheckDiscard;

import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/**
 * Contains String and {@link KeyPrefix} constants with the SharedPreferences keys used by Chrome.
 *
 * All Chrome layer SharedPreferences keys should be declared in this class.
 *
 * To add a new key:
 * 1. Declare it as a String constant in this class. Its value should follow the format
 *    "Chrome.[Feature].[Key]" and the constants names should be in alphabetical order.
 * 2. Add it to {@link #createKeysInUse()}.
 *
 * To deprecate a key that is not used anymore:
 * 1. Add its constant value to createDeprecatedKeysForTesting(), in alphabetical order by value.
 * 2. Remove the key from {@link #createKeysInUse()} or {@link #createGrandfatheredKeysInUse()}.
 * 3. Delete the constant.
 *
 * To add a new KeyPrefix:
 * 1. Declare it as a KeyPrefix constant in this class. Its value should follow the format
 *    "Chrome.[Feature].[KeyPrefix].*" and the constants names should be in alphabetical order.
 * 2. Add PREFIX_CONSTANT.pattern() to {@link #createKeysInUse()}}.
 *
 * To deprecate a KeyPrefix that is not used anymore:
 * 1. Add its String value to {@link #createDeprecatedKeysForTesting()}, including the ".*", in
 *    alphabetical order by value.
 * 2. Remove it from {@link #createKeysInUse()}.
 * 3. Delete the KeyPrefix constant.
 *
 * Tests in ChromePreferenceKeysTest and checks in {@link ChromePreferenceKeyChecker} ensure the
 * sanity of this file.
 */
public final class ChromePreferenceKeys {
    /*
     * Whether the simplified tab switcher is enabled when accessibility mode is enabled. Keep in
     * sync with accessibility_preferences.xml.
     * Default value is true.
     */
    public static final String ACCESSIBILITY_TAB_SWITCHER = "accessibility_tab_switcher";

    public static final String APP_LOCALE = "locale";

    /** Whether Autofill Assistant is enabled */
    public static final String AUTOFILL_ASSISTANT_ENABLED = "autofill_assistant_switch";
    /** Whether the Autofill Assistant onboarding has been accepted. */
    public static final String AUTOFILL_ASSISTANT_ONBOARDING_ACCEPTED =
            "AUTOFILL_ASSISTANT_ONBOARDING_ACCEPTED";
    /**
     * LEGACY preference indicating whether "do not show again" was checked in the autofill
     * assistant onboarding
     */
    public static final String AUTOFILL_ASSISTANT_SKIP_INIT_SCREEN =
            "AUTOFILL_ASSISTANT_SKIP_INIT_SCREEN";

    public static final String BOOKMARKS_LAST_MODIFIED_FOLDER_ID = "last_bookmark_folder_id";
    public static final String BOOKMARKS_LAST_USED_URL = "enhanced_bookmark_last_used_url";
    public static final String BOOKMARKS_LAST_USED_PARENT =
            "enhanced_bookmark_last_used_parent_folder";

    /**
     * Whether Chrome is set as the default browser.
     * Default value is false.
     */
    public static final String CHROME_DEFAULT_BROWSER = "applink.chrome_default_browser";

    /**
     * Marks that the content suggestions surface has been shown.
     * Default value is false.
     */
    public static final String CONTENT_SUGGESTIONS_SHOWN = "content_suggestions_shown";

    /** An all-time counter of Contextual Search panel opens triggered by any gesture.*/
    public static final String CONTEXTUAL_SEARCH_ALL_TIME_OPEN_COUNT =
            "contextual_search_all_time_open_count";
    /** An all-time counter of taps that triggered the Contextual Search peeking panel. */
    public static final String CONTEXTUAL_SEARCH_ALL_TIME_TAP_COUNT =
            "contextual_search_all_time_tap_count";
    /**
     * The number of times a tap gesture caused a Contextual Search Quick Answer to be shown.
     * Cumulative, starting at M-69.
     */
    public static final String CONTEXTUAL_SEARCH_ALL_TIME_TAP_QUICK_ANSWER_COUNT =
            "contextual_search_all_time_tap_quick_answer_count";
    public static final KeyPrefix CONTEXTUAL_SEARCH_CLICKS_WEEK_PREFIX =
            new KeyPrefix("contextual_search_clicks_week_*");
    public static final String CONTEXTUAL_SEARCH_CURRENT_WEEK_NUMBER =
            "contextual_search_current_week_number";
    /**
     * The entity-data impressions count for Contextual Search, i.e. thumbnails shown in the Bar.
     * Cumulative, starting at M-69.
     */
    public static final String CONTEXTUAL_SEARCH_ENTITY_IMPRESSIONS_COUNT =
            "contextual_search_entity_impressions_count";
    /**
     * The entity-data opens count for Contextual Search, e.g. Panel opens following thumbnails
     * shown in the Bar. Cumulative, starting at M-69.
     */
    public static final String CONTEXTUAL_SEARCH_ENTITY_OPENS_COUNT =
            "contextual_search_entity_opens_count";
    public static final KeyPrefix CONTEXTUAL_SEARCH_IMPRESSIONS_WEEK_PREFIX =
            new KeyPrefix("contextual_search_impressions_week_*");
    public static final String CONTEXTUAL_SEARCH_LAST_ANIMATION_TIME =
            "contextual_search_last_animation_time";
    public static final String CONTEXTUAL_SEARCH_NEWEST_WEEK = "contextual_search_newest_week";
    public static final String CONTEXTUAL_SEARCH_OLDEST_WEEK = "contextual_search_oldest_week";
    /**
     * An encoded set of outcomes of user interaction with Contextual Search, stored as an int.
     */
    public static final String CONTEXTUAL_SEARCH_PREVIOUS_INTERACTION_ENCODED_OUTCOMES =
            "contextual_search_previous_interaction_encoded_outcomes";
    /**
     * A user interaction event ID for interaction with Contextual Search, stored as a long.
     */
    public static final String CONTEXTUAL_SEARCH_PREVIOUS_INTERACTION_EVENT_ID =
            "contextual_search_previous_interaction_event_id";
    /**
     * A timestamp indicating when we updated the user interaction with Contextual Search, stored
     * as a long, with resolution in days.
     */
    public static final String CONTEXTUAL_SEARCH_PREVIOUS_INTERACTION_TIMESTAMP =
            "contextual_search_previous_interaction_timestamp";
    /**
     * The number of times the Contextual Search panel was opened with the opt-in promo visible.
     */
    public static final String CONTEXTUAL_SEARCH_PROMO_OPEN_COUNT =
            "contextual_search_promo_open_count";
    /**
     * The Quick Actions ignored count, i.e. phone numbers available but not dialed.
     * Cumulative, starting at M-69.
     */
    public static final String CONTEXTUAL_SEARCH_QUICK_ACTIONS_IGNORED_COUNT =
            "contextual_search_quick_actions_ignored_count";
    /**
     * The Quick Actions taken count for Contextual Search, i.e. phone numbers dialed and similar
     * actions. Cumulative, starting at M-69.
     */
    public static final String CONTEXTUAL_SEARCH_QUICK_ACTIONS_TAKEN_COUNT =
            "contextual_search_quick_actions_taken_count";
    /**
     * The Quick Action impressions count for Contextual Search, i.e. actions presented in the Bar.
     * Cumulative, starting at M-69.
     */
    public static final String CONTEXTUAL_SEARCH_QUICK_ACTION_IMPRESSIONS_COUNT =
            "contextual_search_quick_action_impressions_count";
    /**
     * The number of times that a tap triggered the Contextual Search panel to peek since the last
     * time the panel was opened.  Note legacy string value without "open".
     */
    public static final String CONTEXTUAL_SEARCH_TAP_SINCE_OPEN_COUNT =
            "contextual_search_tap_count";
    /**
     * The number of times a tap gesture caused a Contextual Search Quick Answer to be shown since
     * the last time the panel was opened.  Note legacy string value without "open".
     */
    public static final String CONTEXTUAL_SEARCH_TAP_SINCE_OPEN_QUICK_ANSWER_COUNT =
            "contextual_search_tap_quick_answer_count";
    public static final String CONTEXTUAL_SEARCH_TAP_TRIGGERED_PROMO_COUNT =
            "contextual_search_tap_triggered_promo_count";

    /**
     * Keys that indicates if an item in the context menu has been clicked or not.
     * Used to hide the "new" tag for the items after they are clicked.
     */
    public static final String CONTEXT_MENU_OPEN_IMAGE_IN_EPHEMERAL_TAB_CLICKED =
            "Chrome.Contextmenu.OpenImageInEphemeralTabClicked";
    public static final String CONTEXT_MENU_OPEN_IN_EPHEMERAL_TAB_CLICKED =
            "Chrome.Contextmenu.OpenInEphemeralTabClicked";
    public static final String CONTEXT_MENU_SEARCH_WITH_GOOGLE_LENS_CLICKED =
            "Chrome.ContextMenu.SearchWithGoogleLensClicked";

    public static final String CRASH_UPLOAD_FAILURE_BROWSER = "browser_crash_failure_upload";
    public static final String CRASH_UPLOAD_FAILURE_GPU = "gpu_crash_failure_upload";
    public static final String CRASH_UPLOAD_FAILURE_OTHER = "other_crash_failure_upload";
    public static final String CRASH_UPLOAD_FAILURE_RENDERER = "renderer_crash_failure_upload";
    public static final String CRASH_UPLOAD_SUCCESS_BROWSER = "browser_crash_success_upload";
    public static final String CRASH_UPLOAD_SUCCESS_GPU = "gpu_crash_success_upload";
    public static final String CRASH_UPLOAD_SUCCESS_OTHER = "other_crash_success_upload";
    public static final String CRASH_UPLOAD_SUCCESS_RENDERER = "renderer_crash_success_upload";

    public static final KeyPrefix CUSTOM_TABS_DEX_LAST_UPDATE_TIME_PREF_PREFIX =
            new KeyPrefix("pref_local_custom_tabs_module_dex_last_update_time_*");
    public static final String CUSTOM_TABS_LAST_URL = "pref_last_custom_tab_url";

    /**
     * Key used to save the time in milliseconds since epoch that the first run experience or second
     * run promo was shown.
     */
    public static final String DATA_REDUCTION_DISPLAYED_FRE_OR_SECOND_PROMO_TIME_MS =
            "displayed_data_reduction_promo_time_ms";
    /**
     * Key used to save the Chrome version the first run experience or second run promo was shown
     * in.
     */
    public static final String DATA_REDUCTION_DISPLAYED_FRE_OR_SECOND_PROMO_VERSION =
            "displayed_data_reduction_promo_version";
    /**
     * Key used to save whether the first run experience or second run promo screen has been shown.
     */
    public static final String DATA_REDUCTION_DISPLAYED_FRE_OR_SECOND_RUN_PROMO =
            "displayed_data_reduction_promo";
    /**
     * Key used to save whether the infobar promo has been shown.
     */
    public static final String DATA_REDUCTION_DISPLAYED_INFOBAR_PROMO =
            "displayed_data_reduction_infobar_promo";
    /**
     * Key used to save the Chrome version the infobar promo was shown in.
     */
    public static final String DATA_REDUCTION_DISPLAYED_INFOBAR_PROMO_VERSION =
            "displayed_data_reduction_infobar_promo_version";
    /**
     * Key used to save the saved bytes when the milestone promo was last shown. This value is
     * initialized to the bytes saved for data saver users that had data saver turned on when this
     * pref was added. This prevents us from showing promo for savings that have already happened
     * for existing users.
     * Note: For historical reasons, this pref key is misnamed. This promotion used to be conveyed
     * in a snackbar but was moved to an IPH in M74.
     */
    public static final String DATA_REDUCTION_DISPLAYED_MILESTONE_PROMO_SAVED_BYTES =
            "displayed_data_reduction_snackbar_promo_saved_bytes";

    // Visible for backup and restore
    public static final String DATA_REDUCTION_ENABLED = "BANDWIDTH_REDUCTION_PROXY_ENABLED";
    public static final String DATA_REDUCTION_FIRST_ENABLED_TIME =
            "BANDWIDTH_REDUCTION_FIRST_ENABLED_TIME";
    /**
     * Key used to save whether the user opted out of the data reduction proxy in the FRE promo.
     */
    public static final String DATA_REDUCTION_FRE_PROMO_OPT_OUT = "fre_promo_opt_out";
    /**
     * Key used to save the date on which the site breakdown should be shown. If the user has
     * historical data saver stats, the site breakdown cannot be shown for MAXIMUM_DAYS_IN_CHART.
     */
    public static final String DATA_REDUCTION_SITE_BREAKDOWN_ALLOWED_DATE =
            "data_reduction_site_breakdown_allowed_date";

    public static final String DOWNLOAD_IS_DOWNLOAD_HOME_ENABLED =
            "org.chromium.chrome.browser.download.IS_DOWNLOAD_HOME_ENABLED";
    public static final String DOWNLOAD_PENDING_DOWNLOAD_NOTIFICATIONS =
            "PendingDownloadNotifications";
    public static final String DOWNLOAD_PENDING_OMA_DOWNLOADS = "PendingOMADownloads";
    public static final String DOWNLOAD_UMA_ENTRY = "DownloadUmaEntry";

    /**
     * Indicates whether or not there are prefetched content in chrome that can be viewed offline.
     */
    public static final String EXPLORE_OFFLINE_CONTENT_AVAILABILITY_STATUS =
            "Chrome.NTPExploreOfflineCard.HasExploreOfflineContent";

    public static final String FIRST_RUN_CACHED_TOS_ACCEPTED = "first_run_tos_accepted";
    public static final String FIRST_RUN_FLOW_COMPLETE = "first_run_flow";
    public static final String FIRST_RUN_FLOW_SIGNIN_ACCOUNT_NAME = "first_run_signin_account_name";
    public static final String FIRST_RUN_FLOW_SIGNIN_COMPLETE = "first_run_signin_complete";
    // Needed by ChromeBackupAgent
    public static final String FIRST_RUN_FLOW_SIGNIN_SETUP = "first_run_signin_setup";
    public static final String FIRST_RUN_LIGHTWEIGHT_FLOW_COMPLETE = "lightweight_first_run_flow";
    public static final String FIRST_RUN_SKIP_WELCOME_PAGE = "skip_welcome_page";

    /**
     * Cached feature flags generated by CachedFeatureFlags use this prefix.
     */
    public static final KeyPrefix FLAGS_CACHED = new KeyPrefix("Chrome.Flags.CachedFlag.*");

    /**
     * Cached field trial parameters generated by CachedFeatureFlags use this prefix.
     */
    public static final KeyPrefix FLAGS_FIELD_TRIAL_PARAM_CACHED =
            new KeyPrefix("Chrome.Flags.FieldTrialParamCached.*");

    /**
     * Whether or not the adaptive toolbar is enabled.
     * Default value is true.
     */
    public static final String FLAGS_CACHED_ADAPTIVE_TOOLBAR_ENABLED = "adaptive_toolbar_enabled";
    /**
     * Whether or not the bottom toolbar is enabled.
     * Default value is false.
     */
    public static final String FLAGS_CACHED_BOTTOM_TOOLBAR_ENABLED = "bottom_toolbar_enabled";
    /**
     * Whether or not command line on non-rooted devices is enabled.
     * Default value is false.
     */
    public static final String FLAGS_CACHED_COMMAND_LINE_ON_NON_ROOTED_ENABLED =
            "command_line_on_non_rooted_enabled";
    /**
     * Whether or not the download auto-resumption is enabled in native.
     * Default value is true.
     */
    public static final String FLAGS_CACHED_DOWNLOAD_AUTO_RESUMPTION_IN_NATIVE =
            "download_auto_resumption_in_native";
    /**
     * Whether or not the Duet-TabStrip integration is enabled.
     * Default value is false.
     */
    public static final String FLAGS_CACHED_DUET_TABSTRIP_INTEGRATION_ANDROID_ENABLED =
            "Chrome.Flags.DuetTabstripIntegrationEnabled";
    /**
     * Whether or not the grid tab switcher is enabled.
     * Default value is false.
     */
    public static final String FLAGS_CACHED_GRID_TAB_SWITCHER_ENABLED = "grid_tab_switcher_enabled";
    /**
     * Key to cache whether immersive ui mode is enabled.
     */
    public static final String FLAGS_CACHED_IMMERSIVE_UI_MODE_ENABLED = "immersive_ui_mode_enabled";
    public static final String FLAGS_CACHED_INTEREST_FEED_CONTENT_SUGGESTIONS =
            "interest_feed_content_suggestions";
    /**
     * Whether or not the labeled bottom toolbar is enabled.
     * Default value is false.
     */
    public static final String FLAGS_CACHED_LABELED_BOTTOM_TOOLBAR_ENABLED =
            "labeled_bottom_toolbar_enabled";
    /**
     * Whether warming up network service is enabled.
     * Default value is false.
     */
    public static final String FLAGS_CACHED_NETWORK_SERVICE_WARM_UP_ENABLED =
            "network_service_warm_up_enabled";
    /**
     * Whether or not night mode is available.
     * Default value is false.
     */
    public static final String FLAGS_CACHED_NIGHT_MODE_AVAILABLE = "night_mode_available";
    /**
     * Whether or not night mode is available for custom tabs.
     * Default value is false.
     */
    public static final String FLAGS_CACHED_NIGHT_MODE_CCT_AVAILABLE = "night_mode_cct_available";
    /**
     * Whether or not night mode should set "light" as the default option.
     * Default value is false.
     */
    public static final String FLAGS_CACHED_NIGHT_MODE_DEFAULT_TO_LIGHT =
            "night_mode_default_to_light";
    /**
     * Whether the Paint Preview Capture menu item is enabled.
     * Default value is false.
     */
    public static final String FLAGS_CACHED_PAINT_PREVIEW_TEST_ENABLED_KEY =
            "Chrome.Flags.PaintPreviewTestEnabled";
    /**
     * Whether or not bootstrap tasks should be prioritized (i.e. bootstrap task prioritization
     * experiment is enabled). Default value is true.
     */
    public static final String FLAGS_CACHED_PRIORITIZE_BOOTSTRAP_TASKS =
            "prioritize_bootstrap_tasks";
    /**
     * Key for whether PrefetchBackgroundTask should load native in service manager only mode.
     * Default value is false.
     */
    public static final String FLAGS_CACHED_SERVICE_MANAGER_FOR_BACKGROUND_PREFETCH =
            "service_manager_for_background_prefetch";
    /**
     * Key for whether DownloadResumptionBackgroundTask should load native in service manager only
     * mode.
     * Default value is false.
     */
    public static final String FLAGS_CACHED_SERVICE_MANAGER_FOR_DOWNLOAD_RESUMPTION =
            "service_manager_for_download_resumption";
    /**
     * Whether or not the start surface is enabled.
     * Default value is false.
     */
    public static final String FLAGS_CACHED_START_SURFACE_ENABLED = "start_surface_enabled";

    /**
     * Whether or not the start surface single pane is enabled.
     * Default value is false.
     */
    public static final String START_SURFACE_SINGLE_PANE_ENABLED_KEY =
            "start_surface_single_pane_enabled";

    /**
     * Key to cache whether SWAP_PIXEL_FORMAT_TO_FIX_CONVERT_FROM_TRANSLUCENT is enabled.
     */
    public static final String FLAGS_CACHED_SWAP_PIXEL_FORMAT_TO_FIX_CONVERT_FROM_TRANSLUCENT =
            "swap_pixel_format_to_fix_convert_from_translucent";
    /**
     * Whether or not the tab group is enabled.
     * Default value is false.
     */
    public static final String FLAGS_CACHED_TAB_GROUPS_ANDROID_ENABLED =
            "tab_group_android_enabled";

    public static final String FONT_USER_FONT_SCALE_FACTOR = "user_font_scale_factor";
    public static final String FONT_USER_SET_FORCE_ENABLE_ZOOM = "user_set_force_enable_zoom";

    public static final String HISTORY_SHOW_HISTORY_INFO = "history_home_show_info";

    /** Keys used to save settings related to homepage. */
    public static final String HOMEPAGE_CUSTOM_URI = "homepage_custom_uri";
    public static final String HOMEPAGE_ENABLED = "homepage";
    public static final String HOMEPAGE_USE_DEFAULT_URI = "homepage_partner_enabled";

    /**
     * Key used to save homepage location set by enterprise policy
     */
    public static final String HOMEPAGE_LOCATION_POLICY = "Chrome.Policy.HomepageLocation";

    public static final String INCOGNITO_SHORTCUT_ADDED = "incognito-shortcut-added";

    /**
     * Key for UUID-based generator used for Chrome Invalidations (sync, etc.).
     */
    public static final String INVALIDATIONS_UUID_PREF_KEY = "chromium.invalidations.uuid";

    /**
     * When the user is shown a badge that the current Android OS version is unsupported, and they
     * tap it to display the menu (which has additional information), we store the current version
     * of Chrome to this preference to ensure we only show the badge once. The value is cleared
     * if the Chrome version later changes.
     */
    public static final String LATEST_UNSUPPORTED_VERSION = "android_os_unsupported_chrome_version";

    public static final String LOCALE_MANAGER_AUTO_SWITCH = "LocaleManager_PREF_AUTO_SWITCH";
    public static final String LOCALE_MANAGER_PROMO_SHOWN = "LocaleManager_PREF_PROMO_SHOWN";
    public static final String LOCALE_MANAGER_SEARCH_ENGINE_PROMO_SHOW_STATE =
            "com.android.chrome.SEARCH_ENGINE_PROMO_SHOWN";
    public static final String LOCALE_MANAGER_WAS_IN_SPECIAL_LOCALE =
            "LocaleManager_WAS_IN_SPECIAL_LOCALE";

    public static final String MEDIA_WEBRTC_NOTIFICATION_IDS = "WebRTCNotificationIds";

    public static final String METRICS_MAIN_INTENT_LAUNCH_COUNT = "MainIntent.LaunchCount";
    public static final String METRICS_MAIN_INTENT_LAUNCH_TIMESTAMP = "MainIntent.LaunchTimestamp";

    public static final String NOTIFICATIONS_CHANNELS_VERSION = "channels_version_key";
    public static final String NOTIFICATIONS_LAST_SHOWN_NOTIFICATION_TYPE =
            "NotificationUmaTracker.LastShownNotificationType";
    public static final String NOTIFICATIONS_NEXT_TRIGGER =
            "notification_trigger_scheduler.next_trigger";

    public static final String NTP_SNIPPETS_IS_SCHEDULED = "ntp_snippets.is_scheduled";

    // Name of an application preference variable used to track whether or not the in-progress
    // notification is being shown. This is an alternative to
    // NotificationManager.getActiveNotifications, which isn't available prior to API level 23.
    public static final String OFFLINE_AUTO_FETCH_SHOWING_IN_PROGRESS =
            "offline_auto_fetch_showing_in_progress";
    // The application preference variable which is set to the NotificationAction that triggered the
    // cancellation, when a cancellation is requested by the user.
    public static final String OFFLINE_AUTO_FETCH_USER_CANCEL_ACTION_IN_PROGRESS =
            "offline_auto_fetch_user_cancel_action_in_progress";

    /**
     * Key to cache whether offline indicator v2 (persistent offline indicator) is enabled.
     */
    public static final String OFFLINE_INDICATOR_V2_ENABLED = "offline_indicator_v2_enabled";

    /** Prefix of the preferences to persist use count of the payment instruments. */
    public static final KeyPrefix PAYMENTS_PAYMENT_INSTRUMENT_USE_COUNT =
            new KeyPrefix("payment_instrument_use_count_*");

    /** Prefix of the preferences to persist last use date of the payment instruments. */
    public static final KeyPrefix PAYMENTS_PAYMENT_INSTRUMENT_USE_DATE =
            new KeyPrefix("payment_instrument_use_date_*");

    /** Preference to indicate whether payment request has been completed successfully once.*/
    public static final String PAYMENTS_PAYMENT_COMPLETE_ONCE = "payment_complete_once";

    public static final String PREFETCH_HAS_NEW_PAGES = "prefetch_notification_has_new_pages";
    public static final String PREFETCH_IGNORED_NOTIFICATION_COUNTER =
            "prefetch_notification_ignored_counter";
    public static final String PREFETCH_NOTIFICATION_ENABLED = "prefetch_notification_enabled";
    public static final String PREFETCH_NOTIFICATION_TIME = "prefetch_notification_shown_time";
    public static final String PREFETCH_OFFLINE_COUNTER = "prefetch_notification_offline_counter";

    public static final String PRIVACY_METRICS_REPORTING = "metrics_reporting";
    public static final String PRIVACY_METRICS_IN_SAMPLE = "in_metrics_sample";
    public static final String PRIVACY_NETWORK_PREDICTIONS = "network_predictions";
    public static final String PRIVACY_BANDWIDTH_OLD = "prefetch_bandwidth";
    public static final String PRIVACY_BANDWIDTH_NO_CELLULAR_OLD = "prefetch_bandwidth_no_cellular";
    public static final String PRIVACY_ALLOW_PRERENDER_OLD = "allow_prefetch";

    /**
     * Key to cache the enabled bottom toolbar parameter.
     */
    public static final String VARIATION_CACHED_BOTTOM_TOOLBAR = "bottom_toolbar_variation";

    /**
     * Whether the promotion for data reduction has been skipped on first invocation.
     * Default value is false.
     */
    public static final String PROMOS_SKIPPED_ON_FIRST_START = "promos_skipped_on_first_start";

    /**
     * Contains a trial group that was used to determine whether the reached code profiler should be
     * enabled.
     */
    public static final String REACHED_CODE_PROFILER_GROUP = "reached_code_profiler_group";

    public static final String RLZ_NOTIFIED = "rlz_first_search_notified";

    /** Key used to store the default Search Engine Type before choice is presented. */
    public static final String SEARCH_ENGINE_CHOICE_DEFAULT_TYPE_BEFORE =
            "search_engine_choice_default_type_before";
    /** Key used to store the version of Chrome in which the choice was presented. */
    public static final String SEARCH_ENGINE_CHOICE_PRESENTED_VERSION =
            "search_engine_choice_presented_version";
    /** Key used to store the date of when search engine choice was requested. */
    public static final String SEARCH_ENGINE_CHOICE_REQUESTED_TIMESTAMP =
            "search_engine_choice_requested_timestamp";

    public static final String SETTINGS_DEVELOPER_ENABLED = "developer";
    public static final String SETTINGS_DEVELOPER_TRACING_CATEGORIES = "tracing_categories";
    public static final String SETTINGS_DEVELOPER_TRACING_MODE = "tracing_mode";

    /**
     * SharedPreference name for the preference that disables signing out of Chrome.
     * Signing out is forever disabled once Chrome signs the user in automatically
     * if the device has a child account or if the device is an Android EDU device.
     */
    public static final String SETTINGS_SYNC_SIGN_OUT_ALLOWED = "auto_signed_in_school_account";

    public static final String SETTINGS_PRIVACY_OTHER_FORMS_OF_HISTORY_DIALOG_SHOWN =
            "org.chromium.chrome.browser.settings.privacy."
            + "PREF_OTHER_FORMS_OF_HISTORY_DIALOG_SHOWN";

    public static final String SETTINGS_WEBSITE_FAILED_BUILD_VERSION =
            "ManagedSpace.FailedBuildVersion";

    public static final String SHARING_LAST_SHARED_CLASS_NAME = "last_shared_class_name";
    public static final String SHARING_LAST_SHARED_PACKAGE_NAME = "last_shared_package_name";

    public static final String SIGNIN_ACCOUNTS_CHANGED = "prefs_sync_accounts_changed";

    /**
     * Holds the new account's name if the currently signed in account has been renamed.
     */
    public static final String SIGNIN_ACCOUNT_RENAMED = "prefs_sync_account_renamed";

    /**
     * Holds the last read index of all the account changed events of the current signed in account.
     */
    public static final String SIGNIN_ACCOUNT_RENAME_EVENT_INDEX =
            "prefs_sync_account_rename_event_index";

    /**
     * Generic signin and sync promo preferences.
     */
    public static final String SIGNIN_AND_SYNC_PROMO_SHOW_COUNT =
            "enhanced_bookmark_signin_promo_show_count";

    public static final String SIGNIN_PROMO_IMPRESSIONS_COUNT_BOOKMARKS =
            "signin_promo_impressions_count_bookmarks";
    public static final String SIGNIN_PROMO_IMPRESSIONS_COUNT_SETTINGS =
            "signin_promo_impressions_count_settings";
    public static final String SIGNIN_PROMO_LAST_SHOWN_ACCOUNT_NAMES =
            "signin_promo_last_shown_account_names";
    public static final String SIGNIN_PROMO_LAST_SHOWN_MAJOR_VERSION =
            "signin_promo_last_shown_chrome_version";
    /**
     * Whether the user dismissed the personalized sign in promo from the new tab page.
     * Default value is false.
     */
    public static final String SIGNIN_PROMO_NTP_PROMO_DISMISSED =
            "ntp.personalized_signin_promo_dismissed";
    public static final String SIGNIN_PROMO_NTP_PROMO_SUPPRESSION_PERIOD_START =
            "ntp.signin_promo_suppression_period_start";
    /**
     * Personalized signin promo preference.
     */
    public static final String SIGNIN_PROMO_PERSONALIZED_DECLINED =
            "signin_promo_bookmarks_declined";
    /**
     * Whether the user dismissed the personalized sign in promo from the Settings.
     * Default value is false.
     */
    public static final String SIGNIN_PROMO_SETTINGS_PERSONALIZED_DISMISSED =
            "settings_personalized_signin_promo_dismissed";

    public static final String SNAPSHOT_DATABASE_REMOVED = "snapshot_database_removed";

    public static final String SURVEY_DATE_LAST_ROLLED = "last_rolled_for_chrome_survey_key";
    /**
     *  The survey questions for this survey are the same as those in the survey used for Chrome
     *  Home, so we reuse the old infobar key to prevent the users from seeing the same survey more
     *  than once.
     */
    public static final String SURVEY_INFO_BAR_DISPLAYED = "chrome_home_survey_info_bar_displayed";

    public static final String SYNC_SESSIONS_UUID = "chromium.sync.sessions.id";

    public static final String TABBED_ACTIVITY_LAST_BACKGROUNDED_TIME_MS_PREF =
            "ChromeTabbedActivity.BackgroundTimeMs";

    public static final String TABMODEL_ACTIVE_TAB_ID =
            "org.chromium.chrome.browser.tabmodel.TabPersistentStore.ACTIVE_TAB_ID";
    public static final String TABMODEL_HAS_COMPUTED_MAX_ID =
            "org.chromium.chrome.browser.tabmodel.TabPersistentStore.HAS_COMPUTED_MAX_ID";
    public static final String TABMODEL_HAS_RUN_FILE_MIGRATION =
            "org.chromium.chrome.browser.tabmodel.TabPersistentStore.HAS_RUN_FILE_MIGRATION";
    public static final String TABMODEL_HAS_RUN_MULTI_INSTANCE_FILE_MIGRATION =
            "org.chromium.chrome.browser.tabmodel.TabPersistentStore."
            + "HAS_RUN_MULTI_INSTANCE_FILE_MIGRATION";

    public static final String TAB_ID_MANAGER_NEXT_ID =
            "org.chromium.chrome.browser.tab.TabIdManager.NEXT_ID";

    public static final String TOS_ACKED_ACCOUNTS = "ToS acknowledged accounts";

    /**
     * Keys for deferred recording of the outcomes of showing the clear data dialog after
     * Trusted Web Activity client apps are uninstalled or have their data cleared.
     */
    public static final String TWA_DIALOG_NUMBER_OF_DISMISSALS_ON_CLEAR_DATA =
            "twa_dialog_number_of_dismissals_on_clear_data";
    public static final String TWA_DIALOG_NUMBER_OF_DISMISSALS_ON_UNINSTALL =
            "twa_dialog_number_of_dismissals_on_uninstall";
    public static final String TWA_DISCLOSURE_ACCEPTED_PACKAGES =
            "trusted_web_activity_disclosure_accepted_packages";

    /**
     * Whether or not darken websites is enabled.
     * Default value is false.
     */
    public static final String UI_THEME_DARKEN_WEBSITES_ENABLED = "darken_websites_enabled";
    /**
     * The current theme setting in the user settings.
     * Default value is -1. Use NightModeUtils#getThemeSetting() to retrieve current setting or
     * default theme.
     */
    public static final String UI_THEME_SETTING = "ui_theme_setting";

    public static final String VERIFIED_DIGITAL_ASSET_LINKS = "verified_digital_asset_links";

    public static final String VR_EXIT_TO_2D_COUNT = "VR_EXIT_TO_2D_COUNT";
    public static final String VR_FEEDBACK_OPT_OUT = "VR_FEEDBACK_OPT_OUT";

    /**
     * Whether VR assets component should be registered on startup.
     * Default value is false.
     */
    public static final String VR_SHOULD_REGISTER_ASSETS_COMPONENT_ON_STARTUP =
            "should_register_vr_assets_component_on_startup";

    /**
     * Name of the shared preference for the version number of the dynamically loaded dex.
     */
    public static final String WEBAPK_EXTRACTED_DEX_VERSION =
            "org.chromium.chrome.browser.webapps.extracted_dex_version";

    /**
     * Name of the shared preference for the Android OS version at the time that the dex was last
     * extracted from Chrome's assets and optimized.
     */
    public static final String WEBAPK_LAST_SDK_VERSION =
            "org.chromium.chrome.browser.webapps.last_sdk_version";

    /** Key for deferred recording of list of uninstalled WebAPK packages. */
    public static final String WEBAPK_UNINSTALLED_PACKAGES = "webapk_uninstalled_packages";

    /**
     * These values are currently used as SharedPreferences keys, along with the keys in
     * {@link #createGrandfatheredKeysInUse()}. Add new SharedPreferences keys here.
     *
     * @return The list of [keys in use] conforming to the format.
     */
    @CheckDiscard("Validation is performed in tests and in debug builds.")
    static List<String> createKeysInUse() {
        // clang-format off
        return Arrays.asList(
                CONTEXT_MENU_OPEN_IMAGE_IN_EPHEMERAL_TAB_CLICKED,
                CONTEXT_MENU_OPEN_IN_EPHEMERAL_TAB_CLICKED,
                CONTEXT_MENU_SEARCH_WITH_GOOGLE_LENS_CLICKED,
                EXPLORE_OFFLINE_CONTENT_AVAILABILITY_STATUS,
                FLAGS_CACHED.pattern(),
                FLAGS_CACHED_DUET_TABSTRIP_INTEGRATION_ANDROID_ENABLED,
                FLAGS_CACHED_PAINT_PREVIEW_TEST_ENABLED_KEY,
                FLAGS_FIELD_TRIAL_PARAM_CACHED.pattern(),
                HOMEPAGE_LOCATION_POLICY
        );
        // clang-format on
    }

    /**
     * These values have been used as SharedPreferences keys in the past and should not be reused
     * reused. Do not remove values from this list.
     *
     * @return The list of [deprecated keys].
     */
    @CheckDiscard("Validation is performed in tests and in debug builds.")
    static List<String> createDeprecatedKeysForTesting() {
        // clang-format off
        return Arrays.asList(
                "PersistedNotificationId",
                "PhysicalWeb.ActivityReferral",
                "PhysicalWeb.HasDeferredMetrics",
                "PhysicalWeb.OptIn.DeclineButtonPressed",
                "PhysicalWeb.OptIn.EnableButtonPressed",
                "PhysicalWeb.Prefs.FeatureDisabled",
                "PhysicalWeb.Prefs.FeatureEnabled",
                "PhysicalWeb.Prefs.LocationDenied",
                "PhysicalWeb.Prefs.LocationGranted",
                "PhysicalWeb.ResolveTime.Background",
                "PhysicalWeb.ResolveTime.Foreground",
                "PhysicalWeb.ResolveTime.Refresh",
                "PhysicalWeb.State",
                "PhysicalWeb.TotalUrls.OnInitialDisplay",
                "PhysicalWeb.TotalUrls.OnRefresh",
                "PhysicalWeb.UrlSelected",
                "PrefMigrationVersion",
                "ServiceManagerFeatures",
                "allow_low_end_device_ui",
                "allow_starting_service_manager_only",
                "bookmark_search_history",
                "cellular_experiment",
                "chrome_home_enabled_date",
                "chrome_home_info_promo_shown",
                "chrome_home_opt_out_snackbar_shown",
                "chrome_home_user_enabled",
                "chrome_modern_design_enabled",
                "click_to_call_open_dialer_directly",
                "crash_dump_upload",
                "crash_dump_upload_no_cellular",
                "home_page_button_force_enabled",
                "homepage_tile_enabled",
                "inflate_toolbar_on_background_thread",
                "ntp_button_enabled",
                "ntp_button_variant",
                "physical_web",
                "physical_web_sharing",
                "sole_integration_enabled",
                "tab_persistent_store_task_runner_enabled",
                "webapk_number_of_uninstalls",
                "website_settings_filter"
        );
        // clang-format on
    }

    /**
     * Do not add new constants to this list unless you are migrating old SharedPreferences keys.
     * Instead, declare new keys in the format "Chrome.[Feature].[Key]", for example
     * "Chrome.FooBar.FooEnabled", and add them to {@link #createKeysInUse()}.
     *
     * @return The list of [keys in use] that do not conform to the "Chrome.[Feature].[Key]"
     *     format.
     */
    @CheckDiscard("Validation is performed in tests and in debug builds.")
    static List<String> createGrandfatheredKeysInUse() {
        // clang-format off
        return Arrays.asList(
                ACCESSIBILITY_TAB_SWITCHER,
                APP_LOCALE,
                AUTOFILL_ASSISTANT_ENABLED,
                AUTOFILL_ASSISTANT_ONBOARDING_ACCEPTED,
                AUTOFILL_ASSISTANT_SKIP_INIT_SCREEN,
                BOOKMARKS_LAST_MODIFIED_FOLDER_ID,
                BOOKMARKS_LAST_USED_URL,
                BOOKMARKS_LAST_USED_PARENT,
                CHROME_DEFAULT_BROWSER,
                CONTENT_SUGGESTIONS_SHOWN,
                CONTEXTUAL_SEARCH_ALL_TIME_OPEN_COUNT,
                CONTEXTUAL_SEARCH_ALL_TIME_TAP_COUNT,
                CONTEXTUAL_SEARCH_ALL_TIME_TAP_QUICK_ANSWER_COUNT,
                CONTEXTUAL_SEARCH_CURRENT_WEEK_NUMBER,
                CONTEXTUAL_SEARCH_ENTITY_IMPRESSIONS_COUNT,
                CONTEXTUAL_SEARCH_ENTITY_OPENS_COUNT,
                CONTEXTUAL_SEARCH_LAST_ANIMATION_TIME,
                CONTEXTUAL_SEARCH_NEWEST_WEEK,
                CONTEXTUAL_SEARCH_OLDEST_WEEK,
                CONTEXTUAL_SEARCH_PREVIOUS_INTERACTION_ENCODED_OUTCOMES,
                CONTEXTUAL_SEARCH_PREVIOUS_INTERACTION_EVENT_ID,
                CONTEXTUAL_SEARCH_PREVIOUS_INTERACTION_TIMESTAMP,
                CONTEXTUAL_SEARCH_PROMO_OPEN_COUNT,
                CONTEXTUAL_SEARCH_QUICK_ACTIONS_IGNORED_COUNT,
                CONTEXTUAL_SEARCH_QUICK_ACTIONS_TAKEN_COUNT,
                CONTEXTUAL_SEARCH_QUICK_ACTION_IMPRESSIONS_COUNT,
                CONTEXTUAL_SEARCH_TAP_SINCE_OPEN_COUNT,
                CONTEXTUAL_SEARCH_TAP_SINCE_OPEN_QUICK_ANSWER_COUNT,
                CONTEXTUAL_SEARCH_TAP_TRIGGERED_PROMO_COUNT,
                CRASH_UPLOAD_FAILURE_BROWSER,
                CRASH_UPLOAD_FAILURE_GPU,
                CRASH_UPLOAD_FAILURE_OTHER,
                CRASH_UPLOAD_FAILURE_RENDERER,
                CRASH_UPLOAD_SUCCESS_BROWSER,
                CRASH_UPLOAD_SUCCESS_GPU,
                CRASH_UPLOAD_SUCCESS_OTHER,
                CRASH_UPLOAD_SUCCESS_RENDERER,
                CUSTOM_TABS_LAST_URL,
                DATA_REDUCTION_DISPLAYED_FRE_OR_SECOND_PROMO_TIME_MS,
                DATA_REDUCTION_DISPLAYED_FRE_OR_SECOND_PROMO_VERSION,
                DATA_REDUCTION_DISPLAYED_FRE_OR_SECOND_RUN_PROMO,
                DATA_REDUCTION_DISPLAYED_INFOBAR_PROMO,
                DATA_REDUCTION_DISPLAYED_INFOBAR_PROMO_VERSION,
                DATA_REDUCTION_DISPLAYED_MILESTONE_PROMO_SAVED_BYTES,
                DATA_REDUCTION_ENABLED,
                DATA_REDUCTION_FIRST_ENABLED_TIME,
                DATA_REDUCTION_FRE_PROMO_OPT_OUT,
                DATA_REDUCTION_SITE_BREAKDOWN_ALLOWED_DATE,
                DOWNLOAD_IS_DOWNLOAD_HOME_ENABLED,
                DOWNLOAD_PENDING_DOWNLOAD_NOTIFICATIONS,
                DOWNLOAD_PENDING_OMA_DOWNLOADS,
                DOWNLOAD_UMA_ENTRY,
                FIRST_RUN_CACHED_TOS_ACCEPTED,
                FIRST_RUN_FLOW_COMPLETE,
                FIRST_RUN_FLOW_SIGNIN_ACCOUNT_NAME,
                FIRST_RUN_FLOW_SIGNIN_COMPLETE,
                FIRST_RUN_FLOW_SIGNIN_SETUP,
                FIRST_RUN_LIGHTWEIGHT_FLOW_COMPLETE,
                FIRST_RUN_SKIP_WELCOME_PAGE,
                FLAGS_CACHED_ADAPTIVE_TOOLBAR_ENABLED,
                FLAGS_CACHED_BOTTOM_TOOLBAR_ENABLED,
                FLAGS_CACHED_COMMAND_LINE_ON_NON_ROOTED_ENABLED,
                FLAGS_CACHED_DOWNLOAD_AUTO_RESUMPTION_IN_NATIVE,
                FLAGS_CACHED_GRID_TAB_SWITCHER_ENABLED,
                FLAGS_CACHED_IMMERSIVE_UI_MODE_ENABLED,
                FLAGS_CACHED_INTEREST_FEED_CONTENT_SUGGESTIONS,
                FLAGS_CACHED_LABELED_BOTTOM_TOOLBAR_ENABLED,
                FLAGS_CACHED_NETWORK_SERVICE_WARM_UP_ENABLED,
                FLAGS_CACHED_NIGHT_MODE_AVAILABLE,
                FLAGS_CACHED_NIGHT_MODE_CCT_AVAILABLE,
                FLAGS_CACHED_NIGHT_MODE_DEFAULT_TO_LIGHT,
                FLAGS_CACHED_PRIORITIZE_BOOTSTRAP_TASKS,
                FLAGS_CACHED_SERVICE_MANAGER_FOR_BACKGROUND_PREFETCH,
                FLAGS_CACHED_SERVICE_MANAGER_FOR_DOWNLOAD_RESUMPTION,
                FLAGS_CACHED_START_SURFACE_ENABLED,
                FLAGS_CACHED_SWAP_PIXEL_FORMAT_TO_FIX_CONVERT_FROM_TRANSLUCENT,
                FLAGS_CACHED_TAB_GROUPS_ANDROID_ENABLED,
                FONT_USER_FONT_SCALE_FACTOR,
                FONT_USER_SET_FORCE_ENABLE_ZOOM,
                HISTORY_SHOW_HISTORY_INFO,
                HOMEPAGE_CUSTOM_URI,
                HOMEPAGE_ENABLED,
                HOMEPAGE_USE_DEFAULT_URI,
                INCOGNITO_SHORTCUT_ADDED,
                INVALIDATIONS_UUID_PREF_KEY,
                LATEST_UNSUPPORTED_VERSION,
                LOCALE_MANAGER_AUTO_SWITCH,
                LOCALE_MANAGER_PROMO_SHOWN,
                LOCALE_MANAGER_SEARCH_ENGINE_PROMO_SHOW_STATE,
                LOCALE_MANAGER_WAS_IN_SPECIAL_LOCALE,
                MEDIA_WEBRTC_NOTIFICATION_IDS,
                METRICS_MAIN_INTENT_LAUNCH_COUNT,
                METRICS_MAIN_INTENT_LAUNCH_TIMESTAMP,
                NOTIFICATIONS_CHANNELS_VERSION,
                NOTIFICATIONS_LAST_SHOWN_NOTIFICATION_TYPE,
                NOTIFICATIONS_NEXT_TRIGGER,
                NTP_SNIPPETS_IS_SCHEDULED,
                OFFLINE_AUTO_FETCH_SHOWING_IN_PROGRESS,
                OFFLINE_AUTO_FETCH_USER_CANCEL_ACTION_IN_PROGRESS,
                OFFLINE_INDICATOR_V2_ENABLED,
                PAYMENTS_PAYMENT_COMPLETE_ONCE,
                PREFETCH_HAS_NEW_PAGES,
                PREFETCH_IGNORED_NOTIFICATION_COUNTER,
                PREFETCH_NOTIFICATION_ENABLED,
                PREFETCH_NOTIFICATION_TIME,
                PREFETCH_OFFLINE_COUNTER,
                PRIVACY_ALLOW_PRERENDER_OLD,
                PRIVACY_BANDWIDTH_NO_CELLULAR_OLD,
                PRIVACY_BANDWIDTH_OLD,
                PRIVACY_METRICS_IN_SAMPLE,
                PRIVACY_METRICS_REPORTING,
                PRIVACY_NETWORK_PREDICTIONS,
                PROMOS_SKIPPED_ON_FIRST_START,
                REACHED_CODE_PROFILER_GROUP,
                RLZ_NOTIFIED,
                SEARCH_ENGINE_CHOICE_DEFAULT_TYPE_BEFORE,
                SEARCH_ENGINE_CHOICE_PRESENTED_VERSION,
                SEARCH_ENGINE_CHOICE_REQUESTED_TIMESTAMP,
                SETTINGS_DEVELOPER_ENABLED,
                SETTINGS_DEVELOPER_TRACING_CATEGORIES,
                SETTINGS_DEVELOPER_TRACING_MODE,
                SETTINGS_PRIVACY_OTHER_FORMS_OF_HISTORY_DIALOG_SHOWN,
                SETTINGS_SYNC_SIGN_OUT_ALLOWED,
                SETTINGS_WEBSITE_FAILED_BUILD_VERSION,
                SHARING_LAST_SHARED_CLASS_NAME,
                SHARING_LAST_SHARED_PACKAGE_NAME,
                SIGNIN_ACCOUNTS_CHANGED,
                SIGNIN_ACCOUNT_RENAMED,
                SIGNIN_ACCOUNT_RENAME_EVENT_INDEX,
                SIGNIN_AND_SYNC_PROMO_SHOW_COUNT,
                SIGNIN_PROMO_IMPRESSIONS_COUNT_BOOKMARKS,
                SIGNIN_PROMO_IMPRESSIONS_COUNT_SETTINGS,
                SIGNIN_PROMO_LAST_SHOWN_ACCOUNT_NAMES,
                SIGNIN_PROMO_LAST_SHOWN_MAJOR_VERSION,
                SIGNIN_PROMO_NTP_PROMO_DISMISSED,
                SIGNIN_PROMO_NTP_PROMO_SUPPRESSION_PERIOD_START,
                SIGNIN_PROMO_PERSONALIZED_DECLINED,
                SIGNIN_PROMO_SETTINGS_PERSONALIZED_DISMISSED,
                SNAPSHOT_DATABASE_REMOVED,
                START_SURFACE_SINGLE_PANE_ENABLED_KEY,
                SURVEY_DATE_LAST_ROLLED,
                SURVEY_INFO_BAR_DISPLAYED,
                SYNC_SESSIONS_UUID,
                TABBED_ACTIVITY_LAST_BACKGROUNDED_TIME_MS_PREF,
                TABMODEL_ACTIVE_TAB_ID,
                TABMODEL_HAS_COMPUTED_MAX_ID,
                TABMODEL_HAS_RUN_FILE_MIGRATION,
                TABMODEL_HAS_RUN_MULTI_INSTANCE_FILE_MIGRATION,
                TAB_ID_MANAGER_NEXT_ID,
                TOS_ACKED_ACCOUNTS,
                TWA_DIALOG_NUMBER_OF_DISMISSALS_ON_CLEAR_DATA,
                TWA_DIALOG_NUMBER_OF_DISMISSALS_ON_UNINSTALL,
                TWA_DISCLOSURE_ACCEPTED_PACKAGES,
                UI_THEME_DARKEN_WEBSITES_ENABLED,
                UI_THEME_SETTING,
                VARIATION_CACHED_BOTTOM_TOOLBAR,
                VERIFIED_DIGITAL_ASSET_LINKS,
                VR_EXIT_TO_2D_COUNT,
                VR_FEEDBACK_OPT_OUT,
                VR_SHOULD_REGISTER_ASSETS_COMPONENT_ON_STARTUP,
                WEBAPK_EXTRACTED_DEX_VERSION,
                WEBAPK_LAST_SDK_VERSION,
                WEBAPK_UNINSTALLED_PACKAGES
        );
        // clang-format on
    }

    @CheckDiscard("Validation is performed in tests and in debug builds.")
    static List<KeyPrefix> createGrandfatheredPrefixesInUse() {
        // clang-format off
        return Arrays.asList(
                CONTEXTUAL_SEARCH_CLICKS_WEEK_PREFIX,
                CONTEXTUAL_SEARCH_IMPRESSIONS_WEEK_PREFIX,
                CUSTOM_TABS_DEX_LAST_UPDATE_TIME_PREF_PREFIX,
                PAYMENTS_PAYMENT_INSTRUMENT_USE_COUNT,
                PAYMENTS_PAYMENT_INSTRUMENT_USE_DATE
        );
        // clang-format on
    }

    @CheckDiscard("Validation is performed in tests and in debug builds.")
    static List<KeyPrefix> createDeprecatedPrefixesForTesting() {
        return Collections.EMPTY_LIST;
    }

    private ChromePreferenceKeys() {}
}
