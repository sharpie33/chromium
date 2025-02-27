// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This is the shared code for security interstitials. It is used for both SSL
// interstitials and Safe Browsing interstitials.

/**
 * @typedef {{
 *   dontProceed: function(),
 *   proceed: function(),
 *   showMoreSection: function(),
 *   openHelpCenter: function(),
 *   openDiagnostic: function(),
 *   reload: function(),
 *   openDateSettings: function(),
 *   openLogin: function(),
 *   doReport: function(),
 *   dontReport: function(),
 *   openReportingPrivacy: function(),
 *   openWhitepaper: function(),
 *   reportPhishingError: function(),
 * }}
 */
// eslint-disable-next-line no-var
var certificateErrorPageController;

// Should match security_interstitials::SecurityInterstitialCommand
/** @enum {number} */
const SecurityInterstitialCommandId = {
  CMD_DONT_PROCEED: 0,
  CMD_PROCEED: 1,
  // Ways for user to get more information
  CMD_SHOW_MORE_SECTION: 2,
  CMD_OPEN_HELP_CENTER: 3,
  CMD_OPEN_DIAGNOSTIC: 4,
  // Primary button actions
  CMD_RELOAD: 5,
  CMD_OPEN_DATE_SETTINGS: 6,
  CMD_OPEN_LOGIN: 7,
  // Safe Browsing Extended Reporting
  CMD_DO_REPORT: 8,
  CMD_DONT_REPORT: 9,
  CMD_OPEN_REPORTING_PRIVACY: 10,
  CMD_OPEN_WHITEPAPER: 11,
  // Report a phishing error.
  CMD_REPORT_PHISHING_ERROR: 12
};

const HIDDEN_CLASS = 'hidden';

/**
 * A convenience method for sending commands to the parent page.
 * @param {SecurityInterstitialCommandId} cmd  The command to send.
 */
function sendCommand(cmd) {
  if (window.certificateErrorPageController) {
    switch (cmd) {
      case SecurityInterstitialCommandId.CMD_DONT_PROCEED:
        certificateErrorPageController.dontProceed();
        break;
      case SecurityInterstitialCommandId.CMD_PROCEED:
        certificateErrorPageController.proceed();
        break;
      case SecurityInterstitialCommandId.CMD_SHOW_MORE_SECTION:
        certificateErrorPageController.showMoreSection();
        break;
      case SecurityInterstitialCommandId.CMD_OPEN_HELP_CENTER:
        certificateErrorPageController.openHelpCenter();
        break;
      case SecurityInterstitialCommandId.CMD_OPEN_DIAGNOSTIC:
        certificateErrorPageController.openDiagnostic();
        break;
      case SecurityInterstitialCommandId.CMD_RELOAD:
        certificateErrorPageController.reload();
        break;
      case SecurityInterstitialCommandId.CMD_OPEN_DATE_SETTINGS:
        certificateErrorPageController.openDateSettings();
        break;
      case SecurityInterstitialCommandId.CMD_OPEN_LOGIN:
        certificateErrorPageController.openLogin();
        break;
      case SecurityInterstitialCommandId.CMD_DO_REPORT:
        certificateErrorPageController.doReport();
        break;
      case SecurityInterstitialCommandId.CMD_DONT_REPORT:
        certificateErrorPageController.dontReport();
        break;
      case SecurityInterstitialCommandId.CMD_OPEN_REPORTING_PRIVACY:
        certificateErrorPageController.openReportingPrivacy();
        break;
      case SecurityInterstitialCommandId.CMD_OPEN_WHITEPAPER:
        certificateErrorPageController.openWhitepaper();
        break;
      case SecurityInterstitialCommandId.CMD_REPORT_PHISHING_ERROR:
        certificateErrorPageController.reportPhishingError();
        break;
    }
    return;
  }
  // <if expr="not is_ios">
  window.domAutomationController.send(cmd);
  // </if>
  // <if expr="is_ios">
  // TODO(crbug.com/987407): Used to send commands for non-committed
  // interstitials on iOS. Should be deleted after committed interstitials are
  // fully launched.
  if (!loadTimeData.getBoolean('committed_interstitials_enabled')) {
    const iframe = document.createElement('IFRAME');
    iframe.setAttribute('src', 'js-command:' + cmd);
    document.documentElement.appendChild(iframe);
    iframe.parentNode.removeChild(iframe);
  } else {
    // Used to send commands for iOS committed interstitials.
    /** @suppress {undefinedVars|missingProperties} */ (function() {
      __gCrWeb.message.invokeOnHost({'command': 'blockingPage.' + cmd});
    })();
  }
  // </if>
}

/**
 * Call this to stop clicks on <a href="#"> links from scrolling to the top of
 * the page (and possibly showing a # in the link).
 */
function preventDefaultOnPoundLinkClicks() {
  document.addEventListener('click', function(e) {
    const anchor = findAncestor(/** @type {Node} */ (e.target), function(el) {
      return el.tagName === 'A';
    });
    // Use getAttribute() to prevent URL normalization.
    if (anchor && anchor.getAttribute('href') === '#') {
      e.preventDefault();
    }
  });
}
