// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/layout/ng/ng_out_of_flow_layout_part.h"

#include "third_party/blink/renderer/core/layout/layout_block.h"
#include "third_party/blink/renderer/core/layout/layout_box.h"
#include "third_party/blink/renderer/core/layout/layout_flexible_box.h"
#include "third_party/blink/renderer/core/layout/layout_inline.h"
#include "third_party/blink/renderer/core/layout/layout_object.h"
#include "third_party/blink/renderer/core/layout/ng/inline/ng_physical_line_box_fragment.h"
#include "third_party/blink/renderer/core/layout/ng/layout_box_utils.h"
#include "third_party/blink/renderer/core/layout/ng/ng_absolute_utils.h"
#include "third_party/blink/renderer/core/layout/ng/ng_block_node.h"
#include "third_party/blink/renderer/core/layout/ng/ng_box_fragment_builder.h"
#include "third_party/blink/renderer/core/layout/ng/ng_constraint_space_builder.h"
#include "third_party/blink/renderer/core/layout/ng/ng_fragment.h"
#include "third_party/blink/renderer/core/layout/ng/ng_layout_result.h"
#include "third_party/blink/renderer/core/layout/ng/ng_length_utils.h"
#include "third_party/blink/renderer/core/layout/ng/ng_out_of_flow_positioned_node.h"
#include "third_party/blink/renderer/core/layout/ng/ng_physical_fragment.h"
#include "third_party/blink/renderer/core/paint/paint_layer.h"
#include "third_party/blink/renderer/core/paint/paint_layer_scrollable_area.h"
#include "third_party/blink/renderer/core/style/computed_style.h"

