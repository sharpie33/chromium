// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fuchsia/engine/browser/fuchsia_layout_manager.h"

#include "ui/aura/window_tree_host.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/transform.h"

namespace {

// The background color that is shown when the main window does not fully
// fill the View.
constexpr SkColor kLetterboxBackgroundColor = SK_ColorBLACK;

// Returns a scaling factor that will allow |inset| to fit fully inside
// |container| without clipping.
float ProportionalScale(gfx::Size inset, gfx::Size container) {
  gfx::SizeF inset_f(inset);
  gfx::SizeF container_f(container);

  const float container_aspect_ratio =
      container_f.width() / container_f.height();
  const float inset_aspect_ratio = inset_f.width() / inset_f.height();
  if (container_aspect_ratio > inset_aspect_ratio) {
    // Height is constraining.
    return container_f.height() / inset_f.height();
  } else {
    // Width is constraining.
    return container_f.width() / inset_f.width();
  }
}

}  // namespace

FuchsiaLayoutManager::FuchsiaLayoutManager() {}

FuchsiaLayoutManager::~FuchsiaLayoutManager() {}

void FuchsiaLayoutManager::ForceContentDimensions(gfx::Size size) {
  render_size_override_ = size;
  UpdateContentBounds();
}

void FuchsiaLayoutManager::OnWindowResized() {
  // Resize the child to match the size of the parent.
  UpdateContentBounds();
}

void FuchsiaLayoutManager::OnWindowAddedToLayout(aura::Window* child) {
  if (child->type() == aura::client::WINDOW_TYPE_CONTROL) {
    DCHECK(!main_child_);
    main_child_ = child;
    SetChildBoundsDirect(main_child_,
                         gfx::Rect(main_child_->parent()->bounds().size()));

    UpdateContentBounds();
  }
}

void FuchsiaLayoutManager::OnWillRemoveWindowFromLayout(aura::Window* child) {
  if (child->type() == aura::client::WINDOW_TYPE_CONTROL) {
    DCHECK_EQ(child, main_child_);
    main_child_ = nullptr;
  }
}

void FuchsiaLayoutManager::OnWindowRemovedFromLayout(aura::Window* child) {}

void FuchsiaLayoutManager::OnChildWindowVisibilityChanged(aura::Window* child,
                                                          bool visible) {}

void FuchsiaLayoutManager::SetChildBounds(aura::Window* child,
                                          const gfx::Rect& requested_bounds) {
  if (child != main_child_)
    SetChildBoundsDirect(child, requested_bounds);
}

void FuchsiaLayoutManager::UpdateContentBounds() {
  if (!main_child_)
    return;

  const gfx::Size actual_size(main_child_->parent()->bounds().size());
  if (render_size_override_.IsEmpty()) {
    // Use all of the area available to the View.
    SetChildBoundsDirect(main_child_, gfx::Rect(actual_size));
    main_child_->SetTransform(gfx::Transform());
    return;
  }

  SetChildBoundsDirect(main_child_, gfx::Rect(render_size_override_));

  // Scale the window to fit in the View without clipping.
  const float scale = ProportionalScale(render_size_override_, actual_size);
  gfx::Transform transform;
  transform.Scale(scale, scale);

  // Center the window.
  const float center_x_offset =
      (actual_size.width() - (render_size_override_.width() * scale)) / 2.0;
  const float center_y_offset =
      (actual_size.height() - (render_size_override_.height() * scale)) / 2.0;
  transform.Translate(center_x_offset, center_y_offset);
  main_child_->SetTransform(transform);

  main_child_->parent()->GetHost()->compositor()->SetBackgroundColor(
      kLetterboxBackgroundColor);
}
