// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WEBLAYER_BROWSER_SSL_ERROR_CONTROLLER_CLIENT_H_
#define WEBLAYER_BROWSER_SSL_ERROR_CONTROLLER_CLIENT_H_

#include "components/security_interstitials/content/security_interstitial_controller_client.h"
#include "net/ssl/ssl_info.h"
#include "url/gurl.h"

namespace content {
class WebContents;
}

namespace security_interstitials {
class MetricsHelper;
}

namespace weblayer {

// A stripped-down version of the class by the same name in
// //chrome/browser/ssl, which provides basic functionality for interacting with
// the SSL interstitial.
class SSLErrorControllerClient
    : public security_interstitials::SecurityInterstitialControllerClient {
 public:
  SSLErrorControllerClient(
      content::WebContents* web_contents,
      int cert_error,
      const net::SSLInfo& ssl_info,
      const GURL& request_url,
      std::unique_ptr<security_interstitials::MetricsHelper> metrics_helper);

  ~SSLErrorControllerClient() override = default;

  // security_interstitials::SecurityInterstitialControllerClient:
  void GoBack() override;
  void Proceed() override;
  void OpenUrlInNewForegroundTab(const GURL& url) override;
  bool CanLaunchDateAndTimeSettings() override;

 private:
  const int cert_error_;
  const net::SSLInfo ssl_info_;
  const GURL request_url_;

  DISALLOW_COPY_AND_ASSIGN(SSLErrorControllerClient);
};

}  // namespace weblayer

#endif  // WEBLAYER_BROWSER_SSL_ERROR_CONTROLLER_CLIENT_H_
