// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * @fileoverview
 * 'settings-ui' implements the UI for the Settings page.
 *
 * Example:
 *
 *    <settings-ui prefs="{{prefs}}"></settings-ui>
 */
cr.define('settings', function() {
  /** Global defined when the main Settings script runs. */
  let defaultResourceLoaded = true;  // eslint-disable-line prefer-const

  assert(
      !window.settings || !settings.defaultResourceLoaded,
      'settings_ui.js run twice. You probably have an invalid import.');

  return {defaultResourceLoaded};
});

Polymer({
  is: 'os-settings-ui',

  behaviors: [
    CrContainerShadowBehavior,
    FindShortcutBehavior,
    // Calls currentRouteChanged() in attached(), so ensure other behaviors run
    // their attached() first.
    settings.RouteObserverBehavior,
  ],

  properties: {
    /**
     * Preferences state.
     */
    prefs: Object,

    /** @private */
    advancedOpenedInMain_: {
      type: Boolean,
      value: false,
      notify: true,
      observer: 'onAdvancedOpenedInMainChanged_',
    },

    /** @private */
    advancedOpenedInMenu_: {
      type: Boolean,
      value: false,
      notify: true,
      observer: 'onAdvancedOpenedInMenuChanged_',
    },

    /** @private {boolean} */
    toolbarSpinnerActive_: {
      type: Boolean,
      value: false,
    },

    /**
     * Whether settings is in the narrow state (side nav hidden). Controlled by
     * a binding in the os-toolbar element.
     */
    isNarrow: {
      type: Boolean,
      observer: 'onNarrowChanged_',
    },

    /**
     * @private {!OSPageVisibility}
     */
    pageVisibility_: {type: Object, value: settings.osPageVisibility},

    /** @private */
    havePlayStoreApp_: Boolean,

    /** @private */
    showAndroidApps_: Boolean,

    /** @private */
    showAppManagement_: Boolean,

    /** @private */
    showApps_: Boolean,

    /** @private */
    showCrostini_: Boolean,

    /** @private */
    showPluginVm_: Boolean,

    /** @private */
    showReset_: Boolean,

    /** @private */
    lastSearchQuery_: {
      type: String,
      value: '',
    },
  },

  listeners: {
    'refresh-pref': 'onRefreshPref_',
  },

  /**
   * The route of the selected element in os-settings-menu. Stored here to defer
   * navigation until drawer animation completes.
   * @private {settings.Route}
   */
  activeRoute_: null,

  /** @override */
  created() {
    settings.Router.getInstance().initializeRouteFromUrl();
  },

  /**
   * @override
   * @suppress {es5Strict} Object literals cannot contain duplicate keys in ES5
   *     strict mode.
   */
  ready() {
    // Lazy-create the drawer the first time it is opened or swiped into view.
    listenOnce(this.$.drawer, 'cr-drawer-opening', () => {
      this.$.drawerTemplate.if = true;
    });

    window.addEventListener('popstate', e => {
      this.$.drawer.cancel();
    });

    CrPolicyStrings = {
      controlledSettingExtension:
          loadTimeData.getString('controlledSettingExtension'),
      controlledSettingExtensionWithoutName:
          loadTimeData.getString('controlledSettingExtensionWithoutName'),
      controlledSettingPolicy:
          loadTimeData.getString('controlledSettingPolicy'),
      controlledSettingRecommendedMatches:
          loadTimeData.getString('controlledSettingRecommendedMatches'),
      controlledSettingRecommendedDiffers:
          loadTimeData.getString('controlledSettingRecommendedDiffers'),
      controlledSettingShared:
          loadTimeData.getString('controlledSettingShared'),
      controlledSettingWithOwner:
          loadTimeData.getString('controlledSettingWithOwner'),
      controlledSettingNoOwner:
          loadTimeData.getString('controlledSettingNoOwner'),
      controlledSettingParent:
          loadTimeData.getString('controlledSettingParent'),
      controlledSettingChildRestriction:
          loadTimeData.getString('controlledSettingChildRestriction'),
    };

    this.havePlayStoreApp_ = loadTimeData.getBoolean('havePlayStoreApp');
    this.showAppManagement_ = loadTimeData.getBoolean('showAppManagement');
    this.showAndroidApps_ = loadTimeData.getBoolean('androidAppsVisible');
    this.showApps_ = this.showAppManagement_ || this.showAndroidApps_;
    this.showCrostini_ = loadTimeData.getBoolean('showCrostini');
    this.showPluginVm_ = loadTimeData.getBoolean('showPluginVm');
    this.showReset_ = loadTimeData.getBoolean('allowPowerwash');

    this.addEventListener('show-container', () => {
      this.$.container.style.visibility = 'visible';
    });

    this.addEventListener('hide-container', () => {
      this.$.container.style.visibility = 'hidden';
    });
  },

  /** @override */
  attached() {
    document.documentElement.classList.remove('loading');

    setTimeout(function() {
      chrome.send(
          'metricsHandler:recordTime',
          ['Settings.TimeUntilInteractive', window.performance.now()]);
    });

    // Preload bold Roboto so it doesn't load and flicker the first time used.
    document.fonts.load('bold 12px Roboto');
    settings.setGlobalScrollTarget(this.$.container);

    const scrollToTop = top => new Promise(resolve => {
      if (this.$.container.scrollTop === top) {
        resolve();
        return;
      }

      this.$.container.scrollTo({top: top, behavior: 'auto'});
      const onScroll = () => {
        this.debounce('scrollEnd', () => {
          this.$.container.removeEventListener('scroll', onScroll);
          resolve();
        }, 75);
      };
      this.$.container.addEventListener('scroll', onScroll);
    });
    this.addEventListener('scroll-to-top', e => {
      scrollToTop(e.detail.top).then(e.detail.callback);
    });
    this.addEventListener('scroll-to-bottom', e => {
      scrollToTop(e.detail.bottom - this.$.container.clientHeight)
          .then(e.detail.callback);
    });
  },

  /** @override */
  detached() {
    settings.Router.getInstance().resetRouteForTesting();
  },

  /** @param {!settings.Route} route */
  currentRouteChanged(route) {
    if (route.depth <= 1) {
      // Main page uses scroll visibility to determine shadow.
      this.enableShadowBehavior(true);
    } else {
      // Sub-pages always show the top-container shadow.
      this.enableShadowBehavior(false);
      this.showDropShadows();
    }

    const urlSearchQuery =
        settings.Router.getInstance().getQueryParameters().get('search') || '';
    if (urlSearchQuery == this.lastSearchQuery_) {
      return;
    }

    this.lastSearchQuery_ = urlSearchQuery;

    const toolbar = /** @type {!OsToolbarElement} */ (this.$$('os-toolbar'));
    const searchField =
        /** @type {CrToolbarSearchFieldElement} */ (toolbar.getSearchField());

    // If the search was initiated by directly entering a search URL, need to
    // sync the URL parameter to the textbox.
    if (urlSearchQuery != searchField.getValue()) {
      // Setting the search box value without triggering a 'search-changed'
      // event, to prevent an unnecessary duplicate entry in |window.history|.
      searchField.setValue(urlSearchQuery, true /* noEvent */);
    }

    this.$.main.searchContents(urlSearchQuery);
  },

  // Override FindShortcutBehavior methods.
  handleFindShortcut(modalContextOpen) {
    if (modalContextOpen) {
      return false;
    }
    this.$$('os-toolbar').getSearchField().showAndFocus();
    return true;
  },

  // Override FindShortcutBehavior methods.
  searchInputHasFocus() {
    return this.$$('os-toolbar').getSearchField().isSearchFocused();
  },

  /**
   * @param {!CustomEvent<string>} e
   * @private
   */
  onRefreshPref_(e) {
    return /** @type {SettingsPrefsElement} */ (this.$.prefs).refresh(e.detail);
  },

  /**
   * Handles the 'search-changed' event fired from the toolbar.
   * @param {!Event} e
   * @private
   */
  onSearchChanged_(e) {
    const query = e.detail;
    settings.Router.getInstance().navigateTo(
        settings.routes.BASIC,
        query.length > 0 ?
            new URLSearchParams('search=' + encodeURIComponent(query)) :
            undefined,
        /* removeSearch */ true);
  },

  /**
   * Called when a section is selected.
   * @param {!Event} e
   * @private
   */
  onIronActivate_(e) {
    const section = e.detail.selected;
    const path = new URL(section).pathname;
    const route = settings.Router.getInstance().getRouteForPath(path);
    assert(route, 'os-settings-menu has an entry with an invalid route.');
    this.activeRoute_ = route;

    if (this.isNarrow) {
      // If the onIronActivate event came from the drawer, close the drawer and
      // wait for the menu to close before navigating to |activeRoute_|.
      this.$.drawer.close();
      return;
    }
    this.navigateToActiveRoute_();
  },

  /** @private */
  onMenuButtonTap_() {
    this.$.drawer.toggle();
  },


  /**
   * Navigates to |activeRoute_| if set. Used to delay navigation until after
   * animations complete to ensure focus ends up in the right place.
   * @private
   */
  navigateToActiveRoute_() {
    if (this.activeRoute_) {
      settings.Router.getInstance().navigateTo(
          this.activeRoute_, /* dynamicParams */ null, /* removeSearch */ true);
      this.activeRoute_ = null;
    }
  },

  /**
   * When this is called, The drawer animation is finished, and the dialog no
   * longer has focus. The selected section will gain focus if one was selected.
   * Otherwise, the drawer was closed due being canceled, and the main settings
   * container is given focus. That way the arrow keys can be used to scroll
   * the container, and pressing tab focuses a component in settings.
   * @private
   */
  onMenuClose_() {
    if (!this.$.drawer.wasCanceled()) {
      // If a navigation happened, MainPageBehavior#currentRouteChanged handles
      // focusing the corresponding section when we call settings.NavigateTo().
      this.navigateToActiveRoute_();
      return;
    }

    // Add tab index so that the container can be focused.
    this.$.container.setAttribute('tabindex', '-1');
    this.$.container.focus();

    listenOnce(this.$.container, ['blur', 'pointerdown'], () => {
      this.$.container.removeAttribute('tabindex');
    });
  },

  /** @private */
  onAdvancedOpenedInMainChanged_() {
    // Only sync value when opening, not closing.
    if (this.advancedOpenedInMain_) {
      this.advancedOpenedInMenu_ = true;
    }
  },

  /** @private */
  onAdvancedOpenedInMenuChanged_() {
    // Only sync value when opening, not closing.
    if (this.advancedOpenedInMenu_) {
      this.advancedOpenedInMain_ = true;
    }
  },

  /** @private */
  onNarrowChanged_() {
    if (this.$.drawer.open && !this.isNarrow) {
      this.$.drawer.close();
    }
  },
});
