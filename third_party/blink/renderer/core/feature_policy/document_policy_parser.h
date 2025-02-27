// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_FEATURE_POLICY_DOCUMENT_POLICY_PARSER_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_FEATURE_POLICY_DOCUMENT_POLICY_PARSER_H_

#include "third_party/blink/public/common/feature_policy/document_policy.h"
#include "third_party/blink/public/common/feature_policy/document_policy_features.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/feature_policy/feature_policy_helper.h"
#include "third_party/blink/renderer/platform/wtf/allocator/allocator.h"

namespace blink {
class CORE_EXPORT DocumentPolicyParser {
  STATIC_ONLY(DocumentPolicyParser);

 public:
  // Parse document policy header and 'policy' attribute on iframe to
  // DocumentPolicy::FeatureState.
  static base::Optional<DocumentPolicy::FeatureState> Parse(
      const String& policy_string);

  // Internal parsing method for testing.
  static base::Optional<DocumentPolicy::FeatureState> ParseInternal(
      const String& policy_string,
      const DocumentPolicyNameFeatureMap& name_feature_map,
      const DocumentPolicyFeatureInfoMap& feature_info_map,
      const FeatureSet& available_features);
};
}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_FEATURE_POLICY_DOCUMENT_POLICY_PARSER_H_
