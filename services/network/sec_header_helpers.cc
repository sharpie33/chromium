// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/network/sec_header_helpers.h"

#include <algorithm>
#include <string>

#include "base/feature_list.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "net/http/http_request_headers.h"
#include "net/url_request/url_request.h"
#include "services/network/public/cpp/cors/origin_access_list.h"
#include "services/network/public/cpp/features.h"
#include "services/network/public/cpp/initiator_lock_compatibility.h"
#include "services/network/public/cpp/is_potentially_trustworthy.h"
#include "services/network/public/cpp/request_destination.h"
#include "services/network/public/cpp/request_mode.h"
#include "services/network/public/mojom/fetch_api.mojom.h"
#include "services/network/public/mojom/network_context.mojom.h"

namespace network {

namespace {

const char kSecFetchMode[] = "Sec-Fetch-Mode";
const char kSecFetchSite[] = "Sec-Fetch-Site";
const char kSecFetchUser[] = "Sec-Fetch-User";
const char kSecFetchDest[] = "Sec-Fetch-Dest";

// Sec-Fetch-Site infrastructure:
//
// Note that the order of enum values below is significant - it is important for
// std::max invocations that kSameOrigin < kSameSite < kCrossSite.
enum class SecFetchSiteValue {
  kNoOrigin,
  kSameOrigin,
  kSameSite,
  kCrossSite,
};

const char* GetSecFetchSiteHeaderString(const SecFetchSiteValue& value) {
  switch (value) {
    case SecFetchSiteValue::kNoOrigin:
      return "none";
    case SecFetchSiteValue::kSameOrigin:
      return "same-origin";
    case SecFetchSiteValue::kSameSite:
      return "same-site";
    case SecFetchSiteValue::kCrossSite:
      return "cross-site";
  }
}

SecFetchSiteValue SecFetchSiteHeaderValue(const GURL& target_url,
                                          const url::Origin& initiator) {
  url::Origin target_origin = url::Origin::Create(target_url);

  if (target_origin == initiator)
    return SecFetchSiteValue::kSameOrigin;

  // Cross-scheme initiator should be considered cross-site (even if it's host
  // is same-site with the target).  See also https://crbug.com/979257.
  if (initiator.scheme() == target_origin.scheme() &&
      net::registry_controlled_domains::SameDomainOrHost(
          initiator, target_origin,
          net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES)) {
    return SecFetchSiteValue::kSameSite;
  }

  return SecFetchSiteValue::kCrossSite;
}

void SetSecFetchSiteHeader(
    net::URLRequest* request,
    const GURL* pending_redirect_url,
    const mojom::URLLoaderFactoryParams& factory_params) {
  SecFetchSiteValue header_value;
  url::Origin initiator = GetTrustworthyInitiator(
      factory_params.request_initiator_site_lock, request->initiator());

  // Browser-initiated requests with no initiator origin, and
  // privileged requests initiated from a "non-webby" context will send
  // `Sec-Fetch-Site: None` while unprivileged ones will send
  // `Sec-Fetch-Site: cross-site`. Other requests default to `kSameOrigin`,
  // and walk through the request's URL chain to calculate the
  // correct value.
  if (factory_params.unsafe_non_webby_initiator) {
    cors::OriginAccessList origin_access_list;
    origin_access_list.SetAllowListForOrigin(
        factory_params.factory_bound_access_patterns->source_origin,
        factory_params.factory_bound_access_patterns->allow_patterns);
    if (origin_access_list.CheckAccessState(
            factory_params.factory_bound_access_patterns->source_origin,
            request->url()) == cors::OriginAccessList::AccessState::kAllowed) {
      header_value = SecFetchSiteValue::kNoOrigin;
    } else {
      header_value = SecFetchSiteValue::kCrossSite;
    }
  } else if (factory_params.process_id == mojom::kBrowserProcessId &&
             !request->initiator().has_value()) {
    header_value = SecFetchSiteValue::kNoOrigin;
  } else {
    header_value = SecFetchSiteValue::kSameOrigin;
    for (const GURL& target_url : request->url_chain()) {
      header_value = std::max(header_value,
                              SecFetchSiteHeaderValue(target_url, initiator));
    }
    if (pending_redirect_url) {
      header_value =
          std::max(header_value,
                   SecFetchSiteHeaderValue(*pending_redirect_url, initiator));
    }
  }

  request->SetExtraRequestHeaderByName(
      kSecFetchSite, GetSecFetchSiteHeaderString(header_value),
      /* overwrite = */ true);
}

// Sec-Fetch-Mode
void SetSecFetchModeHeader(net::URLRequest* request,
                           network::mojom::RequestMode mode) {
  std::string header_value = RequestModeToString(mode);

  request->SetExtraRequestHeaderByName(kSecFetchMode, header_value, false);
}

// Sec-Fetch-User
void SetSecFetchUserHeader(net::URLRequest* request, bool has_user_activation) {
  if (has_user_activation)
    request->SetExtraRequestHeaderByName(kSecFetchUser, "?1", true);
  else
    request->RemoveRequestHeaderByName(kSecFetchUser);
}

// Sec-Fetch-Dest
void SetSecFetchDestHeader(net::URLRequest* request,
                           network::mojom::RequestDestination dest) {
  std::string header_value = RequestDestinationToString(dest);
  request->SetExtraRequestHeaderByName(kSecFetchDest, header_value, true);
}

}  // namespace

void SetFetchMetadataHeaders(
    net::URLRequest* request,
    network::mojom::RequestMode mode,
    bool has_user_activation,
    network::mojom::RequestDestination dest,
    const GURL* pending_redirect_url,
    const mojom::URLLoaderFactoryParams& factory_params) {
  DCHECK(request);
  DCHECK_NE(0u, request->url_chain().size());
  if (!base::FeatureList::IsEnabled(features::kFetchMetadata))
    return;

  // Only append the header to potentially trustworthy URLs.
  const GURL& target_url =
      pending_redirect_url ? *pending_redirect_url : request->url();
  if (!IsUrlPotentiallyTrustworthy(target_url))
    return;

  SetSecFetchSiteHeader(request, pending_redirect_url, factory_params);
  SetSecFetchModeHeader(request, mode);
  SetSecFetchUserHeader(request, has_user_activation);
  SetSecFetchDestHeader(request, dest);
}

void MaybeRemoveSecHeaders(net::URLRequest* request,
                           const GURL& pending_redirect_url) {
  DCHECK(request);

  if (!base::FeatureList::IsEnabled(features::kFetchMetadata))
    return;

  // If our redirect destination is not trusted it would not have had sec-ch- or
  // sec-fetch- prefixed headers added to it. Our previous hops may have added
  // these headers if the current url is trustworthy though so we should try to
  // remove these now.
  if (IsUrlPotentiallyTrustworthy(request->url()) &&
      !IsUrlPotentiallyTrustworthy(pending_redirect_url)) {
    // Check each of our request headers and if it is a "sec-ch-" or
    // "sec-fetch-" prefixed header we'll remove it.
    const net::HttpRequestHeaders::HeaderVector request_headers =
        request->extra_request_headers().GetHeaderVector();
    for (const auto& header : request_headers) {
      if (StartsWith(header.key, "sec-ch-",
                     base::CompareCase::INSENSITIVE_ASCII) ||
          StartsWith(header.key, "sec-fetch-",
                     base::CompareCase::INSENSITIVE_ASCII)) {
        request->RemoveRequestHeaderByName(header.key);
      }
    }
  }
}

}  // namespace network
