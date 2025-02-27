// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/download/mixed_content_download_blocking.h"

#include "base/debug/crash_logging.h"
#include "base/debug/dump_without_crashing.h"
#include "base/metrics/field_trial_params.h"
#include "base/metrics/histogram_functions.h"
#include "base/optional.h"
#include "base/strings/string_split.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "chrome/common/chrome_features.h"
#include "components/download/public/common/download_stats.h"
#include "content/public/browser/download_item_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/origin_util.h"
#include "third_party/blink/public/mojom/devtools/console_message.mojom.h"
#include "url/gurl.h"
#include "url/origin.h"

using download::DownloadSource;
using MixedContentStatus = download::DownloadItem::MixedContentStatus;

namespace {

// Configuration for which extensions to warn/block. These parameters are set
// differently for testing, so the listed defaults are only used when the flag
// is manually enabled (and in unit tests).
//
// Extensions must be in lower case! Extensions are compared against save path
// determined by Chrome prior to the user seeing a file picker.
//
// The extension list for each type (warn, block, silent block) can be
// configured in two ways: as an allowlist, or as a blocklist. When the
// extension list is a blocklist, extensions listed will trigger a
// warning/block. If the extension list is configured as an allowlist, all
// extensions EXCEPT those listed will trigger a warning/block.
//
// To make manual testing easier, the defaults are to have a small blocklist for
// block/silent block, and a small allowlist for warnings. This means that
// every mixed content download will at *least* generate a warning.
const base::FeatureParam<bool> kTreatSilentBlockListAsAllowlist(
    &features::kTreatUnsafeDownloadsAsActive,
    "TreatSilentBlockListAsAllowlist",
    false);
const base::FeatureParam<std::string> kSilentBlockExtensionList(
    &features::kTreatUnsafeDownloadsAsActive,
    "SilentBlockExtensionList",
    "silently_blocked_for_testing");

const base::FeatureParam<bool> kTreatBlockListAsAllowlist(
    &features::kTreatUnsafeDownloadsAsActive,
    "TreatBlockListAsAllowlist",
    false);
const base::FeatureParam<std::string> kBlockExtensionList(
    &features::kTreatUnsafeDownloadsAsActive,
    "BlockExtensionList",
    "exe,scr,msi,vb,dmg,pkg,crx,gz,gzip,zip,bz2,rar,7z,tar");

// Note: this is an allowlist, so acts as a catch-all.
const base::FeatureParam<bool> kTreatWarnListAsAllowlist(
    &features::kTreatUnsafeDownloadsAsActive,
    "TreatWarnListAsAllowlist",
    true);
const base::FeatureParam<std::string> kWarnExtensionList(
    &features::kTreatUnsafeDownloadsAsActive,
    "WarnExtensionList",
    "dont_warn_for_testing");

// Map the string file extension to the corresponding histogram enum.
InsecureDownloadExtensions GetExtensionEnumFromString(
    const std::string& extension) {
  if (extension.empty())
    return InsecureDownloadExtensions::kNone;

  auto lower_extension = base::ToLowerASCII(extension);
  for (auto candidate : kExtensionsToEnum) {
    if (candidate.extension == lower_extension)
      return candidate.value;
  }
  return InsecureDownloadExtensions::kUnknown;
}

// Get the appropriate histogram metric name for the initiator/download security
// state combo.
std::string GetDownloadBlockingExtensionMetricName(
    InsecureDownloadSecurityStatus status) {
  switch (status) {
    case InsecureDownloadSecurityStatus::kInitiatorUnknownFileSecure:
      return GetDLBlockingHistogramName(
          kInsecureDownloadExtensionInitiatorUnknown,
          kInsecureDownloadHistogramTargetSecure);
    case InsecureDownloadSecurityStatus::kInitiatorUnknownFileInsecure:
      return GetDLBlockingHistogramName(
          kInsecureDownloadExtensionInitiatorUnknown,
          kInsecureDownloadHistogramTargetInsecure);
    case InsecureDownloadSecurityStatus::kInitiatorSecureFileSecure:
      return GetDLBlockingHistogramName(
          kInsecureDownloadExtensionInitiatorSecure,
          kInsecureDownloadHistogramTargetSecure);
    case InsecureDownloadSecurityStatus::kInitiatorSecureFileInsecure:
      return GetDLBlockingHistogramName(
          kInsecureDownloadExtensionInitiatorSecure,
          kInsecureDownloadHistogramTargetInsecure);
    case InsecureDownloadSecurityStatus::kInitiatorInsecureFileSecure:
      return GetDLBlockingHistogramName(
          kInsecureDownloadExtensionInitiatorInsecure,
          kInsecureDownloadHistogramTargetSecure);
    case InsecureDownloadSecurityStatus::kInitiatorInsecureFileInsecure:
      return GetDLBlockingHistogramName(
          kInsecureDownloadExtensionInitiatorInsecure,
          kInsecureDownloadHistogramTargetInsecure);
    case InsecureDownloadSecurityStatus::kInitiatorInferredSecureFileSecure:
      return GetDLBlockingHistogramName(
          kInsecureDownloadExtensionInitiatorInferredSecure,
          kInsecureDownloadHistogramTargetSecure);
    case InsecureDownloadSecurityStatus::kInitiatorInferredSecureFileInsecure:
      return GetDLBlockingHistogramName(
          kInsecureDownloadExtensionInitiatorInferredSecure,
          kInsecureDownloadHistogramTargetInsecure);
    case InsecureDownloadSecurityStatus::kInitiatorInferredInsecureFileSecure:
      return GetDLBlockingHistogramName(
          kInsecureDownloadExtensionInitiatorInferredInsecure,
          kInsecureDownloadHistogramTargetSecure);
    case InsecureDownloadSecurityStatus::kInitiatorInferredInsecureFileInsecure:
      return GetDLBlockingHistogramName(
          kInsecureDownloadExtensionInitiatorInferredInsecure,
          kInsecureDownloadHistogramTargetInsecure);
    case InsecureDownloadSecurityStatus::kDownloadIgnored:
      NOTREACHED();
  }
  NOTREACHED();
  return std::string();
}

// Get appropriate enum value for the initiator/download security state combo
// for histogram reporting. |dl_secure| signifies whether the download was
// a secure source. |inferred| is whether the initiator value is our best guess.
InsecureDownloadSecurityStatus GetDownloadBlockingEnum(
    base::Optional<url::Origin> initiator,
    bool dl_secure,
    bool inferred) {
  if (inferred) {
    if (initiator->GetURL().SchemeIsCryptographic()) {
      if (dl_secure) {
        return InsecureDownloadSecurityStatus::
            kInitiatorInferredSecureFileSecure;
      }
      return InsecureDownloadSecurityStatus::
          kInitiatorInferredSecureFileInsecure;
    }

    if (dl_secure) {
      return InsecureDownloadSecurityStatus::
          kInitiatorInferredInsecureFileSecure;
    }
    return InsecureDownloadSecurityStatus::
        kInitiatorInferredInsecureFileInsecure;
  }

  if (!initiator.has_value()) {
    if (dl_secure)
      return InsecureDownloadSecurityStatus::kInitiatorUnknownFileSecure;
    return InsecureDownloadSecurityStatus::kInitiatorUnknownFileInsecure;
  }

  if (initiator->GetURL().SchemeIsCryptographic()) {
    if (dl_secure)
      return InsecureDownloadSecurityStatus::kInitiatorSecureFileSecure;
    return InsecureDownloadSecurityStatus::kInitiatorSecureFileInsecure;
  }

  if (dl_secure)
    return InsecureDownloadSecurityStatus::kInitiatorInsecureFileSecure;
  return InsecureDownloadSecurityStatus::kInitiatorInsecureFileInsecure;
}

struct MixedContentDownloadData {
  MixedContentDownloadData(const base::FilePath& path,
                           const download::DownloadItem* item)
      : item_(item) {
    // Configure initiator.
    bool initiator_inferred = false;
    initiator_ = item->GetRequestInitiator();
    if (!initiator_.has_value() && item->GetTabUrl().is_valid()) {
      initiator_inferred = true;
      initiator_ = url::Origin::Create(item->GetTabUrl());
    }

    // Extract extension.
#if defined(OS_WIN)
    extension_ = base::WideToUTF8(path.FinalExtension());
#else
    extension_ = path.FinalExtension();
#endif
    if (!extension_.empty()) {
      DCHECK_EQ(extension_[0], '.');
      extension_ = extension_.substr(1);  // Omit leading dot.
    }

    // Evaluate download security.
    is_redirect_chain_secure_ = true;
    for (const auto& url : item->GetUrlChain()) {
      if (!content::IsOriginSecure(url)) {
        is_redirect_chain_secure_ = false;
        break;
      }
    }
    const GURL& dl_url = item->GetURL();
    bool is_download_secure = is_redirect_chain_secure_ &&
                              (content::IsOriginSecure(dl_url) ||
                               dl_url.SchemeIsBlob() || dl_url.SchemeIsFile());

    // Configure mixed content status.
    // Some downloads don't qualify for blocking, and are thus never
    // mixed-content. At a minimum, this includes:
    //  - retries/reloads (since the original DL would have been blocked, and
    //    initiating context is lost on retry anyway),
    //  - anything triggered directly from the address bar or similar.
    //  - internal-Chrome downloads (e.g. downloading profile photos),
    //  - webview/CCT,
    //  - anything extension related,
    //  - etc.
    //
    // TODO(1029062): INTERNAL_API is also used for background fetch. That
    // probably isn't the correct behavior, since INTERNAL_API is otherwise used
    // for Chrome stuff. Background fetch should probably be HTTPS-only.
    auto download_source = item->GetDownloadSource();
    auto transition_type = item->GetTransitionType();
    if (download_source == DownloadSource::RETRY ||
        (transition_type & ui::PAGE_TRANSITION_RELOAD) ||
        (transition_type & ui::PAGE_TRANSITION_TYPED) ||
        (transition_type & ui::PAGE_TRANSITION_FROM_ADDRESS_BAR) ||
        (transition_type & ui::PAGE_TRANSITION_FORWARD_BACK) ||
        (transition_type & ui::PAGE_TRANSITION_AUTO_TOPLEVEL) ||
        (transition_type & ui::PAGE_TRANSITION_AUTO_BOOKMARK) ||
        (transition_type & ui::PAGE_TRANSITION_FROM_API) ||
        download_source == DownloadSource::OFFLINE_PAGE ||
        download_source == DownloadSource::INTERNAL_API ||
        download_source == DownloadSource::EXTENSION_API ||
        download_source == DownloadSource::EXTENSION_INSTALLER) {
      base::UmaHistogramEnumeration(
          kInsecureDownloadHistogramName,
          InsecureDownloadSecurityStatus::kDownloadIgnored);
      is_mixed_content_ = false;
    } else {  // Not ignorable download.
      // Record some metrics first.
      auto security_status = GetDownloadBlockingEnum(
          initiator_, is_download_secure, initiator_inferred);
      base::UmaHistogramEnumeration(
          GetDownloadBlockingExtensionMetricName(security_status),
          GetExtensionEnumFromString(extension_));
      base::UmaHistogramEnumeration(kInsecureDownloadHistogramName,
                                    security_status);
      download::RecordDownloadValidationMetrics(
          download::DownloadMetricsCallsite::kMixContentDownloadBlocking,
          download::CheckDownloadConnectionSecurity(item->GetURL(),
                                                    item->GetUrlChain()),
          download::DownloadContentFromMimeType(item->GetMimeType(), false));

      is_mixed_content_ =
          (initiator_.has_value() &&
           initiator_->GetURL().SchemeIsCryptographic() && !is_download_secure);
    }
  }

