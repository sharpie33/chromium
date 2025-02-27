// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_CREDENTIAL_PROVIDER_GAIACP_GCPW_STRINGS_H_
#define CHROME_CREDENTIAL_PROVIDER_GAIACP_GCPW_STRINGS_H_

#include <string>
#include <vector>

namespace credential_provider {
// Time parameters to control validity of the offline session.
extern const char kKeyLastSuccessfulOnlineLoginMillis[];
extern const char kKeyValidityPeriodInDays[];

// Registry parameters for gcpw.
extern const wchar_t kKeyAcceptTos[];
// Registry parameter controlling whether features related to GEM
// should be enabled / disabled.
extern const wchar_t kKeyEnableGemFeatures[];
}  // namespace credential_provider

#endif  // CHROME_CREDENTIAL_PROVIDER_GAIACP_GCPW_STRINGS_H_
