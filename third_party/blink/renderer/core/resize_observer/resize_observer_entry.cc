// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/resize_observer/resize_observer_entry.h"
#include "third_party/blink/renderer/core/dom/element.h"
#include "third_party/blink/renderer/core/geometry/dom_rect_read_only.h"
#include "third_party/blink/renderer/core/layout/adjust_for_absolute_zoom.h"
#include "third_party/blink/renderer/core/layout/layout_box.h"
#include "third_party/blink/renderer/core/resize_observer/resize_observation.h"
#include "third_party/blink/renderer/core/resize_observer/resize_observer_size.h"
#include "third_party/blink/renderer/core/style/computed_style.h"
#include "third_party/blink/renderer/core/svg/svg_graphics_element.h"
#include "third_party/blink/renderer/platform/geometry/layout_rect.h"
#include "third_party/blink/renderer/platform/geometry/layout_size.h"
#include "third_party/blink/renderer/platform/runtime_enabled_features.h"

namespace blink {

DOMRectReadOnly* ResizeObserverEntry::ZoomAdjustedLayoutRect(
    LayoutRect content_rect,
    const ComputedStyle& style) {
  content_rect.SetX(
      AdjustForAbsoluteZoom::AdjustLayoutUnit(content_rect.X(), style));
  content_rect.SetY(
      AdjustForAbsoluteZoom::AdjustLayoutUnit(content_rect.Y(), style));
  content_rect.SetWidth(
      AdjustForAbsoluteZoom::AdjustLayoutUnit(content_rect.Width(), style));
  content_rect.SetHeight(
      AdjustForAbsoluteZoom::AdjustLayoutUnit(content_rect.Height(), style));

  return DOMRectReadOnly::FromFloatRect(FloatRect(
      FloatPoint(content_rect.Location()), FloatSize(content_rect.Size())));
}

ResizeObserverSize* ResizeObserverEntry::ZoomAdjustedSize(
    const LayoutSize box_size,
    const ComputedStyle& style) {
  return ResizeObserverSize::Create(
      AdjustForAbsoluteZoom::AdjustLayoutUnit(box_size.Width(), style),
      AdjustForAbsoluteZoom::AdjustLayoutUnit(box_size.Height(), style));
}

ResizeObserverEntry::ResizeObserverEntry(Element* target) : target_(target) {
  if (LayoutObject* layout_object = target->GetLayoutObject()) {
    const ComputedStyle& style = layout_object->StyleRef();
    // SVG box properties are always based on bounding box
    if (auto* svg_graphics_element = DynamicTo<SVGGraphicsElement>(target)) {
      LayoutSize bounding_box_size =
          LayoutSize(svg_graphics_element->GetBBox().Size());
      LayoutRect content_rect(LayoutPoint(), bounding_box_size);
      content_rect_ = ZoomAdjustedLayoutRect(content_rect, style);
      if (RuntimeEnabledFeatures::ResizeObserverUpdatesEnabled()) {
        content_box_size_ = ZoomAdjustedSize(bounding_box_size, style);
        border_box_size_ = ZoomAdjustedSize(bounding_box_size, style);
      }
    } else {
      LayoutBox* layout_box = target->GetLayoutBox();
      LayoutRect content_rect(
          LayoutPoint(layout_box->PaddingLeft(), layout_box->PaddingTop()),
          layout_box->ContentSize());
      content_rect_ = ZoomAdjustedLayoutRect(content_rect, style);

      if (RuntimeEnabledFeatures::ResizeObserverUpdatesEnabled()) {
        LayoutSize content_box_size =
            LayoutSize(layout_box->ContentLogicalWidth(),
                       layout_box->ContentLogicalHeight());
        LayoutSize border_box_size =
            LayoutSize(layout_box->LogicalWidth(), layout_box->LogicalHeight());

        content_box_size_ = ZoomAdjustedSize(content_box_size, style);
        border_box_size_ = ZoomAdjustedSize(border_box_size, style);
      }
    }
  } else {
    content_rect_ = DOMRectReadOnly::FromFloatRect(
        FloatRect(FloatPoint(LayoutPoint()), FloatSize(LayoutSize())));
    content_box_size_ = ResizeObserverSize::Create(0, 0);
    border_box_size_ = ResizeObserverSize::Create(0, 0);
  }
}

void ResizeObserverEntry::Trace(blink::Visitor* visitor) {
  visitor->Trace(target_);
  visitor->Trace(content_rect_);
  visitor->Trace(content_box_size_);
  visitor->Trace(border_box_size_);
  ScriptWrappable::Trace(visitor);
}

}  // namespace blink
