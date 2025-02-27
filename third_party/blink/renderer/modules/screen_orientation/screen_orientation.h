// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_SCREEN_ORIENTATION_SCREEN_ORIENTATION_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_SCREEN_ORIENTATION_SCREEN_ORIENTATION_H_

#include "third_party/blink/public/common/screen_orientation/web_screen_orientation_type.h"
#include "third_party/blink/renderer/core/dom/events/event_target.h"
#include "third_party/blink/renderer/core/execution_context/context_lifecycle_observer.h"
#include "third_party/blink/renderer/platform/bindings/script_wrappable.h"
#include "third_party/blink/renderer/platform/heap/handle.h"
#include "third_party/blink/renderer/platform/wtf/text/atomic_string.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace blink {

class ExceptionState;
class ExecutionContext;
class LocalFrame;
class ScriptPromise;
class ScriptState;
class ScreenOrientationControllerImpl;

class ScreenOrientation final : public EventTargetWithInlineData,
                                public ContextClient {
  DEFINE_WRAPPERTYPEINFO();
  USING_GARBAGE_COLLECTED_MIXIN(ScreenOrientation);

 public:
  static ScreenOrientation* Create(LocalFrame*);

  explicit ScreenOrientation(LocalFrame*);
  ~ScreenOrientation() override;

  // EventTarget implementation.
  const WTF::AtomicString& InterfaceName() const override;
  ExecutionContext* GetExecutionContext() const override;

  String type() const;
  uint16_t angle() const;

  void SetType(WebScreenOrientationType);
  void SetAngle(uint16_t);

  ScriptPromise lock(ScriptState*,
                     const AtomicString& orientation,
                     ExceptionState&);
  void unlock();

  DEFINE_ATTRIBUTE_EVENT_LISTENER(change, kChange)

  // Helper being used by this class and LockOrientationCallback.
  static const AtomicString& OrientationTypeToString(WebScreenOrientationType);

  void Trace(blink::Visitor*) override;

 private:
  ScreenOrientationControllerImpl* Controller();

  WebScreenOrientationType type_;
  uint16_t angle_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_SCREEN_ORIENTATION_SCREEN_ORIENTATION_H_
