// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/download/public/common/download_stats.h"

#include <map>

#include "base/callback.h"
#include "base/files/file_path.h"
#include "base/metrics/histogram_functions.h"
#include "base/metrics/histogram_macros.h"
#include "base/stl_util.h"
#include "base/strings/string_util.h"
#include "build/build_config.h"
#include "components/download/public/common/download_interrupt_reasons.h"
#include "net/http/http_content_disposition.h"
#include "net/http/http_util.h"

namespace download {

namespace {

// The maximium value for download deletion retention time histogram.
const int kMaxDeletionRetentionHours = 720;

// All possible error codes from the network module. Note that the error codes
// are all positive (since histograms expect positive sample values).
const int kAllInterruptReasonCodes[] = {
#define INTERRUPT_REASON(label, value) (value),
#include "components/download/public/common/download_interrupt_reason_values.h"
#undef INTERRUPT_REASON
};

// These values are based on net::HttpContentDisposition::ParseResult values.
// Values other than HEADER_PRESENT and IS_VALID are only measured if |IS_VALID|
// is true.
enum ContentDispositionCountTypes {
  // Count of downloads which had a Content-Disposition headers. The total
  // number of downloads is measured by UNTHROTTLED_COUNT.
  CONTENT_DISPOSITION_HEADER_PRESENT = 0,

  // Either 'filename' or 'filename*' attributes were valid and
  // yielded a non-empty filename.
  CONTENT_DISPOSITION_IS_VALID,

  // The following enum values correspond to
  // net::HttpContentDisposition::ParseResult.
  CONTENT_DISPOSITION_HAS_DISPOSITION_TYPE,
  CONTENT_DISPOSITION_HAS_UNKNOWN_TYPE,

  CONTENT_DISPOSITION_HAS_NAME,  // Obsolete; kept for UMA compatiblity.

  CONTENT_DISPOSITION_HAS_FILENAME,
  CONTENT_DISPOSITION_HAS_EXT_FILENAME,
  CONTENT_DISPOSITION_HAS_NON_ASCII_STRINGS,
  CONTENT_DISPOSITION_HAS_PERCENT_ENCODED_STRINGS,
  CONTENT_DISPOSITION_HAS_RFC2047_ENCODED_STRINGS,

  CONTENT_DISPOSITION_HAS_NAME_ONLY,  // Obsolete; kept for UMA compatiblity.

  CONTENT_DISPOSITION_HAS_SINGLE_QUOTED_FILENAME,

