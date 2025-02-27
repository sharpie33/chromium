// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/common/url_schemes.h"

#include <string.h>

#include <iterator>
#include <utility>

#include "base/no_destructor.h"
#include "base/strings/string_util.h"
#include "build/build_config.h"
#include "content/public/common/content_client.h"
#include "content/public/common/url_constants.h"
#include "url/url_util.h"

namespace content {
namespace {

bool g_registered_url_schemes = false;

const char* const kDefaultSavableSchemes[] = {
  url::kHttpScheme,
  url::kHttpsScheme,
  url::kFileScheme,
  url::kFileSystemScheme,
  url::kFtpScheme,
  kChromeDevToolsScheme,
  kChromeUIScheme,
  url::kDataScheme
};

// These lists are lazily initialized below and are leaked on shutdown to
// prevent any destructors from being called that will slow us down or cause
// problems.
std::vector<std::string>& GetMutableSavableSchemes() {
  static base::NoDestructor<std::vector<std::string>> schemes;
  return *schemes;
}

// This set contains serialized canonicalized origins as well as hostname
// patterns. The latter are canonicalized by component.
std::vector<std::string>& GetMutableServiceWorkerSchemes() {
  static base::NoDestructor<std::vector<std::string>> schemes;
  return *schemes;
}

}  // namespace

void RegisterContentSchemes() {
  // On Android, schemes may have been registered already.
  if (g_registered_url_schemes)
    return;
  g_registered_url_schemes = true;
  ContentClient::Schemes schemes;
  GetContentClient()->AddAdditionalSchemes(&schemes);

  url::AddStandardScheme(kChromeDevToolsScheme, url::SCHEME_WITH_HOST);
  url::AddStandardScheme(kChromeUIScheme, url::SCHEME_WITH_HOST);
  url::AddStandardScheme(kChromeUIUntrustedScheme, url::SCHEME_WITH_HOST);
  url::AddStandardScheme(kGuestScheme, url::SCHEME_WITH_HOST);
  url::AddStandardScheme(kChromeErrorScheme, url::SCHEME_WITH_HOST);

  for (auto& scheme : schemes.standard_schemes)
    url::AddStandardScheme(scheme.c_str(), url::SCHEME_WITH_HOST);

  for (auto& scheme : schemes.referrer_schemes)
    url::AddReferrerScheme(scheme.c_str(), url::SCHEME_WITH_HOST);

  schemes.secure_schemes.push_back(kChromeUIScheme);
  schemes.secure_schemes.push_back(kChromeErrorScheme);
  for (auto& scheme : schemes.secure_schemes)
    url::AddSecureScheme(scheme.c_str());

  for (auto& scheme : schemes.local_schemes)
    url::AddLocalScheme(scheme.c_str());

  schemes.no_access_schemes.push_back(kChromeErrorScheme);
  for (auto& scheme : schemes.no_access_schemes)
    url::AddNoAccessScheme(scheme.c_str());

  schemes.cors_enabled_schemes.push_back(kChromeUIScheme);
  for (auto& scheme : schemes.cors_enabled_schemes)
    url::AddCorsEnabledScheme(scheme.c_str());

  // TODO(mkwst): Investigate whether chrome-error should be included in
  // csp_bypassing_schemes.
  for (auto& scheme : schemes.csp_bypassing_schemes)
    url::AddCSPBypassingScheme(scheme.c_str());

  for (auto& scheme : schemes.empty_document_schemes)
    url::AddEmptyDocumentScheme(scheme.c_str());

#if defined(OS_ANDROID)
  if (schemes.allow_non_standard_schemes_in_origins)
    url::EnableNonStandardSchemesForAndroidWebView();
#endif

  // Combine the default savable schemes with the additional ones given.
  GetMutableSavableSchemes().assign(std::begin(kDefaultSavableSchemes),
                                    std::end(kDefaultSavableSchemes));
  GetMutableSavableSchemes().insert(GetMutableSavableSchemes().end(),
                                    schemes.savable_schemes.begin(),
                                    schemes.savable_schemes.end());

  GetMutableServiceWorkerSchemes() = std::move(schemes.service_worker_schemes);
}

void ReRegisterContentSchemesForTests() {
  g_registered_url_schemes = false;
  RegisterContentSchemes();
}

const std::vector<std::string>& GetSavableSchemes() {
  return GetMutableSavableSchemes();
}

const std::vector<std::string>& GetServiceWorkerSchemes() {
  return GetMutableServiceWorkerSchemes();
}

}  // namespace content
