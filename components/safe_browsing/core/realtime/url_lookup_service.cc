// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/safe_browsing/core/realtime/url_lookup_service.h"

#include "base/base64url.h"
#include "base/metrics/histogram_macros.h"
#include "base/strings/string_piece.h"
#include "base/time/time.h"
#include "components/safe_browsing/core/common/thread_utils.h"
#include "components/safe_browsing/core/db/v4_protocol_manager_util.h"
#include "components/signin/public/identity_manager/identity_manager.h"
#include "net/base/ip_address.h"
#include "net/base/load_flags.h"
#include "net/base/url_util.h"
#include "net/http/http_status_code.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"

namespace safe_browsing {

namespace {

const char kRealTimeLookupUrlPrefix[] =
    "https://safebrowsing.google.com/safebrowsing/clientreport/realtime";

const size_t kMaxFailuresToEnforceBackoff = 3;

const size_t kMinBackOffResetDurationInSeconds = 5 * 60;   //  5 minutes.
const size_t kMaxBackOffResetDurationInSeconds = 30 * 60;  // 30 minutes.

const size_t kURLLookupTimeoutDurationInSeconds = 10;  // 10 seconds.

// Fragements, usernames and passwords are removed, becuase fragments are only
// used for local navigations and usernames/passwords are too privacy sensitive.
GURL SanitizeURL(const GURL& url) {
  GURL::Replacements replacements;
  replacements.ClearRef();
  replacements.ClearUsername();
  replacements.ClearPassword();
  return url.ReplaceComponents(replacements);
}

}  // namespace

RealTimeUrlLookupService::RealTimeUrlLookupService(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory)
    : url_loader_factory_(url_loader_factory) {}

void RealTimeUrlLookupService::StartLookup(
    const GURL& url,
    RTLookupRequestCallback request_callback,
    RTLookupResponseCallback response_callback,
    signin::IdentityManager* identity_manager) {
  DCHECK(CurrentlyOnThread(ThreadID::IO));
  DCHECK(url.is_valid());

  std::unique_ptr<RTLookupRequest> request = FillRequestProto(url);

  std::string req_data;
  request->SerializeToString(&req_data);
  net::NetworkTrafficAnnotationTag traffic_annotation =
      net::DefineNetworkTrafficAnnotation("safe_browsing_realtime_url_lookup",
                                          R"(
        semantics {
          sender: "Safe Browsing"
          description:
            "When Safe Browsing can't detect that a URL is safe based on its "
            "local database, it sends the top-level URL to Google to verify it "
            "before showing a warning to the user."
          trigger:
            "When a main frame URL fails to match the local hash-prefix "
            "database of known safe URLs and a valid result from a prior "
            "lookup is not already cached, this will be sent."
          data: "The main frame URL that did not match the local safelist."
          destination: GOOGLE_OWNED_SERVICE
        }
        policy {
          cookies_allowed: YES
          cookies_store: "Safe Browsing cookie store"
          setting:
            "Users can disable Safe Browsing real time URL checks by "
            "unchecking 'Protect you and your device from dangerous sites' in "
            "Chromium settings under Privacy, or by unchecking 'Make searches "
            "and browsing better (Sends URLs of pages you visit to Google)' in "
            "Chromium settings under Privacy."
          chrome_policy {
            UrlKeyedAnonymizedDataCollectionEnabled {
              policy_options {mode: MANDATORY}
              UrlKeyedAnonymizedDataCollectionEnabled: false
            }
          }
        })");

  auto resource_request = std::make_unique<network::ResourceRequest>();
  resource_request->url = GURL(kRealTimeLookupUrlPrefix);
  resource_request->load_flags = net::LOAD_DISABLE_CACHE;
  resource_request->method = "POST";

  std::unique_ptr<network::SimpleURLLoader> owned_loader =
      network::SimpleURLLoader::Create(std::move(resource_request),
                                       traffic_annotation);
  network::SimpleURLLoader* loader = owned_loader.get();
  owned_loader->AttachStringForUpload(req_data, "application/octet-stream");
  owned_loader->SetTimeoutDuration(
      base::TimeDelta::FromSeconds(kURLLookupTimeoutDurationInSeconds));
  owned_loader->DownloadToStringOfUnboundedSizeUntilCrashAndDie(
      url_loader_factory_.get(),
      base::BindOnce(&RealTimeUrlLookupService::OnURLLoaderComplete,
                     GetWeakPtr(), loader, base::TimeTicks::Now()));

  pending_requests_[owned_loader.release()] = std::move(response_callback);

  std::move(request_callback).Run(std::move(request));
}

RealTimeUrlLookupService::~RealTimeUrlLookupService() {
  for (auto& pending : pending_requests_) {
    // An empty response is treated as safe.
    auto response = std::make_unique<RTLookupResponse>();
    std::move(pending.second).Run(std::move(response));
    delete pending.first;
  }
  pending_requests_.clear();
}

void RealTimeUrlLookupService::OnURLLoaderComplete(
    network::SimpleURLLoader* url_loader,
    base::TimeTicks request_start_time,
    std::unique_ptr<std::string> response_body) {
  DCHECK(CurrentlyOnThread(ThreadID::IO));

  auto it = pending_requests_.find(url_loader);
  DCHECK(it != pending_requests_.end()) << "Request not found";

  UMA_HISTOGRAM_TIMES("SafeBrowsing.RT.Network.Time",
                      base::TimeTicks::Now() - request_start_time);

  int net_error = url_loader->NetError();
  int response_code = 0;
  if (url_loader->ResponseInfo() && url_loader->ResponseInfo()->headers)
    response_code = url_loader->ResponseInfo()->headers->response_code();
  V4ProtocolManagerUtil::RecordHttpResponseOrErrorCode(
      "SafeBrowsing.RT.Network.Result", net_error, response_code);

  auto response = std::make_unique<RTLookupResponse>();
  bool success = (net_error == net::OK) && (response_code == net::HTTP_OK) &&
                 response->ParseFromString(*response_body);
  success ? HandleLookupSuccess() : HandleLookupError();

  std::move(it->second).Run(std::move(response));
  delete it->first;
  pending_requests_.erase(it);

  // If |database_manager| already released current object and there is no
  // pending request left, delete itself.
  if (pending_requests_.empty() && is_self_owned_) {
    delete this;
  }
}

bool RealTimeUrlLookupService::CanCheckUrl(const GURL& url) const {
  if (!url.SchemeIsHTTPOrHTTPS()) {
    return false;
  }

  if (net::IsLocalhost(url)) {
    // Includes: "//localhost/", "//localhost.localdomain/", "//127.0.0.1/"
    return false;
  }

  net::IPAddress ip_address;
  if (url.HostIsIPAddress() && ip_address.AssignFromIPLiteral(url.host()) &&
      !ip_address.IsPubliclyRoutable()) {
    // Includes: "//192.168.1.1/", "//172.16.2.2/", "//10.1.1.1/"
    return false;
  }

  return true;
}

std::unique_ptr<RTLookupRequest> RealTimeUrlLookupService::FillRequestProto(
    const GURL& url) {
  auto request = std::make_unique<RTLookupRequest>();
  request->set_url(SanitizeURL(url).spec());
  request->set_lookup_type(RTLookupRequest::NAVIGATION);
  // TODO(crbug.com/1017499): Set ChromeUserPopulation.
  return request;
}

size_t RealTimeUrlLookupService::GetBackoffDurationInSeconds() const {
  DCHECK(CurrentlyOnThread(ThreadID::IO));
  return did_successful_lookup_since_last_backoff_
             ? kMinBackOffResetDurationInSeconds
             : std::min(kMaxBackOffResetDurationInSeconds,
                        2 * next_backoff_duration_secs_);
}

void RealTimeUrlLookupService::HandleLookupError() {
  DCHECK(CurrentlyOnThread(ThreadID::IO));
  consecutive_failures_++;

  // Any successful lookup clears both |consecutive_failures_| as well as
  // |did_successful_lookup_since_last_backoff_|.
  // On a failure, the following happens:
  // 1) if |consecutive_failures_| < |kMaxFailuresToEnforceBackoff|:
  //    Do nothing more.
  // 2) if already in the backoff mode:
  //    Do nothing more. This can happen if we had some outstanding real time
  //    requests in flight when we entered the backoff mode.
  // 3) if |did_successful_lookup_since_last_backoff_| is true:
  //    Enter backoff mode for |kMinBackOffResetDurationInSeconds| seconds.
  // 4) if |did_successful_lookup_since_last_backoff_| is false:
  //    This indicates that we've had |kMaxFailuresToEnforceBackoff| since
  //    exiting the last backoff with no successful lookups since so do an
  //    exponential backoff.

  if (consecutive_failures_ < kMaxFailuresToEnforceBackoff)
    return;

  if (IsInBackoffMode()) {
    return;
  }

  // Enter backoff mode, calculate duration.
  next_backoff_duration_secs_ = GetBackoffDurationInSeconds();
  backoff_timer_.Start(
      FROM_HERE, base::TimeDelta::FromSeconds(next_backoff_duration_secs_),
      this, &RealTimeUrlLookupService::ResetFailures);
  did_successful_lookup_since_last_backoff_ = false;
}

void RealTimeUrlLookupService::HandleLookupSuccess() {
  DCHECK(CurrentlyOnThread(ThreadID::IO));
  ResetFailures();

  // |did_successful_lookup_since_last_backoff_| is set to true only when we
  // complete a lookup successfully.
  did_successful_lookup_since_last_backoff_ = true;
}

bool RealTimeUrlLookupService::IsInBackoffMode() const {
  DCHECK(CurrentlyOnThread(ThreadID::IO));
  return backoff_timer_.IsRunning();
}

void RealTimeUrlLookupService::ResetFailures() {
  DCHECK(CurrentlyOnThread(ThreadID::IO));
  consecutive_failures_ = 0;
  backoff_timer_.Stop();
}

void RealTimeUrlLookupService::WaitForPendingRequestsOrDelete() {
  if (pending_requests_.empty()) {
    delete this;
    return;
  }
  is_self_owned_ = true;
}

// static
SBThreatType RealTimeUrlLookupService::GetSBThreatTypeForRTThreatType(
    RTLookupResponse::ThreatInfo::ThreatType rt_threat_type) {
  switch (rt_threat_type) {
    case RTLookupResponse::ThreatInfo::WEB_MALWARE:
      return SB_THREAT_TYPE_URL_MALWARE;
    case RTLookupResponse::ThreatInfo::SOCIAL_ENGINEERING:
      return SB_THREAT_TYPE_URL_PHISHING;
    case RTLookupResponse::ThreatInfo::UNWANTED_SOFTWARE:
      return SB_THREAT_TYPE_URL_UNWANTED;
    case RTLookupResponse::ThreatInfo::UNCLEAR_BILLING:
      return SB_THREAT_TYPE_BILLING;
    case RTLookupResponse::ThreatInfo::THREAT_TYPE_UNSPECIFIED:
      NOTREACHED() << "Unexpected RTLookupResponse::ThreatType encountered";
      return SB_THREAT_TYPE_SAFE;
  }
}

base::WeakPtr<RealTimeUrlLookupService> RealTimeUrlLookupService::GetWeakPtr() {
  return weak_factory_.GetWeakPtr();
}

}  // namespace safe_browsing
