// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_RENDERER_SUBRESOURCE_REDIRECT_SUBRESOURCE_REDIRECT_URL_LOADER_THROTTLE_H_
#define CHROME_RENDERER_SUBRESOURCE_REDIRECT_SUBRESOURCE_REDIRECT_URL_LOADER_THROTTLE_H_

#include "base/macros.h"
#include "third_party/blink/public/common/loader/url_loader_throttle.h"
#include "third_party/blink/public/mojom/loader/resource_load_info.mojom-shared.h"

namespace blink {
class WebURLRequest;
}  // namespace blink

namespace subresource_redirect {

class SubresourceRedirectHintsAgent;

// This class handles internal redirects for subresouces on HTTPS sites to
// compressed versions of subresources.
class SubresourceRedirectURLLoaderThrottle : public blink::URLLoaderThrottle {
 public:
  static std::unique_ptr<SubresourceRedirectURLLoaderThrottle>
  MaybeCreateThrottle(const blink::WebURLRequest& request,
                      blink::mojom::ResourceType resource_type,
                      int render_frame_id);

  ~SubresourceRedirectURLLoaderThrottle() override;

  // virtual for testing.
  virtual SubresourceRedirectHintsAgent* GetSubresourceRedirectHintsAgent();

  // blink::URLLoaderThrottle:
  void WillStartRequest(network::ResourceRequest* request,
                        bool* defer) override;
  void WillRedirectRequest(
      net::RedirectInfo* redirect_info,
      const network::mojom::URLResponseHead& response_head,
      bool* defer,
      std::vector<std::string>* to_be_removed_request_headers,
      net::HttpRequestHeaders* modified_request_headers) override;
  void BeforeWillProcessResponse(
      const GURL& response_url,
      const network::mojom::URLResponseHead& response_head,
      bool* defer) override;
  void WillProcessResponse(const GURL& response_url,
                           network::mojom::URLResponseHead* response_head,
                           bool* defer) override;
  void WillOnCompleteWithError(const network::URLLoaderCompletionStatus& status,
                               bool* defer) override;
  // Overridden to do nothing as the default implementation is NOT_REACHED()
  void DetachFromCurrentSequence() override;

 private:
  friend class TestSubresourceRedirectURLLoaderThrottle;

  explicit SubresourceRedirectURLLoaderThrottle(int render_frame_id);

  // Render frame id to get the hints agent of the render frame.
  const int render_frame_id_;

  DISALLOW_COPY_AND_ASSIGN(SubresourceRedirectURLLoaderThrottle);
};

}  // namespace subresource_redirect

#endif  // CHROME_RENDERER_SUBRESOURCE_REDIRECT_SUBRESOURCE_REDIRECT_URL_LOADER_THROTTLE_H_
