// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IOS_WEB_COMMON_FEATURES_H_
#define IOS_WEB_COMMON_FEATURES_H_

#include "base/feature_list.h"

namespace web {
namespace features {

// Used to always allow scaling of the web page, regardless of author intent.
extern const base::Feature kIgnoresViewportScaleLimits;

// Used to crash the browser if unexpected URL change is detected.
// https://crbug.com/841105.
extern const base::Feature kCrashOnUnexpectedURLChange;

// Used to enable the workaround for WKWebView history clobber bug
// (crbug.com/887497).
extern const base::Feature kHistoryClobberWorkaround;

// Used to prevent native apps from being opened when a universal link is tapped
// and the user is browsing in off the record mode.
extern const base::Feature kBlockUniversalLinksInOffTheRecordMode;

// Used to ensure that the render is not suspended.
extern const base::Feature kKeepsRenderProcessAlive;

// Used to enable the workaround for a WKWebView WKNavigation leak.
// (crbug.com/1010765).  Clear older pending navigation records when a
// navigation finishes.
extern const base::Feature kClearOldNavigationRecordsWorkaround;

// Used to enable committed interstitials for SSL errors.
extern const base::Feature kSSLCommittedInterstitials;

// Used to enable using WKWebView.loading for WebState::IsLoading.
extern const base::Feature kUseWKWebViewLoading;

// Feature flag enabling persistent downloads.
extern const base::Feature kEnablePersistentDownloads;

// Feature flag for the new error page workflow, using JavaScript.
extern const base::Feature kUseJSForErrorPage;

// When enabled, for each navigation, the default user agent is chosen by the
// WebClient GetDefaultUserAgent() method. If it is disabled, the mobile version
// is requested by default.
extern const base::Feature kUseDefaultUserAgentInWebClient;

// When enabled, preserves properties of the UIScrollView using CRWPropertyStore
// when the scroll view is recreated. When disabled, only preserve a small set
// of properties using hard coded logic.
extern const base::Feature kPreserveScrollViewProperties;

// Use WKWebView.loading to update WebState::IsLoading.
// TODO(crbug.com/1006012): Clean up this flag after experiment.
bool UseWKWebViewLoading();

// Level at which battery power is considered low, and some cosmetic features
// can be turned off.
const float kLowBatteryLevelThreshold = 0.2;

}  // namespace features
}  // namespace web

#endif  // IOS_WEB_COMMON_FEATURES_H_
