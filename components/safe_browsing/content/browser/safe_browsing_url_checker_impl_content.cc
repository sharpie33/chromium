// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/safe_browsing/core/browser/safe_browsing_url_checker_impl.h"

#include "base/bind.h"
#include "base/metrics/histogram_functions.h"
#include "base/metrics/histogram_macros.h"
#include "base/metrics/histogram_macros_local.h"
#include "base/task/post_task.h"
#include "components/safe_browsing/content/web_ui/safe_browsing_ui.h"
#include "components/safe_browsing/core/common/safebrowsing_constants.h"
#include "components/safe_browsing/core/common/thread_utils.h"
#include "components/safe_browsing/core/realtime/policy_engine.h"
#include "components/safe_browsing/core/realtime/url_lookup_service.h"
#include "components/safe_browsing/core/verdict_cache_manager.h"
#include "components/safe_browsing/core/web_ui/constants.h"

namespace safe_browsing {

bool SafeBrowsingUrlCheckerImpl::CanPerformFullURLLookup(const GURL& url) {
  if (!real_time_lookup_enabled_)
    return false;

  if (!RealTimePolicyEngine::CanPerformFullURLLookupForResourceType(
          resource_type_))
    return false;

  auto* rt_lookup_service = database_manager_->GetRealTimeUrlLookupService();
  if (!rt_lookup_service || !rt_lookup_service->CanCheckUrl(url))
    return false;

  bool in_backoff = rt_lookup_service->IsInBackoffMode();
  UMA_HISTOGRAM_BOOLEAN("SafeBrowsing.RT.Backoff.State", in_backoff);
  return !in_backoff;
}

void SafeBrowsingUrlCheckerImpl::OnRTLookupRequest(
    std::unique_ptr<RTLookupRequest> request) {
  DCHECK(CurrentlyOnThread(ThreadID::IO));

  // The following is to log this RTLookupRequest on any open
  // chrome://safe-browsing pages.
  base::PostTaskAndReplyWithResult(
      FROM_HERE, CreateTaskTraits(ThreadID::UI),
      base::BindOnce(&WebUIInfoSingleton::AddToRTLookupPings,
                     base::Unretained(WebUIInfoSingleton::GetInstance()),
                     *request),
      base::BindOnce(&SafeBrowsingUrlCheckerImpl::SetWebUIToken,
                     weak_factory_.GetWeakPtr()));
}

void SafeBrowsingUrlCheckerImpl::OnRTLookupResponse(
    std::unique_ptr<RTLookupResponse> response) {
  DCHECK(CurrentlyOnThread(ThreadID::IO));
  DCHECK_EQ(ResourceType::kMainFrame, resource_type_);

  if (url_web_ui_token_ != -1) {
    // The following is to log this RTLookupResponse on any open
    // chrome://safe-browsing pages.
    base::PostTask(
        FROM_HERE, CreateTaskTraits(ThreadID::UI),
        base::BindOnce(&WebUIInfoSingleton::AddToRTLookupResponses,
                       base::Unretained(WebUIInfoSingleton::GetInstance()),
                       url_web_ui_token_, *response));
  }

  const GURL& url = urls_[next_index_].url;

  SBThreatType sb_threat_type = SB_THREAT_TYPE_SAFE;
  if (response && (response->threat_info_size() > 0)) {
    base::PostTask(FROM_HERE, CreateTaskTraits(ThreadID::UI),
                   base::BindOnce(&VerdictCacheManager::CacheRealTimeUrlVerdict,
                                  cache_manager_on_ui_, url, *response,
                                  base::Time::Now()));

    // TODO(crbug.com/1033692): Only take the first threat info into account
    // because threat infos are returned in decreasing order of severity.
    // Consider extend it to support multiple threat types.
    if (response->threat_info(0).verdict_type() ==
        RTLookupResponse::ThreatInfo::DANGEROUS) {
      sb_threat_type = RealTimeUrlLookupService::GetSBThreatTypeForRTThreatType(
          response->threat_info(0).threat_type());
    }
  }
  OnUrlResult(url, sb_threat_type, ThreatMetadata());
}

}  // namespace safe_browsing
