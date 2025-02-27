// Copyright 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/layers/render_surface_impl.h"

#include <stddef.h>

#include <algorithm>

#include "base/logging.h"
#include "base/strings/stringprintf.h"
#include "cc/base/math_util.h"
#include "cc/debug/debug_colors.h"
#include "cc/layers/append_quads_data.h"
#include "cc/paint/element_id.h"
#include "cc/paint/filter_operations.h"
#include "cc/trees/damage_tracker.h"
#include "cc/trees/effect_node.h"
#include "cc/trees/layer_tree_impl.h"
#include "cc/trees/occlusion.h"
#include "cc/trees/transform_node.h"
#include "components/viz/common/display/de_jelly.h"
#include "components/viz/common/quads/content_draw_quad_base.h"
#include "components/viz/common/quads/debug_border_draw_quad.h"
#include "components/viz/common/quads/render_pass.h"
#include "components/viz/common/quads/render_pass_draw_quad.h"
#include "components/viz/common/quads/shared_quad_state.h"
#include "components/viz/common/quads/solid_color_draw_quad.h"
#include "components/viz/common/quads/tile_draw_quad.h"
#include "third_party/skia/include/core/SkImageFilter.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/transform.h"

namespace cc {

RenderSurfaceImpl::RenderSurfaceImpl(LayerTreeImpl* layer_tree_impl,
                                     uint64_t stable_id)
    : layer_tree_impl_(layer_tree_impl),
      stable_id_(stable_id),
      effect_tree_index_(EffectTree::kInvalidNodeId),
      num_contributors_(0),
      has_contributing_layer_that_escapes_clip_(false),
      surface_property_changed_(false),
      ancestor_property_changed_(false),
      contributes_to_drawn_surface_(false),
      is_render_surface_list_member_(false),
      can_use_cached_backdrop_filtered_result_(false),
      nearest_occlusion_immune_ancestor_(nullptr) {
  damage_tracker_ = DamageTracker::Create();
}

RenderSurfaceImpl::~RenderSurfaceImpl() = default;

RenderSurfaceImpl* RenderSurfaceImpl::render_target() {
  EffectTree& effect_tree = layer_tree_impl_->property_trees()->effect_tree;
  EffectNode* node = effect_tree.Node(EffectTreeIndex());
  if (node->target_id != EffectTree::kRootNodeId)
    return effect_tree.GetRenderSurface(node->target_id);
  else
    return this;
}

const RenderSurfaceImpl* RenderSurfaceImpl::render_target() const {
  const EffectTree& effect_tree =
      layer_tree_impl_->property_trees()->effect_tree;
  const EffectNode* node = effect_tree.Node(EffectTreeIndex());
  if (node->target_id != EffectTree::kRootNodeId)
    return effect_tree.GetRenderSurface(node->target_id);
  else
    return this;
}

RenderSurfaceImpl::DrawProperties::DrawProperties() {
  draw_opacity = 1.f;
  is_clipped = false;
}

RenderSurfaceImpl::DrawProperties::~DrawProperties() = default;

gfx::RectF RenderSurfaceImpl::DrawableContentRect() const {
  if (content_rect().IsEmpty())
    return gfx::RectF();

  gfx::Rect surface_content_rect = content_rect();
  const FilterOperations& filters = Filters();
  if (!filters.IsEmpty()) {
    surface_content_rect = filters.MapRect(surface_content_rect,
                                           SkMatrix(SurfaceScale().matrix()));
  }
  gfx::RectF drawable_content_rect = MathUtil::MapClippedRect(
      draw_transform(), gfx::RectF(surface_content_rect));
  if (!filters.IsEmpty() && is_clipped()) {
    // Filter could move pixels around, but still need to be clipped.
    drawable_content_rect.Intersect(gfx::RectF(clip_rect()));
  }

  // If the rect has a NaN coordinate, we return empty rect to avoid crashes in
  // functions (for example, gfx::ToEnclosedRect) that are called on this rect.
  if (std::isnan(drawable_content_rect.x()) ||
      std::isnan(drawable_content_rect.y()) ||
      std::isnan(drawable_content_rect.right()) ||
      std::isnan(drawable_content_rect.bottom()))
    return gfx::RectF();

  return drawable_content_rect;
}

SkBlendMode RenderSurfaceImpl::BlendMode() const {
  return OwningEffectNode()->blend_mode;
}

bool RenderSurfaceImpl::UsesDefaultBlendMode() const {
  return BlendMode() == SkBlendMode::kSrcOver;
}

SkColor RenderSurfaceImpl::GetDebugBorderColor() const {
  return DebugColors::SurfaceBorderColor();
}

float RenderSurfaceImpl::GetDebugBorderWidth() const {
  return DebugColors::SurfaceBorderWidth(
      layer_tree_impl_ ? layer_tree_impl_->device_scale_factor() : 1);
}

LayerImpl* RenderSurfaceImpl::BackdropMaskLayer() const {
  ElementId mask_element_id = OwningEffectNode()->backdrop_mask_element_id;
  if (!mask_element_id)
    return nullptr;
  return layer_tree_impl_->LayerByElementId(mask_element_id);
}

bool RenderSurfaceImpl::HasMaskingContributingSurface() const {
  return OwningEffectNode()->has_masking_child;
}

const FilterOperations& RenderSurfaceImpl::Filters() const {
  return OwningEffectNode()->filters;
}

gfx::PointF RenderSurfaceImpl::FiltersOrigin() const {
  return OwningEffectNode()->filters_origin;
}

gfx::Transform RenderSurfaceImpl::SurfaceScale() const {
  gfx::Transform surface_scale;
  surface_scale.Scale(OwningEffectNode()->surface_contents_scale.x(),
                      OwningEffectNode()->surface_contents_scale.y());
  return surface_scale;
}

const FilterOperations& RenderSurfaceImpl::BackdropFilters() const {
  return OwningEffectNode()->backdrop_filters;
}

base::Optional<gfx::RRectF> RenderSurfaceImpl::BackdropFilterBounds() const {
  return OwningEffectNode()->backdrop_filter_bounds;
}

bool RenderSurfaceImpl::TrilinearFiltering() const {
  return OwningEffectNode()->trilinear_filtering;
}

bool RenderSurfaceImpl::HasCopyRequest() const {
  return OwningEffectNode()->has_copy_request;
}

bool RenderSurfaceImpl::ShouldCacheRenderSurface() const {
  return OwningEffectNode()->cache_render_surface;
}

int RenderSurfaceImpl::TransformTreeIndex() const {
  return OwningEffectNode()->transform_id;
}

int RenderSurfaceImpl::ClipTreeIndex() const {
  return OwningEffectNode()->clip_id;
}

int RenderSurfaceImpl::EffectTreeIndex() const {
  return effect_tree_index_;
}

const EffectNode* RenderSurfaceImpl::OwningEffectNode() const {
  return layer_tree_impl_->property_trees()->effect_tree.Node(
      EffectTreeIndex());
}

void RenderSurfaceImpl::SetClipRect(const gfx::Rect& clip_rect) {
  if (clip_rect == draw_properties_.clip_rect)
    return;

  surface_property_changed_ = true;
  draw_properties_.clip_rect = clip_rect;
}

void RenderSurfaceImpl::SetContentRect(const gfx::Rect& content_rect) {
  if (content_rect == draw_properties_.content_rect)
    return;

  surface_property_changed_ = true;
  draw_properties_.content_rect = content_rect;
}

void RenderSurfaceImpl::SetContentRectForTesting(const gfx::Rect& rect) {
  SetContentRect(rect);
}

gfx::Rect RenderSurfaceImpl::CalculateExpandedClipForFilters(
    const gfx::Transform& target_to_surface) {
  gfx::Rect clip_in_surface_space =
      MathUtil::ProjectEnclosingClippedRect(target_to_surface, clip_rect());
  gfx::Rect expanded_clip_in_surface_space = Filters().MapRect(
      clip_in_surface_space, SkMatrix(SurfaceScale().matrix()));
  gfx::Rect expanded_clip_in_target_space = MathUtil::MapEnclosingClippedRect(
      draw_transform(), expanded_clip_in_surface_space);
  return expanded_clip_in_target_space;
}

gfx::Rect RenderSurfaceImpl::CalculateClippedAccumulatedContentRect() {
  if (ShouldCacheRenderSurface() || HasCopyRequest() || !is_clipped())
    return accumulated_content_rect();

  if (accumulated_content_rect().IsEmpty())
    return gfx::Rect();

  // Calculate projection from the target surface rect to local
  // space. Non-invertible draw transforms means no able to bring clipped rect
  // in target space back to local space, early out without clip.
  gfx::Transform target_to_surface(gfx::Transform::kSkipInitialization);
  if (!draw_transform().GetInverse(&target_to_surface))
    return accumulated_content_rect();

  // Clip rect is in target space. Bring accumulated content rect to
  // target space in preparation for clipping.
  gfx::Rect accumulated_rect_in_target_space =
      MathUtil::MapEnclosingClippedRect(draw_transform(),
                                        accumulated_content_rect());
  // If accumulated content rect is contained within clip rect, early out
  // without clipping.
  if (clip_rect().Contains(accumulated_rect_in_target_space))
    return accumulated_content_rect();

  gfx::Rect clipped_accumulated_rect_in_target_space;
  if (Filters().HasFilterThatMovesPixels()) {
    clipped_accumulated_rect_in_target_space =
        CalculateExpandedClipForFilters(target_to_surface);
  } else {
    clipped_accumulated_rect_in_target_space = clip_rect();
  }
  if (layer_tree_impl_->settings().allow_de_jelly_effect) {
    clipped_accumulated_rect_in_target_space.Inset(0, -viz::MaxDeJellyHeight());
  }
  clipped_accumulated_rect_in_target_space.Intersect(
      accumulated_rect_in_target_space);

  if (clipped_accumulated_rect_in_target_space.IsEmpty())
    return gfx::Rect();

  gfx::Rect clipped_accumulated_rect_in_local_space =
      MathUtil::ProjectEnclosingClippedRect(
          target_to_surface, clipped_accumulated_rect_in_target_space);
  // Bringing clipped accumulated rect back to local space may result
  // in inflation due to axis-alignment.
  clipped_accumulated_rect_in_local_space.Intersect(accumulated_content_rect());
  return clipped_accumulated_rect_in_local_space;
}

void RenderSurfaceImpl::CalculateContentRectFromAccumulatedContentRect(
    int max_texture_size) {
  // Root render surface use viewport, and does not calculate content rect.
  DCHECK_NE(render_target(), this);

  // Surface's content rect is the clipped accumulated content rect. By default
  // use accumulated content rect, and then try to clip it.
  gfx::Rect surface_content_rect = CalculateClippedAccumulatedContentRect();

  // The RenderSurfaceImpl backing texture cannot exceed the maximum supported
  // texture size.
  surface_content_rect.set_width(
      std::min(surface_content_rect.width(), max_texture_size));
  surface_content_rect.set_height(
      std::min(surface_content_rect.height(), max_texture_size));

  SetContentRect(surface_content_rect);
}

void RenderSurfaceImpl::SetContentRectToViewport() {
  // Only root render surface use viewport as content rect.
  DCHECK_EQ(render_target(), this);
  gfx::Rect viewport = gfx::ToEnclosingRect(
      layer_tree_impl_->property_trees()->clip_tree.ViewportClip());
  SetContentRect(viewport);
}

void RenderSurfaceImpl::ClearAccumulatedContentRect() {
  accumulated_content_rect_ = gfx::Rect();
}

void RenderSurfaceImpl::AccumulateContentRectFromContributingLayer(
    LayerImpl* layer) {
  DCHECK(layer->DrawsContent());
  DCHECK_EQ(this, layer->render_target());

  // Root render surface doesn't accumulate content rect, it always uses
  // viewport for content rect.
  if (render_target() == this)
    return;

  accumulated_content_rect_.Union(layer->drawable_content_rect());
}

void RenderSurfaceImpl::AccumulateContentRectFromContributingRenderSurface(
    RenderSurfaceImpl* contributing_surface) {
  DCHECK_NE(this, contributing_surface);
  DCHECK_EQ(this, contributing_surface->render_target());

  // Root render surface doesn't accumulate content rect, it always uses
  // viewport for content rect.
  if (render_target() == this)
    return;

  // The content rect of contributing surface is in its own space. Instead, we
  // will use contributing surface's DrawableContentRect which is in target
  // space (local space for this render surface) as required.
  accumulated_content_rect_.Union(
      gfx::ToEnclosedRect(contributing_surface->DrawableContentRect()));
}

bool RenderSurfaceImpl::SurfacePropertyChanged() const {
  // Surface property changes are tracked as follows:
  //
  // - surface_property_changed_ is flagged when the clip_rect or content_rect
  //   change. As of now, these are the only two properties that can be affected
  //   by descendant layers.
  //
  // - all other property changes come from the surface's property tree nodes
  //   (or some ancestor node that propagates its change to one of these nodes).
  //
  return surface_property_changed_ || AncestorPropertyChanged();
}

bool RenderSurfaceImpl::SurfacePropertyChangedOnlyFromDescendant() const {
  return surface_property_changed_ && !AncestorPropertyChanged();
}

bool RenderSurfaceImpl::AncestorPropertyChanged() const {
  const PropertyTrees* property_trees = layer_tree_impl_->property_trees();
  return ancestor_property_changed_ || property_trees->full_tree_damaged ||
         property_trees->transform_tree.Node(TransformTreeIndex())
             ->transform_changed ||
         property_trees->effect_tree.Node(EffectTreeIndex())->effect_changed;
}

void RenderSurfaceImpl::NoteAncestorPropertyChanged() {
  ancestor_property_changed_ = true;
}

bool RenderSurfaceImpl::HasDamageFromeContributingContent() const {
  return damage_tracker_->has_damage_from_contributing_content();
}

gfx::Rect RenderSurfaceImpl::GetDamageRect() const {
  gfx::Rect damage_rect;
  bool is_valid_rect = damage_tracker_->GetDamageRectIfValid(&damage_rect);
  if (!is_valid_rect)
    return content_rect();
  return damage_rect;
}

void RenderSurfaceImpl::ResetPropertyChangedFlags() {
  surface_property_changed_ = false;
  ancestor_property_changed_ = false;
}

std::unique_ptr<viz::RenderPass> RenderSurfaceImpl::CreateRenderPass() {
  std::unique_ptr<viz::RenderPass> pass =
      viz::RenderPass::Create(num_contributors_);
  gfx::Rect damage_rect = GetDamageRect();
  damage_rect.Intersect(content_rect());
  pass->SetNew(id(), content_rect(), damage_rect,
               draw_properties_.screen_space_transform);
  pass->filters = Filters();
  pass->backdrop_filters = BackdropFilters();
  pass->backdrop_filter_bounds = BackdropFilterBounds();
  pass->generate_mipmap = TrilinearFiltering();
  pass->cache_render_pass = ShouldCacheRenderSurface();
  pass->has_damage_from_contributing_content =
      HasDamageFromeContributingContent();
  return pass;
}

void RenderSurfaceImpl::AppendQuads(DrawMode draw_mode,
                                    viz::RenderPass* render_pass,
                                    AppendQuadsData* append_quads_data) {
  gfx::Rect unoccluded_content_rect =
      occlusion_in_content_space().GetUnoccludedContentRect(content_rect());
  if (unoccluded_content_rect.IsEmpty())
    return;

  const PropertyTrees* property_trees = layer_tree_impl_->property_trees();
  int sorting_context_id =
      property_trees->transform_tree.Node(TransformTreeIndex())
          ->sorting_context_id;
  bool contents_opaque = false;
  viz::SharedQuadState* shared_quad_state =
      render_pass->CreateAndAppendSharedQuadState();
  shared_quad_state->SetAll(
      draw_transform(), content_rect(), content_rect(), rounded_corner_bounds(),
      draw_properties_.clip_rect, draw_properties_.is_clipped, contents_opaque,
      draw_properties_.draw_opacity, BlendMode(), sorting_context_id);

  if (layer_tree_impl_->debug_state().show_debug_borders.test(
          DebugBorderType::RENDERPASS)) {
    auto* debug_border_quad =
        render_pass->CreateAndAppendDrawQuad<viz::DebugBorderDrawQuad>();
    debug_border_quad->SetNew(shared_quad_state, content_rect(),
                              unoccluded_content_rect, GetDebugBorderColor(),
                              GetDebugBorderWidth());
  }

  LayerImpl* mask_layer = BackdropMaskLayer();
  viz::ResourceId mask_resource_id = 0;
  gfx::Size mask_texture_size;
  gfx::RectF mask_uv_rect;
  gfx::Vector2dF surface_contents_scale =
      OwningEffectNode()->surface_contents_scale;
  // Resourceless mode does not support masks.
  if (draw_mode != DRAW_MODE_RESOURCELESS_SOFTWARE && mask_layer &&
      mask_layer->DrawsContent() && !mask_layer->bounds().IsEmpty()) {
    // The software renderer applies mask layer and blending in the wrong
    // order but kDstIn doesn't commute with masking. It is okay to not
    // support this configuration because kDstIn was introduced to replace
    // mask layers.
    DCHECK(BlendMode() != SkBlendMode::kDstIn)
        << "kDstIn blend mode with mask layer is unsupported.";
    TRACE_EVENT1("cc", "RenderSurfaceImpl::AppendQuads",
                 "mask_layer_gpu_memory_usage",
                 mask_layer->GPUMemoryUsageInBytes());

    gfx::SizeF mask_uv_size;
    mask_layer->GetContentsResourceId(&mask_resource_id, &mask_texture_size,
                                      &mask_uv_size);
    gfx::SizeF unclipped_mask_target_size =
        gfx::ScaleSize(gfx::SizeF(mask_layer->bounds()),
                       surface_contents_scale.x(), surface_contents_scale.y());
    gfx::Vector2dF mask_offset = gfx::ScaleVector2d(
        mask_layer->offset_to_transform_parent(), surface_contents_scale.x(),
        surface_contents_scale.y());
    // Convert content_rect from target space to normalized mask UV space.
    // Where |unclipped_mask_target_size| maps to |mask_uv_size|.
    mask_uv_rect = gfx::ScaleRect(
        // Translate content_rect into mask resource's space.
        gfx::RectF(content_rect()) - mask_offset,
        mask_uv_size.width() / unclipped_mask_target_size.width(),
        mask_uv_size.height() / unclipped_mask_target_size.height());
  }

  gfx::RectF tex_coord_rect(gfx::Rect(content_rect().size()));
  auto* quad = render_pass->CreateAndAppendDrawQuad<viz::RenderPassDrawQuad>();
  quad->SetNew(shared_quad_state, content_rect(), unoccluded_content_rect, id(),
               mask_resource_id, mask_uv_rect, mask_texture_size,
               surface_contents_scale, FiltersOrigin(), tex_coord_rect,
               !layer_tree_impl_->settings().enable_edge_anti_aliasing,
               OwningEffectNode()->backdrop_filter_quality);
}

}  // namespace cc
