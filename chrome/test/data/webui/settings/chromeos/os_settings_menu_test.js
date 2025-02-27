// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/** @fileoverview Runs tests for the OS settings menu. */

function setupRouter() {
  const routes = {
    BASIC: new settings.Route('/'),
    ADVANCED: new settings.Route('/advanced'),
  };
  routes.BLUETOOTH = routes.BASIC.createSection('/bluetooth', 'bluetooth');
  routes.RESET = routes.ADVANCED.createSection('/reset', 'reset');

  settings.Router.resetInstanceForTesting(new settings.Router(routes));
  settings.routes = routes;
}

suite('OSSettingsMenu', function() {
  let settingsMenu = null;

  setup(function() {
    setupRouter();
    PolymerTest.clearBody();
    settingsMenu = document.createElement('os-settings-menu');
    settingsMenu.pageVisibility = settings.pageVisibility;
    document.body.appendChild(settingsMenu);
  });

  teardown(function() {
    settingsMenu.remove();
  });

  test('advancedOpenedBinding', function() {
    assertFalse(settingsMenu.advancedOpened);
    settingsMenu.advancedOpened = true;
    Polymer.dom.flush();
    assertTrue(settingsMenu.isAdvancedSubmenuOpenedForTest());

    settingsMenu.advancedOpened = false;
    Polymer.dom.flush();
    assertFalse(settingsMenu.isAdvancedSubmenuOpenedForTest());
  });

  test('tapAdvanced', function() {
    assertFalse(settingsMenu.advancedOpened);

    const advancedToggle = settingsMenu.$$('#advancedButton');
    assertTrue(!!advancedToggle);

    advancedToggle.click();
    Polymer.dom.flush();
    assertTrue(settingsMenu.isAdvancedSubmenuOpenedForTest());

    advancedToggle.click();
    Polymer.dom.flush();
    assertFalse(settingsMenu.isAdvancedSubmenuOpenedForTest());
  });

  test('upAndDownIcons', function() {
    // There should be different icons for a top level menu being open
    // vs. being closed. E.g. arrow-drop-up and arrow-drop-down.
    const ironIconElement = settingsMenu.$$('#advancedButton iron-icon');
    assertTrue(!!ironIconElement);

    settingsMenu.advancedOpened = true;
    Polymer.dom.flush();
    const openIcon = ironIconElement.icon;
    assertTrue(!!openIcon);

    settingsMenu.advancedOpened = false;
    Polymer.dom.flush();
    assertNotEquals(openIcon, ironIconElement.icon);
  });
});

suite('OSSettingsMenuReset', function() {
  setup(function() {
    setupRouter();
    PolymerTest.clearBody();
    settings.Router.getInstance().navigateTo(settings.routes.RESET, '');
    settingsMenu = document.createElement('os-settings-menu');
    document.body.appendChild(settingsMenu);
  });

  teardown(function() {
    settingsMenu.remove();
  });

  test('openResetSection', function() {
    const selector = settingsMenu.$.subMenu;
    const path = new window.URL(selector.selected).pathname;
    assertEquals('/reset', path);
  });

  test('navigateToAnotherSection', function() {
    const selector = settingsMenu.$.subMenu;
    let path = new window.URL(selector.selected).pathname;
    assertEquals('/reset', path);

    settings.Router.getInstance().navigateTo(settings.routes.BLUETOOTH, '');
    Polymer.dom.flush();

    path = new window.URL(selector.selected).pathname;
    assertEquals('/bluetooth', path);
  });

  test('navigateToBasic', function() {
    const selector = settingsMenu.$.subMenu;
    const path = new window.URL(selector.selected).pathname;
    assertEquals('/reset', path);

    settings.Router.getInstance().navigateTo(settings.routes.BASIC, '');
    Polymer.dom.flush();

    // BASIC has no sub page selected.
    assertFalse(!!selector.selected);
  });
});
