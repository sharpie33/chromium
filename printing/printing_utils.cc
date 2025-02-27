// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "printing/printing_utils.h"

#include <unicode/ulocdata.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "printing/units.h"
#include "third_party/icu/source/common/unicode/uchar.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/text_elider.h"

namespace printing {

namespace {

constexpr size_t kMaxDocumentTitleLength = 80;
constexpr gfx::Size kIsoA4Microns = gfx::Size(210000, 297000);

}  // namespace

base::string16 SimplifyDocumentTitleWithLength(const base::string16& title,
                                               size_t length) {
  base::string16 no_controls(title);
  no_controls.erase(
      std::remove_if(no_controls.begin(), no_controls.end(), &u_iscntrl),
      no_controls.end());
  base::ReplaceChars(no_controls, base::ASCIIToUTF16("\\"),
                     base::ASCIIToUTF16("_"), &no_controls);
  base::ReplaceChars(no_controls, base::ASCIIToUTF16("/"),
                     base::ASCIIToUTF16("_"), &no_controls);
  base::string16 result;
  gfx::ElideString(no_controls, length, &result);
  return result;
}

base::string16 FormatDocumentTitleWithOwnerAndLength(
    const base::string16& owner,
    const base::string16& title,
    size_t length) {
  const base::string16 separator = base::ASCIIToUTF16(": ");
  DCHECK_LT(separator.size(), length);

  base::string16 short_title =
      SimplifyDocumentTitleWithLength(owner, length - separator.size());
  short_title += separator;
  if (short_title.size() < length) {
    short_title +=
        SimplifyDocumentTitleWithLength(title, length - short_title.size());
  }

  return short_title;
}

base::string16 SimplifyDocumentTitle(const base::string16& title) {
  return SimplifyDocumentTitleWithLength(title, kMaxDocumentTitleLength);
}

base::string16 FormatDocumentTitleWithOwner(const base::string16& owner,
                                            const base::string16& title) {
  return FormatDocumentTitleWithOwnerAndLength(owner, title,
                                               kMaxDocumentTitleLength);
}

gfx::Size GetDefaultPaperSizeFromLocaleMicrons(base::StringPiece locale) {
  if (locale.empty())
    return kIsoA4Microns;

  int32_t width = 0;
  int32_t height = 0;
  UErrorCode error = U_ZERO_ERROR;
  ulocdata_getPaperSize(locale.as_string().c_str(), &height, &width, &error);
  if (error > U_ZERO_ERROR) {
    // If the call failed, assume Letter paper size.
    LOG(WARNING) << "ulocdata_getPaperSize failed, using ISO A4 Paper, error: "
                 << error;

    return kIsoA4Microns;
  }
  // Convert millis to microns
  return gfx::Size(width * 1000, height * 1000);
}

bool SizesEqualWithinEpsilon(const gfx::Size& lhs,
                             const gfx::Size& rhs,
                             int epsilon) {
  DCHECK_GE(epsilon, 0);

  if (lhs.IsEmpty() && rhs.IsEmpty())
    return true;

  return std::abs(lhs.width() - rhs.width()) <= epsilon &&
         std::abs(lhs.height() - rhs.height()) <= epsilon;
}

}  // namespace printing