  base::Optional<url::Origin> initiator_;
  std::string extension_;
  const download::DownloadItem* item_;
  bool is_redirect_chain_secure_;
  bool is_mixed_content_;
};

// Whether or not |extension| is contained in the comma-separated list in a
// feature param specified by |override_param_name|. If |override_param_name| is
// not set, defaults to |default_extensions|.
bool ContainsExtension(const base::FeatureParam<std::string>& extensions,
                       const base::FeatureParam<bool>& is_allowlist,
                       const std::string& download_extension) {
  auto extensions_str = extensions.Get();
  std::vector<base::StringPiece> listed_extensions = base::SplitStringPiece(
      extensions_str, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);

  for (const auto& unsafe_extension : listed_extensions) {
    DCHECK_EQ(base::ToLowerASCII(unsafe_extension), unsafe_extension);
    if (base::LowerCaseEqualsASCII(download_extension, unsafe_extension)) {
      return !is_allowlist.Get();  // aka true when it's a blocklist.
    }
  }

  return is_allowlist.Get();  // aka false when it's a blocklist.
}

// Just print a descriptive message to the console about the blocked download.
// |is_blocked| indicates whether this download will be blocked now.
void PrintConsoleMessage(const MixedContentDownloadData& data,
                         bool is_blocked) {
  content::WebContents* web_contents =
      content::DownloadItemUtils::GetWebContents(data.item_);
  if (!web_contents) {
    return;
  }

  web_contents->GetMainFrame()->AddMessageToConsole(
      blink::mojom::ConsoleMessageLevel::kError,
      base::StringPrintf(
          "Mixed Content: The site at '%s' was loaded over a secure "
          "connection, but the file at '%s' was %s an insecure "
          "connection. This file should be served over HTTPS. "
          "This download %s.",
          data.initiator_->GetURL().spec().c_str(),
          data.item_->GetURL().spec().c_str(),
          (data.is_redirect_chain_secure_ ? "loaded over"
                                          : "redirected through"),
          (is_blocked ? "has been blocked"
                      : "will be blocked in future versions of Chrome")));
}

}  // namespace

