// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "weblayer/renderer/url_loader_throttle_provider.h"

#include <memory>

#include "components/safe_browsing/content/renderer/renderer_url_loader_throttle.h"
#include "content/public/renderer/render_thread.h"
#include "third_party/blink/public/common/loader/resource_type_util.h"

namespace weblayer {

URLLoaderThrottleProvider::URLLoaderThrottleProvider(
    blink::ThreadSafeBrowserInterfaceBrokerProxy* broker,
    content::URLLoaderThrottleProviderType type)
    : type_(type) {
  DETACH_FROM_THREAD(thread_checker_);
  broker->GetInterface(safe_browsing_remote_.InitWithNewPipeAndPassReceiver());
}

URLLoaderThrottleProvider::URLLoaderThrottleProvider(
    const URLLoaderThrottleProvider& other)
    : type_(other.type_) {
  DETACH_FROM_THREAD(thread_checker_);
  if (other.safe_browsing_) {
    other.safe_browsing_->Clone(
        safe_browsing_remote_.InitWithNewPipeAndPassReceiver());
  }
}

std::unique_ptr<content::URLLoaderThrottleProvider>
URLLoaderThrottleProvider::Clone() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  if (safe_browsing_remote_)
    safe_browsing_.Bind(std::move(safe_browsing_remote_));
  return base::WrapUnique(new URLLoaderThrottleProvider(*this));
}

URLLoaderThrottleProvider::~URLLoaderThrottleProvider() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
}

std::vector<std::unique_ptr<blink::URLLoaderThrottle>>
URLLoaderThrottleProvider::CreateThrottles(
    int render_frame_id,
    const blink::WebURLRequest& request,
    blink::mojom::ResourceType resource_type) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  std::vector<std::unique_ptr<blink::URLLoaderThrottle>> throttles;

  bool is_frame_resource = blink::IsResourceTypeFrame(resource_type);

  DCHECK(!is_frame_resource ||
         type_ == content::URLLoaderThrottleProviderType::kFrame);

  if (!is_frame_resource) {
    if (safe_browsing_remote_)
      safe_browsing_.Bind(std::move(safe_browsing_remote_));
    throttles.push_back(
        std::make_unique<safe_browsing::RendererURLLoaderThrottle>(
            safe_browsing_.get(), render_frame_id));
  }

  return throttles;
}

void URLLoaderThrottleProvider::SetOnline(bool is_online) {}

}  // namespace weblayer
