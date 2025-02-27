// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_PUBLIC_TEST_FAKE_LOCAL_FRAME_H_
#define CONTENT_PUBLIC_TEST_FAKE_LOCAL_FRAME_H_

#include "mojo/public/cpp/bindings/associated_receiver_set.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "third_party/blink/public/mojom/frame/frame.mojom.h"
#include "third_party/blink/public/mojom/input/focus_type.mojom-forward.h"

namespace gfx {
class Point;
}

namespace content {

// This class implements a LocalFrame that can be attached to the
// AssociatedInterfaceProvider so that it will be called when the browser
// normally sends a request to the renderer process. But for a unittest
// setup it can be intercepted by this class.
class FakeLocalFrame : public blink::mojom::LocalFrame {
 public:
  FakeLocalFrame();
  ~FakeLocalFrame() override;

  void Init(blink::AssociatedInterfaceProvider* provider);

  // blink::mojom::LocalFrame:
  void GetTextSurroundingSelection(
      uint32_t max_length,
      GetTextSurroundingSelectionCallback callback) override;
  void SendInterventionReport(const std::string& id,
                              const std::string& message) override;
  void NotifyUserActivation() override;
  void AddMessageToConsole(blink::mojom::ConsoleMessageLevel level,
                           const std::string& message,
                           bool discard_duplicates) override;
  void AddInspectorIssue(blink::mojom::InspectorIssueCode code) override;
  void CheckCompleted() override;
  void Collapse(bool collapsed) override;
  void EnableViewSourceMode() override;
  void Focus() override;
  void ClearFocusedElement() override;
  void CopyImageAt(const gfx::Point& window_point) override;
  void SaveImageAt(const gfx::Point& window_point) override;
  void ReportBlinkFeatureUsage(
      const std::vector<blink::mojom::WebFeature>&) override;
  void RenderFallbackContent() override;
  void BeforeUnload(bool is_reload, BeforeUnloadCallback callback) override;
  void MediaPlayerActionAt(const gfx::Point& location,
                           blink::mojom::MediaPlayerActionPtr action) override;
  void AdvanceFocusInForm(blink::mojom::FocusType focus_type) override;

 private:
  void BindFrameHostReceiver(mojo::ScopedInterfaceEndpointHandle handle);

  mojo::AssociatedReceiver<blink::mojom::LocalFrame> receiver_{this};
};

}  // namespace content

#endif  // CONTENT_PUBLIC_TEST_FAKE_LOCAL_FRAME_H_
