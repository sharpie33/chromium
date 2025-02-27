// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

cr.define('settings_reset_page', function() {
  /** @enum {string} */
  const TestNames = {
    ResetProfileDialogAction: 'ResetProfileDialogAction',
    ResetProfileDialogOpenClose: 'ResetProfileDialogOpenClose',
    ResetProfileDialogOriginUnknown: 'ResetProfileDialogOriginUnknown',
    ResetProfileDialogOriginUserClick: 'ResetProfileDialogOriginUserClick',
    ResetProfileDialogOriginTriggeredReset:
        'ResetProfileDialogOriginTriggeredReset',
  };

  suite('DialogTests', function() {
    let resetPage = null;

    /** @type {!settings.ResetPageBrowserProxy} */
    let resetPageBrowserProxy = null;

    setup(function() {
      resetPageBrowserProxy = new reset_page.TestResetBrowserProxy();
      settings.ResetBrowserProxyImpl.instance_ = resetPageBrowserProxy;

      PolymerTest.clearBody();
      resetPage = document.createElement('settings-reset-page');
      document.body.appendChild(resetPage);
    });

    teardown(function() {
      resetPage.remove();
    });

    /**
     * @param {function(SettingsResetProfileDialogElement)}
     *     closeDialogFn A function to call for closing the dialog.
     * @return {!Promise}
     */
    function testOpenCloseResetProfileDialog(closeDialogFn) {
      resetPageBrowserProxy.resetResolver('onShowResetProfileDialog');
      resetPageBrowserProxy.resetResolver('onHideResetProfileDialog');

      // Open reset profile dialog.
      resetPage.$.resetProfile.click();
      Polymer.dom.flush();
      const dialog = resetPage.$$('settings-reset-profile-dialog');
      assertTrue(!!dialog);
      assertTrue(dialog.$.dialog.open);

      const whenDialogClosed = test_util.eventToPromise('close', dialog);

      return resetPageBrowserProxy.whenCalled('onShowResetProfileDialog')
          .then(function() {
            closeDialogFn(dialog);
            return Promise.all([
              whenDialogClosed,
              resetPageBrowserProxy.whenCalled('onHideResetProfileDialog'),
            ]);
          });
    }

    // Tests that the reset profile dialog opens and closes correctly and that
    // resetPageBrowserProxy calls are occurring as expected.
    test(TestNames.ResetProfileDialogOpenClose, function() {
      return testOpenCloseResetProfileDialog(function(dialog) {
               // Test case where the 'cancel' button is clicked.
               dialog.$.cancel.click();
             })
          .then(function() {
            return testOpenCloseResetProfileDialog(function(dialog) {
              // Test case where the browser's 'back' button is clicked.
              resetPage.currentRouteChanged(settings.routes.BASIC);
            });
          });
    });

    // Tests that when user request to reset the profile the appropriate
    // message is sent to the browser.
    test(TestNames.ResetProfileDialogAction, function() {
      // Open reset profile dialog.
      resetPage.$.resetProfile.click();
      Polymer.dom.flush();
      const dialog = resetPage.$$('settings-reset-profile-dialog');
      assertTrue(!!dialog);

      const checkbox = dialog.$$('[slot=footer] cr-checkbox');
      assertTrue(checkbox.checked);
      const showReportedSettingsLink = dialog.$$('[slot=footer] a');
      assertTrue(!!showReportedSettingsLink);
      showReportedSettingsLink.click();

      return resetPageBrowserProxy.whenCalled('showReportedSettings')
          .then(function() {
            // Ensure that the checkbox was not toggled as a result of
            // clicking the link.
            assertTrue(checkbox.checked);
            assertFalse(dialog.$.reset.disabled);
            assertFalse(dialog.$.resetSpinner.active);
            dialog.$.reset.click();
            assertTrue(dialog.$.reset.disabled);
            assertTrue(dialog.$.cancel.disabled);
            assertTrue(dialog.$.resetSpinner.active);
            return resetPageBrowserProxy.whenCalled(
                'performResetProfileSettings');
          });
    });

    function testResetRequestOrigin(expectedOrigin) {
      const dialog = resetPage.$$('settings-reset-profile-dialog');
      assertTrue(!!dialog);
      dialog.$.reset.click();
      return resetPageBrowserProxy.whenCalled('performResetProfileSettings')
          .then(function(resetRequest) {
            assertEquals(expectedOrigin, resetRequest);
          });
    }

    test(TestNames.ResetProfileDialogOriginUnknown, function() {
      settings.Router.getInstance().navigateTo(settings.routes.RESET_DIALOG);
      return resetPageBrowserProxy.whenCalled('onShowResetProfileDialog')
          .then(function() {
            return testResetRequestOrigin('');
          });
    });

    test(TestNames.ResetProfileDialogOriginUserClick, function() {
      resetPage.$.resetProfile.click();
      return resetPageBrowserProxy.whenCalled('onShowResetProfileDialog')
          .then(function() {
            return testResetRequestOrigin('userclick');
          });
    });

    test(TestNames.ResetProfileDialogOriginTriggeredReset, function() {
      settings.Router.getInstance().navigateTo(
          settings.routes.TRIGGERED_RESET_DIALOG);
      return resetPageBrowserProxy.whenCalled('onShowResetProfileDialog')
          .then(function() {
            return testResetRequestOrigin('triggeredreset');
          });
    });
  });
});