namespace blink {

namespace {

bool IsAnonymousContainer(const LayoutObject* layout_object) {
  return layout_object->IsAnonymousBlock() &&
         layout_object->CanContainAbsolutePositionObjects();
}

// This saves the static-position for an OOF-positioned object into its
// paint-layer.
void SaveStaticPositionForLegacy(const LayoutBox* layout_box,
                                 const LayoutObject* container,
                                 const LogicalOffset& offset) {
  const LayoutObject* parent = layout_box->Parent();
  if (parent == container ||
      (parent->IsLayoutInline() && parent->ContainingBlock() == container)) {
    DCHECK(layout_box->Layer());
    layout_box->Layer()->SetStaticInlinePosition(offset.inline_offset);
    layout_box->Layer()->SetStaticBlockPosition(offset.block_offset);
  }
}

// When the containing block is a split inline, Legacy and NG use different
// containers to place the OOF-positioned nodes:
//  - Legacy uses the anonymous block generated by inline.
//  - NG uses the anonymous' parent block, that contains all the anonymous
//    continuations.
// This function finds the correct anonymous parent block.
const LayoutInline* GetOOFContainingBlockFromAnonymous(
    const LayoutObject* anonymous_block,
    EPosition child_position) {
  DCHECK(IsAnonymousContainer(anonymous_block));
  DCHECK(anonymous_block->IsBox());

  // Comments and code copied from
  // LayoutBox::ContainingBlockLogicalWidthForPositioned.
  // Ensure we compute our width based on the width of our rel-pos inline
  // container rather than any anonymous block created to manage a block-flow
  // ancestor of ours in the rel-pos inline's inline flow.
  LayoutBoxModelObject* absolute_containing_block =
      ToLayoutBox(anonymous_block)->Continuation();
  // There may be nested parallel inline continuations. We have now found the
  // innermost inline (which may not be relatively positioned). Locate the
  // inline that serves as the containing block of this box.
  while (!absolute_containing_block->CanContainOutOfFlowPositionedElement(
      child_position)) {
    absolute_containing_block =
        ToLayoutBoxModelObject(absolute_containing_block->Container());
  }
  DCHECK(absolute_containing_block->IsLayoutInline());
  // Make absolute_containing_block continuation root.
  return ToLayoutInline(absolute_containing_block->ContinuationRoot());
}

}  // namespace

NGOutOfFlowLayoutPart::NGOutOfFlowLayoutPart(
    const NGBlockNode& container_node,
    const NGConstraintSpace& container_space,
    const NGBoxStrut& border_scrollbar,
    NGBoxFragmentBuilder* container_builder)
    : NGOutOfFlowLayoutPart(container_node.IsAbsoluteContainer(),
                            container_node.IsFixedContainer(),
                            container_node.Style(),
                            container_space,
                            border_scrollbar,
                            container_builder) {}

NGOutOfFlowLayoutPart::NGOutOfFlowLayoutPart(
    bool is_absolute_container,
    bool is_fixed_container,
    const ComputedStyle& container_style,
    const NGConstraintSpace& container_space,
    const NGBoxStrut& border_scrollbar,
    NGBoxFragmentBuilder* container_builder,
    base::Optional<LogicalSize> initial_containing_block_fixed_size)
    : container_space_(container_space),
      container_builder_(container_builder),
      writing_mode_(container_style.GetWritingMode()),
      is_absolute_container_(is_absolute_container),
      is_fixed_container_(is_fixed_container),
      allow_first_tier_oof_cache_(border_scrollbar.IsEmpty()) {
  if (!container_builder->HasOutOfFlowPositionedCandidates() &&
      !To<LayoutBlock>(container_builder_->GetLayoutObject())
           ->HasPositionedObjects())
    return;

  default_containing_block_.direction = container_style.Direction();
  default_containing_block_.content_size_for_absolute =
      ShrinkAvailableSize(container_builder_->Size(), border_scrollbar);
  default_containing_block_.content_size_for_fixed =
      initial_containing_block_fixed_size
          ? *initial_containing_block_fixed_size
          : default_containing_block_.content_size_for_absolute;

  default_containing_block_.container_offset = LogicalOffset(
      border_scrollbar.inline_start, border_scrollbar.block_start);
}

void NGOutOfFlowLayoutPart::Run(const LayoutBox* only_layout) {
  Vector<NGLogicalOutOfFlowPositionedNode> candidates;
  const LayoutObject* current_container = container_builder_->GetLayoutObject();
  // If the container is display-locked, then we skip the layout of descendants,
  // so we can early out immediately.
  if (current_container->LayoutBlockedByDisplayLock(
          DisplayLockLifecycleTarget::kChildren))
    return;

  container_builder_->SwapOutOfFlowPositionedCandidates(&candidates);

  if (candidates.IsEmpty() &&
      !To<LayoutBlock>(current_container)->HasPositionedObjects())
    return;

  // Special case: containing block is a split inline.
  // If current container was generated by a split inline, do not position
  // OOF-positioned nodes inside this container. Let its non-anonymous parent
  // handle it. Only the parent has geometry information needed to compute
  // containing block geometry.
  // See "Special case: oof css container" comment for detailed description.
  if (candidates.size() > 0 && current_container && !only_layout &&
      IsAnonymousContainer(current_container)) {
    const LayoutInline* absolute_containing_block =
        is_absolute_container_ ? GetOOFContainingBlockFromAnonymous(
                                     current_container, EPosition::kAbsolute)
                               : nullptr;
    const LayoutInline* fixed_containing_block =
        is_fixed_container_ ? GetOOFContainingBlockFromAnonymous(
                                  current_container, EPosition::kFixed)
                            : nullptr;
    for (auto& candidate : candidates) {
      if (absolute_containing_block &&
          absolute_containing_block->CanContainOutOfFlowPositionedElement(
              candidate.node.Style().GetPosition())) {
        candidate.inline_container = absolute_containing_block;
      } else if (fixed_containing_block &&
                 fixed_containing_block->CanContainOutOfFlowPositionedElement(
                     candidate.node.Style().GetPosition())) {
        candidate.inline_container = fixed_containing_block;
      }
      container_builder_->AddOutOfFlowDescendant(candidate);
    }
    return;
  }

  HashSet<const LayoutObject*> placed_objects;
  LayoutCandidates(&candidates, only_layout, &placed_objects);

  if (only_layout)
    return;

  // If we're in a block fragmentation context, we've already ruled out the
  // possibility of having legacy objects in here. The code below would pick up
  // every OOF candidate not in placed_objects, and treat them as a legacy
  // object (even if they aren't one), while in fact it could be an NG object
  // that we have finished laying out in an earlier fragmentainer. Just bail.
  if (container_space_.HasBlockFragmentation())
    return;

  wtf_size_t prev_placed_objects_size = placed_objects.size();
  while (SweepLegacyCandidates(&placed_objects)) {
    container_builder_->SwapOutOfFlowPositionedCandidates(&candidates);

    // We must have at least one new candidate, otherwise we shouldn't have
    // entered this branch.
    DCHECK_GT(candidates.size(), 0u);

    LayoutCandidates(&candidates, only_layout, &placed_objects);

    // Legacy currently has a bug where an OOF-positioned node is present
    // within the current node's |LayoutBlock::PositionedObjects|, however it
    // is not the containing-block for this node.
    //
    // This results in |LayoutDescendantCandidates| never performing layout on
    // any additional objects.
    wtf_size_t placed_objects_size = placed_objects.size();
    if (prev_placed_objects_size == placed_objects_size) {
      NOTREACHED();
      break;
    }
    prev_placed_objects_size = placed_objects_size;
  }
}

// Gather candidates that weren't present in the OOF candidates list.
// This occurs when a candidate is separated from container by a legacy node.
// E.g.
// <div style="position: relative;">
//   <div style="display: flex;">
//     <div style="position: absolute;"></div>
//   </div>
// </div>
// Returns false if no new candidates were found.
bool NGOutOfFlowLayoutPart::SweepLegacyCandidates(
    HashSet<const LayoutObject*>* placed_objects) {
  const auto* container_block =
      DynamicTo<LayoutBlock>(container_builder_->GetLayoutObject());
  if (!container_block)
    return false;
  TrackedLayoutBoxListHashSet* legacy_objects =
      container_block->PositionedObjects();
  if (!legacy_objects || legacy_objects->size() == placed_objects->size())
    return false;
  for (LayoutObject* legacy_object : *legacy_objects) {
    if (placed_objects->Contains(legacy_object))
      continue;

    // Flex OOF children may have center alignment or similar, and in order
    // to determine their static position correctly need to have a valid
    // size first.
    // We perform a pre-layout to correctly determine the static position.
    // Copied from LayoutBlock::LayoutPositionedObject
    // TODO(layout-dev): Remove this once LayoutFlexibleBox is removed.
    LayoutBox* layout_box = ToLayoutBox(legacy_object);
    if (layout_box->Parent()->IsFlexibleBox()) {
      LayoutFlexibleBox* parent = ToLayoutFlexibleBox(layout_box->Parent());
      if (parent->SetStaticPositionForPositionedLayout(*layout_box)) {
        NGLogicalOutOfFlowPositionedNode candidate((NGBlockNode(layout_box)),
                                                   NGLogicalStaticPosition());
        LayoutCandidate(candidate, /* only_layout */ nullptr);
        parent->SetStaticPositionForPositionedLayout(*layout_box);
      }
    }

    NGLogicalStaticPosition static_position =
        LayoutBoxUtils::ComputeStaticPositionFromLegacy(
            *layout_box,
            container_builder_->Borders() + container_builder_->Scrollbar(),
            container_builder_);

    const LayoutObject* css_container = layout_box->Container();
    if (IsAnonymousContainer(css_container)) {
      css_container = GetOOFContainingBlockFromAnonymous(
          css_container, layout_box->Style()->GetPosition());
    }

    container_builder_->AddOutOfFlowLegacyCandidate(
        NGBlockNode(layout_box), static_position,
        ToLayoutInlineOrNull(css_container));
  }
  return true;
}

const NGOutOfFlowLayoutPart::ContainingBlockInfo&
NGOutOfFlowLayoutPart::GetContainingBlockInfo(
    const NGLogicalOutOfFlowPositionedNode& candidate) const {
  if (candidate.inline_container) {
    const auto it = containing_blocks_map_.find(candidate.inline_container);
    DCHECK(it != containing_blocks_map_.end());
    return it->value;
  }
  return default_containing_block_;
}

void NGOutOfFlowLayoutPart::ComputeInlineContainingBlocks(
    const Vector<NGLogicalOutOfFlowPositionedNode>& candidates) {
  NGBoxFragmentBuilder::InlineContainingBlockMap inline_container_fragments;

  for (auto& candidate : candidates) {
    if (candidate.inline_container &&
        !inline_container_fragments.Contains(candidate.inline_container)) {
      NGBoxFragmentBuilder::InlineContainingBlockGeometry inline_geometry = {};
      inline_container_fragments.insert(candidate.inline_container,
                                        inline_geometry);
    }
  }
  // Fetch start/end fragment info.
  container_builder_->ComputeInlineContainerFragments(
      &inline_container_fragments);
  LogicalSize container_builder_size = container_builder_->Size();
  PhysicalSize container_builder_physical_size =
      ToPhysicalSize(container_builder_size, writing_mode_);
  // Translate start/end fragments into ContainingBlockInfo.
  for (auto& block_info : inline_container_fragments) {
    // Variables needed to describe ContainingBlockInfo
    const ComputedStyle* inline_cb_style = block_info.key->Style();
    LogicalSize inline_cb_size;
    LogicalOffset container_offset;

    DCHECK(block_info.value.has_value());
    DCHECK(inline_cb_style);
    NGBoxStrut inline_cb_borders = ComputeBordersForInline(*inline_cb_style);

    // The calculation below determines the size of the inline containing block
    // rect.
    //
    // To perform this calculation we:
    // 1. Determine the start_offset "^", this is at the logical-start (wrt.
    //    default containing block), of the start fragment rect.
    // 2. Determine the end_offset "$", this is at the logical-end (wrt.
    //    default containing block), of the end  fragment rect.
    // 3. Determine the logical rectangle defined by these two offsets.
    //
    // Case 1a: Same direction, overlapping fragments.
    //      +---------------
    // ---> |^*****-------->
    //      +*----*---------
    //       *    *
    // ------*----*+
    // ----> *****$| --->
    // ------------+
    //
    // Case 1b: Different direction, overlapping fragments.
    //      +---------------
    // ---> ^******* <-----|
    //      *------*--------
    //      *      *
    // -----*------*
    // |<-- *******$ --->
    // ------------+
    //
    // Case 2a: Same direction, non-overlapping fragments.
    //             +--------
    // --------->  |^ ----->
    //             +*-------
    //              *
    // --------+    *
    // ------->|    $ --->
    // --------+
    //
    // Case 2b: Same direction, non-overlapping fragments.
    //             +--------
    // --------->  ^ <-----|
    //             *--------
    //             *
    // --------+   *
    // | <------   $  --->
    // --------+
    //
    // Note in cases [1a, 2a] we need to account for the inline borders of the
    // rectangles, where-as in [1b, 2b] we do not. This is handled by the
    // is_same_direction check(s).
    //
    // Note in cases [2a, 2b] we don't allow a "negative" containing block size,
    // we clamp negative sizes to zero.
    TextDirection container_direction = default_containing_block_.direction;

    bool is_same_direction =
        container_direction == inline_cb_style->Direction();

    // Step 1 - determine the start_offset.
    const PhysicalRect& start_rect =
        block_info.value->start_fragment_union_rect;
    LogicalOffset start_offset = start_rect.offset.ConvertToLogical(
        writing_mode_, container_direction, container_builder_physical_size,
        start_rect.size);

    // Make sure we add the inline borders, we don't need to do this in the
    // inline direction if the blocks are in opposite directions.
    start_offset.block_offset += inline_cb_borders.block_start;
    if (is_same_direction)
      start_offset.inline_offset += inline_cb_borders.inline_start;

    // Step 2 - determine the end_offset.
    const PhysicalRect& end_rect = block_info.value->end_fragment_union_rect;
    LogicalOffset end_offset = end_rect.offset.ConvertToLogical(
        writing_mode_, container_direction, container_builder_physical_size,
        end_rect.size);

    // Add in the size of the fragment to get the logical end of the fragment.
    end_offset += end_rect.size.ConvertToLogical(writing_mode_);

    // Make sure we subtract the inline borders, we don't need to do this in the
    // inline direction if the blocks are in opposite directions.
    end_offset.block_offset -= inline_cb_borders.block_end;
    if (is_same_direction)
      end_offset.inline_offset -= inline_cb_borders.inline_end;

    // Make sure we don't end up with a rectangle with "negative" size.
    end_offset.inline_offset =
        std::max(end_offset.inline_offset, start_offset.inline_offset);
    end_offset.block_offset =
        std::max(end_offset.block_offset, start_offset.block_offset);
    // Step 3 - determine the logical rectangle.

    // Determine the logical size of the containing block.
    inline_cb_size = {end_offset.inline_offset - start_offset.inline_offset,
                      end_offset.block_offset - start_offset.block_offset};
    DCHECK_GE(inline_cb_size.inline_size, LayoutUnit());
    DCHECK_GE(inline_cb_size.block_size, LayoutUnit());

    // Set the container padding-box offset.
    container_offset = start_offset;

    containing_blocks_map_.insert(
        block_info.key,
        ContainingBlockInfo{inline_cb_style->Direction(), inline_cb_size,
                            inline_cb_size, container_offset});
  }
}

void NGOutOfFlowLayoutPart::LayoutCandidates(
    Vector<NGLogicalOutOfFlowPositionedNode>* candidates,
    const LayoutBox* only_layout,
    HashSet<const LayoutObject*>* placed_objects) {
  while (candidates->size() > 0) {
    ComputeInlineContainingBlocks(*candidates);
    for (auto& candidate : *candidates) {
      const LayoutBox* layout_box = candidate.node.GetLayoutBox();
      if (IsContainingBlockForCandidate(candidate) &&
          (!only_layout || layout_box == only_layout)) {
        scoped_refptr<const NGLayoutResult> result =
            LayoutCandidate(candidate, only_layout);
        container_builder_->AddChild(result->PhysicalFragment(),
                                     result->OutOfFlowPositionedOffset(),
                                     candidate.inline_container);
        placed_objects->insert(candidate.node.GetLayoutBox());
        if (layout_box != only_layout)
          candidate.node.UseLegacyOutOfFlowPositioning();
      } else {
        SaveStaticPositionForLegacy(layout_box,
                                    container_builder_->GetLayoutObject(),
                                    candidate.static_position.offset);
        container_builder_->AddOutOfFlowDescendant(candidate);
      }
    }
    // Sweep any candidates that might have been added.
    // This happens when an absolute container has a fixed child.
    candidates->Shrink(0);
    container_builder_->SwapOutOfFlowPositionedCandidates(candidates);
  }
}

scoped_refptr<const NGLayoutResult> NGOutOfFlowLayoutPart::LayoutCandidate(
    const NGLogicalOutOfFlowPositionedNode& candidate,
    const LayoutBox* only_layout) {
  NGBlockNode node = candidate.node;

  // "NGOutOfFlowLayoutPart container is ContainingBlock" invariant cannot
  // be enforced for tables. Tables are special, in that the ContainingBlock is
  // TABLE, but constraint space is generated by TBODY/TR/. This happens
  // because TBODY/TR are not LayoutBlocks, but LayoutBoxModelObjects.
  DCHECK((container_builder_->GetLayoutObject() ==
          node.GetLayoutBox()->ContainingBlock()) ||
         node.GetLayoutBox()->ContainingBlock()->IsTable());

  const ContainingBlockInfo& container_info = GetContainingBlockInfo(candidate);
  const TextDirection default_direction = default_containing_block_.direction;
  const ComputedStyle& candidate_style = node.Style();
  const WritingMode candidate_writing_mode = candidate_style.GetWritingMode();
  const TextDirection candidate_direction = candidate_style.Direction();

  LogicalSize container_content_size =
      container_info.ContentSize(candidate_style.GetPosition());
  PhysicalSize container_physical_content_size =
      ToPhysicalSize(container_content_size, writing_mode_);

  // Determine if we need to actually run the full OOF-positioned sizing, and
  // positioning algorithm.
  //
  // The first-tier cache compares the given available-size. However we can't
  // reuse the result if the |ContainingBlockInfo::container_offset| may change.
  // This can occur when:
  //  - The default containing-block has borders and/or scrollbars.
  //  - The candidate has an inline container (instead of the default
  //    containing-block).
  if (allow_first_tier_oof_cache_ && !candidate.inline_container) {
    LogicalSize container_content_size_in_candidate_writing_mode =
        container_physical_content_size.ConvertToLogical(
            candidate_writing_mode);
    if (scoped_refptr<const NGLayoutResult> cached_result =
            node.CachedLayoutResultForOutOfFlowPositioned(
                container_content_size_in_candidate_writing_mode))
      return cached_result;
  }

  // Adjust the |static_position| (which is currently relative to the default
  // container's border-box). ng_absolute_utils expects the static position to
  // be relative to the container's padding-box.
  NGLogicalStaticPosition static_position = candidate.static_position;
  static_position.offset -= container_info.container_offset;

  NGLogicalStaticPosition candidate_static_position =
      static_position
          .ConvertToPhysical(writing_mode_, default_direction,
                             container_physical_content_size)
          .ConvertToLogical(candidate_writing_mode, candidate_direction,
                            container_physical_content_size);

  // Need a constraint space to resolve offsets.
  NGConstraintSpaceBuilder builder(writing_mode_, candidate_writing_mode,
                                   /* is_new_fc */ true);
  builder.SetTextDirection(candidate_direction);
  builder.SetAvailableSize(container_content_size);
  builder.SetPercentageResolutionSize(container_content_size);
  NGConstraintSpace candidate_constraint_space = builder.ToConstraintSpace();

  base::Optional<PaintLayerScrollableArea::FreezeScrollbarsScope>
      freeze_scrollbars;
  do {
    scoped_refptr<const NGLayoutResult> layout_result =
        Layout(node, candidate_constraint_space, candidate_static_position,
               container_content_size, container_info, only_layout);

    if (!freeze_scrollbars.has_value()) {
      // Since out-of-flow positioning sets up a constraint space with fixed
      // inline-size, the regular layout code (|NGBlockNode::Layout()|) cannot
      // re-layout if it discovers that a scrollbar was added or removed. Handle
      // that situation here. The assumption is that if preferred logical widths
      // are dirty after layout, AND its inline-size depends on preferred
      // logical widths, it means that scrollbars appeared or disappeared. We
      // have the same logic in legacy layout in
      // |LayoutBlockFlow::UpdateBlockLayout()|.
      if (node.GetLayoutBox()->PreferredLogicalWidthsDirty() &&
          AbsoluteNeedsChildInlineSize(candidate_style)) {
        // Freeze the scrollbars for this layout pass. We don't want them to
        // change *again*.
        freeze_scrollbars.emplace();
        continue;
      }
    }

    return layout_result;
  } while (true);
}

scoped_refptr<const NGLayoutResult> NGOutOfFlowLayoutPart::Layout(
    NGBlockNode node,
    const NGConstraintSpace& candidate_constraint_space,
    const NGLogicalStaticPosition& candidate_static_position,
    LogicalSize container_content_size,
    const ContainingBlockInfo& container_info,
    const LayoutBox* only_layout) {
  const TextDirection default_direction = default_containing_block_.direction;
  const ComputedStyle& candidate_style = node.Style();
  const WritingMode candidate_writing_mode = candidate_style.GetWritingMode();
  const TextDirection candidate_direction = candidate_style.Direction();
  const TextDirection container_direction = container_info.direction;

  PhysicalSize container_physical_content_size =
      ToPhysicalSize(container_content_size, writing_mode_);
  LogicalSize container_content_size_in_candidate_writing_mode =
      container_physical_content_size.ConvertToLogical(candidate_writing_mode);
  NGBoxStrut border_padding =
      ComputeBorders(candidate_constraint_space, node) +
      ComputePadding(candidate_constraint_space, candidate_style);

  // The |block_estimate| is wrt. the candidate's writing mode.
  base::Optional<LayoutUnit> block_estimate;
  base::Optional<MinMaxSize> min_max_size;
  scoped_refptr<const NGLayoutResult> layout_result = nullptr;

  // In order to calculate the offsets, we may need to know the size.

  // In some cases we will need the fragment size in order to calculate the
  // offset. We may have to lay out to get the fragment size. For block
  // fragmentation, we *need* to know the block-offset before layout. In other
  // words, in that case, we may have to lay out, calculate the offset, and
  // then lay out again at the correct block-offset.

  bool is_replaced = node.IsReplaced();
  bool should_be_considered_as_replaced = node.ShouldBeConsideredAsReplaced();

  if (AbsoluteNeedsChildInlineSize(candidate_style) ||
      NeedMinMaxSize(candidate_style) || should_be_considered_as_replaced) {
    // This is a new formatting context, so whatever happened on the outside
    // doesn't concern us.
    MinMaxSizeInput input(container_content_size.block_size);
    min_max_size = ComputeMinAndMaxContentSizeForOutOfFlow(
        candidate_constraint_space, node, border_padding, input);
  }

  base::Optional<LogicalSize> replaced_size;
  base::Optional<LogicalSize> replaced_aspect_ratio;
  bool is_replaced_with_only_aspect_ratio = false;
  if (is_replaced) {
    ComputeReplacedSize(node, candidate_constraint_space, min_max_size,
                        &replaced_size, &replaced_aspect_ratio);
    is_replaced_with_only_aspect_ratio = !replaced_size &&
                                         replaced_aspect_ratio &&
                                         !replaced_aspect_ratio->IsEmpty();
    // If we only have aspect ratio, and no replaced size, intrinsic size
    // defaults to 300x150. min_max_size gets computed from the intrinsic size.
    // We reset the min_max_size because spec says that OOF-positioned size
    // should not be constrained by intrinsic size in this case.
    // https://www.w3.org/TR/CSS22/visudet.html#inline-replaced-width
    if (is_replaced_with_only_aspect_ratio)
      min_max_size = MinMaxSize{LayoutUnit(), LayoutUnit::NearlyMax()};
  } else if (should_be_considered_as_replaced) {
    replaced_size =
        LogicalSize{min_max_size->ShrinkToFit(
                        candidate_constraint_space.AvailableSize().inline_size),
                    kIndefiniteSize};
  }
  NGLogicalOutOfFlowPosition node_position =
      ComputePartialAbsoluteWithChildInlineSize(
          candidate_constraint_space, candidate_style, border_padding,
          candidate_static_position, min_max_size, replaced_size, writing_mode_,
          container_direction);

  // |should_be_considered_as_replaced| sets the inline-size.
  // It does not set the block-size. This is a compatibility quirk.
  if (!is_replaced && should_be_considered_as_replaced)
    replaced_size.reset();

  // Replaced elements with only aspect ratio compute their block size from
  // inline size and aspect ratio.
  // https://www.w3.org/TR/css-sizing-3/#intrinsic-sizes
  if (is_replaced_with_only_aspect_ratio) {
    replaced_size = LogicalSize(
        node_position.size.inline_size,
        (replaced_aspect_ratio->block_size *
         ((node_position.size.inline_size - border_padding.InlineSum()) /
          replaced_aspect_ratio->inline_size)) +
            border_padding.BlockSum());
  }
  if (AbsoluteNeedsChildBlockSize(candidate_style)) {
    layout_result =
        GenerateFragment(node, container_content_size_in_candidate_writing_mode,
                         block_estimate, node_position);

    // TODO(layout-dev): Handle abortions caused by block fragmentation.
    DCHECK(layout_result->Status() != NGLayoutResult::kOutOfFragmentainerSpace);

    NGFragment fragment(candidate_writing_mode,
                        layout_result->PhysicalFragment());

    block_estimate = fragment.BlockSize();
  }

  // Calculate the offsets.

  ComputeFullAbsoluteWithChildBlockSize(
      candidate_constraint_space, candidate_style, border_padding,
      candidate_static_position, block_estimate, replaced_size, writing_mode_,
      container_direction, &node_position);

  NGBoxStrut inset =
      node_position.inset
          .ConvertToPhysical(candidate_writing_mode, candidate_direction)
          .ConvertToLogical(writing_mode_, default_direction);

  // |inset| is relative to the container's padding-box. Convert this to being
  // relative to the default container's border-box.
  LogicalOffset offset = container_info.container_offset;
  offset.inline_offset += inset.inline_start;
  offset.block_offset += inset.block_start;

  if (!only_layout) {
    // Special case: oof css container is a split inline.
    // When css container spans multiple anonymous blocks, its dimensions can
    // only be computed by a block that is an ancestor of all fragments
    // generated by css container. That block is parent of anonymous containing
    // block.
    // That is why instead of OOF being placed by its anonymous container,
    // they get placed by anonymous container's parent.
    // This is different from all other OOF blocks, and requires special
    // handling in several places in the OOF code.
    // There is an exception to special case: if anonymous block is Legacy, we
    // cannot do the fancy multiple anonymous block traversal, and we handle it
    // like regular blocks.
    //
    // Detailed example:
    //
    // If Layout tree looks like this:
    // LayoutNGBlockFlow#container
    //   LayoutNGBlockFlow (anonymous#1)
    //     LayoutInline#1 (relative)
    //   LayoutNGBlockFlow (anonymous#2 relative)
    //     LayoutNGBlockFlow#oof (positioned)
    //   LayoutNGBlockFlow (anonymous#3)
    //     LayoutInline#3 (continuation)
    //
    // The containing block geometry is defined by split inlines,
    // LayoutInline#1, LayoutInline#3.
    // Css container anonymous#2 does not have information needed
    // to compute containing block geometry.
    // Therefore, #oof cannot be placed by anonymous#2. NG handles this case
    // by placing #oof in parent of anonymous (#container).
    //
    // But, PaintPropertyTreeBuilder expects #oof.Location() to be wrt css
    // container, #anonymous2. This is why the code below adjusts the legacy
    // offset from being wrt #container to being wrt #anonymous2.
    const LayoutObject* container = node.GetLayoutBox()->Container();
    if (container->IsAnonymousBlock()) {
      LogicalOffset container_offset =
          container_builder_->GetChildOffset(container);
      offset -= container_offset;
    } else if (container->IsLayoutInline() &&
               container->ContainingBlock()->IsAnonymousBlock()) {
      // Location of OOF with inline container, and anonymous containing block
      // is wrt container.
      LogicalOffset container_offset =
          container_builder_->GetChildOffset(container->ContainingBlock());
      offset -= container_offset;
    }
  }

  // We have calculated the offsets, and if we need to lay out, we can do so at
  // the correct block-start offset now.

  // TODO(mstensho): Actually pass the block-start offset to layout.

  // Skip this step if we produced a fragment when estimating the block-size.
  if (!layout_result) {
    block_estimate = node_position.size.block_size;
    layout_result =
        GenerateFragment(node, container_content_size_in_candidate_writing_mode,
                         block_estimate, node_position);
  }

  // TODO(layout-dev): Handle abortions caused by block fragmentation.
  DCHECK_EQ(layout_result->Status(), NGLayoutResult::kSuccess);

  // TODO(mstensho): Move the rest of this method back into LayoutCandidate().

  if (node.GetLayoutBox()->IsLayoutNGObject()) {
    To<LayoutBlock>(node.GetLayoutBox())
        ->SetIsLegacyInitiatedOutOfFlowLayout(false);
  }
  // Legacy grid and flexbox handle OOF-positioned margins on their own, and
  // break if we set them here.
  if (!container_builder_->GetLayoutObject()
           ->Style()
           ->IsDisplayFlexibleOrGridBox()) {
    node.GetLayoutBox()->SetMargin(node_position.margins.ConvertToPhysical(
        candidate_writing_mode, candidate_direction));
  }

  // Adjusting the offset for a dialog after layout is fine, since we cannot
  // have dialogs needing alignment inside block fragmentation.
  base::Optional<LayoutUnit> y = ComputeAbsoluteDialogYPosition(
      *node.GetLayoutBox(), layout_result->PhysicalFragment().Size().height);
  if (y.has_value()) {
    DCHECK(!container_space_.HasBlockFragmentation());
    if (IsHorizontalWritingMode(writing_mode_))
      offset.block_offset = *y;
    else
      offset.inline_offset = *y;
  }

  layout_result->GetMutableForOutOfFlow().SetOutOfFlowPositionedOffset(
      offset, allow_first_tier_oof_cache_);
  return layout_result;
}

bool NGOutOfFlowLayoutPart::IsContainingBlockForCandidate(
    const NGLogicalOutOfFlowPositionedNode& candidate) {
  EPosition position = candidate.node.Style().GetPosition();

  // Candidates whose containing block is inline are always positioned inside
  // closest parent block flow.
  if (candidate.inline_container) {
    DCHECK(
        candidate.node.Style().GetPosition() == EPosition::kAbsolute &&
            candidate.inline_container->CanContainAbsolutePositionObjects() ||
        (candidate.node.Style().GetPosition() == EPosition::kFixed &&
         candidate.inline_container->CanContainFixedPositionObjects()));
    return container_builder_->GetLayoutObject() ==
           candidate.node.GetLayoutBox()->ContainingBlock();
  }
  return (is_absolute_container_ && position == EPosition::kAbsolute) ||
         (is_fixed_container_ && position == EPosition::kFixed);
}

// The fragment is generated in one of these two scenarios:
// 1. To estimate candidate's block size, in this case block_size is
//    container's available size.
// 2. To compute final fragment, when block size is known from the absolute
//    position calculation.
scoped_refptr<const NGLayoutResult> NGOutOfFlowLayoutPart::GenerateFragment(
    NGBlockNode node,
    const LogicalSize& container_content_size_in_candidate_writing_mode,
    const base::Optional<LayoutUnit>& block_estimate,
    const NGLogicalOutOfFlowPosition& node_position) {
  // As the |block_estimate| is always in the node's writing mode, we build the
  // constraint space in the node's writing mode.
  WritingMode writing_mode = node.Style().GetWritingMode();

  LayoutUnit inline_size = node_position.size.inline_size;
  LayoutUnit block_size = block_estimate.value_or(
      container_content_size_in_candidate_writing_mode.block_size);

  LogicalSize available_size(inline_size, block_size);

  // TODO(atotic) will need to be adjusted for scrollbars.
  NGConstraintSpaceBuilder builder(writing_mode, writing_mode,
                                   /* is_new_fc */ true);
  builder.SetAvailableSize(available_size);
  builder.SetTextDirection(node.Style().Direction());
  builder.SetPercentageResolutionSize(
      container_content_size_in_candidate_writing_mode);
  builder.SetIsFixedInlineSize(true);
  if (block_estimate)
    builder.SetIsFixedBlockSize(true);
  NGConstraintSpace space = builder.ToConstraintSpace();

  return node.Layout(space);
}

}  // namespace blink
