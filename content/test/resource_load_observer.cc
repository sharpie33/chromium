// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/test/resource_load_observer.h"

#include <string>
#include <vector>

#include "base/path_service.h"
#include "base/run_loop.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/common/content_paths.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace content {

ResourceLoadObserver::ResourceLoadObserver(Shell* shell)
    : WebContentsObserver(shell->web_contents()) {}

ResourceLoadObserver::~ResourceLoadObserver() = default;

// Use this method with the SCOPED_TRACE macro, so it shows the caller context
// if it fails.
void ResourceLoadObserver::CheckResourceLoaded(
    const GURL& original_url,
    const GURL& referrer,
    const std::string& load_method,
    blink::mojom::ResourceType resource_type,
    const base::FilePath::StringPieceType& served_file_name,
    const std::string& mime_type,
    const std::string& ip_address,
    bool was_cached,
    bool first_network_request,
    const base::TimeTicks& before_request,
    const base::TimeTicks& after_request) {
  bool resource_load_info_found = false;
  for (const auto& resource_load_info : resource_load_infos_) {
    if (resource_load_info->original_url != original_url)
      continue;

    resource_load_info_found = true;
    int64_t file_size = -1;
    if (!served_file_name.empty()) {
      base::ScopedAllowBlockingForTesting allow_blocking;
      base::FilePath test_dir;
      ASSERT_TRUE(base::PathService::Get(content::DIR_TEST_DATA, &test_dir));
      base::FilePath served_file = test_dir.Append(served_file_name);
      ASSERT_TRUE(GetFileSize(served_file, &file_size));
    }
    EXPECT_EQ(referrer, resource_load_info->referrer);
    EXPECT_EQ(load_method, resource_load_info->method);
    EXPECT_EQ(resource_type, resource_load_info->resource_type);
    if (!first_network_request)
      EXPECT_GT(resource_load_info->request_id, 0);
    EXPECT_EQ(mime_type, resource_load_info->mime_type);
    ASSERT_TRUE(resource_load_info->network_info->remote_endpoint);
    EXPECT_EQ(ip_address, resource_load_info->network_info->remote_endpoint
                              ->ToStringWithoutPort());
    EXPECT_EQ(was_cached, resource_load_info->was_cached);
    // Simple sanity check of the load timing info.
    auto CheckTime = [before_request, after_request](auto actual) {
      EXPECT_LE(before_request, actual);
      EXPECT_GT(after_request, actual);
    };
    const net::LoadTimingInfo& timing = resource_load_info->load_timing_info;
    CheckTime(timing.request_start);
    CheckTime(timing.receive_headers_end);
    CheckTime(timing.send_start);
    CheckTime(timing.send_end);
    if (!was_cached) {
      CheckTime(timing.connect_timing.dns_start);
      CheckTime(timing.connect_timing.dns_end);
      CheckTime(timing.connect_timing.connect_start);
      CheckTime(timing.connect_timing.connect_end);
    }
    if (file_size != -1) {
      EXPECT_EQ(file_size, resource_load_info->raw_body_bytes);
      EXPECT_LT(file_size, resource_load_info->total_received_bytes);
    }
  }
  EXPECT_TRUE(resource_load_info_found);
}

// Returns the resource with the given url if found, otherwise nullptr.
blink::mojom::ResourceLoadInfoPtr* ResourceLoadObserver::FindResource(
    const GURL& original_url) {
  for (auto& resource : resource_load_infos_) {
    if (resource->original_url == original_url)
      return &resource;
  }
  return nullptr;
}

void ResourceLoadObserver::Reset() {
  resource_load_infos_.clear();
  memory_cached_loaded_urls_.clear();
  resource_is_associated_with_main_frame_.clear();
}

void ResourceLoadObserver::WaitForResourceCompletion(const GURL& original_url) {
  // If we've already seen the resource, return immediately.
  for (const auto& load_info : resource_load_infos_) {
    if (load_info->original_url == original_url)
      return;
  }

  // Otherwise wait for it.
  base::RunLoop loop;
  waiting_original_url_ = original_url;
  waiting_callback_ = loop.QuitClosure();
  loop.Run();
}

// WebContentsObserver implementation:
void ResourceLoadObserver::ResourceLoadComplete(
    content::RenderFrameHost* render_frame_host,
    const GlobalRequestID& request_id,
    const blink::mojom::ResourceLoadInfo& resource_load_info) {
  EXPECT_NE(nullptr, render_frame_host);
  resource_load_infos_.push_back(resource_load_info.Clone());
  resource_is_associated_with_main_frame_.push_back(
      render_frame_host->GetParent() == nullptr);

  // Have we been waiting for this resource? If so, run the callback.
  if (waiting_original_url_.is_valid() &&
      resource_load_info.original_url == waiting_original_url_) {
    waiting_original_url_ = GURL();
    std::move(waiting_callback_).Run();
  }
}

void ResourceLoadObserver::DidLoadResourceFromMemoryCache(
    const GURL& url,
    const std::string& mime_type,
    blink::mojom::ResourceType resource_type) {
  memory_cached_loaded_urls_.push_back(url);
}

}  // namespace content
