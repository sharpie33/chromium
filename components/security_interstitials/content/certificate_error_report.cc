// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/security_interstitials/content/certificate_error_report.h"

#include <vector>

#include "base/stl_util.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "components/network_time/network_time_tracker.h"
#include "net/cert/cert_verifier.h"
#include "net/cert/x509_certificate.h"
#include "net/ssl/ssl_info.h"

#if defined(OS_ANDROID)
#include "net/cert/cert_verify_proc_android.h"
#endif

#if defined(OS_MACOSX)
#include "net/cert/internal/trust_store_mac.h"
#endif

#include "net/cert/cert_verify_result.h"

using network_time::NetworkTimeTracker;

namespace {

// Add any errors from |cert_status| to |cert_errors|. (net::CertStatus can
// represent both errors and non-error status codes.)
void AddCertStatusToReportErrors(
    net::CertStatus cert_status,
    ::google::protobuf::RepeatedField<int>* cert_errors) {
#define COPY_CERT_STATUS(error) RENAME_CERT_STATUS(error, CERT_##error)
#define RENAME_CERT_STATUS(status_error, logger_error) \
  if (cert_status & net::CERT_STATUS_##status_error)   \
    cert_errors->Add(chrome_browser_ssl::CertLoggerRequest::ERR_##logger_error);

  COPY_CERT_STATUS(REVOKED)
  COPY_CERT_STATUS(INVALID)
  RENAME_CERT_STATUS(PINNED_KEY_MISSING, SSL_PINNED_KEY_NOT_IN_CERT_CHAIN)
  COPY_CERT_STATUS(AUTHORITY_INVALID)
  COPY_CERT_STATUS(COMMON_NAME_INVALID)
  COPY_CERT_STATUS(NON_UNIQUE_NAME)
  COPY_CERT_STATUS(NAME_CONSTRAINT_VIOLATION)
  COPY_CERT_STATUS(WEAK_SIGNATURE_ALGORITHM)
  COPY_CERT_STATUS(WEAK_KEY)
  COPY_CERT_STATUS(DATE_INVALID)
  COPY_CERT_STATUS(VALIDITY_TOO_LONG)
  COPY_CERT_STATUS(UNABLE_TO_CHECK_REVOCATION)
  COPY_CERT_STATUS(NO_REVOCATION_MECHANISM)
  RENAME_CERT_STATUS(CERTIFICATE_TRANSPARENCY_REQUIRED,
                     CERTIFICATE_TRANSPARENCY_REQUIRED)
  COPY_CERT_STATUS(SYMANTEC_LEGACY)
  COPY_CERT_STATUS(KNOWN_INTERCEPTION_BLOCKED)

#undef RENAME_CERT_STATUS
#undef COPY_CERT_STATUS
}

// Add any non-error codes from |cert_status| to |cert_errors|.
// (net::CertStatus can represent both errors and non-error status codes.)
void AddCertStatusToReportStatus(
    net::CertStatus cert_status,
    ::google::protobuf::RepeatedField<int>* report_status) {
#define COPY_CERT_STATUS(error)               \
  if (cert_status & net::CERT_STATUS_##error) \
    report_status->Add(chrome_browser_ssl::CertLoggerRequest::STATUS_##error);

  COPY_CERT_STATUS(IS_EV)
  COPY_CERT_STATUS(REV_CHECKING_ENABLED)
  COPY_CERT_STATUS(SHA1_SIGNATURE_PRESENT)
  COPY_CERT_STATUS(CT_COMPLIANCE_FAILED)
  COPY_CERT_STATUS(KNOWN_INTERCEPTION_DETECTED)

#undef COPY_CERT_STATUS
}

#if BUILDFLAG(TRIAL_COMPARISON_CERT_VERIFIER_SUPPORTED)
void AddVerifyFlagsToReport(
    bool enable_rev_checking,
    bool require_rev_checking_local_anchors,
    bool enable_sha1_local_anchors,
    bool disable_symantec_enforcement,
    ::google::protobuf::RepeatedField<int>* report_flags) {
  if (enable_rev_checking) {
    report_flags->Add(
        chrome_browser_ssl::TrialVerificationInfo::VERIFY_REV_CHECKING_ENABLED);
  }
  if (require_rev_checking_local_anchors) {
    report_flags->Add(chrome_browser_ssl::TrialVerificationInfo::
                          VERIFY_REV_CHECKING_REQUIRED_LOCAL_ANCHORS);
  }
  if (enable_sha1_local_anchors) {
    report_flags->Add(chrome_browser_ssl::TrialVerificationInfo::
                          VERIFY_ENABLE_SHA1_LOCAL_ANCHORS);
  }
  if (disable_symantec_enforcement) {
    report_flags->Add(chrome_browser_ssl::TrialVerificationInfo::
                          VERIFY_DISABLE_SYMANTEC_ENFORCEMENT);
  }
}

#if defined(OS_MACOSX)
void AddMacTrustFlagsToReport(
    int mac_trust_flags,
    ::google::protobuf::RepeatedField<int>* report_flags) {
#define COPY_TRUST_FLAGS(flag)                            \
  if (mac_trust_flags & net::TrustStoreMac::TRUST_##flag) \
    report_flags->Add(                                    \
        chrome_browser_ssl::TrialVerificationInfo::MAC_TRUST_##flag);

  COPY_TRUST_FLAGS(SETTINGS_ARRAY_EMPTY);
  COPY_TRUST_FLAGS(SETTINGS_DICT_EMPTY);
  COPY_TRUST_FLAGS(SETTINGS_DICT_UNKNOWN_KEY);
  COPY_TRUST_FLAGS(SETTINGS_DICT_CONTAINS_POLICY);
  COPY_TRUST_FLAGS(SETTINGS_DICT_INVALID_POLICY_TYPE);
  COPY_TRUST_FLAGS(SETTINGS_DICT_CONTAINS_APPLICATION);
  COPY_TRUST_FLAGS(SETTINGS_DICT_CONTAINS_POLICY_STRING);
  COPY_TRUST_FLAGS(SETTINGS_DICT_CONTAINS_KEY_USAGE);
  COPY_TRUST_FLAGS(SETTINGS_DICT_CONTAINS_RESULT);
  COPY_TRUST_FLAGS(SETTINGS_DICT_INVALID_RESULT_TYPE);
  COPY_TRUST_FLAGS(SETTINGS_DICT_CONTAINS_ALLOWED_ERROR);

#undef COPY_TRUST_FLAGS
}

void AddMacPlatformDebugInfoToReport(
    const network::mojom::MacPlatformVerifierDebugInfoPtr&
        mac_platform_debug_info,
    chrome_browser_ssl::TrialVerificationInfo* trial_report) {
  if (!mac_platform_debug_info)
    return;
  chrome_browser_ssl::MacPlatformDebugInfo* report_info =
      trial_report->mutable_mac_platform_debug_info();
  report_info->set_trust_result(mac_platform_debug_info->trust_result);
  report_info->set_result_code(mac_platform_debug_info->result_code);
  for (const auto& cert_info : mac_platform_debug_info->status_chain) {
    chrome_browser_ssl::MacCertEvidenceInfo* report_cert_info =
        report_info->add_status_chain();
    report_cert_info->set_status_bits(cert_info->status_bits);
    for (auto code : cert_info->status_codes)
      report_cert_info->add_status_codes(code);
  }
}
#endif  // defined(OS_MACOSX)
#endif  // BUILDFLAG(TRIAL_COMPARISON_CERT_VERIFIER_SUPPORTED)

bool CertificateChainToString(const net::X509Certificate& cert,
                              std::string* result) {
  std::vector<std::string> pem_encoded_chain;
  if (!cert.GetPEMEncodedChain(&pem_encoded_chain))
    return false;

  *result = base::StrCat(pem_encoded_chain);
  return true;
}

}  // namespace

CertificateErrorReport::CertificateErrorReport()
    : cert_report_(new chrome_browser_ssl::CertLoggerRequest()) {}

CertificateErrorReport::CertificateErrorReport(const std::string& hostname,
                                               const net::SSLInfo& ssl_info)
    : CertificateErrorReport(hostname,
                             *ssl_info.cert,
                             ssl_info.unverified_cert.get(),
                             ssl_info.is_issued_by_known_root,
                             ssl_info.cert_status) {
  cert_report_->add_pin(ssl_info.pinning_failure_log);
}

#if BUILDFLAG(TRIAL_COMPARISON_CERT_VERIFIER_SUPPORTED)
CertificateErrorReport::CertificateErrorReport(
    const std::string& hostname,
    const net::X509Certificate& unverified_cert,
    bool enable_rev_checking,
    bool require_rev_checking_local_anchors,
    bool enable_sha1_local_anchors,
    bool disable_symantec_enforcement,
    const net::CertVerifyResult& primary_result,
    const net::CertVerifyResult& trial_result,
    network::mojom::CertVerifierDebugInfoPtr debug_info)
    : CertificateErrorReport(hostname,
                             *primary_result.verified_cert,
                             &unverified_cert,
                             primary_result.is_issued_by_known_root,
                             primary_result.cert_status) {
  chrome_browser_ssl::CertLoggerFeaturesInfo* features_info =
      cert_report_->mutable_features_info();
  chrome_browser_ssl::TrialVerificationInfo* trial_report =
      features_info->mutable_trial_verification_info();
  if (!CertificateChainToString(*trial_result.verified_cert,
                                trial_report->mutable_cert_chain())) {
    LOG(ERROR) << "Could not get PEM encoded chain.";
  }
  trial_report->set_is_issued_by_known_root(
      trial_result.is_issued_by_known_root);
  AddCertStatusToReportErrors(trial_result.cert_status,
                              trial_report->mutable_cert_error());
  AddCertStatusToReportStatus(trial_result.cert_status,
                              trial_report->mutable_cert_status());
  AddVerifyFlagsToReport(
      enable_rev_checking, require_rev_checking_local_anchors,
      enable_sha1_local_anchors, disable_symantec_enforcement,
      trial_report->mutable_verify_flags());
#if defined(OS_MACOSX)
  AddMacPlatformDebugInfoToReport(debug_info->mac_platform_debug_info,
                                  trial_report);
  AddMacTrustFlagsToReport(
      debug_info->mac_combined_trust_debug_info,
      trial_report->mutable_mac_combined_trust_debug_info());
#endif
  if (!debug_info->trial_verification_time.is_null()) {
    trial_report->set_trial_verification_time_usec(
        debug_info->trial_verification_time.ToDeltaSinceWindowsEpoch()
            .InMicroseconds());
  }
  if (!debug_info->trial_der_verification_time.empty()) {
    trial_report->set_trial_der_verification_time(
        debug_info->trial_der_verification_time);
  }
}
#endif  // BUILDFLAG(TRIAL_COMPARISON_CERT_VERIFIER_SUPPORTED)

CertificateErrorReport::~CertificateErrorReport() {}

bool CertificateErrorReport::InitializeFromString(
    const std::string& serialized_report) {
  return cert_report_->ParseFromString(serialized_report);
}

bool CertificateErrorReport::Serialize(std::string* output) const {
  return cert_report_->SerializeToString(output);
}

void CertificateErrorReport::SetInterstitialInfo(
    const InterstitialReason& interstitial_reason,
    const ProceedDecision& proceed_decision,
    const Overridable& overridable,
    const base::Time& interstitial_time) {
  chrome_browser_ssl::CertLoggerInterstitialInfo* interstitial_info =
      cert_report_->mutable_interstitial_info();

  switch (interstitial_reason) {
    case INTERSTITIAL_SSL:
      interstitial_info->set_interstitial_reason(
          chrome_browser_ssl::CertLoggerInterstitialInfo::INTERSTITIAL_SSL);
      break;
    case INTERSTITIAL_CAPTIVE_PORTAL:
      interstitial_info->set_interstitial_reason(
          chrome_browser_ssl::CertLoggerInterstitialInfo::
              INTERSTITIAL_CAPTIVE_PORTAL);
      break;
    case INTERSTITIAL_CLOCK:
      interstitial_info->set_interstitial_reason(
          chrome_browser_ssl::CertLoggerInterstitialInfo::INTERSTITIAL_CLOCK);
      break;
    case INTERSTITIAL_SUPERFISH:
      interstitial_info->set_interstitial_reason(
          chrome_browser_ssl::CertLoggerInterstitialInfo::
              INTERSTITIAL_SUPERFISH);
      break;
    case INTERSTITIAL_MITM_SOFTWARE:
      interstitial_info->set_interstitial_reason(
          chrome_browser_ssl::CertLoggerInterstitialInfo::
              INTERSTITIAL_MITM_SOFTWARE);
      break;
    case INTERSTITIAL_BLOCKED_INTERCEPTION:
      interstitial_info->set_interstitial_reason(
          chrome_browser_ssl::CertLoggerInterstitialInfo::
              INTERSTITIAL_BLOCKED_INTERCEPTION);
      break;
    case INTERSTITIAL_LEGACY_TLS:
      interstitial_info->set_interstitial_reason(
          chrome_browser_ssl::CertLoggerInterstitialInfo::
              INTERSTITIAL_LEGACY_TLS);
  }

  interstitial_info->set_user_proceeded(proceed_decision == USER_PROCEEDED);
  interstitial_info->set_overridable(overridable == INTERSTITIAL_OVERRIDABLE);
  interstitial_info->set_interstitial_created_time_usec(
      interstitial_time.ToDeltaSinceWindowsEpoch().InMicroseconds());
}

void CertificateErrorReport::AddNetworkTimeInfo(
    const NetworkTimeTracker* network_time_tracker) {
  chrome_browser_ssl::CertLoggerFeaturesInfo* features_info =
      cert_report_->mutable_features_info();
  chrome_browser_ssl::CertLoggerFeaturesInfo::NetworkTimeQueryingInfo*
      network_time_info = features_info->mutable_network_time_querying_info();
  network_time_info->set_network_time_queries_enabled(
      network_time_tracker->AreTimeFetchesEnabled());
  NetworkTimeTracker::FetchBehavior behavior =
      network_time_tracker->GetFetchBehavior();
  chrome_browser_ssl::CertLoggerFeaturesInfo::NetworkTimeQueryingInfo::
      NetworkTimeFetchBehavior report_behavior =
          chrome_browser_ssl::CertLoggerFeaturesInfo::NetworkTimeQueryingInfo::
              NETWORK_TIME_FETCHES_UNKNOWN;

  switch (behavior) {
    case NetworkTimeTracker::FETCH_BEHAVIOR_UNKNOWN:
      report_behavior = chrome_browser_ssl::CertLoggerFeaturesInfo::
          NetworkTimeQueryingInfo::NETWORK_TIME_FETCHES_UNKNOWN;
      break;
    case NetworkTimeTracker::FETCHES_IN_BACKGROUND_ONLY:
      report_behavior = chrome_browser_ssl::CertLoggerFeaturesInfo::
          NetworkTimeQueryingInfo::NETWORK_TIME_FETCHES_BACKGROUND_ONLY;
      break;
    case NetworkTimeTracker::FETCHES_ON_DEMAND_ONLY:
      report_behavior = chrome_browser_ssl::CertLoggerFeaturesInfo::
          NetworkTimeQueryingInfo::NETWORK_TIME_FETCHES_ON_DEMAND_ONLY;
      break;
    case NetworkTimeTracker::FETCHES_IN_BACKGROUND_AND_ON_DEMAND:
      report_behavior =
          chrome_browser_ssl::CertLoggerFeaturesInfo::NetworkTimeQueryingInfo::
              NETWORK_TIME_FETCHES_IN_BACKGROUND_AND_ON_DEMAND;
      break;
  }
  network_time_info->set_network_time_query_behavior(report_behavior);
}

void CertificateErrorReport::AddChromeChannel(version_info::Channel channel) {
  switch (channel) {
    case version_info::Channel::STABLE:
      cert_report_->set_chrome_channel(
          chrome_browser_ssl::CertLoggerRequest::CHROME_CHANNEL_STABLE);
      break;

    case version_info::Channel::BETA:
      cert_report_->set_chrome_channel(
          chrome_browser_ssl::CertLoggerRequest::CHROME_CHANNEL_BETA);
      break;

    case version_info::Channel::CANARY:
      cert_report_->set_chrome_channel(
          chrome_browser_ssl::CertLoggerRequest::CHROME_CHANNEL_CANARY);
      break;

    case version_info::Channel::DEV:
      cert_report_->set_chrome_channel(
          chrome_browser_ssl::CertLoggerRequest::CHROME_CHANNEL_DEV);
      break;

    case version_info::Channel::UNKNOWN:
      cert_report_->set_chrome_channel(
          chrome_browser_ssl::CertLoggerRequest::CHROME_CHANNEL_UNKNOWN);
      break;
  }
}

void CertificateErrorReport::SetIsEnterpriseManaged(
    bool is_enterprise_managed) {
  cert_report_->set_is_enterprise_managed(is_enterprise_managed);
}

void CertificateErrorReport::SetIsRetryUpload(bool is_retry_upload) {
  cert_report_->set_is_retry_upload(is_retry_upload);
}

const std::string& CertificateErrorReport::hostname() const {
  return cert_report_->hostname();
}

chrome_browser_ssl::CertLoggerRequest::ChromeChannel
CertificateErrorReport::chrome_channel() const {
  return cert_report_->chrome_channel();
}

bool CertificateErrorReport::is_enterprise_managed() const {
  return cert_report_->is_enterprise_managed();
}

bool CertificateErrorReport::is_retry_upload() const {
  return cert_report_->is_retry_upload();
}

CertificateErrorReport::CertificateErrorReport(
    const std::string& hostname,
    const net::X509Certificate& cert,
    const net::X509Certificate* unverified_cert,
    bool is_issued_by_known_root,
    net::CertStatus cert_status)
    : cert_report_(new chrome_browser_ssl::CertLoggerRequest()) {
  base::Time now = base::Time::Now();
  cert_report_->set_time_usec(now.ToDeltaSinceWindowsEpoch().InMicroseconds());
  cert_report_->set_hostname(hostname);

  if (!CertificateChainToString(cert, cert_report_->mutable_cert_chain())) {
    LOG(ERROR) << "Could not get PEM encoded chain.";
  }

  if (unverified_cert &&
      !CertificateChainToString(
          *unverified_cert, cert_report_->mutable_unverified_cert_chain())) {
    LOG(ERROR) << "Could not get PEM encoded unverified certificate chain.";
  }

  cert_report_->set_is_issued_by_known_root(is_issued_by_known_root);

  AddCertStatusToReportErrors(cert_status, cert_report_->mutable_cert_error());
  AddCertStatusToReportStatus(cert_status, cert_report_->mutable_cert_status());

#if defined(OS_ANDROID)
  chrome_browser_ssl::CertLoggerFeaturesInfo* features_info =
      cert_report_->mutable_features_info();
  features_info->set_android_aia_fetching_status(
      chrome_browser_ssl::CertLoggerFeaturesInfo::ANDROID_AIA_FETCHING_ENABLED);
#endif
}