MixedContentStatus GetMixedContentStatusForDownload(
    const base::FilePath& path,
    const download::DownloadItem* item) {
  MixedContentDownloadData data(path, item);

  if (!data.is_mixed_content_) {
    return MixedContentStatus::SAFE;
  }

  // As of M81, print a console message even if no other blocking is enabled.
  if (!base::FeatureList::IsEnabled(features::kTreatUnsafeDownloadsAsActive)) {
    PrintConsoleMessage(data, false);
    return MixedContentStatus::SAFE;
  }

  if (ContainsExtension(kSilentBlockExtensionList,
                        kTreatSilentBlockListAsAllowlist, data.extension_)) {
    PrintConsoleMessage(data, true);

    // Only permit silent blocking when not initiated by an explicit user
    // action.  Otherwise, fall back to visible blocking.
    auto download_source = data.item_->GetDownloadSource();
    if (download_source == DownloadSource::CONTEXT_MENU ||
        download_source == DownloadSource::WEB_CONTENTS_API) {
      return MixedContentStatus::BLOCK;
    }

    return MixedContentStatus::SILENT_BLOCK;
  }

  if (ContainsExtension(kBlockExtensionList, kTreatBlockListAsAllowlist,
                        data.extension_)) {
    PrintConsoleMessage(data, true);
    return MixedContentStatus::BLOCK;
  }

  if (ContainsExtension(kWarnExtensionList, kTreatWarnListAsAllowlist,
                        data.extension_)) {
    PrintConsoleMessage(data, true);
    return MixedContentStatus::WARN;
  }

  // The download is still mixed content, but we're not blocking it yet.
  PrintConsoleMessage(data, false);
  return MixedContentStatus::SAFE;
}