  CONTENT_DISPOSITION_LAST_ENTRY
};

void RecordContentDispositionCount(ContentDispositionCountTypes type,
                                   bool record) {
  if (!record)
    return;
  UMA_HISTOGRAM_ENUMERATION("Download.ContentDisposition", type,
                            CONTENT_DISPOSITION_LAST_ENTRY);
}

void RecordContentDispositionCountFlag(
    ContentDispositionCountTypes type,
    int flags_to_test,
    net::HttpContentDisposition::ParseResultFlags flag) {
  RecordContentDispositionCount(type, (flags_to_test & flag) == flag);
}

// The maximum size in KB for the file size metric, file size larger than this
// will be kept in overflow bucket.
const int64_t kMaxFileSizeKb = 4 * 1024 * 1024; /* 4GB. */

const int64_t kHighBandwidthBytesPerSecond = 30 * 1024 * 1024;

// Helper method to calculate the bandwidth given the data length and time.
int64_t CalculateBandwidthBytesPerSecond(size_t length,
                                         base::TimeDelta elapsed_time) {
  int64_t elapsed_time_ms = elapsed_time.InMilliseconds();
  if (0 == elapsed_time_ms)
    elapsed_time_ms = 1;
  return 1000 * static_cast<int64_t>(length) / elapsed_time_ms;
}

// Helper method to record the bandwidth for a given metric.
void RecordBandwidthMetric(const std::string& metric, int bandwidth) {
  base::UmaHistogramCustomCounts(metric, bandwidth, 1, 50 * 1000 * 1000, 50);
}

// Records a histogram with download source suffix.
std::string CreateHistogramNameWithSuffix(const std::string& name,
                                          DownloadSource download_source) {
  std::string suffix;
  switch (download_source) {
    case DownloadSource::UNKNOWN:
      suffix = "UnknownSource";
      break;
    case DownloadSource::NAVIGATION:
      suffix = "Navigation";
      break;
    case DownloadSource::DRAG_AND_DROP:
      suffix = "DragAndDrop";
      break;
    case DownloadSource::FROM_RENDERER:
      suffix = "FromRenderer";
      break;
    case DownloadSource::EXTENSION_API:
      suffix = "ExtensionAPI";
      break;
    case DownloadSource::EXTENSION_INSTALLER:
      suffix = "ExtensionInstaller";
      break;
    case DownloadSource::INTERNAL_API:
      suffix = "InternalAPI";
      break;
    case DownloadSource::WEB_CONTENTS_API:
      suffix = "WebContentsAPI";
      break;
    case DownloadSource::OFFLINE_PAGE:
      suffix = "OfflinePage";
      break;
    case DownloadSource::CONTEXT_MENU:
      suffix = "ContextMenu";
      break;
    case DownloadSource::RETRY:
      suffix = "Retry";
      break;
  }

  return name + "." + suffix;
}

}  // namespace

void RecordDownloadCount(DownloadCountTypes type) {
  UMA_HISTOGRAM_ENUMERATION("Download.Counts", type,
                            DOWNLOAD_COUNT_TYPES_LAST_ENTRY);
}

void RecordDownloadCountWithSource(DownloadCountTypes type,
                                   DownloadSource download_source) {
  RecordDownloadCount(type);

  std::string name =
      CreateHistogramNameWithSuffix("Download.Counts", download_source);
  base::UmaHistogramEnumeration(name, type, DOWNLOAD_COUNT_TYPES_LAST_ENTRY);
}

void RecordDownloadCompleted(int64_t download_len,
                             bool is_parallelizable,
                             DownloadSource download_source,
                             bool has_resumed,
                             bool has_strong_validators) {
  RecordDownloadCountWithSource(COMPLETED_COUNT, download_source);
  int64_t max = 1024 * 1024 * 1024;  // One Terabyte.
  download_len /= 1024;              // In Kilobytes
  UMA_HISTOGRAM_CUSTOM_COUNTS("Download.DownloadSize", download_len, 1, max,
                              256);
  if (is_parallelizable) {
    UMA_HISTOGRAM_CUSTOM_COUNTS("Download.DownloadSize.Parallelizable",
                                download_len, 1, max, 256);
  }

  if (has_resumed) {
    base::UmaHistogramBoolean("Download.ResumptionComplete.HasStrongValidators",
                              has_strong_validators);
  }
}

void RecordDownloadDeletion(base::Time completion_time,
                            const std::string& mime_type) {
  if (completion_time == base::Time())
    return;

  // Records how long the user keeps media files on disk.
  base::TimeDelta retention_time = base::Time::Now() - completion_time;
  int retention_hours = retention_time.InHours();

  DownloadContent type = DownloadContentFromMimeType(mime_type, false);
  if (type == DownloadContent::VIDEO) {
    UMA_HISTOGRAM_CUSTOM_COUNTS("Download.DeleteRetentionTime.Video",
                                retention_hours, 1, kMaxDeletionRetentionHours,
                                50);
  }
  if (type == DownloadContent::AUDIO) {
    UMA_HISTOGRAM_CUSTOM_COUNTS("Download.DeleteRetentionTime.Audio",
                                retention_hours, 1, kMaxDeletionRetentionHours,
                                50);
  }
}

void RecordDownloadInterrupted(DownloadInterruptReason reason,
                               int64_t received,
                               int64_t total,
                               bool is_parallelizable,
                               bool is_parallel_download_enabled,
                               DownloadSource download_source) {
  RecordDownloadCountWithSource(INTERRUPTED_COUNT, download_source);
  if (is_parallelizable) {
    RecordParallelizableDownloadCount(INTERRUPTED_COUNT,
                                      is_parallel_download_enabled);
  }

  std::vector<base::HistogramBase::Sample> samples =
      base::CustomHistogram::ArrayToCustomEnumRanges(kAllInterruptReasonCodes);
  UMA_HISTOGRAM_CUSTOM_ENUMERATION("Download.InterruptedReason", reason,
                                   samples);

  std::string name = CreateHistogramNameWithSuffix("Download.InterruptedReason",
                                                   download_source);
  base::HistogramBase* counter = base::CustomHistogram::FactoryGet(
      name, samples, base::HistogramBase::kUmaTargetedHistogramFlag);
  counter->Add(reason);

  if (is_parallel_download_enabled) {
    UMA_HISTOGRAM_CUSTOM_ENUMERATION(
        "Download.InterruptedReason.ParallelDownload", reason, samples);
  }

  // The maximum should be 2^kBuckets, to have the logarithmic bucket
  // boundaries fall on powers of 2.
  static const int kBuckets = 30;
  static const int64_t kMaxKb = 1 << kBuckets;  // One Terabyte, in Kilobytes.
  int64_t delta_bytes = total - received;
  bool unknown_size = total <= 0;
  int64_t received_kb = received / 1024;
  int64_t total_kb = total / 1024;
  if (is_parallel_download_enabled) {
    UMA_HISTOGRAM_CUSTOM_COUNTS(
        "Download.InterruptedReceivedSizeK.ParallelDownload", received_kb, 1,
        kMaxKb, kBuckets);
  }

  if (!unknown_size) {
    UMA_HISTOGRAM_CUSTOM_COUNTS("Download.InterruptedTotalSizeK", total_kb, 1,
                                kMaxKb, kBuckets);
    if (is_parallel_download_enabled) {
      UMA_HISTOGRAM_CUSTOM_COUNTS(
          "Download.InterruptedTotalSizeK.ParallelDownload", total_kb, 1,
          kMaxKb, kBuckets);
    }
    if (delta_bytes == 0) {
      RecordDownloadCountWithSource(INTERRUPTED_AT_END_COUNT, download_source);
      if (is_parallelizable) {
        RecordParallelizableDownloadCount(INTERRUPTED_AT_END_COUNT,
                                          is_parallel_download_enabled);
      }
    }
  }
}

namespace {

int GetMimeTypeMatch(const std::string& mime_type_string,
                     std::map<std::string, int> mime_type_map) {
  for (const auto& entry : mime_type_map) {
    if (entry.first == mime_type_string) {
      return entry.second;
    }
  }
  return 0;
}

static std::map<std::string, DownloadContent>
getMimeTypeToDownloadContentMap() {
  return {
      {"application/octet-stream", DownloadContent::OCTET_STREAM},
      {"binary/octet-stream", DownloadContent::OCTET_STREAM},
      {"application/pdf", DownloadContent::PDF},
      {"application/msword", DownloadContent::DOCUMENT},
      {"application/"
       "vnd.openxmlformats-officedocument.wordprocessingml.document",
       DownloadContent::DOCUMENT},
      {"application/rtf", DownloadContent::DOCUMENT},
      {"application/vnd.oasis.opendocument.text", DownloadContent::DOCUMENT},
      {"application/vnd.google-apps.document", DownloadContent::DOCUMENT},
      {"application/vnd.ms-excel", DownloadContent::SPREADSHEET},
      {"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
       DownloadContent::SPREADSHEET},
      {"application/vnd.oasis.opendocument.spreadsheet",
       DownloadContent::SPREADSHEET},
      {"application/vnd.google-apps.spreadsheet", DownloadContent::SPREADSHEET},
      {"application/vns.ms-powerpoint", DownloadContent::PRESENTATION},
      {"application/"
       "vnd.openxmlformats-officedocument.presentationml.presentation",
       DownloadContent::PRESENTATION},
      {"application/vnd.oasis.opendocument.presentation",
       DownloadContent::PRESENTATION},
      {"application/vnd.google-apps.presentation",
       DownloadContent::PRESENTATION},
      {"application/zip", DownloadContent::ARCHIVE},
      {"application/x-gzip", DownloadContent::ARCHIVE},
      {"application/x-rar-compressed", DownloadContent::ARCHIVE},
      {"application/x-tar", DownloadContent::ARCHIVE},
      {"application/x-bzip", DownloadContent::ARCHIVE},
      {"application/x-bzip2", DownloadContent::ARCHIVE},
      {"application/x-7z-compressed", DownloadContent::ARCHIVE},
      {"application/x-exe", DownloadContent::EXECUTABLE},
      {"application/java-archive", DownloadContent::EXECUTABLE},
      {"application/vnd.apple.installer+xml", DownloadContent::EXECUTABLE},
      {"application/x-csh", DownloadContent::EXECUTABLE},
      {"application/x-sh", DownloadContent::EXECUTABLE},
      {"application/x-apple-diskimage", DownloadContent::DMG},
      {"application/x-chrome-extension", DownloadContent::CRX},
      {"application/xhtml+xml", DownloadContent::WEB},
      {"application/xml", DownloadContent::WEB},
      {"application/javascript", DownloadContent::WEB},
      {"application/json", DownloadContent::WEB},
      {"application/typescript", DownloadContent::WEB},
      {"application/vnd.mozilla.xul+xml", DownloadContent::WEB},
      {"application/vnd.amazon.ebook", DownloadContent::EBOOK},
      {"application/epub+zip", DownloadContent::EBOOK},
      {"application/vnd.android.package-archive", DownloadContent::APK}};
}

// NOTE: Keep in sync with DownloadImageType in
// tools/metrics/histograms/enums.xml.
enum DownloadImage {
  DOWNLOAD_IMAGE_UNRECOGNIZED = 0,
  DOWNLOAD_IMAGE_GIF = 1,
  DOWNLOAD_IMAGE_JPEG = 2,
  DOWNLOAD_IMAGE_PNG = 3,
  DOWNLOAD_IMAGE_TIFF = 4,
  DOWNLOAD_IMAGE_ICON = 5,
  DOWNLOAD_IMAGE_WEBP = 6,
  DOWNLOAD_IMAGE_PSD = 7,
  DOWNLOAD_IMAGE_SVG = 8,
  DOWNLOAD_IMAGE_MAX = 9,
};

static std::map<std::string, int> getMimeTypeToDownloadImageMap() {
  return {{"image/gif", DOWNLOAD_IMAGE_GIF},
          {"image/jpeg", DOWNLOAD_IMAGE_JPEG},
          {"image/png", DOWNLOAD_IMAGE_PNG},
          {"image/tiff", DOWNLOAD_IMAGE_TIFF},
          {"image/vnd.microsoft.icon", DOWNLOAD_IMAGE_ICON},
          {"image/x-icon", DOWNLOAD_IMAGE_ICON},
          {"image/webp", DOWNLOAD_IMAGE_WEBP},
          {"image/vnd.adobe.photoshop", DOWNLOAD_IMAGE_PSD},
          {"image/svg+xml", DOWNLOAD_IMAGE_SVG}};
}

void RecordDownloadImageType(const std::string& mime_type_string) {
  DownloadImage download_image = DownloadImage(
      GetMimeTypeMatch(mime_type_string, getMimeTypeToDownloadImageMap()));
  UMA_HISTOGRAM_ENUMERATION("Download.ContentType.Image", download_image,
                            DOWNLOAD_IMAGE_MAX);
}

/** Text categories **/

// NOTE: Keep in sync with DownloadTextType in
// tools/metrics/histograms/enums.xml.
enum DownloadText {
  DOWNLOAD_TEXT_UNRECOGNIZED = 0,
  DOWNLOAD_TEXT_PLAIN = 1,
  DOWNLOAD_TEXT_CSS = 2,
  DOWNLOAD_TEXT_CSV = 3,
  DOWNLOAD_TEXT_HTML = 4,
  DOWNLOAD_TEXT_CALENDAR = 5,
  DOWNLOAD_TEXT_MAX = 6,
};

static std::map<std::string, int> getMimeTypeToDownloadTextMap() {
  return {{"text/plain", DOWNLOAD_TEXT_PLAIN},
          {"text/css", DOWNLOAD_TEXT_CSS},
          {"text/csv", DOWNLOAD_TEXT_CSV},
          {"text/html", DOWNLOAD_TEXT_HTML},
          {"text/calendar", DOWNLOAD_TEXT_CALENDAR}};
}

void RecordDownloadTextType(const std::string& mime_type_string) {
  DownloadText download_text = DownloadText(
      GetMimeTypeMatch(mime_type_string, getMimeTypeToDownloadTextMap()));
  UMA_HISTOGRAM_ENUMERATION("Download.ContentType.Text", download_text,
                            DOWNLOAD_TEXT_MAX);
}

/* Audio categories */

// NOTE: Keep in sync with DownloadAudioType in
// tools/metrics/histograms/enums.xml.
enum DownloadAudio {
  DOWNLOAD_AUDIO_UNRECOGNIZED = 0,
  DOWNLOAD_AUDIO_AAC = 1,
  DOWNLOAD_AUDIO_MIDI = 2,
  DOWNLOAD_AUDIO_OGA = 3,
  DOWNLOAD_AUDIO_WAV = 4,
  DOWNLOAD_AUDIO_WEBA = 5,
  DOWNLOAD_AUDIO_3GP = 6,
  DOWNLOAD_AUDIO_3G2 = 7,
  DOWNLOAD_AUDIO_MP3 = 8,
  DOWNLOAD_AUDIO_MAX = 9,
};

static std::map<std::string, int> getMimeTypeToDownloadAudioMap() {
  return {
      {"audio/aac", DOWNLOAD_AUDIO_AAC},   {"audio/midi", DOWNLOAD_AUDIO_MIDI},
      {"audio/ogg", DOWNLOAD_AUDIO_OGA},   {"audio/x-wav", DOWNLOAD_AUDIO_WAV},
      {"audio/webm", DOWNLOAD_AUDIO_WEBA}, {"audio/3gpp", DOWNLOAD_AUDIO_3GP},
      {"audio/3gpp2", DOWNLOAD_AUDIO_3G2}, {"audio/mp3", DOWNLOAD_AUDIO_MP3}};
}

void RecordDownloadAudioType(const std::string& mime_type_string) {
  DownloadAudio download_audio = DownloadAudio(
      GetMimeTypeMatch(mime_type_string, getMimeTypeToDownloadAudioMap()));
  UMA_HISTOGRAM_ENUMERATION("Download.ContentType.Audio", download_audio,
                            DOWNLOAD_AUDIO_MAX);
}

/* Video categories */

// NOTE: Keep in sync with DownloadVideoType in
// tools/metrics/histograms/enums.xml.
enum DownloadVideo {
  DOWNLOAD_VIDEO_UNRECOGNIZED = 0,
  DOWNLOAD_VIDEO_AVI = 1,
  DOWNLOAD_VIDEO_MPEG = 2,
  DOWNLOAD_VIDEO_OGV = 3,
  DOWNLOAD_VIDEO_WEBM = 4,
  DOWNLOAD_VIDEO_3GP = 5,
  DOWNLOAD_VIDEO_3G2 = 6,
  DOWNLOAD_VIDEO_MP4 = 7,
  DOWNLOAD_VIDEO_MOV = 8,
  DOWNLOAD_VIDEO_WMV = 9,
  DOWNLOAD_VIDEO_MAX = 10,
};

static std::map<std::string, int> getMimeTypeToDownloadVideoMap() {
  return {{"video/x-msvideo", DOWNLOAD_VIDEO_AVI},
          {"video/mpeg", DOWNLOAD_VIDEO_MPEG},
          {"video/ogg", DOWNLOAD_VIDEO_OGV},
          {"video/webm", DOWNLOAD_VIDEO_WEBM},
          {"video/3gpp", DOWNLOAD_VIDEO_3GP},
          {"video/3ggp2", DOWNLOAD_VIDEO_3G2},
          {"video/mp4", DOWNLOAD_VIDEO_MP4},
          {"video/quicktime", DOWNLOAD_VIDEO_MOV},
          {"video/x-ms-wmv", DOWNLOAD_VIDEO_WMV}};
}

void RecordDownloadVideoType(const std::string& mime_type_string) {
  DownloadVideo download_video = DownloadVideo(
      GetMimeTypeMatch(mime_type_string, getMimeTypeToDownloadVideoMap()));
  UMA_HISTOGRAM_ENUMERATION("Download.ContentType.Video", download_video,
                            DOWNLOAD_VIDEO_MAX);
}

// These histograms summarize download mime-types. The same data is recorded in
// a few places, as they exist to sanity-check and understand other metrics.
const char* const kDownloadMetricsVerificationNameItemSecure =
    "Download.InsecureBlocking.Verification.Item.Secure";
const char* const kDownloadMetricsVerificationNameItemInsecure =
    "Download.InsecureBlocking.Verification.Item.Insecure";
const char* const kDownloadMetricsVerificationNameItemOther =
    "Download.InsecureBlocking.Verification.Item.Other";
const char* const kDownloadMetricsVerificationNameManagerSecure =
    "Download.InsecureBlocking.Verification.Manager.Secure";
const char* const kDownloadMetricsVerificationNameManagerInsecure =
    "Download.InsecureBlocking.Verification.Manager.Insecure";
const char* const kDownloadMetricsVerificationNameManagerOther =
    "Download.InsecureBlocking.Verification.Manager.Other";

const char* GetDownloadValidationMetricName(
    const DownloadMetricsCallsite& callsite,
    const DownloadConnectionSecurity& state) {
  DCHECK(callsite == DownloadMetricsCallsite::kDownloadItem ||
         callsite == DownloadMetricsCallsite::kMixContentDownloadBlocking);

  switch (state) {
    case DOWNLOAD_SECURE:
    case DOWNLOAD_TARGET_BLOB:
    case DOWNLOAD_TARGET_DATA:
    case DOWNLOAD_TARGET_FILE:
      if (callsite == DownloadMetricsCallsite::kDownloadItem)
        return kDownloadMetricsVerificationNameItemSecure;
      return kDownloadMetricsVerificationNameManagerSecure;
    case DOWNLOAD_TARGET_INSECURE:
    case DOWNLOAD_REDIRECT_INSECURE:
    case DOWNLOAD_REDIRECT_TARGET_INSECURE:
      if (callsite == DownloadMetricsCallsite::kDownloadItem)
        return kDownloadMetricsVerificationNameItemInsecure;
      return kDownloadMetricsVerificationNameManagerInsecure;
    case DOWNLOAD_TARGET_OTHER:
    case DOWNLOAD_TARGET_FILESYSTEM:
    case DOWNLOAD_TARGET_FTP:
      if (callsite == DownloadMetricsCallsite::kDownloadItem)
        return kDownloadMetricsVerificationNameItemOther;
      return kDownloadMetricsVerificationNameManagerOther;
    case DOWNLOAD_CONNECTION_SECURITY_MAX:
      NOTREACHED();
  }
  NOTREACHED();
  return nullptr;
}

}  // namespace

DownloadContent DownloadContentFromMimeType(const std::string& mime_type_string,
                                            bool record_content_subcategory) {
  DownloadContent download_content = DownloadContent::UNRECOGNIZED;
  for (const auto& entry : getMimeTypeToDownloadContentMap()) {
    if (entry.first == mime_type_string) {
      download_content = entry.second;
    }
  }

  // Do partial matches.
  if (download_content == DownloadContent::UNRECOGNIZED) {
    if (base::StartsWith(mime_type_string, "text/",
                         base::CompareCase::SENSITIVE)) {
      download_content = DownloadContent::TEXT;
      if (record_content_subcategory)
        RecordDownloadTextType(mime_type_string);
    } else if (base::StartsWith(mime_type_string, "image/",
                                base::CompareCase::SENSITIVE)) {
      download_content = DownloadContent::IMAGE;
      if (record_content_subcategory)
        RecordDownloadImageType(mime_type_string);
    } else if (base::StartsWith(mime_type_string, "audio/",
                                base::CompareCase::SENSITIVE)) {
      download_content = DownloadContent::AUDIO;
      if (record_content_subcategory)
        RecordDownloadAudioType(mime_type_string);
    } else if (base::StartsWith(mime_type_string, "video/",
                                base::CompareCase::SENSITIVE)) {
      download_content = DownloadContent::VIDEO;
      if (record_content_subcategory)
        RecordDownloadVideoType(mime_type_string);
    } else if (base::StartsWith(mime_type_string, "font/",
                                base::CompareCase::SENSITIVE)) {
      download_content = DownloadContent::FONT;
    }
  }

  return download_content;
}

void RecordDownloadMimeType(const std::string& mime_type_string) {
  DownloadContent download_content =
      DownloadContentFromMimeType(mime_type_string, true);
  UMA_HISTOGRAM_ENUMERATION("Download.Start.ContentType", download_content,
                            DownloadContent::MAX);
}

void RecordDownloadMimeTypeForNormalProfile(
    const std::string& mime_type_string) {
  UMA_HISTOGRAM_ENUMERATION(
      "Download.Start.ContentType.NormalProfile",
      DownloadContentFromMimeType(mime_type_string, false),
      DownloadContent::MAX);
}

void RecordDownloadContentDisposition(
    const std::string& content_disposition_string) {
  if (content_disposition_string.empty())
    return;
  net::HttpContentDisposition content_disposition(content_disposition_string,
                                                  std::string());
  int result = content_disposition.parse_result_flags();

  bool is_valid = !content_disposition.filename().empty();
  RecordContentDispositionCount(CONTENT_DISPOSITION_HEADER_PRESENT, true);
  RecordContentDispositionCount(CONTENT_DISPOSITION_IS_VALID, is_valid);
  if (!is_valid)
    return;

  RecordContentDispositionCountFlag(
      CONTENT_DISPOSITION_HAS_DISPOSITION_TYPE, result,
      net::HttpContentDisposition::HAS_DISPOSITION_TYPE);
  RecordContentDispositionCountFlag(
      CONTENT_DISPOSITION_HAS_UNKNOWN_TYPE, result,
      net::HttpContentDisposition::HAS_UNKNOWN_DISPOSITION_TYPE);
  RecordContentDispositionCountFlag(CONTENT_DISPOSITION_HAS_FILENAME, result,
                                    net::HttpContentDisposition::HAS_FILENAME);
  RecordContentDispositionCountFlag(
      CONTENT_DISPOSITION_HAS_EXT_FILENAME, result,
      net::HttpContentDisposition::HAS_EXT_FILENAME);
  RecordContentDispositionCountFlag(
      CONTENT_DISPOSITION_HAS_NON_ASCII_STRINGS, result,
      net::HttpContentDisposition::HAS_NON_ASCII_STRINGS);
  RecordContentDispositionCountFlag(
      CONTENT_DISPOSITION_HAS_PERCENT_ENCODED_STRINGS, result,
      net::HttpContentDisposition::HAS_PERCENT_ENCODED_STRINGS);
  RecordContentDispositionCountFlag(
      CONTENT_DISPOSITION_HAS_RFC2047_ENCODED_STRINGS, result,
      net::HttpContentDisposition::HAS_RFC2047_ENCODED_STRINGS);
  RecordContentDispositionCountFlag(
      CONTENT_DISPOSITION_HAS_SINGLE_QUOTED_FILENAME, result,
      net::HttpContentDisposition::HAS_SINGLE_QUOTED_FILENAME);
}

void RecordOpen(const base::Time& end) {
  if (!end.is_null())
    UMA_HISTOGRAM_LONG_TIMES("Download.OpenTime", (base::Time::Now() - end));
}

void RecordOpensOutstanding(int size) {
  UMA_HISTOGRAM_CUSTOM_COUNTS("Download.OpensOutstanding", size, 1 /*min*/,
                              (1 << 10) /*max*/, 64 /*num_buckets*/);
}

void RecordFileBandwidth(size_t length,
                         base::TimeDelta elapsed_time) {
  RecordBandwidthMetric("Download.BandwidthOverallBytesPerSecond",
                        CalculateBandwidthBytesPerSecond(length, elapsed_time));
}

void RecordParallelizableDownloadCount(DownloadCountTypes type,
                                       bool is_parallel_download_enabled) {
  std::string histogram_name = is_parallel_download_enabled
                                   ? "Download.Counts.ParallelDownload"
                                   : "Download.Counts.ParallelizableDownload";
  base::UmaHistogramEnumeration(histogram_name, type,
                                DOWNLOAD_COUNT_TYPES_LAST_ENTRY);
}

void RecordParallelDownloadRequestCount(int request_count) {
  UMA_HISTOGRAM_CUSTOM_COUNTS("Download.ParallelDownloadRequestCount",
                              request_count, 1, 10, 11);
}

void RecordParallelDownloadAddStreamSuccess(bool success,
                                            bool support_range_request) {
  if (support_range_request) {
    base::UmaHistogramBoolean("Download.ParallelDownloadAddStreamSuccess",
                              success);
  } else {
    base::UmaHistogramBoolean(
        "Download.ParallelDownloadAddStreamSuccess.NoAcceptRangesHeader",
        success);
  }
}

void RecordParallelRequestCreationFailure(DownloadInterruptReason reason) {
  base::UmaHistogramSparse("Download.ParallelDownload.CreationFailureReason",
                           reason);
}

void RecordParallelizableContentLength(int64_t content_length) {
  UMA_HISTOGRAM_CUSTOM_COUNTS("Download.ContentLength.Parallelizable",
                              content_length / 1024, 1, kMaxFileSizeKb, 50);
}

void RecordParallelizableDownloadStats(
    size_t bytes_downloaded_with_parallel_streams,
    base::TimeDelta time_with_parallel_streams,
    size_t bytes_downloaded_without_parallel_streams,
    base::TimeDelta time_without_parallel_streams,
    bool uses_parallel_requests) {
  RecordParallelizableDownloadAverageStats(
      bytes_downloaded_with_parallel_streams +
          bytes_downloaded_without_parallel_streams,
      time_with_parallel_streams + time_without_parallel_streams);

  int64_t bandwidth_without_parallel_streams = 0;
  if (bytes_downloaded_without_parallel_streams > 0) {
    bandwidth_without_parallel_streams = CalculateBandwidthBytesPerSecond(
        bytes_downloaded_without_parallel_streams,
        time_without_parallel_streams);
    if (uses_parallel_requests) {
      RecordBandwidthMetric(
          "Download.ParallelizableDownloadBandwidth."
          "WithParallelRequestsSingleStream",
          bandwidth_without_parallel_streams);
    } else {
      RecordBandwidthMetric(
          "Download.ParallelizableDownloadBandwidth."
          "WithoutParallelRequests",
          bandwidth_without_parallel_streams);
    }
  }

  if (!uses_parallel_requests)
    return;

  base::TimeDelta time_saved;
  if (bytes_downloaded_with_parallel_streams > 0) {
    int64_t bandwidth_with_parallel_streams = CalculateBandwidthBytesPerSecond(
        bytes_downloaded_with_parallel_streams, time_with_parallel_streams);
    RecordBandwidthMetric(
        "Download.ParallelizableDownloadBandwidth."
        "WithParallelRequestsMultipleStreams",
        bandwidth_with_parallel_streams);
    if (bandwidth_without_parallel_streams > 0) {
      time_saved = base::TimeDelta::FromMilliseconds(
                       1000.0 * bytes_downloaded_with_parallel_streams /
                       bandwidth_without_parallel_streams) -
                   time_with_parallel_streams;
    }
  }

  int kMillisecondsPerHour =
      base::checked_cast<int>(base::Time::kMillisecondsPerSecond * 60 * 60);
  if (time_saved >= base::TimeDelta()) {
    UMA_HISTOGRAM_CUSTOM_COUNTS(
        "Download.EstimatedTimeSavedWithParallelDownload",
        time_saved.InMilliseconds(), 0, kMillisecondsPerHour, 50);
  }
}

void RecordParallelizableDownloadAverageStats(
    int64_t bytes_downloaded,
    const base::TimeDelta& time_span) {
  if (time_span.is_zero() || bytes_downloaded <= 0)
    return;

  int64_t average_bandwidth =
      CalculateBandwidthBytesPerSecond(bytes_downloaded, time_span);
  int64_t file_size_kb = bytes_downloaded / 1024;
  RecordBandwidthMetric("Download.ParallelizableDownloadBandwidth",
                        average_bandwidth);
  UMA_HISTOGRAM_LONG_TIMES("Download.Parallelizable.DownloadTime", time_span);
  UMA_HISTOGRAM_CUSTOM_COUNTS("Download.Parallelizable.FileSize", file_size_kb,
                              1, kMaxFileSizeKb, 50);
  if (average_bandwidth > kHighBandwidthBytesPerSecond) {
    UMA_HISTOGRAM_LONG_TIMES(
        "Download.Parallelizable.DownloadTime.HighDownloadBandwidth",
        time_span);
    UMA_HISTOGRAM_CUSTOM_COUNTS(
        "Download.Parallelizable.FileSize.HighDownloadBandwidth", file_size_kb,
        1, kMaxFileSizeKb, 50);
  }
}

void RecordParallelDownloadCreationEvent(ParallelDownloadCreationEvent event) {
  UMA_HISTOGRAM_ENUMERATION("Download.ParallelDownload.CreationEvent", event,
                            ParallelDownloadCreationEvent::COUNT);
}

void RecordSavePackageEvent(SavePackageEvent event) {
  UMA_HISTOGRAM_ENUMERATION("Download.SavePackage", event,
                            SAVE_PACKAGE_LAST_ENTRY);
}

DownloadConnectionSecurity CheckDownloadConnectionSecurity(
    const GURL& download_url,
    const std::vector<GURL>& url_chain) {
  DownloadConnectionSecurity state = DOWNLOAD_TARGET_OTHER;
  if (download_url.SchemeIsHTTPOrHTTPS()) {
    bool is_final_download_secure = download_url.SchemeIsCryptographic();
    bool is_redirect_chain_secure = true;
    if (url_chain.size() > std::size_t(1)) {
      for (std::size_t i = std::size_t(0); i < url_chain.size() - 1; i++) {
        if (!url_chain[i].SchemeIsCryptographic()) {
          is_redirect_chain_secure = false;
          break;
        }
      }
    }
    state = is_final_download_secure
                ? is_redirect_chain_secure ? DOWNLOAD_SECURE
                                           : DOWNLOAD_REDIRECT_INSECURE
                : is_redirect_chain_secure ? DOWNLOAD_TARGET_INSECURE
                                           : DOWNLOAD_REDIRECT_TARGET_INSECURE;
  } else if (download_url.SchemeIsBlob()) {
    state = DOWNLOAD_TARGET_BLOB;
  } else if (download_url.SchemeIs(url::kDataScheme)) {
    state = DOWNLOAD_TARGET_DATA;
  } else if (download_url.SchemeIsFile()) {
    state = DOWNLOAD_TARGET_FILE;
  } else if (download_url.SchemeIsFileSystem()) {
    state = DOWNLOAD_TARGET_FILESYSTEM;
  } else if (download_url.SchemeIs(url::kFtpScheme)) {
    state = DOWNLOAD_TARGET_FTP;
  }
  return state;
}

void RecordDownloadValidationMetrics(DownloadMetricsCallsite callsite,
                                     DownloadConnectionSecurity state,
                                     DownloadContent file_type) {
  base::UmaHistogramEnumeration(
      GetDownloadValidationMetricName(callsite, state), file_type,
      DownloadContent::MAX);
}

void RecordDownloadContentTypeSecurity(
    const GURL& download_url,
    const std::vector<GURL>& url_chain,
    const std::string& mime_type,
    const base::RepeatingCallback<bool(const GURL&)>&
        is_origin_secure_callback) {
  bool is_final_download_secure = is_origin_secure_callback.Run(download_url);
  bool is_redirect_chain_secure = true;
  for (const auto& url : url_chain) {
    if (!is_origin_secure_callback.Run(url)) {
      is_redirect_chain_secure = false;
      break;
    }
  }

  DownloadContent download_content =
      download::DownloadContentFromMimeType(mime_type, false);
  if (is_final_download_secure && is_redirect_chain_secure) {
    UMA_HISTOGRAM_ENUMERATION("Download.Start.ContentType.SecureChain",
                              download_content, DownloadContent::MAX);
  } else {
    UMA_HISTOGRAM_ENUMERATION("Download.Start.ContentType.InsecureChain",
                              download_content, DownloadContent::MAX);
  }
}

void RecordDownloadSourcePageTransitionType(
    const base::Optional<ui::PageTransition>& page_transition) {
  if (!page_transition)
    return;

  UMA_HISTOGRAM_ENUMERATION(
      "Download.PageTransition",
      ui::PageTransitionStripQualifier(page_transition.value()),
      ui::PAGE_TRANSITION_LAST_CORE + 1);
}

void RecordDownloadHttpResponseCode(int response_code,
                                    bool is_background_mode) {
  int status_code = net::HttpUtil::MapStatusCodeForHistogram(response_code);
  std::vector<int> status_codes = net::HttpUtil::GetStatusCodesForHistogram();
  UMA_HISTOGRAM_CUSTOM_ENUMERATION("Download.HttpResponseCode", status_code,
                                   status_codes);
  if (is_background_mode) {
    UMA_HISTOGRAM_CUSTOM_ENUMERATION(
        "Download.HttpResponseCode.BackgroundDownload", status_code,
        status_codes);
  }
}

void RecordInProgressDBCount(InProgressDBCountTypes type) {
  UMA_HISTOGRAM_ENUMERATION("Download.InProgressDB.Counts", type);
}

void RecordDuplicateInProgressDownloadIdCount(int count) {
  UMA_HISTOGRAM_CUSTOM_COUNTS("Download.DuplicateInProgressDownloadIdCount",
                              count, 1, 10, 11);
}

void RecordResumptionRestartReason(DownloadInterruptReason reason) {
  base::UmaHistogramSparse("Download.ResumptionRestart.Reason", reason);
}

void RecordResumptionRestartCount(ResumptionRestartCountTypes type) {
  base::UmaHistogramEnumeration("Download.ResumptionRestart.Counts", type);
}

void RecordDownloadResumed(bool has_strong_validators) {
  base::UmaHistogramBoolean("Download.ResumptionStart.HasStrongValidators",
                            has_strong_validators);
}

void RecordDownloadConnectionInfo(
    net::HttpResponseInfo::ConnectionInfo connection_info) {
  base::UmaHistogramEnumeration(
      "Download.ConnectionInfo", connection_info,
      net::HttpResponseInfo::ConnectionInfo::NUM_OF_CONNECTION_INFOS);
}

void RecordDownloadManagerCreationTimeSinceStartup(
    base::TimeDelta elapsed_time) {
  base::UmaHistogramLongTimes("Download.DownloadManager.CreationDelay",
                              elapsed_time);
}

void RecordDownloadManagerMemoryUsage(size_t bytes_used) {
  base::UmaHistogramMemoryKB("Download.DownloadManager.MemoryUsage",
                             bytes_used / 1000);
}

#if defined(OS_ANDROID)
void RecordFirstBackgroundDownloadInterruptReason(
    DownloadInterruptReason reason,
    bool download_started) {
  if (download_started)
    base::UmaHistogramSparse("MobileDownload.FirstBackground.StartedReason",
                             reason);
  else
    base::UmaHistogramSparse("MobileDownload.FirstBackground.Reason", reason);
}

void RecordBackgroundTargetDeterminationResult(
    BackgroudTargetDeterminationResultTypes type) {
  base::UmaHistogramEnumeration(
      "MobileDownload.Background.TargetDeterminationResult", type);
}
#endif  // defined(OS_ANDROID)

#if defined(OS_WIN)
void RecordWinFileMoveError(int os_error) {
  base::UmaHistogramSparse("Download.WinFileMoveError", os_error);
}
#endif  // defined(OS_WIN)
}  // namespace download
