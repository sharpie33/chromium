// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_INSPECTOR_INSPECTOR_ISSUE_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_INSPECTOR_INSPECTOR_ISSUE_H_

#include "third_party/blink/public/mojom/devtools/inspector_issue.mojom-blink.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/platform/heap/handle.h"
#include "third_party/blink/renderer/platform/wtf/forward.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace blink {

class CORE_EXPORT InspectorIssue final
    : public GarbageCollected<InspectorIssue> {
 public:
  InspectorIssue(mojom::blink::InspectorIssueCode code);
  ~InspectorIssue();

  static InspectorIssue* Create(mojom::blink::InspectorIssueCode code);

  const mojom::blink::InspectorIssueCode& Code() const;

  void Trace(blink::Visitor*);

 private:
  mojom::blink::InspectorIssueCode code_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_INSPECTOR_INSPECTOR_ISSUE_H_
