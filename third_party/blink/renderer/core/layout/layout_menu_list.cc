/*
 * This file is part of the select element layoutObject in WebCore.
 *
 * Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2006, 2007, 2008, 2009, 2010, 2011 Apple Inc.
 *               All rights reserved.
 *           (C) 2009 Torch Mobile Inc. All rights reserved.
 *               (http://www.torchmobile.com/)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#include "third_party/blink/renderer/core/layout/layout_menu_list.h"

#include <math.h>
#include "third_party/blink/public/strings/grit/blink_strings.h"
#include "third_party/blink/renderer/core/accessibility/ax_object_cache.h"
#include "third_party/blink/renderer/core/dom/node_computed_style.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/html/forms/html_option_element.h"
#include "third_party/blink/renderer/core/html/forms/html_select_element.h"
#include "third_party/blink/renderer/core/html/forms/menu_list_inner_element.h"
#include "third_party/blink/renderer/core/layout/layout_block_flow.h"
#include "third_party/blink/renderer/core/layout/layout_text.h"
#include "third_party/blink/renderer/core/layout/layout_theme.h"
#include "third_party/blink/renderer/core/paint/paint_layer.h"
#include "third_party/blink/renderer/platform/text/platform_locale.h"

namespace blink {

LayoutMenuList::LayoutMenuList(Element* element)
    : LayoutFlexibleBox(element),
      options_width_(0) {
  DCHECK(IsA<HTMLSelectElement>(element));
}

LayoutMenuList::~LayoutMenuList() = default;

bool LayoutMenuList::IsChildAllowed(LayoutObject* object,
                                    const ComputedStyle&) const {
  // For a size=1 <select>, we only render the active option label through the
  // InnerElement. We do not allow adding layout objects for options,
  // optgroups, ::before, or ::after.
  return object->GetNode() == &SelectElement()->InnerElement();
}

// TODO(tkent): Move this to menu_list_inner_element.cc.
void MenuListInnerElement::AdjustInnerStyle(const ComputedStyle& parent_style,
                                            ComputedStyle& inner_style) const {
  inner_style.SetFlexGrow(1);
  inner_style.SetFlexShrink(1);
  // min-width: 0; is needed for correct shrinking.
  inner_style.SetMinWidth(Length::Fixed(0));
  inner_style.SetHasLineIfEmpty(true);

  // Use margin:auto instead of align-items:center to get safe centering, i.e.
  // when the content overflows, treat it the same as align-items: flex-start.
  // But we only do that for the cases where html.css would otherwise use
  // center.
  if (parent_style.AlignItemsPosition() == ItemPosition::kCenter) {
    inner_style.SetMarginTop(Length());
    inner_style.SetMarginBottom(Length());
    inner_style.SetAlignSelfPosition(ItemPosition::kFlexStart);
  }

  LayoutTheme& theme = LayoutTheme::GetTheme();
  Length padding_start =
      Length::Fixed(theme.PopupInternalPaddingStart(parent_style));
  Length padding_end = Length::Fixed(
      theme.PopupInternalPaddingEnd(GetDocument().GetFrame(), parent_style));
  if (parent_style.IsLeftToRightDirection()) {
    inner_style.SetTextAlign(ETextAlign::kLeft);
    inner_style.SetPaddingLeft(padding_start);
    inner_style.SetPaddingRight(padding_end);
  } else {
    inner_style.SetTextAlign(ETextAlign::kRight);
    inner_style.SetPaddingLeft(padding_end);
    inner_style.SetPaddingRight(padding_start);
  }
  inner_style.SetPaddingTop(
      Length::Fixed(theme.PopupInternalPaddingTop(parent_style)));
  inner_style.SetPaddingBottom(
      Length::Fixed(theme.PopupInternalPaddingBottom(parent_style)));

  if (const ComputedStyle* option_style =
          To<HTMLSelectElement>(OwnerShadowHost())->OptionStyle()) {
    inner_style.SetDirection(option_style->Direction());
    inner_style.SetUnicodeBidi(option_style->GetUnicodeBidi());
  }
}

HTMLSelectElement* LayoutMenuList::SelectElement() const {
  return To<HTMLSelectElement>(GetNode());
}

LayoutBlock* LayoutMenuList::InnerBlock() const {
  return To<LayoutBlock>(SelectElement()->InnerElement().GetLayoutObject());
}

void LayoutMenuList::UpdateOptionsWidth() const {
  if (ShouldApplySizeContainment()) {
    options_width_ = 0;
    return;
  }

  float max_option_width = 0;

  for (auto* const option : SelectElement()->GetOptionList()) {
    String text = option->TextIndentedToRespectGroupLabel();
    const ComputedStyle* item_style =
        option->GetComputedStyle() ? option->GetComputedStyle() : Style();
    item_style->ApplyTextTransform(&text);
    // We apply SELECT's style, not OPTION's style because m_optionsWidth is
    // used to determine intrinsic width of the menulist box.
    TextRun text_run = ConstructTextRun(StyleRef().GetFont(), text, *Style());
    max_option_width =
        std::max(max_option_width, StyleRef().GetFont().Width(text_run));
  }
  options_width_ = static_cast<int>(ceilf(max_option_width));
}

String LayoutMenuList::GetText() const {
  return SelectElement()->InnerElement().innerText();
}

PhysicalRect LayoutMenuList::ControlClipRect(
    const PhysicalOffset& additional_offset) const {
  // Clip to the intersection of the content box and the content box for the
  // inner box. This will leave room for the arrows which sit in the inner box
  // padding, and if the inner box ever spills out of the outer box, that will
  // get clipped too.
  PhysicalRect outer_box = PhysicalContentBoxRect();
  outer_box.offset += additional_offset;

  LayoutBlock* block = InnerBlock();
  PhysicalRect inner_box(
      additional_offset + block->PhysicalLocation() +
          PhysicalOffset(block->PaddingLeft(), block->PaddingTop()),
      block->ContentSize());

  return Intersection(outer_box, inner_box);
}

void LayoutMenuList::ComputeIntrinsicLogicalWidths(
    LayoutUnit& min_logical_width,
    LayoutUnit& max_logical_width) const {
  UpdateOptionsWidth();

  LayoutBlock* block = InnerBlock();
  max_logical_width =
      std::max(options_width_,
               LayoutTheme::GetTheme().MinimumMenuListSize(StyleRef())) +
      block->PaddingLeft() + block->PaddingRight();
  if (!StyleRef().Width().IsPercentOrCalc())
    min_logical_width = max_logical_width;
  else
    min_logical_width = LayoutUnit();
}

void LayoutMenuList::ComputeLogicalHeight(
    LayoutUnit logical_height,
    LayoutUnit logical_top,
    LogicalExtentComputedValues& computed_values) const {
  if (StyleRef().HasEffectiveAppearance()) {
    const SimpleFontData* font_data = StyleRef().GetFont().PrimaryFont();
    DCHECK(font_data);
    const LayoutUnit inner_block_height =
        (font_data ? font_data->GetFontMetrics().Height() : 0) +
        InnerBlock()->BorderAndPaddingHeight();
    logical_height = inner_block_height + BorderAndPaddingHeight();
  }
  LayoutBox::ComputeLogicalHeight(logical_height, logical_top, computed_values);
}

LayoutUnit LayoutMenuList::ClientPaddingLeft() const {
  return PaddingLeft() + InnerBlock()->PaddingLeft();
}

LayoutUnit LayoutMenuList::ClientPaddingRight() const {
  return PaddingRight() + InnerBlock()->PaddingRight();
}

}  // namespace blink
