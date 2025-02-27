// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/layout/ng/ng_flex_layout_algorithm.h"

#include <memory>
#include "third_party/blink/renderer/core/layout/flexible_box_algorithm.h"
#include "third_party/blink/renderer/core/layout/layout_box.h"
#include "third_party/blink/renderer/core/layout/layout_flexible_box.h"
#include "third_party/blink/renderer/core/layout/ng/ng_block_break_token.h"
#include "third_party/blink/renderer/core/layout/ng/ng_box_fragment.h"
#include "third_party/blink/renderer/core/layout/ng/ng_constraint_space.h"
#include "third_party/blink/renderer/core/layout/ng/ng_constraint_space_builder.h"
#include "third_party/blink/renderer/core/layout/ng/ng_flex_child_iterator.h"
#include "third_party/blink/renderer/core/layout/ng/ng_fragment.h"
#include "third_party/blink/renderer/core/layout/ng/ng_length_utils.h"
#include "third_party/blink/renderer/core/layout/ng/ng_out_of_flow_layout_part.h"
#include "third_party/blink/renderer/core/layout/ng/ng_physical_box_fragment.h"
#include "third_party/blink/renderer/core/layout/ng/ng_space_utils.h"
#include "third_party/blink/renderer/core/paint/paint_layer_scrollable_area.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"

