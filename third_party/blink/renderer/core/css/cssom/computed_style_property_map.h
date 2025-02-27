// Copyright 2016 the Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_CSS_CSSOM_COMPUTED_STYLE_PROPERTY_MAP_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_CSS_CSSOM_COMPUTED_STYLE_PROPERTY_MAP_H_

#include "base/macros.h"
#include "third_party/blink/renderer/core/css/css_computed_style_declaration.h"
#include "third_party/blink/renderer/core/css/css_selector.h"
#include "third_party/blink/renderer/core/css/cssom/style_property_map_read_only_main_thread.h"
#include "third_party/blink/renderer/core/dom/node.h"

namespace blink {

// This class implements computed StylePropertMapReadOnly in the Typed CSSOM
// API. The specification is here:
// https://drafts.css-houdini.org/css-typed-om/#computed-StylePropertyMapReadOnly-objects
//
// The computed StylePropertyMapReadOnlyMainThread retrieves computed styles and
// returns them as CSSStyleValues. The IDL for this class is in
// StylePropertyMap.idl. The computed StylePropertyMapReadOnlyMainThread for an
// element is accessed via element.computedStyleMap() (see
// ElementComputedStyleMap.idl/h)
class CORE_EXPORT ComputedStylePropertyMap
    : public StylePropertyMapReadOnlyMainThread {
 public:
  ComputedStylePropertyMap(Node* node, const String& pseudo_element = String())
      : StylePropertyMapReadOnlyMainThread(),
        pseudo_id_(CSSSelector::ParsePseudoId(pseudo_element)),
        node_(node) {}

  void Trace(blink::Visitor* visitor) override {
    visitor->Trace(node_);
    StylePropertyMapReadOnlyMainThread::Trace(visitor);
  }

  unsigned int size() const override;

  // ComputedStylePropertyMap needs to be sorted. This puts CSS properties
  // first, then prefixed properties, then custom properties. Everything is
  // sorted by code point within each category.
  static bool ComparePropertyNames(const CSSPropertyName&,
                                   const CSSPropertyName&);

 protected:
  const CSSValue* GetProperty(CSSPropertyID) const override;
  const CSSValue* GetCustomProperty(AtomicString) const override;
  void ForEachProperty(const IterationCallback&) override;

  String SerializationForShorthand(const CSSProperty&) const final;

 private:
  // TODO: Pseudo-element support requires reintroducing Element.pseudo(...).
  // See
  // https://github.com/w3c/css-houdini-drafts/issues/350#issuecomment-294690156
  PseudoId pseudo_id_;
  Member<Node> node_;

  Node* StyledNode() const;
  const ComputedStyle* UpdateStyle() const;
  DISALLOW_COPY_AND_ASSIGN(ComputedStylePropertyMap);
};

}  // namespace blink

#endif
