// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_HTML_FORMS_MENU_LIST_INNER_ELEMENT_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_HTML_FORMS_MENU_LIST_INNER_ELEMENT_H_

#include "third_party/blink/renderer/core/html/html_div_element.h"

namespace blink {

class MenuListInnerElement : public HTMLDivElement {
 public:
  explicit MenuListInnerElement(Document& document);

 private:
  scoped_refptr<ComputedStyle> CustomStyleForLayoutObject() override;
  void AdjustInnerStyle(const ComputedStyle& parent_style,
                        ComputedStyle& inner_style) const;
};

}  // namespace blink
#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_HTML_FORMS_MENU_LIST_INNER_ELEMENT_H_