namespace blink {

NGFlexLayoutAlgorithm::NGFlexLayoutAlgorithm(
    const NGLayoutAlgorithmParams& params)
    : NGLayoutAlgorithm(params),
      border_padding_(params.fragment_geometry.border +
                      params.fragment_geometry.padding),
      border_scrollbar_padding_(border_padding_ +
                                params.fragment_geometry.scrollbar),
      is_column_(Style().ResolvedIsColumnFlexDirection()),
      is_horizontal_flow_(FlexLayoutAlgorithm::IsHorizontalFlow(Style())),
      is_cross_size_definite_(IsContainerCrossSizeDefinite()) {
  container_builder_.SetIsNewFormattingContext(
      params.space.IsNewFormattingContext());
  container_builder_.SetInitialFragmentGeometry(params.fragment_geometry);

  border_box_size_ = container_builder_.InitialBorderBoxSize();
  content_box_size_ =
      ShrinkAvailableSize(border_box_size_, border_scrollbar_padding_);
  child_percentage_size_ = CalculateChildPercentageSize(
      ConstraintSpace(), Node(), content_box_size_);
  algorithm_.emplace(&Style(), MainAxisContentExtent(LayoutUnit::Max()));
}

bool NGFlexLayoutAlgorithm::MainAxisIsInlineAxis(
    const NGBlockNode& child) const {
  return child.Style().IsHorizontalWritingMode() ==
         FlexLayoutAlgorithm::IsHorizontalFlow(Style());
}

LayoutUnit NGFlexLayoutAlgorithm::MainAxisContentExtent(
    LayoutUnit sum_hypothetical_main_size) const {
  if (Style().ResolvedIsColumnFlexDirection()) {
    return ComputeBlockSizeForFragment(
               ConstraintSpace(), Style(), border_padding_,
               sum_hypothetical_main_size +
                   border_scrollbar_padding_.BlockSum()) -
           border_scrollbar_padding_.BlockSum();
  }
  return content_box_size_.inline_size;
}

namespace {

enum AxisEdge { kStart, kCenter, kEnd };

// Maps the resolved justify-content value to a static-position edge.
AxisEdge MainAxisStaticPositionEdge(const ComputedStyle& style,
                                    bool is_column) {
  const StyleContentAlignmentData justify =
      FlexLayoutAlgorithm::ResolvedJustifyContent(style);
  const ContentPosition content_position = justify.GetPosition();
  bool is_reverse_flex = is_column
                             ? style.ResolvedIsColumnReverseFlexDirection()
                             : style.ResolvedIsRowReverseFlexDirection();

  if (content_position == ContentPosition::kFlexEnd)
    return is_reverse_flex ? AxisEdge::kStart : AxisEdge::kEnd;

  if (content_position == ContentPosition::kCenter ||
      justify.Distribution() == ContentDistributionType::kSpaceAround ||
      justify.Distribution() == ContentDistributionType::kSpaceEvenly)
    return AxisEdge::kCenter;

  return is_reverse_flex ? AxisEdge::kEnd : AxisEdge::kStart;
}

// Maps the resolved alignment value to a static-position edge.
AxisEdge CrossAxisStaticPositionEdge(const ComputedStyle& style,
                                     const ComputedStyle& child_style) {
  ItemPosition alignment =
      FlexLayoutAlgorithm::AlignmentForChild(style, child_style);
  bool is_wrap_reverse = style.FlexWrap() == EFlexWrap::kWrapReverse;

  if (alignment == ItemPosition::kFlexEnd)
    return is_wrap_reverse ? AxisEdge::kStart : AxisEdge::kEnd;

  if (alignment == ItemPosition::kCenter)
    return AxisEdge::kCenter;

  return is_wrap_reverse ? AxisEdge::kEnd : AxisEdge::kStart;
}

}  // namespace

void NGFlexLayoutAlgorithm::HandleOutOfFlowPositioned(NGBlockNode child) {
  AxisEdge main_axis_edge = MainAxisStaticPositionEdge(Style(), is_column_);
  AxisEdge cross_axis_edge =
      CrossAxisStaticPositionEdge(Style(), child.Style());

  AxisEdge inline_axis_edge = is_column_ ? cross_axis_edge : main_axis_edge;
  AxisEdge block_axis_edge = is_column_ ? main_axis_edge : cross_axis_edge;

  using InlineEdge = NGLogicalStaticPosition::InlineEdge;
  using BlockEdge = NGLogicalStaticPosition::BlockEdge;

  InlineEdge inline_edge;
  BlockEdge block_edge;
  LogicalOffset offset(border_scrollbar_padding_.inline_start,
                       border_scrollbar_padding_.block_start);

  // Determine the static-position based off the axis-edge.
  if (inline_axis_edge == AxisEdge::kStart) {
    inline_edge = InlineEdge::kInlineStart;
  } else if (inline_axis_edge == AxisEdge::kCenter) {
    inline_edge = InlineEdge::kInlineCenter;
    offset.inline_offset += content_box_size_.inline_size / 2;
  } else {
    inline_edge = InlineEdge::kInlineEnd;
    offset.inline_offset += content_box_size_.inline_size;
  }

  // We may not know the final block-size of the fragment yet. This will be
  // adjusted within the |NGContainerFragmentBuilder| once set.
  if (block_axis_edge == AxisEdge::kStart) {
    block_edge = BlockEdge::kBlockStart;
  } else if (block_axis_edge == AxisEdge::kCenter) {
    block_edge = BlockEdge::kBlockCenter;
    offset.block_offset -= border_scrollbar_padding_.BlockSum() / 2;
  } else {
    block_edge = BlockEdge::kBlockEnd;
    offset.block_offset -= border_scrollbar_padding_.BlockSum();
  }

  container_builder_.AddOutOfFlowChildCandidate(child, offset, inline_edge,
                                                block_edge);
}

bool NGFlexLayoutAlgorithm::IsColumnContainerMainSizeDefinite() const {
  DCHECK(is_column_);
  // If this flex container is also a flex item, it might have a definite size
  // imposed on it by its parent flex container.
  // We can't rely on BlockLengthUnresolvable for this case because that
  // considers Auto as unresolvable even when the block size is fixed and
  // definite.
  if (ConstraintSpace().IsFixedBlockSize() &&
      !ConstraintSpace().IsFixedBlockSizeIndefinite())
    return true;
  Length main_size = Style().LogicalHeight();
  return !BlockLengthUnresolvable(ConstraintSpace(), main_size,
                                  LengthResolvePhase::kLayout);
}

bool NGFlexLayoutAlgorithm::IsContainerCrossSizeDefinite() const {
  // A column flexbox's cross axis is an inline size, so is definite.
  if (is_column_)
    return true;
  // If this flex container is also a flex item, it might have a definite size
  // imposed on it by its parent flex container.
  // TODO(dgrogan): Removing this check doesn't cause any tests to fail. Remove
  // it if unneeded or add a test that needs it.
  if (ConstraintSpace().IsFixedBlockSize() &&
      !ConstraintSpace().IsFixedBlockSizeIndefinite())
    return true;

  return !BlockLengthUnresolvable(ConstraintSpace(), Style().LogicalHeight(),
                                  LengthResolvePhase::kLayout);
}

bool NGFlexLayoutAlgorithm::DoesItemStretch(const NGBlockNode& child) const {
  if (!DoesItemCrossSizeComputeToAuto(child))
    return false;
  const ComputedStyle& child_style = child.Style();
  // https://drafts.csswg.org/css-flexbox/#valdef-align-items-stretch
  // If the cross size property of the flex item computes to auto, and neither
  // of the cross-axis margins are auto, the flex item is stretched.
  if (is_horizontal_flow_ &&
      (child_style.MarginTop().IsAuto() || child_style.MarginBottom().IsAuto()))
    return false;
  if (!is_horizontal_flow_ &&
      (child_style.MarginLeft().IsAuto() || child_style.MarginRight().IsAuto()))
    return false;
  return FlexLayoutAlgorithm::AlignmentForChild(Style(), child_style) ==
         ItemPosition::kStretch;
}

// This behavior is under discussion: the item's pre-flexing main size
// definiteness may no longer imply post-flexing definiteness.
// TODO(dgrogan): Have https://crbug.com/1003506 and
// https://github.com/w3c/csswg-drafts/issues/4305 been resolved yet?
bool NGFlexLayoutAlgorithm::IsItemMainSizeDefinite(
    const NGBlockNode& child) const {
  DCHECK(is_column_);
  // Inline sizes are always definite.
  // TODO(dgrogan): The relevant tests, the last two cases in
  // css/css-flexbox/percentage-heights-003.html passed even without this, so it
  // may be untested or unnecessary.
  if (MainAxisIsInlineAxis(child))
    return true;
  // We need a constraint space for the child to determine resolvability and the
  // space for flex-basis is sufficient, even though it has some unnecessary
  // stuff (ShrinkToFit and fixed cross sizes).
  return !BlockLengthUnresolvable(BuildSpaceForFlexBasis(child),
                                  child.Style().LogicalHeight(),
                                  LengthResolvePhase::kLayout);
}

bool NGFlexLayoutAlgorithm::IsItemCrossAxisLengthDefinite(
    const NGBlockNode& child,
    const Length& length) const {
  // Inline min/max value of 'auto' for the cross-axis isn't definite here.
  // Block value of 'auto' is always indefinite.
  if (length.IsAuto())
    return false;
  // But anything else in the inline direction is definite.
  if (!MainAxisIsInlineAxis(child))
    return true;
  // If we get here, cross axis is block axis.
  return !BlockLengthUnresolvable(BuildSpaceForFlexBasis(child), length,
                                  LengthResolvePhase::kLayout);
}

bool NGFlexLayoutAlgorithm::DoesItemCrossSizeComputeToAuto(
    const NGBlockNode& child) const {
  const ComputedStyle& child_style = child.Style();
  if (is_horizontal_flow_)
    return child_style.Height().IsAuto();
  return child_style.Width().IsAuto();
}

// This function is used to handle two requirements from the spec.
// (1) Calculating flex base size; case 3E at
// https://drafts.csswg.org/css-flexbox/#algo-main-item : If a cross size is
// needed to determine the main size (e.g. when the flex item’s main size is
// in its block axis) and the flex item’s cross size is auto and not
// definite, in this calculation use fit-content as the flex item’s cross size.
// The flex base size is the item’s resulting main size.
// (2) Cross size determination after main size has been calculated.
// https://drafts.csswg.org/css-flexbox/#algo-cross-item : Determine the
// hypothetical cross size of each item by performing layout with the used main
// size and the available space, treating auto as fit-content.
bool NGFlexLayoutAlgorithm::ShouldItemShrinkToFit(
    const NGBlockNode& child) const {
  if (MainAxisIsInlineAxis(child)) {
    // In this case, the cross size is in the item's block axis. The item's
    // block size is never needed to determine its inline size so don't use
    // fit-content.
    return false;
  }
  if (!child.Style().LogicalWidth().IsAuto()) {
    DCHECK(!DoesItemCrossSizeComputeToAuto(child));
    // The cross size (item's inline size) is already specified, so don't use
    // fit-content.
    return false;
  }
  DCHECK(DoesItemCrossSizeComputeToAuto(child));
  // If execution reaches here, the item's inline size is its cross size and
  // computes to auto. In that situation, we only don't use fit-content if the
  // item qualifies for the first case in
  // https://drafts.csswg.org/css-flexbox/#definite-sizes :
  // 1. If a single-line flex container has a definite cross size, the outer
  // cross size of any stretched flex items is the flex container’s inner cross
  // size (clamped to the flex item’s min and max cross size) and is considered
  // definite.
  if (WillChildCrossSizeBeContainerCrossSize(child))
    return false;
  return true;
}

bool NGFlexLayoutAlgorithm::WillChildCrossSizeBeContainerCrossSize(
    const NGBlockNode& child) const {
  return !algorithm_->IsMultiline() && is_cross_size_definite_ &&
         DoesItemStretch(child);
}

NGConstraintSpace NGFlexLayoutAlgorithm::BuildSpaceForIntrinsicBlockSize(
    const NGBlockNode& flex_item) const {
  const ComputedStyle& child_style = flex_item.Style();
  NGConstraintSpaceBuilder space_builder(ConstraintSpace(),
                                         child_style.GetWritingMode(),
                                         /* is_new_fc */ true);
  SetOrthogonalFallbackInlineSizeIfNeeded(Style(), flex_item, &space_builder);
  space_builder.SetCacheSlot(NGCacheSlot::kMeasure);
  space_builder.SetIsPaintedAtomically(true);

  if (ShouldItemShrinkToFit(flex_item))
    space_builder.SetIsShrinkToFit(true);
  if (WillChildCrossSizeBeContainerCrossSize(flex_item)) {
    if (is_column_) {
      space_builder.SetIsFixedInlineSize(true);
    } else {
      space_builder.SetIsFixedBlockSize(true);
      DCHECK_NE(content_box_size_.block_size, kIndefiniteSize);
    }
  }

  space_builder.SetNeedsBaseline(
      ConstraintSpace().NeedsBaseline() ||
      FlexLayoutAlgorithm::AlignmentForChild(Style(), child_style) ==
          ItemPosition::kBaseline);

  // For determining the intrinsic block-size we make %-block-sizes resolve
  // against an indefinite size.
  LogicalSize child_percentage_size = child_percentage_size_;
  if (is_column_)
    child_percentage_size.block_size = kIndefiniteSize;

  // TODO(dgrogan): The SetReplacedPercentageResolutionSize calls in this file
  // may be untested. Write a test or determine why they're unnecessary.
  space_builder.SetAvailableSize(content_box_size_);
  space_builder.SetPercentageResolutionSize(child_percentage_size);
  space_builder.SetReplacedPercentageResolutionSize(child_percentage_size);
  space_builder.SetTextDirection(child_style.Direction());
  return space_builder.ToConstraintSpace();
}

NGConstraintSpace NGFlexLayoutAlgorithm::BuildSpaceForFlexBasis(
    const NGBlockNode& flex_item) const {
  NGConstraintSpaceBuilder space_builder(ConstraintSpace(),
                                         flex_item.Style().GetWritingMode(),
                                         /* is_new_fc */ true);
  SetOrthogonalFallbackInlineSizeIfNeeded(Style(), flex_item, &space_builder);

  // This space is only used for resolving lengths, not for layout. We only
  // need the available and percentage sizes.
  space_builder.SetAvailableSize(content_box_size_);
  space_builder.SetPercentageResolutionSize(child_percentage_size_);
  space_builder.SetReplacedPercentageResolutionSize(child_percentage_size_);
  return space_builder.ToConstraintSpace();
}

void NGFlexLayoutAlgorithm::ConstructAndAppendFlexItems() {
  NGFlexChildIterator iterator(Node());
  for (NGBlockNode child = iterator.NextChild(); child;
       child = iterator.NextChild()) {
    if (child.IsOutOfFlowPositioned()) {
      HandleOutOfFlowPositioned(child);
      continue;
    }

    const ComputedStyle& child_style = child.Style();
    NGConstraintSpace flex_basis_space = BuildSpaceForFlexBasis(child);

    NGBoxStrut border_padding_in_child_writing_mode =
        ComputeBorders(flex_basis_space, child) +
        ComputePadding(flex_basis_space, child_style);

    NGPhysicalBoxStrut physical_border_padding(
        border_padding_in_child_writing_mode.ConvertToPhysical(
            child_style.GetWritingMode(), child_style.Direction()));

    LayoutUnit main_axis_border_padding =
        is_horizontal_flow_ ? physical_border_padding.HorizontalSum()
                            : physical_border_padding.VerticalSum();
    LayoutUnit cross_axis_border_padding =
        is_horizontal_flow_ ? physical_border_padding.VerticalSum()
                            : physical_border_padding.HorizontalSum();

    base::Optional<LayoutUnit> intrinsic_block_size;
    auto IntrinsicBlockSizeFunc = [&]() -> LayoutUnit {
      if (!intrinsic_block_size) {
        NGConstraintSpace child_space = BuildSpaceForIntrinsicBlockSize(child);
        intrinsic_block_size =
            child.Layout(child_space, /* break_token */ nullptr)
                ->IntrinsicBlockSize();
      }
      return *intrinsic_block_size;
    };

    base::Optional<MinMaxSize> min_max_size;
    auto MinMaxSizeFunc = [&]() -> MinMaxSize {
      if (!min_max_size) {
        // We want the child's min/max size in its writing mode, not ours.
        // We'll only ever use it if the child's inline axis is our main axis.
        NGConstraintSpace child_space = BuildSpaceForIntrinsicBlockSize(child);
        min_max_size = child.ComputeMinMaxSize(
            child_style.GetWritingMode(),
            MinMaxSizeInput(content_box_size_.block_size), &child_space);
      }
      return *min_max_size;
    };

    LayoutUnit flex_base_border_box;
    const Length& specified_length_in_main_axis =
        is_horizontal_flow_ ? child_style.Width() : child_style.Height();
    const Length& flex_basis = child_style.FlexBasis();
    // TODO(dgrogan): Generalize IsAuto: See the <'width'> section of
    // https://drafts.csswg.org/css-flexbox/#valdef-flex-flex-basis
    // and https://drafts.csswg.org/css-flexbox/#flex-basis-property, which says
    // that if a flex-basis value would resolve to auto (but not literally auto)
    // we should interpret it as flex-basis:content.
    if (flex_basis.IsAuto() && specified_length_in_main_axis.IsAuto()) {
      if (MainAxisIsInlineAxis(child))
        flex_base_border_box = MinMaxSizeFunc().max_size;
      else
        flex_base_border_box = IntrinsicBlockSizeFunc();
    } else {
      // TODO(dgrogan): Check for definiteness.
      // This block covers case A in
      // https://drafts.csswg.org/css-flexbox/#algo-main-item.
      const Length& length_to_resolve =
          flex_basis.IsAuto() ? specified_length_in_main_axis : flex_basis;
      DCHECK(!length_to_resolve.IsAuto());

      if (MainAxisIsInlineAxis(child)) {
        flex_base_border_box = ResolveMainInlineLength(
            flex_basis_space, child_style, border_padding_in_child_writing_mode,
            MinMaxSizeFunc, length_to_resolve);
      } else {
        // Flex container's main axis is in child's block direction. Child's
        // flex basis is in child's block direction.
        flex_base_border_box = ResolveMainBlockLength(
            flex_basis_space, child_style, border_padding_in_child_writing_mode,
            length_to_resolve, IntrinsicBlockSizeFunc,
            LengthResolvePhase::kLayout);
      }
    }

    // Spec calls this "flex base size"
    // https://www.w3.org/TR/css-flexbox-1/#algo-main-item
    // Blink's FlexibleBoxAlgorithm expects it to be content + scrollbar widths,
    // but no padding or border.
    LayoutUnit flex_base_content_size =
        flex_base_border_box - main_axis_border_padding;

    MinMaxSize min_max_sizes_in_main_axis_direction{LayoutUnit(),
                                                    LayoutUnit::Max()};
    MinMaxSize min_max_sizes_in_cross_axis_direction{LayoutUnit(),
                                                     LayoutUnit::Max()};
    const Length& max_property_in_main_axis = is_horizontal_flow_
                                                  ? child.Style().MaxWidth()
                                                  : child.Style().MaxHeight();
    const Length& max_property_in_cross_axis = is_horizontal_flow_
                                                   ? child.Style().MaxHeight()
                                                   : child.Style().MaxWidth();
    const Length& min_property_in_cross_axis = is_horizontal_flow_
                                                   ? child.Style().MinHeight()
                                                   : child.Style().MinWidth();
    if (MainAxisIsInlineAxis(child)) {
      min_max_sizes_in_main_axis_direction.max_size = ResolveMaxInlineLength(
          flex_basis_space, child_style, border_padding_in_child_writing_mode,
          MinMaxSizeFunc, max_property_in_main_axis,
          LengthResolvePhase::kLayout);
      min_max_sizes_in_cross_axis_direction.max_size = ResolveMaxBlockLength(
          flex_basis_space, child_style, border_padding_in_child_writing_mode,
          max_property_in_cross_axis, IntrinsicBlockSizeFunc,
          LengthResolvePhase::kLayout);
      min_max_sizes_in_cross_axis_direction.min_size = ResolveMinBlockLength(
          flex_basis_space, child_style, border_padding_in_child_writing_mode,
          min_property_in_cross_axis, IntrinsicBlockSizeFunc,
          LengthResolvePhase::kLayout);
    } else {
      min_max_sizes_in_main_axis_direction.max_size = ResolveMaxBlockLength(
          flex_basis_space, child_style, border_padding_in_child_writing_mode,
          max_property_in_main_axis, IntrinsicBlockSizeFunc,
          LengthResolvePhase::kLayout);
      min_max_sizes_in_cross_axis_direction.max_size = ResolveMaxInlineLength(
          flex_basis_space, child_style, border_padding_in_child_writing_mode,
          MinMaxSizeFunc, max_property_in_cross_axis,
          LengthResolvePhase::kLayout);
      min_max_sizes_in_cross_axis_direction.min_size = ResolveMinInlineLength(
          flex_basis_space, child_style, border_padding_in_child_writing_mode,
          MinMaxSizeFunc, min_property_in_cross_axis,
          LengthResolvePhase::kLayout);
    }

    const Length& min = is_horizontal_flow_ ? child.Style().MinWidth()
                                            : child.Style().MinHeight();
    if (min.IsAuto()) {
      if (algorithm_->ShouldApplyMinSizeAutoForChild(*child.GetLayoutBox())) {
        // TODO(dgrogan): This should probably apply to column flexboxes also,
        // but that's not what legacy does.
        if (child.IsTable() && !is_column_) {
          MinMaxSize table_preferred_widths =
              ComputeMinAndMaxContentContribution(
                  Style(), child,
                  MinMaxSizeInput(child_percentage_size_.block_size));
          min_max_sizes_in_main_axis_direction.min_size =
              table_preferred_widths.min_size;
        } else {
          // TODO(dgrogan): Do the aspect ratio parts of
          // https://www.w3.org/TR/css-flexbox-1/#min-size-auto

          LayoutUnit content_size_suggestion = MainAxisIsInlineAxis(child)
                                                   ? MinMaxSizeFunc().min_size
                                                   : IntrinsicBlockSizeFunc();
          content_size_suggestion =
              std::min(content_size_suggestion,
                       min_max_sizes_in_main_axis_direction.max_size);

          if (child.MayHaveAspectRatio()) {
            // TODO(dgrogan): We're including borders/padding in both
            // content_size_suggestion and
            // min_max_sizes_in_cross_axis_direction. Maybe we need to multiply
            // the content size by the aspect ratio and then apply the
            // border/padding from the other axis inside the Adjust* function.
            // Test legacy/firefox. Start with
            // https://jsfiddle.net/dgrogan/9uyg3aro/
            content_size_suggestion =
                AdjustChildSizeForAspectRatioCrossAxisMinAndMax(
                    child, content_size_suggestion,
                    min_max_sizes_in_cross_axis_direction.min_size,
                    min_max_sizes_in_cross_axis_direction.max_size);
          }

          LayoutUnit specified_size_suggestion(LayoutUnit::Max());
          // If the item’s computed main size property is definite, then the
          // specified size suggestion is that size.
          if (MainAxisIsInlineAxis(child)) {
            if (!specified_length_in_main_axis.IsAuto()) {
              // TODO(dgrogan): Optimization opportunity: we may have already
              // resolved specified_length_in_main_axis in the flex basis
              // calculation. Reuse that if possible.
              specified_size_suggestion = ResolveMainInlineLength(
                  flex_basis_space, child_style,
                  border_padding_in_child_writing_mode, MinMaxSizeFunc,
                  specified_length_in_main_axis);
            }
          } else if (!BlockLengthUnresolvable(flex_basis_space,
                                              specified_length_in_main_axis,
                                              LengthResolvePhase::kLayout)) {
            specified_size_suggestion = ResolveMainBlockLength(
                flex_basis_space, child_style,
                border_padding_in_child_writing_mode,
                specified_length_in_main_axis, IntrinsicBlockSizeFunc,
                LengthResolvePhase::kLayout);
            DCHECK_NE(specified_size_suggestion, kIndefiniteSize);
          }
          // Spec says to clamp specified_size_suggestion by max size but
          // because content_size_suggestion already is, and we take the min of
          // those two, we don't need to clamp specified_size_suggestion.
          // https://github.com/w3c/csswg-drafts/issues/3669

          min_max_sizes_in_main_axis_direction.min_size =
              std::min(specified_size_suggestion, content_size_suggestion);
        }
      }
    } else if (MainAxisIsInlineAxis(child)) {
      min_max_sizes_in_main_axis_direction.min_size = ResolveMinInlineLength(
          flex_basis_space, child_style, border_padding_in_child_writing_mode,
          MinMaxSizeFunc, min, LengthResolvePhase::kLayout);
    } else {
      min_max_sizes_in_main_axis_direction.min_size = ResolveMinBlockLength(
          flex_basis_space, child_style, border_padding_in_child_writing_mode,
          min, IntrinsicBlockSizeFunc, LengthResolvePhase::kLayout);
    }
    min_max_sizes_in_main_axis_direction -= main_axis_border_padding;

    // TODO(dgrogan): Should min_max_sizes_in_cross_axis_direction include
    // cross_axis_border_padding?
    NGPhysicalBoxStrut physical_child_margins =
        ComputePhysicalMargins(flex_basis_space, child_style);
    algorithm_
        ->emplace_back(nullptr, child.Style(), flex_base_content_size,
                       min_max_sizes_in_main_axis_direction,
                       min_max_sizes_in_cross_axis_direction,
                       main_axis_border_padding, cross_axis_border_padding,
                       physical_child_margins)
        .ng_input_node = child;
  }
}

LayoutUnit
NGFlexLayoutAlgorithm::AdjustChildSizeForAspectRatioCrossAxisMinAndMax(
    const NGBlockNode& child,
    LayoutUnit content_suggestion,
    LayoutUnit cross_min,
    LayoutUnit cross_max) {
  DCHECK(child.MayHaveAspectRatio());
  // Clamp content_suggestion by any definite min and max cross size properties
  // converted through the aspect ratio.

  base::Optional<LayoutUnit> computed_inline_size;
  base::Optional<LayoutUnit> computed_block_size;
  LogicalSize aspect_ratio;

  child.IntrinsicSize(&computed_inline_size, &computed_block_size,
                      &aspect_ratio);

  // TODO(dgrogan): Should we quit here if only the denominator is 0?
  if (aspect_ratio.inline_size == 0 || aspect_ratio.block_size == 0)
    return content_suggestion;

  double ratio = aspect_ratio.inline_size / aspect_ratio.block_size;

  // Multiplying by ratio will take something in the item's block axis and
  // convert it to the inline axis. We want to convert from cross size to main
  // size. If block axis and cross axis are the same, then we already have what
  // we need. Otherwise we need to use the reciprocal.
  if (!MainAxisIsInlineAxis(child))
    ratio = 1 / ratio;

  const Length& cross_max_length = is_horizontal_flow_
                                       ? child.Style().MaxHeight()
                                       : child.Style().MaxWidth();
  if (IsItemCrossAxisLengthDefinite(child, cross_max_length)) {
    LayoutUnit max_main_length = LayoutUnit(cross_max * ratio);
    content_suggestion = std::min(max_main_length, content_suggestion);
  }

  const Length& cross_min_length = is_horizontal_flow_
                                       ? child.Style().MinHeight()
                                       : child.Style().MinWidth();
  if (IsItemCrossAxisLengthDefinite(child, cross_min_length)) {
    LayoutUnit min_main_length = LayoutUnit(cross_min * ratio);
    content_suggestion = std::max(min_main_length, content_suggestion);
  }
  return content_suggestion;
}

scoped_refptr<const NGLayoutResult> NGFlexLayoutAlgorithm::Layout() {
  PaintLayerScrollableArea::DelayScrollOffsetClampScope delay_clamp_scope;
  ConstructAndAppendFlexItems();

  LayoutUnit main_axis_start_offset;
  LayoutUnit main_axis_end_offset;
  LayoutUnit cross_axis_offset = border_scrollbar_padding_.block_start;
  if (is_column_) {
    const bool is_column_reverse =
        Style().ResolvedIsColumnReverseFlexDirection();
    main_axis_start_offset = is_column_reverse
                                 ? LayoutUnit()
                                 : border_scrollbar_padding_.block_start;
    main_axis_end_offset =
        is_column_reverse ? LayoutUnit() : border_scrollbar_padding_.block_end;
    cross_axis_offset = border_scrollbar_padding_.inline_start;
  } else if (Style().ResolvedIsRowReverseFlexDirection()) {
    main_axis_start_offset = border_scrollbar_padding_.inline_end;
    main_axis_end_offset = border_scrollbar_padding_.inline_start;
  } else {
    main_axis_start_offset = border_scrollbar_padding_.inline_start;
    main_axis_end_offset = border_scrollbar_padding_.inline_end;
  }
  FlexLine* line;
  while (
      (line = algorithm_->ComputeNextFlexLine(border_box_size_.inline_size))) {
    line->SetContainerMainInnerSize(
        MainAxisContentExtent(line->sum_hypothetical_main_size));
    line->FreezeInflexibleItems();
    while (!line->ResolveFlexibleLengths()) {
      continue;
    }
    for (wtf_size_t i = 0; i < line->line_items.size(); ++i) {
      FlexItem& flex_item = line->line_items[i];

      const ComputedStyle& child_style = flex_item.ng_input_node.Style();
      NGConstraintSpaceBuilder space_builder(ConstraintSpace(),
                                             child_style.GetWritingMode(),
                                             /* is_new_fc */ true);
      SetOrthogonalFallbackInlineSizeIfNeeded(Style(), flex_item.ng_input_node,
                                              &space_builder);
      space_builder.SetTextDirection(child_style.Direction());
      space_builder.SetIsPaintedAtomically(true);

      LogicalSize available_size;
      if (is_column_) {
        available_size.inline_size = content_box_size_.inline_size;
        available_size.block_size =
            flex_item.flexed_content_size + flex_item.main_axis_border_padding;
        space_builder.SetIsFixedBlockSize(true);
        // https://drafts.csswg.org/css-flexbox/#definite-sizes
        // If the flex container has a definite main size, a flex item's
        // post-flexing main size is treated as definite, even though it can
        // rely on the indefinite sizes of any flex items in the same line.
        if (!IsColumnContainerMainSizeDefinite() &&
            !IsItemMainSizeDefinite(flex_item.ng_input_node)) {
          space_builder.SetIsFixedBlockSizeIndefinite(true);
        }
      } else {
        available_size.inline_size =
            flex_item.flexed_content_size + flex_item.main_axis_border_padding;
        available_size.block_size = content_box_size_.block_size;
        space_builder.SetIsFixedInlineSize(true);
      }
      if (WillChildCrossSizeBeContainerCrossSize(flex_item.ng_input_node)) {
        if (is_column_)
          space_builder.SetIsFixedInlineSize(true);
        else
          space_builder.SetIsFixedBlockSize(true);
      }

      space_builder.SetNeedsBaseline(
          ConstraintSpace().NeedsBaseline() ||
          FlexLayoutAlgorithm::AlignmentForChild(Style(), child_style) ==
              ItemPosition::kBaseline);

      space_builder.SetAvailableSize(available_size);
      space_builder.SetPercentageResolutionSize(child_percentage_size_);
      space_builder.SetReplacedPercentageResolutionSize(child_percentage_size_);

      // https://drafts.csswg.org/css-flexbox/#algo-cross-item
      // Determine the hypothetical cross size of each item by performing layout
      // with the used main size and the available space, treating auto as
      // fit-content.
      if (ShouldItemShrinkToFit(flex_item.ng_input_node))
        space_builder.SetIsShrinkToFit(true);

      NGConstraintSpace child_space = space_builder.ToConstraintSpace();
      flex_item.layout_result =
          flex_item.ng_input_node.Layout(child_space, nullptr /*break token*/);

      // TODO(layout-dev): Handle abortions caused by block fragmentation.
      DCHECK_EQ(flex_item.layout_result->Status(), NGLayoutResult::kSuccess);

      flex_item.cross_axis_size =
          is_horizontal_flow_
              ? flex_item.layout_result->PhysicalFragment().Size().height
              : flex_item.layout_result->PhysicalFragment().Size().width;
    }
    // cross_axis_offset is updated in each iteration of the loop, for passing
    // in to the next iteration.
    line->ComputeLineItemsPosition(main_axis_start_offset, main_axis_end_offset,
                                   cross_axis_offset);
  }

  LayoutUnit intrinsic_block_size = algorithm_->IntrinsicContentBlockSize() +
                                    border_scrollbar_padding_.BlockSum();

  intrinsic_block_size =
      ClampIntrinsicBlockSize(ConstraintSpace(), Node(),
                              border_scrollbar_padding_, intrinsic_block_size);
  LayoutUnit block_size = ComputeBlockSizeForFragment(
      ConstraintSpace(), Style(), border_padding_, intrinsic_block_size);

  container_builder_.SetIntrinsicBlockSize(intrinsic_block_size);
  container_builder_.SetBlockSize(block_size);

  GiveLinesAndItemsFinalPositionAndSize();

  NGOutOfFlowLayoutPart(
      Node(), ConstraintSpace(),
      container_builder_.Borders() + container_builder_.Scrollbar(),
      &container_builder_)
      .Run();

  return container_builder_.ToBoxFragment();
}

void NGFlexLayoutAlgorithm::ApplyStretchAlignmentToChild(FlexItem& flex_item) {
  const ComputedStyle& child_style = flex_item.ng_input_node.Style();
  NGConstraintSpaceBuilder space_builder(ConstraintSpace(),
                                         child_style.GetWritingMode(),
                                         /* is_new_fc */ true);
  SetOrthogonalFallbackInlineSizeIfNeeded(Style(), flex_item.ng_input_node,
                                          &space_builder);
  space_builder.SetIsPaintedAtomically(true);

  LogicalSize available_size(
      flex_item.flexed_content_size + flex_item.main_axis_border_padding,
      flex_item.cross_axis_size);
  if (is_column_) {
    available_size.Transpose();
    if (!IsColumnContainerMainSizeDefinite() &&
        !IsItemMainSizeDefinite(flex_item.ng_input_node)) {
      space_builder.SetIsFixedBlockSizeIndefinite(true);
    }
  }

  space_builder.SetNeedsBaseline(
      ConstraintSpace().NeedsBaseline() ||
      FlexLayoutAlgorithm::AlignmentForChild(Style(), child_style) ==
          ItemPosition::kBaseline);

  space_builder.SetTextDirection(child_style.Direction());
  space_builder.SetAvailableSize(available_size);
  space_builder.SetPercentageResolutionSize(child_percentage_size_);
  space_builder.SetReplacedPercentageResolutionSize(child_percentage_size_);
  space_builder.SetIsFixedInlineSize(true);
  space_builder.SetIsFixedBlockSize(true);
  NGConstraintSpace child_space = space_builder.ToConstraintSpace();
  flex_item.layout_result =
      flex_item.ng_input_node.Layout(child_space, /* break_token */ nullptr);
}

void NGFlexLayoutAlgorithm::GiveLinesAndItemsFinalPositionAndSize() {
  Vector<FlexLine>& line_contexts = algorithm_->FlexLines();
  const LayoutUnit cross_axis_start_edge =
      line_contexts.IsEmpty() ? LayoutUnit()
                              : line_contexts[0].cross_axis_offset;

  LayoutUnit final_content_main_size =
      container_builder_.InlineSize() - border_scrollbar_padding_.InlineSum();
  LayoutUnit final_content_cross_size =
      container_builder_.BlockSize() - border_scrollbar_padding_.BlockSum();
  if (is_column_)
    std::swap(final_content_main_size, final_content_cross_size);

  if (!algorithm_->IsMultiline() && !line_contexts.IsEmpty())
    line_contexts[0].cross_axis_extent = final_content_cross_size;

  algorithm_->AlignFlexLines(final_content_cross_size);

  algorithm_->AlignChildren();

  if (Style().FlexWrap() == EFlexWrap::kWrapReverse) {
    // flex-wrap: wrap-reverse reverses the order of the lines in the container;
    // FlipForWrapReverse recalculates each item's cross axis position. We have
    // to do that after AlignChildren sets an initial cross axis position.
    algorithm_->FlipForWrapReverse(cross_axis_start_edge,
                                   final_content_cross_size);
  }

  if (Style().ResolvedIsColumnReverseFlexDirection()) {
    algorithm_->LayoutColumnReverse(final_content_main_size,
                                    border_scrollbar_padding_.block_start);
  }

  base::Optional<LayoutUnit> fallback_baseline;

  LayoutUnit overflow_block_size;
  for (FlexLine& line_context : line_contexts) {
    for (wtf_size_t child_number = 0;
         child_number < line_context.line_items.size(); ++child_number) {
      FlexItem& flex_item = line_context.line_items[child_number];

      if (DoesItemStretch(flex_item.ng_input_node))
        ApplyStretchAlignmentToChild(flex_item);

      const auto& physical_fragment = To<NGPhysicalBoxFragment>(
          flex_item.layout_result->PhysicalFragment());

      // flex_item.desired_location stores the main axis offset in X and the
      // cross axis offset in Y. But AddChild wants offset from parent
      // rectangle, so we have to transpose for columns. AddChild takes care of
      // any writing mode differences though.
      LayoutPoint location = is_column_
                                 ? flex_item.desired_location.TransposedPoint()
                                 : flex_item.desired_location;

      NGBoxFragment fragment(ConstraintSpace().GetWritingMode(),
                             ConstraintSpace().Direction(), physical_fragment);
      // Only propagate baselines from children on the first flex-line.
      if (&line_context == line_contexts.begin()) {
        PropagateBaselineFromChild(flex_item, fragment, location.Y(),
                                   &fallback_baseline);
      }

      container_builder_.AddChild(physical_fragment,
                                  {location.X(), location.Y()});

      flex_item.ng_input_node.StoreMargins(flex_item.physical_margins);

      LayoutUnit margin_block_end =
          flex_item.physical_margins
              .ConvertToLogical(ConstraintSpace().GetWritingMode(),
                                ConstraintSpace().Direction())
              .block_end;
      overflow_block_size =
          std::max(overflow_block_size,
                   location.Y() + fragment.BlockSize() + margin_block_end);
    }
  }

  container_builder_.SetOverflowBlockSize(overflow_block_size +
                                          border_scrollbar_padding_.block_end);

  // Set the baseline to the fallback, if we didn't find any children with
  // baseline alignment.
  if (!container_builder_.Baseline() && fallback_baseline)
    container_builder_.SetBaseline(*fallback_baseline);
}

void NGFlexLayoutAlgorithm::PropagateBaselineFromChild(
    const FlexItem& flex_item,
    const NGBoxFragment& fragment,
    LayoutUnit block_offset,
    base::Optional<LayoutUnit>* fallback_baseline) {
  // Check if we've already found an appropriate baseline.
  if (container_builder_.Baseline())
    return;

  LayoutUnit baseline_offset =
      block_offset + fragment.Baseline().value_or(fragment.BlockSize());

  // We prefer a baseline from a child with baseline alignment, and no
  // auto-margins in the cross axis (even if we have to synthesize the
  // baseline).
  if (FlexLayoutAlgorithm::AlignmentForChild(Style(), flex_item.style) ==
          ItemPosition::kBaseline &&
      !flex_item.HasAutoMarginsInCrossAxis()) {
    container_builder_.SetBaseline(baseline_offset);
    return;
  }

  // Set the fallback baseline if it doesn't have a value yet.
  *fallback_baseline = fallback_baseline->value_or(baseline_offset);
}

base::Optional<MinMaxSize> NGFlexLayoutAlgorithm::ComputeMinMaxSize(
    const MinMaxSizeInput& input) const {
  base::Optional<MinMaxSize> sizes = CalculateMinMaxSizesIgnoringChildren(
      Node(), border_scrollbar_padding_, input.size_type);
  if (sizes)
    return sizes;

  sizes.emplace();
  LayoutUnit child_percentage_resolution_block_size =
      CalculateChildPercentageBlockSizeForMinMax(
          ConstraintSpace(), Node(), border_padding_,
          input.percentage_resolution_block_size);

  // Use default MinMaxSizeInput:
  //   - Children of flexbox ignore any specified float properties, so children
  //     never have to take floated siblings into account, and external floats
  //     don't make it through the new formatting context that flexbox
  //     establishes.
  //   - We want the child's border box MinMaxSize, which is the default.
  MinMaxSizeInput child_input(child_percentage_resolution_block_size);

  NGFlexChildIterator iterator(Node());
  for (NGBlockNode child = iterator.NextChild(); child;
       child = iterator.NextChild()) {
    if (child.IsOutOfFlowPositioned())
      continue;

    MinMaxSize child_min_max_sizes =
        ComputeMinAndMaxContentContribution(Style(), child, child_input);
    NGBoxStrut child_margins = ComputeMinMaxMargins(Style(), child);
    child_min_max_sizes += child_margins.InlineSum();
    if (is_column_) {
      sizes->min_size = std::max(sizes->min_size, child_min_max_sizes.min_size);
      sizes->max_size = std::max(sizes->max_size, child_min_max_sizes.max_size);
    } else {
      sizes->max_size += child_min_max_sizes.max_size;
      if (IsMultiline()) {
        sizes->min_size =
            std::max(sizes->min_size, child_min_max_sizes.min_size);
      } else {
        sizes->min_size += child_min_max_sizes.min_size;
      }
    }
  }
  sizes->max_size = std::max(sizes->max_size, sizes->min_size);

  // Due to negative margins, it is possible that we calculated a negative
  // intrinsic width. Make sure that we never return a negative width.
  sizes->Encompass(LayoutUnit());

  if (input.size_type == NGMinMaxSizeType::kBorderBoxSize)
    *sizes += border_scrollbar_padding_.InlineSum();

  return sizes;
}

bool NGFlexLayoutAlgorithm::IsMultiline() const {
  return Style().FlexWrap() != EFlexWrap::kNowrap;
}

}  // namespace blink
