/*
 * (C) 1999 Lars Knoll (knoll@kde.org)
 * (C) 2000 Dirk Mueller (mueller@kde.org)
 * Copyright (C) 2004, 2005, 2006, 2007 Apple Inc. All rights reserved.
 * Copyright (C) 2006 Andrew Wellington (proton@wiretapped.net)
 * Copyright (C) 2006 Graham Dennis (graham.dennis@gmail.com)
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

#include "third_party/blink/renderer/core/layout/layout_text.h"

#include <algorithm>
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/platform/task_type.h"
#include "third_party/blink/renderer/core/accessibility/ax_object_cache.h"
#include "third_party/blink/renderer/core/content_capture/content_capture_manager.h"
#include "third_party/blink/renderer/core/dom/dom_node_ids.h"
#include "third_party/blink/renderer/core/dom/text.h"
#include "third_party/blink/renderer/core/editing/ephemeral_range.h"
#include "third_party/blink/renderer/core/editing/frame_selection.h"
#include "third_party/blink/renderer/core/editing/inline_box_position.h"
#include "third_party/blink/renderer/core/editing/inline_box_traversal.h"
#include "third_party/blink/renderer/core/editing/iterators/text_iterator.h"
#include "third_party/blink/renderer/core/editing/text_affinity.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/frame/settings.h"
#include "third_party/blink/renderer/core/layout/api/line_layout_api_shim.h"
#include "third_party/blink/renderer/core/layout/api/line_layout_box.h"
#include "third_party/blink/renderer/core/layout/geometry/logical_rect.h"
#include "third_party/blink/renderer/core/layout/layout_block.h"
#include "third_party/blink/renderer/core/layout/layout_object_factory.h"
#include "third_party/blink/renderer/core/layout/layout_table_cell.h"
#include "third_party/blink/renderer/core/layout/layout_text_combine.h"
#include "third_party/blink/renderer/core/layout/layout_view.h"
#include "third_party/blink/renderer/core/layout/line/abstract_inline_text_box.h"
#include "third_party/blink/renderer/core/layout/line/ellipsis_box.h"
#include "third_party/blink/renderer/core/layout/line/glyph_overflow.h"
#include "third_party/blink/renderer/core/layout/line/inline_text_box.h"
#include "third_party/blink/renderer/core/layout/ng/inline/layout_ng_text.h"
#include "third_party/blink/renderer/core/layout/ng/inline/ng_abstract_inline_text_box.h"
#include "third_party/blink/renderer/core/layout/ng/inline/ng_fragment_item.h"
#include "third_party/blink/renderer/core/layout/ng/inline/ng_inline_cursor.h"
#include "third_party/blink/renderer/core/layout/ng/inline/ng_inline_node.h"
#include "third_party/blink/renderer/core/layout/ng/inline/ng_offset_mapping.h"
#include "third_party/blink/renderer/core/layout/ng/inline/ng_physical_text_fragment.h"
#include "third_party/blink/renderer/core/layout/ng/layout_ng_block_flow.h"
#include "third_party/blink/renderer/core/layout/ng/ng_physical_box_fragment.h"
#include "third_party/blink/renderer/core/layout/text_autosizer.h"
#include "third_party/blink/renderer/core/paint/ng/ng_paint_fragment.h"
#include "third_party/blink/renderer/core/paint/ng/ng_paint_fragment_traversal.h"
#include "third_party/blink/renderer/platform/fonts/character_range.h"
#include "third_party/blink/renderer/platform/geometry/float_quad.h"
#include "third_party/blink/renderer/platform/runtime_enabled_features.h"
#include "third_party/blink/renderer/platform/scheduler/public/thread.h"
#include "third_party/blink/renderer/platform/scheduler/public/thread_scheduler.h"
#include "third_party/blink/renderer/platform/text/bidi_resolver.h"
#include "third_party/blink/renderer/platform/text/character.h"
#include "third_party/blink/renderer/platform/text/hyphenation.h"
#include "third_party/blink/renderer/platform/text/text_break_iterator.h"
#include "third_party/blink/renderer/platform/text/text_run_iterator.h"
#include "third_party/blink/renderer/platform/wtf/text/string_buffer.h"
#include "third_party/blink/renderer/platform/wtf/text/string_builder.h"

namespace blink {

struct SameSizeAsLayoutText : public LayoutObject {
  uint32_t bitfields : 12;
  float widths[4];
  String text;
  void* pointers[2];
  DOMNodeId node_id;
};

static_assert(sizeof(LayoutText) == sizeof(SameSizeAsLayoutText),
              "LayoutText should stay small");

class SecureTextTimer;
typedef HashMap<LayoutText*, SecureTextTimer*> SecureTextTimerMap;
static SecureTextTimerMap* g_secure_text_timers = nullptr;

class SecureTextTimer final : public TimerBase {
 public:
  SecureTextTimer(LayoutText* layout_text)
      : TimerBase(layout_text->GetDocument().GetTaskRunner(
            TaskType::kUserInteraction)),
        layout_text_(layout_text),
        last_typed_character_offset_(-1) {}

  void RestartWithNewText(unsigned last_typed_character_offset) {
    last_typed_character_offset_ = last_typed_character_offset;
    if (Settings* settings = layout_text_->GetDocument().GetSettings()) {
      StartOneShot(base::TimeDelta::FromSecondsD(
                       settings->GetPasswordEchoDurationInSeconds()),
                   FROM_HERE);
    }
  }
  void Invalidate() { last_typed_character_offset_ = -1; }
  unsigned LastTypedCharacterOffset() { return last_typed_character_offset_; }

 private:
  void Fired() override {
    DCHECK(g_secure_text_timers->Contains(layout_text_));
    // Forcing setting text as it may be masked later
    layout_text_->ForceSetText(layout_text_->GetText().Impl());
  }

  LayoutText* layout_text_;
  int last_typed_character_offset_;
};

LayoutText::LayoutText(Node* node, scoped_refptr<StringImpl> str)
    : LayoutObject(node),
      has_tab_(false),
      lines_dirty_(false),
      valid_ng_items_(false),
      has_bidi_control_items_(false),
      contains_reversed_text_(false),
      known_to_have_no_overflow_and_no_fallback_fonts_(false),
      contains_only_whitespace_or_nbsp_(
          static_cast<unsigned>(OnlyWhitespaceOrNbsp::kUnknown)),
      is_text_fragment_(false),
      has_abstract_inline_text_box_(false),
      min_width_(-1),
      max_width_(-1),
      first_line_min_width_(0),
      last_line_line_min_width_(0),
      text_(std::move(str)),
      text_boxes_() {
  DCHECK(text_);
  DCHECK(!node || !node->IsDocumentNode());

  SetIsText();

  if (node)
    GetFrameView()->IncrementVisuallyNonEmptyCharacterCount(text_.length());
}

LayoutText::~LayoutText() {
#if DCHECK_IS_ON()
  if (IsInLayoutNGInlineFormattingContext())
    DCHECK(!first_paint_fragment_);
  else
    text_boxes_.AssertIsEmpty();
#endif
}

LayoutText* LayoutText::CreateEmptyAnonymous(
    Document& doc,
    scoped_refptr<const ComputedStyle> style,
    LegacyLayout legacy) {
  LayoutText* text =
      LayoutObjectFactory::CreateText(nullptr, StringImpl::empty_, legacy);
  text->SetDocumentForAnonymous(&doc);
  text->SetStyle(std::move(style));
  return text;
}

bool LayoutText::IsWordBreak() const {
  return false;
}

void LayoutText::StyleDidChange(StyleDifference diff,
                                const ComputedStyle* old_style) {
  // There is no need to ever schedule paint invalidations from a style change
  // of a text run, since we already did this for the parent of the text run.
  // We do have to schedule layouts, though, since a style change can force us
  // to need to relayout.
  if (diff.NeedsFullLayout()) {
    SetNeedsLayoutAndPrefWidthsRecalc(layout_invalidation_reason::kStyleChange);
    known_to_have_no_overflow_and_no_fallback_fonts_ = false;
  }

  const ComputedStyle& new_style = StyleRef();
  ETextTransform old_transform =
      old_style ? old_style->TextTransform() : ETextTransform::kNone;
  ETextSecurity old_security =
      old_style ? old_style->TextSecurity() : ETextSecurity::kNone;
  if (old_transform != new_style.TextTransform() ||
      old_security != new_style.TextSecurity())
    TransformText();

  // This is an optimization that kicks off font load before layout.
  if (!GetText().ContainsOnlyWhitespaceOrEmpty())
    new_style.GetFont().WillUseFontData(GetText());

  TextAutosizer* text_autosizer = GetDocument().GetTextAutosizer();
  if (!old_style && text_autosizer)
    text_autosizer->Record(this);

  if (diff.NeedsReshape()) {
    valid_ng_items_ = false;
    SetNeedsCollectInlines();
  }

  SetHorizontalWritingMode(new_style.IsHorizontalWritingMode());
}

void LayoutText::RemoveAndDestroyTextBoxes() {
  if (!DocumentBeingDestroyed()) {
    if (FirstTextBox()) {
      if (IsBR()) {
        RootInlineBox* next = FirstTextBox()->Root().NextRootBox();
        if (next)
          next->MarkDirty();
      }
      for (InlineTextBox* box : TextBoxes())
        box->Remove();
    } else {
      if (RuntimeEnabledFeatures::LayoutNGFragmentItemEnabled()) {
        if (has_abstract_inline_text_box_)
          ClearFirstInlineFragmentItemIndex();
      } else if (NGPaintFragment* first_inline_fragment =
                     FirstInlineFragment()) {
        first_inline_fragment->LayoutObjectWillBeDestroyed();
        SetFirstInlineFragment(nullptr);
      }
      if (Parent())
        Parent()->DirtyLinesFromChangedChild(this);
    }
  } else if (RuntimeEnabledFeatures::LayoutNGFragmentItemEnabled()) {
    if (has_abstract_inline_text_box_)
      ClearFirstInlineFragmentItemIndex();
  } else if (NGPaintFragment* first_inline_fragment = FirstInlineFragment()) {
    // Still do this to clear the global hash map in  NGAbstractInlineTextBox.
    SetFirstInlineFragment(nullptr);
  }
  DeleteTextBoxes();
}

void LayoutText::WillBeDestroyed() {
  if (SecureTextTimer* secure_text_timer =
          g_secure_text_timers ? g_secure_text_timers->Take(this) : nullptr)
    delete secure_text_timer;

  if (node_id_ != kInvalidDOMNodeId) {
    if (auto* manager = GetContentCaptureManager())
      manager->OnLayoutTextWillBeDestroyed(*GetNode());
    node_id_ = kInvalidDOMNodeId;
  }

  RemoveAndDestroyTextBoxes();
  LayoutObject::WillBeDestroyed();
  valid_ng_items_ = false;
}

void LayoutText::ExtractTextBox(InlineTextBox* box) {
  MutableTextBoxes().ExtractLineBox(box);
}

void LayoutText::AttachTextBox(InlineTextBox* box) {
  MutableTextBoxes().AttachLineBox(box);
}

void LayoutText::RemoveTextBox(InlineTextBox* box) {
  MutableTextBoxes().RemoveLineBox(box);
}

void LayoutText::DeleteTextBoxes() {
  if (!IsInLayoutNGInlineFormattingContext())
    return MutableTextBoxes().DeleteLineBoxes();
  DetachAbstractInlineTextBoxesIfNeeded();
}

void LayoutText::DetachAbstractInlineTextBoxesIfNeeded() {
  // TODO(layout-dev): Because We should call |WillDestroy()| once for
  // associated fragments, when you reuse fragments, you should construct
  // NGAbstractInlineTextBox for them.
  if (!has_abstract_inline_text_box_)
    return;
  has_abstract_inline_text_box_ = false;
  if (!RuntimeEnabledFeatures::LayoutNGFragmentItemEnabled()) {
    for (NGPaintFragment* fragment : NGPaintFragment::InlineFragmentsFor(this))
      NGAbstractInlineTextBox::WillDestroy(fragment);
    return;
  }
  // TODO(yosin): Make sure we call this function within valid containg block
  // of |this|.
  NGInlineCursor cursor;
  for (cursor.MoveTo(*this); cursor; cursor.MoveToNextForSameLayoutObject())
    NGAbstractInlineTextBox::WillDestroy(cursor);
}

void LayoutText::SetFirstInlineFragment(NGPaintFragment* first_fragment) {
  CHECK(IsInLayoutNGInlineFormattingContext());
  // TODO(yosin): Once we remove |NGPaintFragment|, we should get rid of
  // |!fragment|.
  DCHECK(!first_fragment ||
         !RuntimeEnabledFeatures::LayoutNGFragmentItemEnabled());
  DetachAbstractInlineTextBoxesIfNeeded();
  first_paint_fragment_ = first_fragment;
}

void LayoutText::ClearFirstInlineFragmentItemIndex() {
  CHECK(IsInLayoutNGInlineFormattingContext()) << *this;
  DCHECK(RuntimeEnabledFeatures::LayoutNGFragmentItemEnabled());
  DetachAbstractInlineTextBoxesIfNeeded();
  first_fragment_item_index_ = 0u;
}

void LayoutText::SetFirstInlineFragmentItemIndex(wtf_size_t index) {
  CHECK(IsInLayoutNGInlineFormattingContext());
  // TODO(yosin): Call |NGAbstractInlineTextBox::WillDestroy()|.
  DCHECK(RuntimeEnabledFeatures::LayoutNGFragmentItemEnabled());
  DCHECK_NE(index, 0u);
  DetachAbstractInlineTextBoxesIfNeeded();
  // TDOO(yosin): Once we update all |LayoutObject::FirstInlineFragment()|,
  // we should enable below.
  // first_fragment_item_index_ = index;
}

void LayoutText::InLayoutNGInlineFormattingContextWillChange(bool new_value) {
  DeleteTextBoxes();

  // Because |first_paint_fragment_| and |text_boxes_| are union, when one is
  // deleted, the other should be initialized to nullptr.
  DCHECK(new_value ? !first_paint_fragment_ : !text_boxes_.First());

  // Because there are no inline boxes associated to this text, we should not
  // have abstract inline text boxes too.
  DCHECK(!has_abstract_inline_text_box_);
}

Vector<LayoutText::TextBoxInfo> LayoutText::GetTextBoxInfo() const {
  // This function may kick the layout (e.g., |LocalRect()|), but Inspector may
  // call this function outside of the layout phase.
  FontCachePurgePreventer fontCachePurgePreventer;

  Vector<TextBoxInfo> results;
  if (const NGOffsetMapping* mapping = GetNGOffsetMapping()) {
    bool in_hidden_for_paint = false;
    NGInlineCursor cursor;
    cursor.MoveTo(*this);
    for (; cursor; cursor.MoveToNextForSameLayoutObject()) {
      // TODO(yosin): We should introduce
      // |NGPhysicalTextFragment::IsTruncated()| to skip them instead of using
      // |IsHiddenForPaint()| with ordering of fragments.
      if (cursor.IsHiddenForPaint()) {
        in_hidden_for_paint = true;
      } else if (in_hidden_for_paint) {
        // Because of we finished original fragments (not painted), we should
        // ignore truncated fragments (actually painted).
        break;
      }
      // We don't put generated texts, e.g. ellipsis, hyphen, etc. not in text
      // content, into results. Note: CSS "content" aren't categorized this.
      if (cursor.IsGeneratedTextType())
        continue;
      // When the corresponding DOM range contains collapsed whitespaces, NG
      // produces one fragment but legacy produces multiple text boxes broken at
      // collapsed whitespaces. We break the fragment at collapsed whitespaces
      // to match the legacy output.
      for (const NGOffsetMappingUnit& unit :
           mapping->GetMappingUnitsForTextContentOffsetRange(
               cursor.CurrentTextStartOffset(),
               cursor.CurrentTextEndOffset())) {
        DCHECK_EQ(unit.GetLayoutObject(), this);
        if (unit.GetType() == NGOffsetMappingUnitType::kCollapsed)
          continue;
        // [clamped_start, clamped_end] of |fragment| matches a legacy text box.
        const unsigned clamped_start =
            std::max(unit.TextContentStart(), cursor.CurrentTextStartOffset());
        const unsigned clamped_end =
            std::min(unit.TextContentEnd(), cursor.CurrentTextEndOffset());
        DCHECK_LT(clamped_start, clamped_end);
        const unsigned box_length = clamped_end - clamped_start;

        // Compute rect of the legacy text box.
        LayoutRect rect =
            cursor.CurrentLocalRect(clamped_start, clamped_end).ToLayoutRect();
        rect.MoveBy(cursor.Current().OffsetInContainerBlock().ToLayoutPoint());

        // Compute start of the legacy text box.
        if (unit.AssociatedNode()) {
          // In case of |text_| comes from DOM node.
          const base::Optional<unsigned> box_start =
              CaretOffsetForPosition(mapping->GetLastPosition(clamped_start));
          DCHECK(box_start.has_value());
          results.push_back(TextBoxInfo{rect, *box_start, box_length});
          continue;
        }
        // Handle CSS generated content, e.g. ::before/::after
        const NGOffsetMappingUnit* const mapping_unit =
            mapping->GetLastMappingUnit(clamped_start);
        DCHECK(mapping_unit) << this << " at " << clamped_start;
        const unsigned dom_offset =
            mapping_unit->ConvertTextContentToLastDOMOffset(clamped_start);
        results.push_back(TextBoxInfo{rect, dom_offset, box_length});
      }
    }
    return results;
  }

  for (const InlineTextBox* text_box : TextBoxes()) {
    results.push_back(
        TextBoxInfo{text_box->FrameRect(), text_box->Start(), text_box->Len()});
  }
  return results;
}

bool LayoutText::HasTextBoxes() const {
  if (RuntimeEnabledFeatures::LayoutNGEnabled()) {
    if (IsInLayoutNGInlineFormattingContext()) {
      NGInlineCursor cursor;
      cursor.MoveTo(*this);
      return cursor.IsNotNull();
    }

    // When legacy is forced, IsInLayoutNGInlineFormattingContext is false,
    // and we fall back to normal HasTextBox
    return FirstTextBox();
  }
  return FirstTextBox();
}

scoped_refptr<StringImpl> LayoutText::OriginalText() const {
  auto* text_node = DynamicTo<Text>(GetNode());
  return text_node ? text_node->DataImpl() : nullptr;
}

String LayoutText::PlainText() const {
  if (GetNode())
    return blink::PlainText(EphemeralRange::RangeOfContents(*GetNode()));

  // FIXME: this is just a stopgap until TextIterator is adapted to support
  // generated text.
  StringBuilder plain_text_builder;
  for (InlineTextBox* text_box : TextBoxes()) {
    String text = text_.Substring(text_box->Start(), text_box->Len())
                      .SimplifyWhiteSpace(WTF::kDoNotStripWhiteSpace);
    plain_text_builder.Append(text);
    if (text_box->NextForSameLayoutObject() &&
        text_box->NextForSameLayoutObject()->Start() > text_box->end() &&
        text.length() && !text.Right(1).ContainsOnlyWhitespaceOrEmpty())
      plain_text_builder.Append(kSpaceCharacter);
  }
  return plain_text_builder.ToString();
}

static LayoutRect LocalQuadForTextBox(InlineTextBox* box,
                                      unsigned start,
                                      unsigned end) {
  unsigned real_end = std::min(box->end() + 1, end);
  const bool include_newline_space_width = false;
  LayoutRect r =
      box->LocalSelectionRect(start, real_end, include_newline_space_width);
  if (r.Height()) {
    // Change the height and y position (or width and x for vertical text)
    // because selectionRect uses selection-specific values.
    if (box->IsHorizontal()) {
      r.SetHeight(box->Height());
      r.SetY(box->Y());
    } else {
      r.SetWidth(box->Width());
      r.SetX(box->X());
    }
    return r;
  }
  return LayoutRect();
}

static IntRect EllipsisRectForBox(InlineTextBox* box,
                                  unsigned start_pos,
                                  unsigned end_pos) {
  if (!box)
    return IntRect();

  uint16_t truncation = box->Truncation();
  if (truncation == kCNoTruncation)
    return IntRect();

  if (EllipsisBox* ellipsis = box->Root().GetEllipsisBox()) {
    int ellipsis_start_position = std::max<int>(start_pos - box->Start(), 0);
    int ellipsis_end_position =
        std::min<int>(end_pos - box->Start(), box->Len());

    // The ellipsis should be considered to be selected if the end of the
    // selection is past the beginning of the truncation and the beginning of
    // the selection is before or at the beginning of the truncation.
    if (ellipsis_end_position >= truncation &&
        ellipsis_start_position <= truncation)
      return ellipsis->SelectionRect();
  }

  return IntRect();
}

template <typename PhysicalRectCollector>
void LayoutText::CollectLineBoxRects(const PhysicalRectCollector& yield,
                                     ClippingOption option) const {
  if (IsInLayoutNGInlineFormattingContext()) {
    NGInlineCursor cursor;
    cursor.MoveTo(*this);
    for (; cursor; cursor.MoveToNextForSameLayoutObject()) {
      if (UNLIKELY(option != ClippingOption::kNoClipping)) {
        DCHECK_EQ(option, ClippingOption::kClipToEllipsis);
        if (cursor.IsHiddenForPaint())
          continue;
      }
      yield(cursor.Current().RectInContainerBlock());
    }
    return;
  }

  const LayoutBlock* block_for_flipping =
      UNLIKELY(HasFlippedBlocksWritingMode()) ? ContainingBlock() : nullptr;
  for (InlineTextBox* box : TextBoxes()) {
    LayoutRect boundaries = box->FrameRect();
    const IntRect ellipsis_rect = (option == kClipToEllipsis)
                                      ? EllipsisRectForBox(box, 0, TextLength())
                                      : IntRect();
    if (!ellipsis_rect.IsEmpty()) {
      if (IsHorizontalWritingMode())
        boundaries.SetWidth(ellipsis_rect.MaxX() - boundaries.X());
      else
        boundaries.SetHeight(ellipsis_rect.MaxY() - boundaries.Y());
    }
    yield(FlipForWritingMode(boundaries, block_for_flipping));
  }
}

void LayoutText::AbsoluteQuads(Vector<FloatQuad>& quads,
                               MapCoordinatesFlags mode) const {
  CollectLineBoxRects([this, &quads, mode](const PhysicalRect& r) {
    quads.push_back(LocalRectToAbsoluteQuad(r, mode));
  });
}

bool LayoutText::MapDOMOffsetToTextContentOffset(const NGOffsetMapping& mapping,
                                                 unsigned* start,
                                                 unsigned* end) const {
  DCHECK_LE(*start, *end);

  // Adjust |start| to the next non-collapsed offset if |start| is collapsed.
  Position start_position =
      PositionForCaretOffset(std::min(*start, TextLength()));
  Position non_collapsed_start_position =
      mapping.StartOfNextNonCollapsedContent(start_position);

  // If all characters after |start| are collapsed, adjust to the last
  // non-collapsed offset.
  if (non_collapsed_start_position.IsNull()) {
    non_collapsed_start_position =
        mapping.EndOfLastNonCollapsedContent(start_position);

    // If all characters are collapsed, return false.
    if (non_collapsed_start_position.IsNull())
      return false;
  }

  *start = mapping.GetTextContentOffset(non_collapsed_start_position).value();

  // Adjust |end| to the last non-collapsed offset if |end| is collapsed.
  Position end_position = PositionForCaretOffset(std::min(*end, TextLength()));
  Position non_collpased_end_position =
      mapping.EndOfLastNonCollapsedContent(end_position);

  if (non_collpased_end_position.IsNull() ||
      non_collpased_end_position.OffsetInContainerNode() <=
          non_collapsed_start_position.OffsetInContainerNode()) {
    // If all characters in the range are collapsed, make |end| = |start|.
    *end = *start;
  } else {
    *end = mapping.GetTextContentOffset(non_collpased_end_position).value();
  }

  DCHECK_LE(*start, *end);
  return true;
}

void LayoutText::AbsoluteQuadsForRange(Vector<FloatQuad>& quads,
                                       unsigned start,
                                       unsigned end) const {
  // Work around signed/unsigned issues. This function takes unsigneds, and is
  // often passed UINT_MAX to mean "all the way to the end". InlineTextBox
  // coordinates are unsigneds, so changing this function to take ints causes
  // various internal mismatches. But selectionRect takes ints, and passing
  // UINT_MAX to it causes trouble. Ideally we'd change selectionRect to take
  // unsigneds, but that would cause many ripple effects, so for now we'll just
  // clamp our unsigned parameters to INT_MAX.
  DCHECK(end == UINT_MAX || end <= INT_MAX);
  DCHECK_LE(start, static_cast<unsigned>(INT_MAX));
  start = std::min(start, static_cast<unsigned>(INT_MAX));
  end = std::min(end, static_cast<unsigned>(INT_MAX));

  if (auto* mapping = GetNGOffsetMapping()) {
    if (!MapDOMOffsetToTextContentOffset(*mapping, &start, &end))
      return;

    // We don't want to add collapsed (i.e., start == end) quads from text
    // fragments that intersect [start, end] only at the boundary, unless they
    // are the only quads found. For example, when we have
    // - text fragments: ABC  DEF  GHI
    // - text offsets:   012  345  678
    // and input range [3, 6], since fragment "DEF" gives non-collapsed quad,
    // we no longer add quads from "ABC" and "GHI" since they are collapsed.
    // TODO(layout-dev): This heuristic doesn't cover all cases, as we return
    // 2 collapsed quads (instead of 1) for range [3, 3] in the above example.
    bool found_non_collapsed_quad = false;
    Vector<FloatQuad, 1> collapsed_quads_candidates;

    // Find fragments that have text for the specified range.
    DCHECK_LE(start, end);
    const LayoutBlock* block_for_flipping = nullptr;
    if (UNLIKELY(HasFlippedBlocksWritingMode()))
      block_for_flipping = ContainingBlock();
    NGInlineCursor cursor;
    for (cursor.MoveTo(*this); cursor; cursor.MoveToNextForSameLayoutObject()) {
      const NGTextOffset offset = cursor.CurrentTextOffset();
      if (start > offset.end || end < offset.start)
        continue;
      const unsigned clamped_start = std::max(start, offset.start);
      const unsigned clamped_end = std::min(end, offset.end);
      PhysicalRect rect = cursor.CurrentLocalRect(clamped_start, clamped_end);
      rect.Move(cursor.Current().OffsetInContainerBlock());
      const FloatQuad quad = LocalRectToAbsoluteQuad(rect);
      if (clamped_start < clamped_end) {
        quads.push_back(quad);
        found_non_collapsed_quad = true;
      } else {
        collapsed_quads_candidates.push_back(quad);
      }
    }
    if (!found_non_collapsed_quad)
      quads.AppendVector(collapsed_quads_candidates);
    return;
  }

  const unsigned caret_min_offset = static_cast<unsigned>(CaretMinOffset());
  const unsigned caret_max_offset = static_cast<unsigned>(CaretMaxOffset());

  // Narrows |start| and |end| into |caretMinOffset| and |careMaxOffset|
  // to ignore unrendered leading and trailing whitespaces.
  start = std::min(std::max(caret_min_offset, start), caret_max_offset);
  end = std::min(std::max(caret_min_offset, end), caret_max_offset);

  // This function is always called in sequence that this check should work.
  bool has_checked_box_in_range = !quads.IsEmpty();

  const LayoutBlock* block_for_flipping =
      UNLIKELY(HasFlippedBlocksWritingMode()) ? ContainingBlock() : nullptr;
  for (InlineTextBox* box : TextBoxes()) {
    // Note: box->end() returns the index of the last character, not the index
    // past it
    LayoutRect rect;
    if (start <= box->Start() && box->end() < end) {
      rect = box->FrameRect();
      if (!has_checked_box_in_range) {
        has_checked_box_in_range = true;
        quads.clear();
      }
    } else if ((box->Start() <= start && start <= box->end()) ||
               (box->Start() < end && end <= box->end())) {
      rect = LocalQuadForTextBox(box, start, end);
      if (!rect.Size().IsZero()) {
        if (!has_checked_box_in_range) {
          has_checked_box_in_range = true;
          quads.clear();
        }
      }
    } else if (!has_checked_box_in_range) {
      // consider when the offset of range is area of leading or trailing
      // whitespace
      rect = LocalQuadForTextBox(box, start, end);
    }
    if (!rect.Size().IsZero()) {
      PhysicalRect physical_rect = FlipForWritingMode(rect, block_for_flipping);
      quads.push_back(LocalRectToAbsoluteQuad(physical_rect));
    }
  }
}

FloatRect LayoutText::LocalBoundingBoxRectForAccessibility() const {
  FloatRect result;
  const LayoutBlock* block_for_flipping =
      UNLIKELY(HasFlippedBlocksWritingMode()) ? ContainingBlock() : nullptr;
  CollectLineBoxRects(
      [this, &result, block_for_flipping](const PhysicalRect& r) {
        LayoutRect rect = FlipForWritingMode(r, block_for_flipping);
        result.Unite(FloatRect(rect));
      },
      kClipToEllipsis);
  // TODO(wangxianzhu): This is one of a few cases that a FloatRect is required
  // to be in flipped blocks direction. Should eliminite them.
  return result;
}

namespace {

enum ShouldAffinityBeDownstream {
  kAlwaysDownstream,
  kAlwaysUpstream,
  kUpstreamIfPositionIsNotAtStart
};

bool LineDirectionPointFitsInBox(
    int point_line_direction,
    InlineTextBox* box,
    ShouldAffinityBeDownstream& should_affinity_be_downstream) {
  should_affinity_be_downstream = kAlwaysDownstream;

  // the x coordinate is equal to the left edge of this box the affinity must be
  // downstream so the position doesn't jump back to the previous line except
  // when box is the first box in the line
  if (point_line_direction <= box->LogicalLeft()) {
    should_affinity_be_downstream = !box->PrevLeafChild()
                                        ? kUpstreamIfPositionIsNotAtStart
                                        : kAlwaysDownstream;
    return true;
  }

  // and the x coordinate is to the left of the right edge of this box
  // check to see if position goes in this box
  if (point_line_direction < box->LogicalRight()) {
    should_affinity_be_downstream = kUpstreamIfPositionIsNotAtStart;
    return true;
  }

  // box is first on line
  // and the x coordinate is to the left of the first text box left edge
  if (!box->PrevLeafChildIgnoringLineBreak() &&
      point_line_direction < box->LogicalLeft())
    return true;

  if (!box->NextLeafChildIgnoringLineBreak()) {
    // box is last on line and the x coordinate is to the right of the last text
    // box right edge generate VisiblePosition, use TextAffinity::Upstream
    // affinity if possible
    should_affinity_be_downstream = kUpstreamIfPositionIsNotAtStart;
    return true;
  }

  return false;
}

PositionWithAffinity CreatePositionWithAffinityForBox(
    const InlineBox* box,
    int offset,
    ShouldAffinityBeDownstream should_affinity_be_downstream) {
  TextAffinity affinity = TextAffinity::kDefault;
  switch (should_affinity_be_downstream) {
    case kAlwaysDownstream:
      affinity = TextAffinity::kDownstream;
      break;
    case kAlwaysUpstream:
      affinity = TextAffinity::kUpstreamIfPossible;
      break;
    case kUpstreamIfPositionIsNotAtStart:
      affinity = offset > box->CaretMinOffset()
                     ? TextAffinity::kUpstreamIfPossible
                     : TextAffinity::kDownstream;
      break;
  }
  int text_start_offset =
      box->GetLineLayoutItem().IsText()
          ? LineLayoutText(box->GetLineLayoutItem()).TextStartOffset()
          : 0;
  return box->GetLineLayoutItem().CreatePositionWithAffinity(
      offset + text_start_offset, affinity);
}

PositionWithAffinity
CreatePositionWithAffinityForBoxAfterAdjustingOffsetForBiDi(
    const InlineTextBox* box,
    int offset,
    ShouldAffinityBeDownstream should_affinity_be_downstream) {
  DCHECK(box);
  DCHECK_GE(offset, 0);
  DCHECK_LE(static_cast<unsigned>(offset), box->Len());

  if (offset && static_cast<unsigned>(offset) < box->Len()) {
    return CreatePositionWithAffinityForBox(box, box->Start() + offset,
                                            should_affinity_be_downstream);
  }

  const InlineBoxPosition adjusted = BidiAdjustment::AdjustForHitTest(
      InlineBoxPosition(box, box->Start() + offset));
  return CreatePositionWithAffinityForBox(adjusted.inline_box,
                                          adjusted.offset_in_box,
                                          should_affinity_be_downstream);
}

}  // namespace

PositionWithAffinity LayoutText::PositionForPoint(
    const PhysicalOffset& point) const {
  if (const LayoutBlockFlow* ng_block_flow = ContainingNGBlockFlow())
    return ng_block_flow->PositionForPoint(*this, point);

  DCHECK(CanUseInlineBox(*this));
  if (!FirstTextBox() || TextLength() == 0)
    return CreatePositionWithAffinity(0);

  LayoutPoint flipped_point = FlipForWritingMode(point);
  LayoutUnit point_line_direction =
      IsHorizontalWritingMode() ? flipped_point.X() : flipped_point.Y();
  LayoutUnit point_block_direction =
      IsHorizontalWritingMode() ? flipped_point.Y() : flipped_point.X();
  bool blocks_are_flipped = StyleRef().IsFlippedBlocksWritingMode();

  InlineTextBox* last_box = nullptr;
  for (InlineTextBox* box : TextBoxes()) {
    if (box->IsLineBreak() && !box->PrevLeafChild() && box->NextLeafChild() &&
        !box->NextLeafChild()->IsLineBreak())
      box = box->NextForSameLayoutObject();

    RootInlineBox& root_box = box->Root();
    LayoutUnit top = std::min(root_box.SelectionTop(), root_box.LineTop());
    if (point_block_direction > top ||
        (!blocks_are_flipped && point_block_direction == top)) {
      LayoutUnit bottom = root_box.SelectionBottom();
      if (root_box.NextRootBox())
        bottom = std::min(bottom, root_box.NextRootBox()->LineTop());

      if (point_block_direction < bottom ||
          (blocks_are_flipped && point_block_direction == bottom)) {
        ShouldAffinityBeDownstream should_affinity_be_downstream;
        if (LineDirectionPointFitsInBox(point_line_direction.ToInt(), box,
                                        should_affinity_be_downstream)) {
          const int offset = box->OffsetForPosition(
              point_line_direction, IncludePartialGlyphs, BreakGlyphs);
          return CreatePositionWithAffinityForBoxAfterAdjustingOffsetForBiDi(
              box, offset, should_affinity_be_downstream);
        }
      }
    }
    last_box = box;
  }

  if (last_box) {
    const int offset = last_box->OffsetForPosition(
        point_line_direction, IncludePartialGlyphs, BreakGlyphs);
    ShouldAffinityBeDownstream should_affinity_be_downstream;
    LineDirectionPointFitsInBox(point_line_direction.ToInt(), last_box,
                                should_affinity_be_downstream);
    return CreatePositionWithAffinityForBoxAfterAdjustingOffsetForBiDi(
        last_box, offset, should_affinity_be_downstream);
  }
  return CreatePositionWithAffinity(0);
}

LayoutRect LayoutText::LocalCaretRect(
    const InlineBox* inline_box,
    int caret_offset,
    LayoutUnit* extra_width_to_end_of_line) const {
  if (!inline_box)
    return LayoutRect();

  DCHECK(inline_box->IsInlineTextBox());
  if (!inline_box->IsInlineTextBox())
    return LayoutRect();

  const InlineTextBox* box = ToInlineTextBox(inline_box);
  // Find an InlineBox before caret position, which is used to get caret height.
  const InlineBox* caret_box = box;
  if (box->GetLineLayoutItem().Style(box->IsFirstLineStyle())->Direction() ==
      TextDirection::kLtr) {
    if (box->PrevLeafChild() && caret_offset == 0)
      caret_box = box->PrevLeafChild();
  } else {
    if (box->NextLeafChild() && caret_offset == 0)
      caret_box = box->NextLeafChild();
  }

  // Get caret height from a font of character.
  const ComputedStyle* style_to_use =
      caret_box->GetLineLayoutItem().Style(caret_box->IsFirstLineStyle());
  if (!style_to_use->GetFont().PrimaryFont())
    return LayoutRect();

  int height = style_to_use->GetFont().PrimaryFont()->GetFontMetrics().Height();
  int top = caret_box->LogicalTop().ToInt();

  // Go ahead and round left to snap it to the nearest pixel.
  LayoutUnit left = box->PositionForOffset(caret_offset);
  LayoutUnit caret_width = GetFrameView()->CaretWidth();

  // Distribute the caret's width to either side of the offset.
  LayoutUnit caret_width_left_of_offset = caret_width / 2;
  left -= caret_width_left_of_offset;
  LayoutUnit caret_width_right_of_offset =
      caret_width - caret_width_left_of_offset;

  left = LayoutUnit(left.Round());

  LayoutUnit root_left = box->Root().LogicalLeft();
  LayoutUnit root_right = box->Root().LogicalRight();

  // FIXME: should we use the width of the root inline box or the
  // width of the containing block for this?
  if (extra_width_to_end_of_line) {
    *extra_width_to_end_of_line =
        (box->Root().LogicalWidth() + root_left) - (left + 1);
  }

  LayoutBlock* cb = ContainingBlock();
  const ComputedStyle& cb_style = cb->StyleRef();

  LayoutUnit left_edge;
  LayoutUnit right_edge;
  left_edge = std::min(LayoutUnit(), root_left);
  right_edge = std::max(cb->LogicalWidth(), root_right);

  bool right_aligned = false;
  switch (cb_style.GetTextAlign()) {
    case ETextAlign::kRight:
    case ETextAlign::kWebkitRight:
      right_aligned = true;
      break;
    case ETextAlign::kLeft:
    case ETextAlign::kWebkitLeft:
    case ETextAlign::kCenter:
    case ETextAlign::kWebkitCenter:
      break;
    case ETextAlign::kJustify:
    case ETextAlign::kStart:
      right_aligned = !cb_style.IsLeftToRightDirection();
      break;
    case ETextAlign::kEnd:
      right_aligned = cb_style.IsLeftToRightDirection();
      break;
  }

  // for unicode-bidi: plaintext, use inlineBoxBidiLevel() to test the correct
  // direction for the cursor.
  if (right_aligned && StyleRef().GetUnicodeBidi() == UnicodeBidi::kPlaintext) {
    if (inline_box->BidiLevel() % 2 != 1)
      right_aligned = false;
  }

  if (right_aligned) {
    left = std::max(left, left_edge);
    left = std::min(left, root_right - caret_width);
  } else {
    left = std::min(left, right_edge - caret_width_right_of_offset);
    left = std::max(left, root_left);
  }

  return LayoutRect(
      StyleRef().IsHorizontalWritingMode()
          ? IntRect(left.ToInt(), top, caret_width.ToInt(), height)
          : IntRect(top, left.ToInt(), height, caret_width.ToInt()));
}

ALWAYS_INLINE float LayoutText::WidthFromFont(
    const Font& f,
    int start,
    int len,
    float lead_width,
    float text_width_so_far,
    TextDirection text_direction,
    HashSet<const SimpleFontData*>* fallback_fonts,
    FloatRect* glyph_bounds_accumulation,
    float expansion) const {
  if (StyleRef().HasTextCombine() && IsCombineText()) {
    const LayoutTextCombine* combine_text = ToLayoutTextCombine(this);
    if (combine_text->IsCombined())
      return combine_text->CombinedTextWidth(f);
  }

  TextRun run =
      ConstructTextRun(f, this, start, len, StyleRef(), text_direction);
  run.SetCharactersLength(TextLength() - start);
  DCHECK_GE(run.CharactersLength(), run.length());
  run.SetTabSize(!StyleRef().CollapseWhiteSpace(), StyleRef().GetTabSize());
  run.SetXPos(lead_width + text_width_so_far);
  run.SetExpansion(expansion);

  FloatRect new_glyph_bounds;
  float result =
      f.Width(run, fallback_fonts,
              glyph_bounds_accumulation ? &new_glyph_bounds : nullptr);
  if (glyph_bounds_accumulation) {
    new_glyph_bounds.Move(text_width_so_far, 0);
    glyph_bounds_accumulation->Unite(new_glyph_bounds);
  }
  return result;
}

void LayoutText::TrimmedPrefWidths(LayoutUnit lead_width_layout_unit,
                                   LayoutUnit& first_line_min_width,
                                   bool& has_breakable_start,
                                   LayoutUnit& last_line_min_width,
                                   bool& has_breakable_end,
                                   bool& has_breakable_char,
                                   bool& has_break,
                                   LayoutUnit& first_line_max_width,
                                   LayoutUnit& last_line_max_width,
                                   LayoutUnit& min_width,
                                   LayoutUnit& max_width,
                                   bool& strip_front_spaces,
                                   TextDirection direction) {
  float float_min_width = 0.0f, float_max_width = 0.0f;

  // Convert leadWidth to a float here, to avoid multiple implict conversions
  // below.
  float lead_width = lead_width_layout_unit.ToFloat();

  bool collapse_white_space = StyleRef().CollapseWhiteSpace();
  if (!collapse_white_space)
    strip_front_spaces = false;

  if (has_tab_ || PreferredLogicalWidthsDirty())
    ComputePreferredLogicalWidths(lead_width);

  has_breakable_start = !strip_front_spaces && has_breakable_start_;
  has_breakable_end = has_breakable_end_;

  int len = TextLength();

  if (!len || (strip_front_spaces &&
               GetText().Impl()->ContainsOnlyWhitespaceOrEmpty())) {
    first_line_min_width = LayoutUnit();
    last_line_min_width = LayoutUnit();
    first_line_max_width = LayoutUnit();
    last_line_max_width = LayoutUnit();
    min_width = LayoutUnit();
    max_width = LayoutUnit();
    has_break = false;
    return;
  }

  float_min_width = min_width_;
  float_max_width = max_width_;

  first_line_min_width = LayoutUnit(first_line_min_width_);
  last_line_min_width = LayoutUnit(last_line_line_min_width_);

  has_breakable_char = has_breakable_char_;
  has_break = has_break_;

  DCHECK(text_);
  StringImpl& text = *text_.Impl();
  if (text[0] == kSpaceCharacter ||
      (text[0] == kNewlineCharacter && !StyleRef().PreserveNewline()) ||
      text[0] == kTabulationCharacter) {
    const Font& font = StyleRef().GetFont();  // FIXME: This ignores first-line.
    if (strip_front_spaces) {
      const UChar kSpaceChar = kSpaceCharacter;
      TextRun run =
          ConstructTextRun(font, &kSpaceChar, 1, StyleRef(), direction);
      float space_width = font.Width(run);
      float_max_width -= space_width;
    } else {
      float_max_width += font.GetFontDescription().WordSpacing();
    }
  }

  strip_front_spaces = collapse_white_space && has_end_white_space_;

  if (!StyleRef().AutoWrap() || float_min_width > float_max_width)
    float_min_width = float_max_width;

  // Compute our max widths by scanning the string for newlines.
  if (has_break) {
    const Font& f = StyleRef().GetFont();  // FIXME: This ignores first-line.
    bool first_line = true;
    first_line_max_width = LayoutUnit(float_max_width);
    last_line_max_width = LayoutUnit(float_max_width);
    for (int i = 0; i < len; i++) {
      int linelen = 0;
      while (i + linelen < len && text[i + linelen] != kNewlineCharacter)
        linelen++;

      if (linelen) {
        last_line_max_width = LayoutUnit(WidthFromFont(
            f, i, linelen, lead_width, last_line_max_width.ToFloat(), direction,
            nullptr, nullptr));
        if (first_line) {
          first_line = false;
          lead_width = 0.f;
          first_line_max_width = last_line_max_width;
        }
        i += linelen;
      } else if (first_line) {
        first_line_max_width = LayoutUnit();
        first_line = false;
        lead_width = 0.f;
      }

      if (i == len - 1) {
        // A <pre> run that ends with a newline, as in, e.g.,
        // <pre>Some text\n\n<span>More text</pre>
        last_line_max_width = LayoutUnit();
      }
    }
  }

  min_width = LayoutUnit::FromFloatCeil(float_min_width);
  max_width = LayoutUnit::FromFloatCeil(float_max_width);
}

float LayoutText::MinLogicalWidth() const {
  if (PreferredLogicalWidthsDirty())
    const_cast<LayoutText*>(this)->ComputePreferredLogicalWidths(0);

  return min_width_;
}

float LayoutText::MaxLogicalWidth() const {
  if (PreferredLogicalWidthsDirty())
    const_cast<LayoutText*>(this)->ComputePreferredLogicalWidths(0);

  return max_width_;
}

void LayoutText::ComputePreferredLogicalWidths(float lead_width) {
  HashSet<const SimpleFontData*> fallback_fonts;
  FloatRect glyph_bounds;
  ComputePreferredLogicalWidths(lead_width, fallback_fonts, glyph_bounds);
}

static float MinWordFragmentWidthForBreakAll(
    LayoutText* layout_text,
    const ComputedStyle& style,
    const Font& font,
    TextDirection text_direction,
    int start,
    int length,
    EWordBreak break_all_or_break_word) {
  DCHECK_GT(length, 0);
  DCHECK(break_all_or_break_word == EWordBreak::kBreakAll ||
         break_all_or_break_word == EWordBreak::kBreakWord);
  LazyLineBreakIterator break_iterator(layout_text->GetText(),
                                       style.LocaleForLineBreakIterator());
  int next_breakable = -1;
  float min = std::numeric_limits<float>::max();
  int end = start + length;
  LineBreakType line_break_type =
      break_all_or_break_word == EWordBreak::kBreakAll
          ? LineBreakType::kBreakAll
          : LineBreakType::kBreakCharacter;
  for (int i = start; i < end;) {
    break_iterator.IsBreakable(i + 1, next_breakable, line_break_type);
    int fragment_length = (next_breakable > i ? next_breakable : length) - i;

    // Ensure that malformed surrogate pairs don't cause us to read
    // past the end of the string.
    int text_length = layout_text->TextLength();
    if (i + fragment_length > text_length)
      fragment_length = std::max(text_length - i, 0);

    // The correct behavior is to measure width without re-shaping, but we
    // reshape each fragment here because a) the current line breaker does not
    // support it, b) getCharacterRange() can reshape if the text is too long
    // to fit in the cache, and c) each fragment here is almost 1 char and thus
    // reshape is fast.
    TextRun run = ConstructTextRun(font, layout_text, i, fragment_length, style,
                                   text_direction);
    float fragment_width = font.Width(run);
    min = std::min(min, fragment_width);
    i += fragment_length;
  }
  return min;
}

static float MaxWordFragmentWidth(LayoutText* layout_text,
                                  const ComputedStyle& style,
                                  const Font& font,
                                  TextDirection text_direction,
                                  Hyphenation& hyphenation,
                                  wtf_size_t word_offset,
                                  wtf_size_t word_length,
                                  int& suffix_start) {
  suffix_start = 0;
  if (word_length <= Hyphenation::kMinimumSuffixLength)
    return 0;

  Vector<wtf_size_t, 8> hyphen_locations = hyphenation.HyphenLocations(
      StringView(layout_text->GetText(), word_offset, word_length));
  if (hyphen_locations.IsEmpty())
    return 0;

  float minimum_fragment_width_to_consider =
      font.GetFontDescription().MinimumPrefixWidthToHyphenate();
  float max_fragment_width = 0;
  TextRun run = ConstructTextRun(font, layout_text, word_offset, word_length,
                                 style, text_direction);
  wtf_size_t end = word_length;
  for (wtf_size_t start : hyphen_locations) {
    float fragment_width = font.GetCharacterRange(run, start, end).Width();

    if (fragment_width <= minimum_fragment_width_to_consider)
      continue;

    max_fragment_width = std::max(max_fragment_width, fragment_width);
    end = start;
  }
  suffix_start = hyphen_locations.front();
  return max_fragment_width + layout_text->HyphenWidth(font, text_direction);
}

void LayoutText::ComputePreferredLogicalWidths(
    float lead_width,
    HashSet<const SimpleFontData*>& fallback_fonts,
    FloatRect& glyph_bounds) {
  DCHECK(has_tab_ || PreferredLogicalWidthsDirty() ||
         !known_to_have_no_overflow_and_no_fallback_fonts_);

  min_width_ = 0;
  max_width_ = 0;
  first_line_min_width_ = 0;
  last_line_line_min_width_ = 0;

  if (IsBR())
    return;

  float curr_min_width = 0;
  float curr_max_width = 0;
  has_breakable_char_ = false;
  has_break_ = false;
  has_tab_ = false;
  has_breakable_start_ = false;
  has_breakable_end_ = false;
  has_end_white_space_ = false;
  contains_only_whitespace_or_nbsp_ =
      static_cast<unsigned>(OnlyWhitespaceOrNbsp::kYes);

  const ComputedStyle& style_to_use = StyleRef();
  const Font& f = style_to_use.GetFont();  // FIXME: This ignores first-line.
  float word_spacing = style_to_use.WordSpacing();
  int len = TextLength();
  LazyLineBreakIterator break_iterator(
      text_, style_to_use.LocaleForLineBreakIterator());
  bool needs_word_spacing = false;
  bool ignoring_spaces = false;
  bool is_space = false;
  bool first_word = true;
  bool first_line = true;
  int next_breakable = -1;
  int last_word_boundary = 0;
  float cached_word_trailing_space_width[2] = {0, 0};  // LTR, RTL

  EWordBreak break_all_or_break_word = EWordBreak::kNormal;
  LineBreakType line_break_type = LineBreakType::kNormal;
  if (style_to_use.AutoWrap()) {
    if (style_to_use.WordBreak() == EWordBreak::kBreakAll ||
        style_to_use.WordBreak() == EWordBreak::kBreakWord) {
      break_all_or_break_word = style_to_use.WordBreak();
    } else if (style_to_use.WordBreak() == EWordBreak::kKeepAll) {
      line_break_type = LineBreakType::kKeepAll;
    }
    if (style_to_use.OverflowWrap() == EOverflowWrap::kAnywhere)
      break_all_or_break_word = EWordBreak::kBreakWord;
  }

  Hyphenation* hyphenation =
      style_to_use.AutoWrap() ? style_to_use.GetHyphenation() : nullptr;
  bool disable_soft_hyphen = style_to_use.GetHyphens() == Hyphens::kNone;
  float max_word_width = 0;
  if (!hyphenation)
    max_word_width = std::numeric_limits<float>::infinity();

  BidiResolver<TextRunIterator, BidiCharacterRun> bidi_resolver;
  BidiCharacterRun* run;
  TextDirection text_direction = style_to_use.Direction();
  if ((Is8Bit() && text_direction == TextDirection::kLtr) ||
      IsOverride(style_to_use.GetUnicodeBidi())) {
    run = nullptr;
  } else {
    TextRun text_run(GetText());
    BidiStatus status(text_direction, false);
    bidi_resolver.SetStatus(status);
    bidi_resolver.SetPositionIgnoringNestedIsolates(
        TextRunIterator(&text_run, 0));
    bool hard_line_break = false;
    bool reorder_runs = false;
    bidi_resolver.CreateBidiRunsForLine(
        TextRunIterator(&text_run, text_run.length()), kNoVisualOverride,
        hard_line_break, reorder_runs);
    BidiRunList<BidiCharacterRun>& bidi_runs = bidi_resolver.Runs();
    run = bidi_runs.FirstRun();
  }

  for (int i = 0; i < len; i++) {
    UChar c = UncheckedCharacterAt(i);

    if (run) {
      // Treat adjacent runs with the same resolved directionality
      // (TextDirection as opposed to WTF::unicode::Direction) as belonging
      // to the same run to avoid breaking unnecessarily.
      while (i >= run->Stop() ||
             (run->Next() && run->Next()->Direction() == run->Direction()))
        run = run->Next();

      DCHECK(run);
      DCHECK_LE(i, run->Stop());
      text_direction = run->Direction();
    }

    bool previous_character_is_space = is_space;
    bool is_newline = false;
    if (c == kNewlineCharacter) {
      if (style_to_use.PreserveNewline()) {
        has_break_ = true;
        is_newline = true;
        is_space = false;
      } else {
        is_space = true;
      }
    } else if (c == kTabulationCharacter) {
      if (!style_to_use.CollapseWhiteSpace()) {
        has_tab_ = true;
        is_space = false;
      } else {
        is_space = true;
      }
    } else if (c == kSpaceCharacter) {
      is_space = true;
    } else if (c == kNoBreakSpaceCharacter) {
      is_space = false;
    } else {
      is_space = false;
      contains_only_whitespace_or_nbsp_ =
          static_cast<unsigned>(OnlyWhitespaceOrNbsp::kNo);
    }

    bool is_breakable_location =
        is_newline || (is_space && style_to_use.AutoWrap()) ||
        break_all_or_break_word == EWordBreak::kBreakWord;
    if (!i)
      has_breakable_start_ = is_breakable_location;
    if (i == len - 1) {
      has_breakable_end_ = is_breakable_location;
      has_end_white_space_ = is_newline || is_space;
    }

    if (!ignoring_spaces && style_to_use.CollapseWhiteSpace() &&
        previous_character_is_space && is_space)
      ignoring_spaces = true;

    if (ignoring_spaces && !is_space)
      ignoring_spaces = false;

    // Ignore spaces and soft hyphens
    if (ignoring_spaces) {
      DCHECK_EQ(last_word_boundary, i);
      last_word_boundary++;
      continue;
    }
    if (c == kSoftHyphenCharacter && !disable_soft_hyphen) {
      curr_max_width += WidthFromFont(
          f, last_word_boundary, i - last_word_boundary, lead_width,
          curr_max_width, text_direction, &fallback_fonts, &glyph_bounds);
      last_word_boundary = i + 1;
      continue;
    }

    bool has_break =
        break_iterator.IsBreakable(i, next_breakable, line_break_type);
    bool between_words = true;
    int j = i;
    while (c != kNewlineCharacter && c != kSpaceCharacter &&
           c != kTabulationCharacter &&
           (c != kSoftHyphenCharacter || disable_soft_hyphen)) {
      j++;
      if (j == len)
        break;
      c = UncheckedCharacterAt(j);
      if (break_iterator.IsBreakable(j, next_breakable) &&
          CharacterAt(j - 1) != kSoftHyphenCharacter)
        break;
    }

    // Terminate word boundary at bidi run boundary.
    if (run)
      j = std::min(j, run->Stop() + 1);
    int word_len = j - i;
    if (word_len) {
      bool is_space = (j < len) && c == kSpaceCharacter;

      // Non-zero only when kerning is enabled, in which case we measure words
      // with their trailing space, then subtract its width.
      float word_trailing_space_width = 0;
      if (is_space &&
          (f.GetFontDescription().GetTypesettingFeatures() & kKerning)) {
        const unsigned text_direction_index =
            static_cast<unsigned>(text_direction);
        DCHECK_GE(text_direction_index, 0U);
        DCHECK_LE(text_direction_index, 1U);
        if (!cached_word_trailing_space_width[text_direction_index]) {
          cached_word_trailing_space_width[text_direction_index] =
              f.Width(ConstructTextRun(f, &kSpaceCharacter, 1, style_to_use,
                                       text_direction)) +
              word_spacing;
        }
        word_trailing_space_width =
            cached_word_trailing_space_width[text_direction_index];
      }

      float w;
      if (word_trailing_space_width && is_space) {
        w = WidthFromFont(f, i, word_len + 1, lead_width, curr_max_width,
                          text_direction, &fallback_fonts, &glyph_bounds) -
            word_trailing_space_width;
      } else {
        w = WidthFromFont(f, i, word_len, lead_width, curr_max_width,
                          text_direction, &fallback_fonts, &glyph_bounds);
        if (c == kSoftHyphenCharacter && !disable_soft_hyphen)
          curr_min_width += HyphenWidth(f, text_direction);
      }

      if (w > max_word_width) {
        DCHECK(hyphenation);
        int suffix_start;
        float max_fragment_width =
            MaxWordFragmentWidth(this, style_to_use, f, text_direction,
                                 *hyphenation, i, word_len, suffix_start);
        if (suffix_start) {
          float suffix_width;
          if (word_trailing_space_width && is_space) {
            suffix_width =
                WidthFromFont(f, i + suffix_start, word_len - suffix_start + 1,
                              lead_width, curr_max_width, text_direction,
                              &fallback_fonts, &glyph_bounds) -
                word_trailing_space_width;
          } else {
            suffix_width = WidthFromFont(
                f, i + suffix_start, word_len - suffix_start, lead_width,
                curr_max_width, text_direction, &fallback_fonts, &glyph_bounds);
          }
          max_fragment_width = std::max(max_fragment_width, suffix_width);
          curr_min_width += max_fragment_width - w;
          max_word_width = std::max(max_word_width, max_fragment_width);
        } else {
          max_word_width = w;
        }
      }

      if (break_all_or_break_word != EWordBreak::kNormal) {
        has_breakable_char_ = true;
        // Because sum of character widths may not be equal to the word width,
        // we need to measure twice; once with normal break for max width,
        // another with break-all for min width.
        curr_min_width = MinWordFragmentWidthForBreakAll(
            this, style_to_use, f, text_direction, i, word_len,
            break_all_or_break_word);
      } else {
        curr_min_width += w;
      }
      if (between_words) {
        if (last_word_boundary == i) {
          curr_max_width += w;
        } else {
          curr_max_width += WidthFromFont(
              f, last_word_boundary, j - last_word_boundary, lead_width,
              curr_max_width, text_direction, &fallback_fonts, &glyph_bounds);
        }
        last_word_boundary = j;
      }

      bool is_collapsible_white_space =
          (j < len) && style_to_use.IsCollapsibleWhiteSpace(c);
      if (j < len && style_to_use.AutoWrap())
        has_breakable_char_ = true;

      // Add in wordSpacing to our currMaxWidth, but not if this is the last
      // word on a line or the
      // last word in the run.
      if (word_spacing && (is_space || is_collapsible_white_space) &&
          !ContainsOnlyWhitespace(j, len - j))
        curr_max_width += word_spacing;

      if (first_word) {
        first_word = false;
        // If the first character in the run is breakable, then we consider
        // ourselves to have a beginning minimum width of 0, since a break could
        // occur right before our run starts, preventing us from ever being
        // appended to a previous text run when considering the total minimum
        // width of the containing block.
        if (has_break)
          has_breakable_char_ = true;
        first_line_min_width_ = has_break ? 0 : curr_min_width;
      }
      last_line_line_min_width_ = curr_min_width;

      if (curr_min_width > min_width_)
        min_width_ = curr_min_width;
      curr_min_width = 0;

      i += word_len - 1;
    } else {
      // Nowrap can never be broken, so don't bother setting the breakable
      // character boolean. Pre can only be broken if we encounter a newline.
      if (StyleRef().AutoWrap() || is_newline)
        has_breakable_char_ = true;

      if (curr_min_width > min_width_)
        min_width_ = curr_min_width;
      curr_min_width = 0;

      // Only set if preserveNewline was true and we saw a newline.
      if (is_newline) {
        if (first_line) {
          first_line = false;
          lead_width = 0;
          if (!style_to_use.AutoWrap())
            first_line_min_width_ = curr_max_width;
        }

        if (curr_max_width > max_width_)
          max_width_ = curr_max_width;
        curr_max_width = 0;
      } else {
        TextRun run =
            ConstructTextRun(f, this, i, 1, style_to_use, text_direction);
        run.SetCharactersLength(len - i);
        DCHECK_GE(run.CharactersLength(), run.length());
        run.SetTabSize(!StyleRef().CollapseWhiteSpace(),
                       StyleRef().GetTabSize());
        run.SetXPos(lead_width + curr_max_width);

        curr_max_width += f.Width(run);
        needs_word_spacing =
            is_space && !previous_character_is_space && i == len - 1;
      }
      DCHECK_EQ(last_word_boundary, i);
      last_word_boundary++;
    }
  }
  if (run)
    bidi_resolver.Runs().DeleteRuns();

  if ((needs_word_spacing && len > 1) || (ignoring_spaces && !first_word))
    curr_max_width += word_spacing;

  min_width_ = std::max(curr_min_width, min_width_);
  max_width_ = std::max(curr_max_width, max_width_);

  if (!style_to_use.AutoWrap())
    min_width_ = max_width_;

  if (style_to_use.WhiteSpace() == EWhiteSpace::kPre) {
    if (first_line)
      first_line_min_width_ = max_width_;
    last_line_line_min_width_ = curr_max_width;
  }

  GlyphOverflow glyph_overflow;
  glyph_overflow.SetFromBounds(glyph_bounds, f, max_width_);
  // We shouldn't change our mind once we "know".
  DCHECK(!known_to_have_no_overflow_and_no_fallback_fonts_ ||
         (fallback_fonts.IsEmpty() && glyph_overflow.IsApproximatelyZero()));
  known_to_have_no_overflow_and_no_fallback_fonts_ =
      fallback_fonts.IsEmpty() && glyph_overflow.IsApproximatelyZero();

  ClearPreferredLogicalWidthsDirty();
}

bool LayoutText::IsAllCollapsibleWhitespace() const {
  unsigned length = TextLength();
  if (Is8Bit()) {
    for (unsigned i = 0; i < length; ++i) {
      if (!StyleRef().IsCollapsibleWhiteSpace(Characters8()[i]))
        return false;
    }
    return true;
  }
  for (unsigned i = 0; i < length; ++i) {
    if (!StyleRef().IsCollapsibleWhiteSpace(Characters16()[i]))
      return false;
  }
  return true;
}

bool LayoutText::ContainsOnlyWhitespace(unsigned from, unsigned len) const {
  DCHECK(text_);
  StringImpl& text = *text_.Impl();
  unsigned curr_pos;
  for (curr_pos = from;
       curr_pos < from + len && (text[curr_pos] == kNewlineCharacter ||
                                 text[curr_pos] == kSpaceCharacter ||
                                 text[curr_pos] == kTabulationCharacter);
       curr_pos++) {
  }
  return curr_pos >= (from + len);
}

UChar32 LayoutText::FirstCharacterAfterWhitespaceCollapsing() const {
  if (InlineTextBox* text_box = FirstTextBox()) {
    String text = text_box->GetText();
    return text.length() ? text.CharacterStartingAt(0) : 0;
  }
  if (IsInLayoutNGInlineFormattingContext()) {
    NGInlineCursor cursor;
    cursor.MoveTo(*this);
    if (cursor) {
      const StringView text = cursor.CurrentText();
      return text.length() ? text.CodepointAt(0) : 0;
    }
  }
  return 0;
}

UChar32 LayoutText::LastCharacterAfterWhitespaceCollapsing() const {
  if (InlineTextBox* text_box = LastTextBox()) {
    String text = text_box->GetText();
    return text.length() ? StringView(text).CodepointAt(text.length() - 1) : 0;
  }
  if (IsInLayoutNGInlineFormattingContext()) {
    NGInlineCursor cursor;
    cursor.MoveTo(*this);
    if (cursor) {
      const StringView text = cursor.CurrentText();
      return text.length() ? text.CodepointAt(text.length() - 1) : 0;
    }
  }
  return 0;
}

PhysicalOffset LayoutText::FirstLineBoxTopLeft() const {
  if (IsInLayoutNGInlineFormattingContext()) {
    // TODO(kojii): Some clients call this against dirty-tree, but NG fragments
    // are not safe to read for dirty-tree. crbug.com/963103
    if (UNLIKELY(!IsFirstInlineFragmentSafe()))
      return PhysicalOffset();
    NGInlineCursor cursor;
    cursor.MoveTo(*this);
    return cursor ? cursor.Current().OffsetInContainerBlock()
                  : PhysicalOffset();
  }
  if (const auto* text_box = FirstTextBox()) {
    LayoutPoint location = text_box->Location();
    if (UNLIKELY(HasFlippedBlocksWritingMode())) {
      location.Move(text_box->Width(), LayoutUnit());
      return ContainingBlock()->FlipForWritingMode(location);
    }
    return PhysicalOffset(location);
  }
  return PhysicalOffset();
}

bool LayoutText::CanOptimizeSetText() const {
  // If we have only one line of text and "contain: layout size" we can avoid
  // doing a layout and only paint in the SetText() operation.
  auto* parent_block_flow = DynamicTo<LayoutBlockFlow>(Parent());
  return parent_block_flow && Parent()->ShouldApplyLayoutContainment() &&
         Parent()->ShouldApplySizeContainment() &&
         // If we have "text-overflow: ellipsis" we need to check if we need or
         // not ellipsis in the new text and recompute its position.
         !parent_block_flow->ShouldTruncateOverflowingText() &&
         !PreviousSibling() && !NextSibling() && FirstTextBox() &&
         FirstTextBox() == LastTextBox() &&
         // If "line-height" is "normal" we might need to recompute the
         // baseline which is not straight forward.
         !StyleRef().LineHeight().IsNegative() &&
         // We would need to recompute the position if "direction" is "rtl".
         StyleRef().IsLeftToRightDirection() &&
         // We would need to layout the text if it is justified.
         (StyleRef().GetTextAlign(true) != ETextAlign::kJustify);
}

void LayoutText::SetFirstTextBoxLogicalLeft(float text_width) const {
  DCHECK(FirstTextBox());
  DCHECK(ContainingBlock());
  DCHECK(StyleRef().IsLeftToRightDirection());

  LayoutUnit offset_left = ContainingBlock()->LogicalLeftOffsetForContent();
  LayoutUnit available_space = ContainingBlock()->ContentLogicalWidth();

  // If |text_width| is bigger than |available_space| it's because the text
  // doesn't wrap so we don't need to align anything.
  if (text_width < available_space) {
    switch (StyleRef().GetTextAlign(true)) {
      case ETextAlign::kLeft:
      case ETextAlign::kWebkitLeft:
      case ETextAlign::kJustify:
      case ETextAlign::kStart:
        // Do nothing.
        break;
      case ETextAlign::kRight:
      case ETextAlign::kWebkitRight:
      case ETextAlign::kEnd:
        offset_left += available_space - text_width;
        break;
      case ETextAlign::kCenter:
      case ETextAlign::kWebkitCenter:
        offset_left += (available_space - text_width) / 2;
        break;
    }
  }

  FirstTextBox()->SetLogicalLeft(offset_left);
}

void LayoutText::SetTextWithOffset(scoped_refptr<StringImpl> text,
                                   unsigned offset,
                                   unsigned len) {
  if (Equal(text_.Impl(), text.get()))
    return;

  // Check that we are replacing the whole text.
  if (offset == 0 && len == TextLength() && CanOptimizeSetText()) {
    const ComputedStyle* style_to_use =
        FirstTextBox()->GetLineLayoutItem().Style(
            FirstTextBox()->IsFirstLineStyle());
    TextRun text_run = TextRun(String(text));
    text_run.SetTabSize(!style_to_use->CollapseWhiteSpace(),
                        style_to_use->GetTabSize());
    FloatRect glyph_bounds;
    float text_width =
        style_to_use->GetFont().Width(text_run, nullptr, &glyph_bounds);
    // If the text is not wrapping we don't care if it fits or not in the
    // container as it's not going to be split in multiple lines.
    if (!style_to_use->AutoWrap() ||
        (text_width <= ContainingBlock()->ContentLogicalWidth())) {
      FirstTextBox()->ManuallySetStartLenAndLogicalWidth(
          offset, text->length(), LayoutUnit(text_width));
      SetFirstTextBoxLogicalLeft(text_width);
      SetTextInternal(std::move(text));
      SetShouldDoFullPaintInvalidation();
      TextDidChangeWithoutInvalidation();
      lines_dirty_ = false;
      valid_ng_items_ = false;
      return;
    }
  }

  if (NGInlineNode::SetTextWithOffset(this, text, offset, len)) {
    DCHECK(!NeedsCollectInlines());
    // Prevent |TextDidChange()| to propagate |NeedsCollectInlines|
    SetNeedsCollectInlines(true);
    TextDidChange();
    valid_ng_items_ = true;
    ClearNeedsCollectInlines();
    return;
  }

  unsigned old_len = TextLength();
  unsigned new_len = text->length();
  int delta = new_len - old_len;
  unsigned end = len ? offset + len - 1 : offset;

  RootInlineBox* first_root_box = nullptr;
  RootInlineBox* last_root_box = nullptr;

  bool dirtied_lines = false;

  // Dirty all text boxes that include characters in between offset and
  // offset+len.
  for (InlineTextBox* curr : TextBoxes()) {
    // FIXME: This shouldn't rely on the end of a dirty line box. See
    // https://bugs.webkit.org/show_bug.cgi?id=97264
    // Text run is entirely before the affected range.
    if (curr->end() < offset)
      continue;

    // Text run is entirely after the affected range.
    if (curr->Start() > end) {
      curr->OffsetRun(delta);
      RootInlineBox* root = &curr->Root();
      if (!first_root_box) {
        first_root_box = root;
        // The affected area was in between two runs. Go ahead and mark the root
        // box of the run after the affected area as dirty.
        first_root_box->MarkDirty();
        dirtied_lines = true;
      }
      last_root_box = root;
    } else if (curr->end() >= offset && curr->end() <= end) {
      // Text run overlaps with the left end of the affected range.
      curr->DirtyLineBoxes();
      dirtied_lines = true;
    } else if (curr->Start() <= offset && curr->end() >= end) {
      // Text run subsumes the affected range.
      curr->DirtyLineBoxes();
      dirtied_lines = true;
    } else if (curr->Start() <= end && curr->end() >= end) {
      // Text run overlaps with right end of the affected range.
      curr->DirtyLineBoxes();
      dirtied_lines = true;
    }
  }

  // Now we have to walk all of the clean lines and adjust their cached line
  // break information to reflect our updated offsets.
  if (last_root_box)
    last_root_box = last_root_box->NextRootBox();
  if (first_root_box) {
    RootInlineBox* prev = first_root_box->PrevRootBox();
    if (prev)
      first_root_box = prev;
  } else if (LastTextBox()) {
    DCHECK(!last_root_box);
    first_root_box = &LastTextBox()->Root();
    first_root_box->MarkDirty();
    dirtied_lines = true;
  }
  for (RootInlineBox* curr = first_root_box; curr && curr != last_root_box;
       curr = curr->NextRootBox()) {
    if (curr->LineBreakObj().IsEqual(this) && curr->LineBreakPos() > end)
      curr->SetLineBreakPos(clampTo<int>(curr->LineBreakPos() + delta));
  }

  // If the text node is empty, dirty the line where new text will be inserted.
  if (!FirstTextBox() && Parent()) {
    Parent()->DirtyLinesFromChangedChild(this);
    dirtied_lines = true;
  }

  lines_dirty_ = dirtied_lines;
  ForceSetText(std::move(text));

  // TODO(layout-dev): Invalidation is currently all or nothing in LayoutNG,
  // this is probably fine for NGInlineItem reuse as recreating the individual
  // items is relatively cheap. If partial relayout performance improvement are
  // needed partial re-shapes are likely to be sufficient. Revisit as needed.
  valid_ng_items_ = false;
}

void LayoutText::TransformText() {
  if (scoped_refptr<StringImpl> text_to_transform = OriginalText())
    ForceSetText(std::move(text_to_transform));
}

static inline bool IsInlineFlowOrEmptyText(const LayoutObject* o) {
  if (o->IsLayoutInline())
    return true;
  if (!o->IsText())
    return false;
  return ToLayoutText(o)->GetText().IsEmpty();
}

OnlyWhitespaceOrNbsp LayoutText::ContainsOnlyWhitespaceOrNbsp() const {
  return PreferredLogicalWidthsDirty() ? OnlyWhitespaceOrNbsp::kUnknown
                                       : static_cast<OnlyWhitespaceOrNbsp>(
                                             contains_only_whitespace_or_nbsp_);
}

UChar LayoutText::PreviousCharacter() const {
  // find previous text layoutObject if one exists
  const LayoutObject* previous_text = PreviousInPreOrder();
  for (; previous_text; previous_text = previous_text->PreviousInPreOrder()) {
    if (!IsInlineFlowOrEmptyText(previous_text))
      break;
  }
  UChar prev = kSpaceCharacter;
  if (previous_text && previous_text->IsText()) {
    if (StringImpl* previous_string =
            ToLayoutText(previous_text)->GetText().Impl())
      prev = (*previous_string)[previous_string->length() - 1];
  }
  return prev;
}

void LayoutText::SetTextInternal(scoped_refptr<StringImpl> text) {
  DCHECK(text);
  text_ = String(std::move(text));
  DCHECK(text_);
  DCHECK(!IsBR() || (TextLength() == 1 && text_[0] == kNewlineCharacter));
}

void LayoutText::ApplyTextTransform() {
  if (const ComputedStyle* style = Style()) {
    style->ApplyTextTransform(&text_, PreviousCharacter());

    // We use the same characters here as for list markers.
    // See the listMarkerText function in LayoutListMarker.cpp.
    switch (style->TextSecurity()) {
      case ETextSecurity::kNone:
        break;
      case ETextSecurity::kCircle:
        SecureText(kWhiteBulletCharacter);
        break;
      case ETextSecurity::kDisc:
        SecureText(kBulletCharacter);
        break;
      case ETextSecurity::kSquare:
        SecureText(kBlackSquareCharacter);
    }
  }
}

void LayoutText::SecureText(UChar mask) {
  if (!text_.length())
    return;

  int last_typed_character_offset_to_reveal = -1;
  UChar revealed_text;
  SecureTextTimer* secure_text_timer =
      g_secure_text_timers ? g_secure_text_timers->at(this) : nullptr;
  if (secure_text_timer && secure_text_timer->IsActive()) {
    last_typed_character_offset_to_reveal =
        secure_text_timer->LastTypedCharacterOffset();
    if (last_typed_character_offset_to_reveal >= 0)
      revealed_text = text_[last_typed_character_offset_to_reveal];
  }

  text_.Fill(mask);
  if (last_typed_character_offset_to_reveal >= 0) {
    text_.replace(last_typed_character_offset_to_reveal, 1,
                  String(&revealed_text, 1));
    // m_text may be updated later before timer fires. We invalidate the
    // lastTypedCharacterOffset to avoid inconsistency.
    secure_text_timer->Invalidate();
  }
}

void LayoutText::SetTextIfNeeded(scoped_refptr<StringImpl> text) {
  DCHECK(text);

  if (Equal(text_.Impl(), text.get()))
    return;
  ForceSetText(std::move(text));
}

void LayoutText::ForceSetText(scoped_refptr<StringImpl> text) {
  DCHECK(text);
  SetTextInternal(std::move(text));
  TextDidChange();
}

void LayoutText::TextDidChange() {
  // If preferredLogicalWidthsDirty() of an orphan child is true,
  // LayoutObjectChildList::insertChildNode() fails to set true to owner.
  // To avoid that, we call setNeedsLayoutAndPrefWidthsRecalc() only if this
  // LayoutText has parent.
  if (Parent()) {
    SetNeedsLayoutAndPrefWidthsRecalcAndFullPaintInvalidation(
        layout_invalidation_reason::kTextChanged);
  }
  TextDidChangeWithoutInvalidation();
}

void LayoutText::TextDidChangeWithoutInvalidation() {
  ApplyTextTransform();
  known_to_have_no_overflow_and_no_fallback_fonts_ = false;

  if (AXObjectCache* cache = GetDocument().ExistingAXObjectCache())
    cache->TextChanged(this);

  TextAutosizer* text_autosizer = GetDocument().GetTextAutosizer();
  if (text_autosizer)
    text_autosizer->Record(this);

  if (HasNodeId()) {
    if (auto* content_capture_manager = GetContentCaptureManager())
      content_capture_manager->OnNodeTextChanged(*GetNode());
  }

  valid_ng_items_ = false;
  SetNeedsCollectInlines();
}

void LayoutText::DirtyOrDeleteLineBoxesIfNeeded(bool full_layout) {
  if (full_layout)
    DeleteTextBoxes();
  else if (!lines_dirty_)
    DirtyLineBoxes();
  lines_dirty_ = false;
  valid_ng_items_ = false;
}

void LayoutText::DirtyLineBoxes() {
  for (InlineTextBox* box : TextBoxes())
    box->DirtyLineBoxes();
  lines_dirty_ = false;
  valid_ng_items_ = false;
}

InlineTextBox* LayoutText::CreateTextBox(int start, uint16_t length) {
  return new InlineTextBox(LineLayoutItem(this), start, length);
}

InlineTextBox* LayoutText::CreateInlineTextBox(int start, uint16_t length) {
  InlineTextBox* text_box = CreateTextBox(start, length);
  MutableTextBoxes().AppendLineBox(text_box);
  return text_box;
}

void LayoutText::PositionLineBox(InlineBox* box) {
  InlineTextBox* s = ToInlineTextBox(box);

  // FIXME: should not be needed!!!
  if (!s->Len()) {
    // We want the box to be destroyed.
    s->Remove(kDontMarkLineBoxes);
    MutableTextBoxes().RemoveLineBox(s);
    s->Destroy();
    return;
  }

  contains_reversed_text_ |= !s->IsLeftToRightDirection();
}

float LayoutText::Width(unsigned from,
                        unsigned len,
                        LayoutUnit x_pos,
                        TextDirection text_direction,
                        bool first_line,
                        HashSet<const SimpleFontData*>* fallback_fonts,
                        FloatRect* glyph_bounds,
                        float expansion) const {
  if (from >= TextLength())
    return 0;

  if (len > TextLength() || from + len > TextLength())
    len = TextLength() - from;

  return Width(from, len, Style(first_line)->GetFont(), x_pos, text_direction,
               fallback_fonts, glyph_bounds, expansion);
}

float LayoutText::Width(unsigned from,
                        unsigned len,
                        const Font& f,
                        LayoutUnit x_pos,
                        TextDirection text_direction,
                        HashSet<const SimpleFontData*>* fallback_fonts,
                        FloatRect* glyph_bounds,
                        float expansion) const {
  DCHECK_LE(from + len, TextLength());
  if (!TextLength())
    return 0;

  const SimpleFontData* font_data = f.PrimaryFont();
  DCHECK(font_data);
  if (!font_data)
    return 0;

  float w;
  if (&f == &StyleRef().GetFont()) {
    if (!StyleRef().PreserveNewline() && !from && len == TextLength()) {
      if (fallback_fonts) {
        DCHECK(glyph_bounds);
        if (PreferredLogicalWidthsDirty() ||
            !known_to_have_no_overflow_and_no_fallback_fonts_) {
          const_cast<LayoutText*>(this)->ComputePreferredLogicalWidths(
              0, *fallback_fonts, *glyph_bounds);
        } else {
          *glyph_bounds =
              FloatRect(0, -font_data->GetFontMetrics().FloatAscent(),
                        max_width_, font_data->GetFontMetrics().FloatHeight());
        }
        w = max_width_;
      } else {
        w = MaxLogicalWidth();
      }
    } else {
      w = WidthFromFont(f, from, len, x_pos.ToFloat(), 0, text_direction,
                        fallback_fonts, glyph_bounds, expansion);
    }
  } else {
    TextRun run =
        ConstructTextRun(f, this, from, len, StyleRef(), text_direction);
    run.SetCharactersLength(TextLength() - from);
    DCHECK_GE(run.CharactersLength(), run.length());

    run.SetTabSize(!StyleRef().CollapseWhiteSpace(), StyleRef().GetTabSize());
    run.SetXPos(x_pos.ToFloat());
    w = f.Width(run, fallback_fonts, glyph_bounds);
  }

  return w;
}

PhysicalRect LayoutText::PhysicalLinesBoundingBox() const {
  PhysicalRect result;
  CollectLineBoxRects(
      [&result](const PhysicalRect& r) { result.UniteIfNonZero(r); });
  // Some callers expect correct offset even if the rect is empty.
  if (result == PhysicalRect())
    result.offset = FirstLineBoxTopLeft();
  return result;
}

PhysicalRect LayoutText::PhysicalVisualOverflowRect() const {
  if (IsInLayoutNGInlineFormattingContext()) {
    DCHECK(RuntimeEnabledFeatures::LayoutNGEnabled());
    if (RuntimeEnabledFeatures::LayoutNGFragmentItemEnabled())
      return NGFragmentItem::LocalVisualRectFor(*this);

    if (base::Optional<PhysicalRect> rect =
            NGPaintFragment::LocalVisualRectFor(*this))
      return *rect;
  }

  if (!FirstTextBox())
    return PhysicalRect();

  // Return the width of the minimal left side and the maximal right side.
  LayoutUnit logical_left_side = LayoutUnit::Max();
  LayoutUnit logical_right_side = LayoutUnit::Min();
  for (InlineTextBox* curr : TextBoxes()) {
    LayoutRect logical_visual_overflow = curr->LogicalOverflowRect();
    logical_left_side =
        std::min(logical_left_side, logical_visual_overflow.X());
    logical_right_side =
        std::max(logical_right_side, logical_visual_overflow.MaxX());
  }

  LayoutUnit logical_top = FirstTextBox()->LogicalTopVisualOverflow();
  LayoutUnit logical_width = logical_right_side - logical_left_side;
  LayoutUnit logical_height =
      LastTextBox()->LogicalBottomVisualOverflow() - logical_top;

  // Inflate visual overflow if we have adjusted ascent/descent causing the
  // painted glyphs to overflow the layout geometries based on the adjusted
  // ascent/descent.
  unsigned inflation_for_ascent = 0;
  unsigned inflation_for_descent = 0;
  const auto* font_data =
      StyleRef(FirstTextBox()->IsFirstLineStyle()).GetFont().PrimaryFont();
  if (font_data)
    inflation_for_ascent = font_data->VisualOverflowInflationForAscent();
  if (LastTextBox()->IsFirstLineStyle() != FirstTextBox()->IsFirstLineStyle()) {
    font_data =
        StyleRef(LastTextBox()->IsFirstLineStyle()).GetFont().PrimaryFont();
  }
  if (font_data)
    inflation_for_descent = font_data->VisualOverflowInflationForDescent();
  logical_top -= LayoutUnit(inflation_for_ascent);
  logical_height += LayoutUnit(inflation_for_ascent + inflation_for_descent);

  LayoutRect rect(logical_left_side, logical_top, logical_width,
                  logical_height);
  if (!StyleRef().IsHorizontalWritingMode())
    rect = rect.TransposedRect();
  return FlipForWritingMode(rect);
}

PhysicalRect LayoutText::LocalVisualRectIgnoringVisibility() const {
  return UnionRect(PhysicalVisualOverflowRect(), LocalSelectionVisualRect());
}

PhysicalRect LayoutText::LocalSelectionVisualRect() const {
  DCHECK(!NeedsLayout());

  if (!IsSelected())
    return PhysicalRect();

  const FrameSelection& frame_selection = GetFrame()->Selection();
  if (IsInLayoutNGInlineFormattingContext()) {
    PhysicalRect rect;
    NGInlineCursor cursor(*RootInlineFormattingContext());
    for (cursor.MoveTo(*this); cursor; cursor.MoveToNextForSameLayoutObject()) {
      if (cursor.IsHiddenForPaint())
        continue;
      const LayoutSelectionStatus status =
          frame_selection.ComputeLayoutSelectionStatus(cursor);
      if (status.start == status.end)
        continue;
      PhysicalRect item_rect = ComputeLocalSelectionRectForText(cursor, status);
      item_rect.offset += cursor.Current().OffsetInContainerBlock();
      rect.Unite(item_rect);
    }
    return rect;
  }

  const LayoutTextSelectionStatus& selection_status =
      frame_selection.ComputeLayoutSelectionStatus(*this);
  const unsigned start_pos = selection_status.start;
  const unsigned end_pos = selection_status.end;
  DCHECK_LE(start_pos, end_pos);
  LayoutRect rect;
  for (InlineTextBox* box : TextBoxes()) {
    rect.Unite(box->LocalSelectionRect(start_pos, end_pos));
    rect.Unite(LayoutRect(EllipsisRectForBox(box, start_pos, end_pos)));
  }

  return FlipForWritingMode(rect);
}

const NGOffsetMapping* LayoutText::GetNGOffsetMapping() const {
  if (!RuntimeEnabledFeatures::LayoutNGEnabled())
    return nullptr;
  return NGOffsetMapping::GetFor(this);
}

Position LayoutText::PositionForCaretOffset(unsigned offset) const {
  // ::first-letter handling should be done by LayoutTextFragment override.
  DCHECK(!IsTextFragment());
  // BR handling should be done by LayoutBR override.
  DCHECK(!IsBR());
  // WBR handling should be done by LayoutWordBreak override.
  DCHECK(!IsWordBreak());
  DCHECK_LE(offset, TextLength());
  const Node* node = GetNode();
  if (!node)
    return Position();
  auto* text_node = To<Text>(node);
  // TODO(layout-dev): Support offset change due to text-transform.
#if DCHECK_IS_ON()
  // Ensures that the clamping hack kicks in only with text-transform.
  if (StyleRef().TextTransform() == ETextTransform::kNone)
    DCHECK_LE(offset, text_node->length());
#endif
  const unsigned clamped_offset = std::min(offset, text_node->length());
  return Position(node, clamped_offset);
}

base::Optional<unsigned> LayoutText::CaretOffsetForPosition(
    const Position& position) const {
  // ::first-letter handling should be done by LayoutTextFragment override.
  DCHECK(!IsTextFragment());
  // BR handling should be done by LayoutBR override.
  DCHECK(!IsBR());
  // WBR handling should be done by LayoutWordBreak override.
  DCHECK(!IsWordBreak());
  if (position.IsNull() || position.AnchorNode() != GetNode())
    return base::nullopt;
  DCHECK(GetNode()->IsTextNode());
  if (position.IsBeforeAnchor())
    return 0;
  // TODO(layout-dev): Support offset change due to text-transform.
  if (position.IsAfterAnchor())
    return TextLength();
  DCHECK(position.IsOffsetInAnchor()) << position;
  DCHECK_LE(position.OffsetInContainerNode(), static_cast<int>(TextLength()))
      << position;
  return position.OffsetInContainerNode();
}

int LayoutText::CaretMinOffset() const {
  DCHECK(!GetDocument().NeedsLayoutTreeUpdate());

  if (auto* mapping = GetNGOffsetMapping()) {
    const Position first_position = PositionForCaretOffset(0);
    if (first_position.IsNull())
      return 0;
    base::Optional<unsigned> candidate = CaretOffsetForPosition(
        mapping->StartOfNextNonCollapsedContent(first_position));
    // Align with the legacy behavior that 0 is returned if the entire node
    // contains only collapsed whitespaces.
    const bool fully_collapsed = !candidate || *candidate == TextLength();
    return fully_collapsed ? 0 : *candidate;
  }

  InlineTextBox* box = FirstTextBox();
  if (!box)
    return 0;
  int min_offset = box->Start();
  while ((box = box->NextForSameLayoutObject()))
    min_offset = std::min<int>(min_offset, box->Start());
  return min_offset;
}

int LayoutText::CaretMaxOffset() const {
  DCHECK(!GetDocument().NeedsLayoutTreeUpdate());

  if (auto* mapping = GetNGOffsetMapping()) {
    const Position last_position = PositionForCaretOffset(TextLength());
    if (last_position.IsNull())
      return TextLength();
    base::Optional<unsigned> candidate = CaretOffsetForPosition(
        mapping->EndOfLastNonCollapsedContent(last_position));
    // Align with the legacy behavior that |TextLenght()| is returned if the
    // entire node contains only collapsed whitespaces.
    const bool fully_collapsed = !candidate || *candidate == 0u;
    return fully_collapsed ? TextLength() : *candidate;
  }

  InlineTextBox* box = LastTextBox();
  if (!LastTextBox())
    return TextLength();

  int max_offset = box->Start() + box->Len();
  while ((box = box->PrevForSameLayoutObject()))
    max_offset = std::max<int>(max_offset, box->Start() + box->Len());
  return max_offset;
}

unsigned LayoutText::ResolvedTextLength() const {
  if (auto* mapping = GetNGOffsetMapping()) {
    const Position start_position = PositionForCaretOffset(0);
    const Position end_position = PositionForCaretOffset(TextLength());
    if (start_position.IsNull()) {
      DCHECK(end_position.IsNull()) << end_position;
      return 0;
    }
    DCHECK(end_position.IsNotNull()) << start_position;
    base::Optional<unsigned> start =
        mapping->GetTextContentOffset(start_position);
    base::Optional<unsigned> end = mapping->GetTextContentOffset(end_position);
    if (!start.has_value() || !end.has_value()) {
      DCHECK(!start.has_value()) << this;
      DCHECK(!end.has_value()) << this;
      return 0;
    }
    DCHECK_LE(*start, *end);
    return *end - *start;
  }

  int len = 0;
  for (InlineTextBox* box : TextBoxes())
    len += box->Len();
  return len;
}

bool LayoutText::HasNonCollapsedText() const {
  if (GetNGOffsetMapping())
    return ResolvedTextLength();
  return FirstTextBox();
}

bool LayoutText::ContainsCaretOffset(int text_offset) const {
  DCHECK_GE(text_offset, 0);
  if (auto* mapping = GetNGOffsetMapping()) {
    if (text_offset > static_cast<int>(TextLength()))
      return false;
    const Position position = PositionForCaretOffset(text_offset);
    if (position.IsNull())
      return false;
    if (text_offset < static_cast<int>(TextLength()) &&
        mapping->IsBeforeNonCollapsedContent(position))
      return true;
    if (!text_offset || !mapping->IsAfterNonCollapsedContent(position))
      return false;
    return *mapping->GetCharacterBefore(position) != kNewlineCharacter;
  }

  for (InlineTextBox* box : TextBoxes()) {
    if (text_offset < static_cast<int>(box->Start()) &&
        !ContainsReversedText()) {
      // The offset we're looking for is before this node
      // this means the offset must be in content that is
      // not laid out. Return false.
      return false;
    }
    if (box->ContainsCaretOffset(text_offset))
      return true;
  }
  return false;
}

// Returns true if |box| at |text_offset| can not continue on next line.
static bool CanNotContinueOnNextLine(const LayoutText& text_layout_object,
                                     InlineBox* box,
                                     unsigned text_offset) {
  InlineTextBox* const last_text_box = text_layout_object.LastTextBox();
  if (box == last_text_box)
    return true;
  return LineLayoutAPIShim::LayoutObjectFrom(box->GetLineLayoutItem()) ==
             text_layout_object &&
         ToInlineTextBox(box)->Start() >= text_offset;
}

// The text continues on the next line only if the last text box is not on this
// line and none of the boxes on this line have a larger start offset.
static bool DoesContinueOnNextLine(const LayoutText& text_layout_object,
                                   InlineBox* box,
                                   unsigned text_offset) {
  InlineTextBox* const last_text_box = text_layout_object.LastTextBox();
  DCHECK_NE(box, last_text_box);
  for (InlineBox* runner = box->NextLeafChild(); runner;
       runner = runner->NextLeafChild()) {
    if (CanNotContinueOnNextLine(text_layout_object, runner, text_offset))
      return false;
  }

  for (InlineBox* runner = box->PrevLeafChild(); runner;
       runner = runner->PrevLeafChild()) {
    if (CanNotContinueOnNextLine(text_layout_object, runner, text_offset))
      return false;
  }

  return true;
}

bool LayoutText::IsBeforeNonCollapsedCharacter(unsigned text_offset) const {
  if (auto* mapping = GetNGOffsetMapping()) {
    if (text_offset >= TextLength())
      return false;
    const Position position = PositionForCaretOffset(text_offset);
    if (position.IsNull())
      return false;
    return mapping->IsBeforeNonCollapsedContent(position);
  }

  InlineTextBox* const last_text_box = LastTextBox();
  for (InlineTextBox* box : TextBoxes()) {
    if (text_offset <= box->end()) {
      if (text_offset >= box->Start())
        return true;
      continue;
    }

    if (box == last_text_box || text_offset != box->Start() + box->Len())
      continue;

    // Now that |text_offset == box->Start() + box->Len()|, check if this is the
    // start offset of a whitespace collapsed due to line wrapping, e.g.
    // <div style="width: 100px">foooooooooooooooo baaaaaaaaaaaaaaaaaaaar</div>
    // The whitespace is collapsed away due to line wrapping, while the two
    // positions next to it are still different caret positions. Hence, when the
    // offset is at "...oo| baa...", we should return true.
    if (DoesContinueOnNextLine(*this, box, text_offset))
      return true;
  }
  return false;
}

bool LayoutText::IsAfterNonCollapsedCharacter(unsigned text_offset) const {
  if (auto* mapping = GetNGOffsetMapping()) {
    if (!text_offset)
      return false;
    const Position position = PositionForCaretOffset(text_offset);
    if (position.IsNull())
      return false;
    return mapping->IsAfterNonCollapsedContent(position);
  }

  InlineTextBox* const last_text_box = LastTextBox();
  for (InlineTextBox* box : TextBoxes()) {
    if (text_offset == box->Start())
      continue;
    if (text_offset <= box->Start() + box->Len()) {
      if (text_offset > box->Start())
        return true;
      continue;
    }

    if (box == last_text_box || text_offset != box->Start() + box->Len() + 1)
      continue;

    // Now that |text_offset == box->Start() + box->Len() + 1|, check if this is
    // the end offset of a whitespace collapsed due to line wrapping, e.g.
    // <div style="width: 100px">foooooooooooooooo baaaaaaaaaaaaaaaaaaaar</div>
    // The whitespace is collapsed away due to line wrapping, while the two
    // positions next to it are still different caret positions. Hence, when the
    // offset is at "...oo |baa...", we should return true.
    if (DoesContinueOnNextLine(*this, box, text_offset + 1))
      return true;
  }
  return false;
}

void LayoutText::MomentarilyRevealLastTypedCharacter(
    unsigned last_typed_character_offset) {
  if (!g_secure_text_timers)
    g_secure_text_timers = new SecureTextTimerMap;

  SecureTextTimer* secure_text_timer = g_secure_text_timers->at(this);
  if (!secure_text_timer) {
    secure_text_timer = new SecureTextTimer(this);
    g_secure_text_timers->insert(this, secure_text_timer);
  }
  secure_text_timer->RestartWithNewText(last_typed_character_offset);
}

scoped_refptr<AbstractInlineTextBox> LayoutText::FirstAbstractInlineTextBox() {
  if (IsInLayoutNGInlineFormattingContext()) {
    NGInlineCursor cursor;
    cursor.MoveTo(*this);
    return NGAbstractInlineTextBox::GetOrCreate(cursor);
  }
  return LegacyAbstractInlineTextBox::GetOrCreate(LineLayoutText(this),
                                                  FirstTextBox());
}

void LayoutText::InvalidateDisplayItemClients(
    PaintInvalidationReason invalidation_reason) const {
  ObjectPaintInvalidator paint_invalidator(*this);

  if (RuntimeEnabledFeatures::LayoutNGBlockFragmentationEnabled() &&
      !RuntimeEnabledFeatures::LayoutNGFragmentItemEnabled()) {
    auto fragments = NGPaintFragment::InlineFragmentsFor(this);
    if (fragments.IsInLayoutNGInlineFormattingContext()) {
      for (NGPaintFragment* fragment : fragments) {
        paint_invalidator.InvalidateDisplayItemClient(*fragment,
                                                      invalidation_reason);
      }
      return;
    }
  }

  if (IsInLayoutNGInlineFormattingContext()) {
    NGInlineCursor cursor;
    for (cursor.MoveTo(*this); cursor; cursor.MoveToNextForSameLayoutObject()) {
      paint_invalidator.InvalidateDisplayItemClient(
          *cursor.Current().GetDisplayItemClient(), invalidation_reason);
    }
    return;
  }

  paint_invalidator.InvalidateDisplayItemClient(*this, invalidation_reason);

  for (InlineTextBox* box : TextBoxes()) {
    paint_invalidator.InvalidateDisplayItemClient(*box, invalidation_reason);
    if (EllipsisBox* ellipsis_box = box->Root().GetEllipsisBox()) {
      paint_invalidator.InvalidateDisplayItemClient(*ellipsis_box,
                                                    invalidation_reason);
    }
  }
}

PhysicalRect LayoutText::DebugRect() const {
  return PhysicalRect(EnclosingIntRect(PhysicalLinesBoundingBox()));
}

DOMNodeId LayoutText::EnsureNodeId() {
  if (node_id_ == kInvalidDOMNodeId) {
    auto* content_capture_manager = GetContentCaptureManager();
    if (content_capture_manager)
      content_capture_manager->ScheduleTaskIfNeeded();

    // If either content capture or accessibility are enabled, store a node ID.
    if (content_capture_manager || GetDocument().ExistingAXObjectCache())
      node_id_ = DOMNodeIds::IdForNode(GetNode());
  }
  return node_id_;
}

ContentCaptureManager* LayoutText::GetContentCaptureManager() {
  if (auto* node = GetNode()) {
    if (auto* frame = node->GetDocument().GetFrame()) {
      return frame->LocalFrameRoot().GetContentCaptureManager();
    }
  }
  return nullptr;
}

void LayoutText::SetInlineItems(NGInlineItem* begin, NGInlineItem* end) {
#if DCHECK_IS_ON()
  for (NGInlineItem* item = begin; item != end; ++item) {
    DCHECK_EQ(item->GetLayoutObject(), this);
  }
#endif
  base::span<NGInlineItem>* items = GetNGInlineItems();
  if (!items)
    return;
  valid_ng_items_ = true;
  *items = base::make_span(begin, end);
}

void LayoutText::ClearInlineItems() {
  has_bidi_control_items_ = false;
  valid_ng_items_ = false;
  if (base::span<NGInlineItem>* items = GetNGInlineItems())
    *items = base::span<NGInlineItem>();
}

const base::span<NGInlineItem>& LayoutText::InlineItems() const {
  DCHECK(valid_ng_items_);
  DCHECK(GetNGInlineItems());
  DCHECK(!GetNGInlineItems()->empty());
  return *GetNGInlineItems();
}

}  // namespace blink
