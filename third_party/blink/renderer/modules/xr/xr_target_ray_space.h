// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_XR_XR_TARGET_RAY_SPACE_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_XR_XR_TARGET_RAY_SPACE_H_

#include <memory>

#include "third_party/blink/renderer/modules/xr/xr_space.h"

namespace blink {

class XRTargetRaySpace : public XRSpace {
 public:
  XRTargetRaySpace(XRSession* session, XRInputSource* input_space);

  std::unique_ptr<TransformationMatrix> MojoFromNative() override;
  std::unique_ptr<TransformationMatrix> NativeFromMojo() override;
  bool EmulatedPosition() const override;

  base::Optional<XRNativeOriginInformation> NativeOrigin() const override;

  void Trace(blink::Visitor*) override;

 private:
  Member<XRInputSource> input_source_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_XR_XR_TARGET_RAY_SPACE_H_
