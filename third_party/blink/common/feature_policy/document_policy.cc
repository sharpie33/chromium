// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/public/common/feature_policy/document_policy.h"

#include "base/no_destructor.h"
#include "third_party/blink/public/common/feature_policy/document_policy_features.h"
#include "third_party/blink/public/common/http/structured_header.h"
#include "third_party/blink/public/mojom/feature_policy/feature_policy_feature.mojom.h"

namespace blink {

// static
std::unique_ptr<DocumentPolicy> DocumentPolicy::CreateWithHeaderPolicy(
    const FeatureState& header_policy) {
  DocumentPolicy::FeatureState feature_defaults;
  for (const auto& entry : GetDocumentPolicyFeatureInfoMap())
    feature_defaults.emplace(entry.first, entry.second.default_value);
  return CreateWithHeaderPolicy(header_policy, feature_defaults);
}

namespace {
http_structured_header::Item PolicyValueToItem(const PolicyValue& value) {
  switch (value.Type()) {
    case mojom::PolicyValueType::kBool:
      return http_structured_header::Item{value.BoolValue()};
    case mojom::PolicyValueType::kDecDouble:
      return http_structured_header::Item{value.DoubleValue()};
    default:
      NOTREACHED();
      return http_structured_header::Item{
          nullptr, http_structured_header::Item::ItemType::kNullType};
  }
}

}  // namespace

// static
base::Optional<std::string> DocumentPolicy::Serialize(
    const FeatureState& policy) {
  http_structured_header::List root;
  root.reserve(policy.size());

  std::vector<std::pair<mojom::FeaturePolicyFeature, PolicyValue>>
      sorted_policy(policy.begin(), policy.end());
  std::sort(sorted_policy.begin(), sorted_policy.end(),
            [](const auto& a, const auto& b) {
              const auto& m = GetDocumentPolicyFeatureInfoMap();
              const std::string& feature_a = m.at(a.first).feature_name;
              const std::string& feature_b = m.at(b.first).feature_name;
              return feature_a < feature_b;
            });

  for (const auto& policy_entry : sorted_policy) {
    const auto& info =
        GetDocumentPolicyFeatureInfoMap().at(policy_entry.first /* feature */);

    const PolicyValue& value = policy_entry.second;
    if (value.Type() == mojom::PolicyValueType::kBool) {
      root.push_back(http_structured_header::ParameterizedMember(
          http_structured_header::Item(
              (value.BoolValue() ? "" : "no-") + info.feature_name,
              http_structured_header::Item::ItemType::kTokenType),
          {}));
    } else {
      http_structured_header::ParameterizedMember::Parameters params;
      params.push_back(std::pair<std::string, http_structured_header::Item>{
          info.feature_param_name, PolicyValueToItem(value)});
      root.push_back(http_structured_header::ParameterizedMember(
          http_structured_header::Item(
              info.feature_name,
              http_structured_header::Item::ItemType::kTokenType),
          params));
    }
  }

  return http_structured_header::SerializeList(root);
}


// static
DocumentPolicy::FeatureState DocumentPolicy::MergeFeatureState(
    const DocumentPolicy::FeatureState& policy1,
    const DocumentPolicy::FeatureState& policy2) {
  DocumentPolicy::FeatureState result;
  auto i1 = policy1.begin();
  auto i2 = policy2.begin();

  // Because std::map is by default ordered in ascending order based on key
  // value, we can run 2 iterators simultaneously through both maps to merge
  // them.
  while (i1 != policy1.end() || i2 != policy2.end()) {
    if (i1 == policy1.end()) {
      result.insert(*i2);
      i2++;
    } else if (i2 == policy2.end()) {
      result.insert(*i1);
      i1++;
    } else {
      if (i1->first == i2->first) {
        // Take the stricter policy when there is a key conflict.
        result.emplace(i1->first, std::min(i1->second, i2->second));
        i1++;
        i2++;
      } else if (i1->first < i2->first) {
        result.insert(*i1);
        i1++;
      } else {
        result.insert(*i2);
        i2++;
      }
    }
  }

  return result;
}

bool DocumentPolicy::IsFeatureEnabled(
    mojom::FeaturePolicyFeature feature) const {
  mojom::PolicyValueType feature_type =
      GetDocumentPolicyFeatureInfoMap().at(feature).default_value.Type();
  return IsFeatureEnabled(feature,
                          PolicyValue::CreateMaxPolicyValue(feature_type));
}

bool DocumentPolicy::IsFeatureEnabled(
    mojom::FeaturePolicyFeature feature,
    const PolicyValue& threshold_value) const {
  return GetFeatureValue(feature) >= threshold_value;
}

PolicyValue DocumentPolicy::GetFeatureValue(
    mojom::FeaturePolicyFeature feature) const {
  return internal_feature_state_[static_cast<size_t>(feature)];
}

bool DocumentPolicy::IsFeatureSupported(
    mojom::FeaturePolicyFeature feature) const {
  // TODO(iclelland): Generate this switch block
  switch (feature) {
    case mojom::FeaturePolicyFeature::kFontDisplay:
    case mojom::FeaturePolicyFeature::kUnoptimizedLosslessImages:
      return true;
    default:
      return false;
  }
}

void DocumentPolicy::UpdateFeatureState(const FeatureState& feature_state) {
  for (const auto& feature_and_value : feature_state) {
    internal_feature_state_[static_cast<size_t>(feature_and_value.first)] =
        feature_and_value.second;
  }
}

DocumentPolicy::DocumentPolicy(const FeatureState& defaults) {
  UpdateFeatureState(defaults);
}

// static
std::unique_ptr<DocumentPolicy> DocumentPolicy::CreateWithHeaderPolicy(
    const FeatureState& header_policy,
    const DocumentPolicy::FeatureState& defaults) {
  std::unique_ptr<DocumentPolicy> new_policy =
      base::WrapUnique(new DocumentPolicy(defaults));
  new_policy->UpdateFeatureState(header_policy);
  return new_policy;
}

// static
bool DocumentPolicy::IsPolicyCompatible(
    const DocumentPolicy::FeatureState& required_policy,
    const DocumentPolicy::FeatureState& incoming_policy) {
  for (const auto& incoming_entry : incoming_policy) {
    // feature value > threshold => enabled, where feature value is the value in
    // document policy and threshold is the value to test against.
    // The smaller the feature value, the stricter the policy.
    // Incoming policy should be at least as strict as the required one.
    const auto required_entry =
        required_policy.find(incoming_entry.first /* feature */);

    if (required_entry != required_policy.end() &&
        required_entry->second /* required_value */ <
            incoming_entry.second /* incoming_value */)
      return false;
  }
  return true;
}
}  // namespace blink
