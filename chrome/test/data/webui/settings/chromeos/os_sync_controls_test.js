// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/** @implements {settings.OsSyncBrowserProxy} */
class TestOsSyncBrowserProxy extends TestBrowserProxy {
  constructor() {
    super([
      'didNavigateToOsSyncPage',
      'didNavigateAwayFromOsSyncPage',
      'setOsSyncFeatureEnabled',
      'setOsSyncDatatypes',
    ]);
  }

  /** @override */
  didNavigateToOsSyncPage() {
    this.methodCalled('didNavigateToOsSyncPage');
  }

  /** @override */
  didNavigateAwayFromOsSyncPage() {
    this.methodCalled('didNavigateAwayFromSyncPage');
  }

  /** @override */
  setOsSyncFeatureEnabled(enabled) {
    this.methodCalled('setOsSyncFeatureEnabled', enabled);
  }

  /** @override */
  setOsSyncDatatypes(osSyncPrefs) {
    this.methodCalled('setOsSyncDatatypes', osSyncPrefs);
  }
}

/**
 * Returns a sync prefs dictionary with either all or nothing syncing.
 * @param {boolean} syncAll
 * @return {!settings.OsSyncPrefs}
 */
function getOsSyncPrefs(syncAll) {
  return {
    osAppsRegistered: true,
    osAppsSynced: syncAll,
    osPreferencesRegistered: true,
    osPreferencesSynced: syncAll,
    syncAllOsTypes: syncAll,
    wallpaperEnabled: syncAll,
    wifiConfigurationsRegistered: true,
    wifiConfigurationsSynced: syncAll,
  };
}

function getSyncAllPrefs() {
  return getOsSyncPrefs(true);
}

function getSyncNothingPrefs() {
  return getOsSyncPrefs(false);
}

function setupWithFeatureEnabled() {
  cr.webUIListenerCallback(
      'os-sync-prefs-changed', /*featureEnabled=*/ true, getSyncAllPrefs());
  Polymer.dom.flush();
}

function setupWithFeatureDisabled() {
  cr.webUIListenerCallback(
      'os-sync-prefs-changed', /*featureEnabled=*/ false,
      getSyncNothingPrefs());
  Polymer.dom.flush();
}

suite('OsSyncControlsTest', function() {
  let syncControls = null;
  let browserProxy = null;

  setup(function() {
    browserProxy = new TestOsSyncBrowserProxy();
    settings.OsSyncBrowserProxyImpl.instance_ = browserProxy;

    PolymerTest.clearBody();
    syncControls = document.createElement('os-sync-controls');
    document.body.appendChild(syncControls);
  });

  teardown(function() {
    syncControls.remove();
  });

  test('ControlsHiddenUntilInitialUpdateSent', function() {
    assertTrue(syncControls.hidden);
    setupWithFeatureEnabled();
    assertFalse(syncControls.hidden);
  });

  test('FeatureDisabled', function() {
    setupWithFeatureDisabled();

    assertFalse(syncControls.$.turnOnSyncButton.hidden);
    assertTrue(syncControls.$.turnOffSyncButton.hidden);

    assertTrue(syncControls.$.syncEverythingCheckboxLabel.hasAttribute(
        'label-disabled'));

    const syncAllControl = syncControls.$.syncAllOsTypesControl;
    assertTrue(syncAllControl.disabled);
    assertFalse(syncAllControl.checked);

    const labels = syncControls.shadowRoot.querySelectorAll(
        '.list-item:not([hidden]) > div');
    for (const label of labels) {
      assertTrue(label.hasAttribute('label-disabled'));
    }

    const datatypeControls = syncControls.shadowRoot.querySelectorAll(
        '.list-item:not([hidden]) > cr-toggle');
    for (const control of datatypeControls) {
      assertTrue(control.disabled);
      assertFalse(control.checked);
    }
  });

  test('FeatureEnabled', function() {
    setupWithFeatureEnabled();

    assertTrue(syncControls.$.turnOnSyncButton.hidden);
    assertFalse(syncControls.$.turnOffSyncButton.hidden);

    assertFalse(syncControls.$.syncEverythingCheckboxLabel.hasAttribute(
        'label-disabled'));

    const syncAllControl = syncControls.$.syncAllOsTypesControl;
    assertFalse(syncAllControl.disabled);
    assertTrue(syncAllControl.checked);

    const labels = syncControls.shadowRoot.querySelectorAll(
        '.list-item:not([hidden]) > div.checkbox-label');
    for (const label of labels) {
      assertFalse(label.hasAttribute('label-disabled'));
    }

    const datatypeControls = syncControls.shadowRoot.querySelectorAll(
        '.list-item:not([hidden]) > cr-toggle');
    for (const control of datatypeControls) {
      assertTrue(control.disabled);
      assertTrue(control.checked);
    }
  });

  test('ClickingTurnOffDisablesFeature', async function() {
    setupWithFeatureEnabled();
    syncControls.$.turnOffSyncButton.click();
    const enabled = await browserProxy.whenCalled('setOsSyncFeatureEnabled');
    assertFalse(enabled);
  });

  test('ClickingTurnOnEnablesFeature', async function() {
    setupWithFeatureDisabled();
    syncControls.$.turnOnSyncButton.click();
    enabled = await browserProxy.whenCalled('setOsSyncFeatureEnabled');
    assertTrue(enabled);
  });

  test('UncheckingSyncAllEnablesAllIndividualControls', async function() {
    setupWithFeatureEnabled();
    syncControls.$.syncAllOsTypesControl.click();
    const prefs = await browserProxy.whenCalled('setOsSyncDatatypes');

    const expectedPrefs = getSyncAllPrefs();
    expectedPrefs.syncAllOsTypes = false;
    assertEquals(JSON.stringify(expectedPrefs), JSON.stringify(prefs));
  });

  test('PrefChangeUpdatesControls', function() {
    const prefs = getSyncAllPrefs();
    prefs.syncAllOsTypes = false;
    cr.webUIListenerCallback(
        'os-sync-prefs-changed', /*featureEnabled=*/ true, prefs);

    const datatypeControls = syncControls.shadowRoot.querySelectorAll(
        '.list-item:not([hidden]) > cr-toggle');
    for (const control of datatypeControls) {
      assertFalse(control.disabled);
      assertTrue(control.checked);
    }
  });

  test('DisablingOneControlUpdatesPrefs', async function() {
    setupWithFeatureEnabled();

    // Disable "Sync All".
    syncControls.$.syncAllOsTypesControl.click();
    // Disable "Settings".
    syncControls.$.osPreferencesControl.click();
    const prefs = await browserProxy.whenCalled('setOsSyncDatatypes');

    const expectedPrefs = getSyncAllPrefs();
    expectedPrefs.syncAllOsTypes = false;
    expectedPrefs.osPreferencesSynced = false;
    assertEquals(JSON.stringify(expectedPrefs), JSON.stringify(prefs));
  });
});

suite('OsSyncControlsNavigationTest', function() {
  test('DidNavigateEvents', async function() {
    const browserProxy = new TestOsSyncBrowserProxy();
    settings.OsSyncBrowserProxyImpl.instance_ = browserProxy;

    settings.Router.getInstance().navigateTo(settings.routes.OS_SYNC);
    await browserProxy.methodCalled('didNavigateToOsSyncPage');

    settings.Router.getInstance().navigateTo(settings.routes.PEOPLE);
    await browserProxy.methodCalled('didNavigateAwayFromOsSyncPage');
  });
});
