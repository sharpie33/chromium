// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/tabs/tab_strip.h"

#include <stddef.h>

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/compiler_specific.h"
#include "base/containers/adapters.h"
#include "base/containers/flat_map.h"
#include "base/feature_list.h"
#include "base/macros.h"
#include "base/metrics/histogram.h"
#include "base/metrics/histogram_functions.h"
#include "base/metrics/histogram_macros.h"
#include "base/metrics/user_metrics.h"
#include "base/no_destructor.h"
#include "base/stl_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/timer/elapsed_timer.h"
#include "build/build_config.h"
#include "chrome/browser/defaults.h"
#include "chrome/browser/themes/theme_properties.h"
#include "chrome/browser/ui/layout_constants.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/tab_types.h"
#include "chrome/browser/ui/ui_features.h"
#include "chrome/browser/ui/view_ids.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/tabs/browser_tab_strip_controller.h"
#include "chrome/browser/ui/views/tabs/new_tab_button.h"
#include "chrome/browser/ui/views/tabs/stacked_tab_strip_layout.h"
#include "chrome/browser/ui/views/tabs/tab.h"
#include "chrome/browser/ui/views/tabs/tab_drag_controller.h"
#include "chrome/browser/ui/views/tabs/tab_group_header.h"
#include "chrome/browser/ui/views/tabs/tab_group_highlight.h"
#include "chrome/browser/ui/views/tabs/tab_group_underline.h"
#include "chrome/browser/ui/views/tabs/tab_group_views.h"
#include "chrome/browser/ui/views/tabs/tab_hover_card_bubble_view.h"
#include "chrome/browser/ui/views/tabs/tab_slot_view.h"
#include "chrome/browser/ui/views/tabs/tab_strip_controller.h"
#include "chrome/browser/ui/views/tabs/tab_strip_layout_helper.h"
#include "chrome/browser/ui/views/tabs/tab_strip_layout_types.h"
#include "chrome/browser/ui/views/tabs/tab_strip_observer.h"
#include "chrome/browser/ui/views/tabs/tab_style_views.h"
#include "chrome/browser/ui/views/touch_uma/touch_uma.h"
#include "chrome/grit/generated_resources.h"
#include "chrome/grit/theme_resources.h"
#include "components/tab_groups/tab_group_color.h"
#include "components/tab_groups/tab_group_id.h"
#include "components/tab_groups/tab_group_visual_data.h"
#include "third_party/skia/include/core/SkColorFilter.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/effects/SkLayerDrawLooper.h"
#include "third_party/skia/include/pathops/SkPathOps.h"
#include "ui/accessibility/ax_node_data.h"
#include "ui/base/clipboard/clipboard.h"
#include "ui/base/default_theme_provider.h"
#include "ui/base/dragdrop/drag_drop_types.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/models/list_selection_model.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/gfx/animation/throb_animation.h"
#include "ui/gfx/animation/tween.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/gfx/image/image_skia_operations.h"
#include "ui/gfx/skia_util.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/image_view.h"
#include "ui/views/masked_targeter_delegate.h"
#include "ui/views/mouse_watcher_view_host.h"
#include "ui/views/rect_based_targeting_utils.h"
#include "ui/views/view_model_utils.h"
#include "ui/views/view_observer.h"
#include "ui/views/view_targeter.h"
#include "ui/views/widget/root_view.h"
#include "ui/views/widget/widget.h"
#include "ui/views/window/non_client_view.h"

#if defined(OS_WIN)
#include "base/win/windows_version.h"
#include "ui/display/win/screen_win.h"
#include "ui/gfx/win/hwnd_util.h"
#include "ui/views/win/hwnd_util.h"
#endif

#if defined(USE_AURA)
#include "ui/aura/window.h"
#endif

using MD = ui::MaterialDesignController;

namespace {

// Distance from the next/previous stacked before before we consider the tab
// close enough to trigger moving.
const int kStackedDistance = 36;

// Given the bounds of a dragged tab, return the X coordinate to use for
// computing where in the strip to insert/move the tab.
int GetDraggedX(const gfx::Rect& dragged_bounds) {
  return dragged_bounds.x() + TabStyle::GetTabInternalPadding().left();
}

// Max number of stacked tabs.
constexpr int kMaxStackedCount = 4;

// Padding between stacked tabs.
constexpr int kStackedPadding = 6;

// Size of the drop indicator.
int g_drop_indicator_width = 0;
int g_drop_indicator_height = 0;

// The offset in indices when shifting a tab or group in the given direction.
enum ShiftOffset { kLeft = -1, kRight = 1 };

// Listens in on the browser event stream (as a pre target event handler) and
// hides an associated hover card on any keypress.
class TabHoverCardEventSniffer : public ui::EventHandler {
 public:
  TabHoverCardEventSniffer(TabHoverCardBubbleView* hover_card,
                           TabStrip* tab_strip)
      : hover_card_(hover_card),
        tab_strip_(tab_strip),
        widget_(tab_strip->GetWidget()) {
#if defined(OS_MACOSX)
    if (widget_->GetRootView())
      widget_->GetRootView()->AddPreTargetHandler(this);
#else
    if (widget_->GetNativeWindow())
      widget_->GetNativeWindow()->AddPreTargetHandler(this);
#endif
  }

  ~TabHoverCardEventSniffer() override {
#if defined(OS_MACOSX)
    widget_->GetRootView()->RemovePreTargetHandler(this);
#else
    widget_->GetNativeWindow()->RemovePreTargetHandler(this);
#endif
  }

 protected:
  // ui::EventTarget:
  void OnKeyEvent(ui::KeyEvent* event) override {
    if (!tab_strip_->IsFocusInTabs())
      tab_strip_->UpdateHoverCard(nullptr);
  }

  void OnMouseEvent(ui::MouseEvent* event) override {
    if (event->IsAnyButton())
      hover_card_->FadeOutToHide();
  }

  void OnGestureEvent(ui::GestureEvent* event) override {
    hover_card_->FadeOutToHide();
  }

 private:
  TabHoverCardBubbleView* const hover_card_;
  TabStrip* tab_strip_;
  views::Widget* widget_;
};

// Animation delegate used for any automatic tab movement.  Hides the tab if it
// is not fully visible within the tabstrip area, to prevent overflow clipping.
class TabAnimationDelegate : public gfx::AnimationDelegate {
 public:
  TabAnimationDelegate(TabStrip* tab_strip, Tab* tab);
  ~TabAnimationDelegate() override;

  void AnimationProgressed(const gfx::Animation* animation) override;

 protected:
  TabStrip* tab_strip() { return tab_strip_; }
  Tab* tab() { return tab_; }

 private:
  TabStrip* const tab_strip_;
  Tab* const tab_;

  DISALLOW_COPY_AND_ASSIGN(TabAnimationDelegate);
};

TabAnimationDelegate::TabAnimationDelegate(TabStrip* tab_strip, Tab* tab)
    : tab_strip_(tab_strip), tab_(tab) {}

TabAnimationDelegate::~TabAnimationDelegate() {}

void TabAnimationDelegate::AnimationProgressed(
    const gfx::Animation* animation) {
  tab_->SetVisible(tab_strip_->ShouldTabBeVisible(tab_));
}

// Animation delegate used when a dragged tab is released. When done sets the
// dragging state to false.
class ResetDraggingStateDelegate : public TabAnimationDelegate {
 public:
  ResetDraggingStateDelegate(TabStrip* tab_strip, Tab* tab);
  ~ResetDraggingStateDelegate() override;

  void AnimationEnded(const gfx::Animation* animation) override;
  void AnimationCanceled(const gfx::Animation* animation) override;

 private:
  DISALLOW_COPY_AND_ASSIGN(ResetDraggingStateDelegate);
};

ResetDraggingStateDelegate::ResetDraggingStateDelegate(TabStrip* tab_strip,
                                                       Tab* tab)
    : TabAnimationDelegate(tab_strip, tab) {}

ResetDraggingStateDelegate::~ResetDraggingStateDelegate() {}

void ResetDraggingStateDelegate::AnimationEnded(
    const gfx::Animation* animation) {
  tab()->set_dragging(false);
  AnimationProgressed(animation);  // Forces tab visibility to update.
}

void ResetDraggingStateDelegate::AnimationCanceled(
    const gfx::Animation* animation) {
  AnimationEnded(animation);
}

// If |dest| contains the point |point_in_source| the event handler from |dest|
// is returned. Otherwise returns null.
views::View* ConvertPointToViewAndGetEventHandler(
    views::View* source,
    views::View* dest,
    const gfx::Point& point_in_source) {
  gfx::Point dest_point(point_in_source);
  views::View::ConvertPointToTarget(source, dest, &dest_point);
  return dest->HitTestPoint(dest_point)
             ? dest->GetEventHandlerForPoint(dest_point)
             : nullptr;
}

// Gets a tooltip handler for |point_in_source| from |dest|. Note that |dest|
// should return null if it does not contain the point.
views::View* ConvertPointToViewAndGetTooltipHandler(
    views::View* source,
    views::View* dest,
    const gfx::Point& point_in_source) {
  gfx::Point dest_point(point_in_source);
  views::View::ConvertPointToTarget(source, dest, &dest_point);
  return dest->GetTooltipHandlerForPoint(dest_point);
}

TabDragController::EventSource EventSourceFromEvent(
    const ui::LocatedEvent& event) {
  return event.IsGestureEvent() ? TabDragController::EVENT_SOURCE_TOUCH
                                : TabDragController::EVENT_SOURCE_MOUSE;
}

int GetStackableTabWidth() {
  return TabStyle::GetTabOverlap() + (MD::touch_ui() ? 136 : 102);
}

}  // namespace

///////////////////////////////////////////////////////////////////////////////
// TabStrip::RemoveTabDelegate
//
// AnimationDelegate used when removing a tab. Does the necessary cleanup when
// done.
class TabStrip::RemoveTabDelegate : public TabAnimationDelegate {
 public:
  RemoveTabDelegate(TabStrip* tab_strip, Tab* tab);

  void AnimationEnded(const gfx::Animation* animation) override;
  void AnimationCanceled(const gfx::Animation* animation) override;

 private:
  DISALLOW_COPY_AND_ASSIGN(RemoveTabDelegate);
};

TabStrip::RemoveTabDelegate::RemoveTabDelegate(TabStrip* tab_strip, Tab* tab)
    : TabAnimationDelegate(tab_strip, tab) {}

void TabStrip::RemoveTabDelegate::AnimationEnded(
    const gfx::Animation* animation) {
  tab_strip()->OnTabCloseAnimationCompleted(tab());
}

void TabStrip::RemoveTabDelegate::AnimationCanceled(
    const gfx::Animation* animation) {
  AnimationEnded(animation);
}

///////////////////////////////////////////////////////////////////////////////
// TabStrip::TabDragContextImpl
//
class TabStrip::TabDragContextImpl : public TabDragContext {
 public:
  explicit TabDragContextImpl(TabStrip* tab_strip) : tab_strip_(tab_strip) {}

  bool IsDragStarted() const {
    return drag_controller_ && drag_controller_->started_drag();
  }

  bool IsMutating() const {
    return drag_controller_ && drag_controller_->is_mutating();
  }

  bool IsDraggingWindow() const {
    return drag_controller_ && drag_controller_->is_dragging_window();
  }

  bool IsDraggingTab(content::WebContents* contents) const {
    return contents && drag_controller_ &&
           drag_controller_->IsDraggingTab(contents);
  }

  void SetMoveBehavior(TabDragController::MoveBehavior move_behavior) {
    if (drag_controller_)
      drag_controller_->SetMoveBehavior(move_behavior);
  }

  void MaybeStartDrag(TabSlotView* source,
                      const ui::LocatedEvent& event,
                      const ui::ListSelectionModel& original_selection) {
    std::vector<TabSlotView*> dragging_views;
    int x = source->GetMirroredXInView(event.x());
    int y = event.y();

    // Build the set of selected tabs to drag and calculate the offset from the
    // source.
    ui::ListSelectionModel selection_model;
    if (source->GetTabSlotViewType() ==
        TabSlotView::ViewType::kTabGroupHeader) {
      dragging_views.push_back(source);

      const std::vector<int> grouped_tabs =
          tab_strip_->controller()->ListTabsInGroup(source->group().value());
      for (int index : grouped_tabs) {
        dragging_views.push_back(GetTabAt(index));
        // Set |selection_model| if and only if the original selection does not
        // match the group exactly. See TabDragController::Init() for details
        // on how |selection_model| is used.
        if (!original_selection.IsSelected(index))
          selection_model = original_selection;
      }
      if (grouped_tabs.size() != original_selection.size())
        selection_model = original_selection;
    } else {
      for (int i = 0; i < GetTabCount(); ++i) {
        Tab* other_tab = GetTabAt(i);
        if (tab_strip_->IsTabSelected(other_tab)) {
          dragging_views.push_back(other_tab);
          if (other_tab == source)
            x += GetSizeNeededForViews(dragging_views) - other_tab->width();
        }
      }
      if (!original_selection.IsSelected(tab_strip_->GetModelIndexOf(source)))
        selection_model = original_selection;
    }

    DCHECK(!dragging_views.empty());
    DCHECK(base::Contains(dragging_views, source));

    // Delete the existing DragController before creating a new one. We do this
    // as creating the DragController remembers the WebContents delegates and we
    // need to make sure the existing DragController isn't still a delegate.
    drag_controller_.reset();
    TabDragController::MoveBehavior move_behavior = TabDragController::REORDER;

    // Use MOVE_VISIBLE_TABS in the following conditions:
    // . Mouse event generated from touch and the left button is down (the right
    //   button corresponds to a long press, which we want to reorder).
    // . Gesture tap down and control key isn't down.
    // . Real mouse event and control is down. This is mostly for testing.
    DCHECK(event.type() == ui::ET_MOUSE_PRESSED ||
           event.type() == ui::ET_GESTURE_TAP_DOWN);
    if (tab_strip_->touch_layout_ &&
        ((event.type() == ui::ET_MOUSE_PRESSED &&
          (((event.flags() & ui::EF_FROM_TOUCH) &&
            static_cast<const ui::MouseEvent&>(event).IsLeftMouseButton()) ||
           (!(event.flags() & ui::EF_FROM_TOUCH) &&
            static_cast<const ui::MouseEvent&>(event).IsControlDown()))) ||
         (event.type() == ui::ET_GESTURE_TAP_DOWN && !event.IsControlDown()))) {
      move_behavior = TabDragController::MOVE_VISIBLE_TABS;
    }

    drag_controller_ = std::make_unique<TabDragController>();
    drag_controller_->Init(this, source, dragging_views, gfx::Point(x, y),
                           event.x(), std::move(selection_model), move_behavior,
                           EventSourceFromEvent(event));
  }

  void ContinueDrag(views::View* view, const ui::LocatedEvent& event) {
    if (drag_controller_.get() &&
        drag_controller_->event_source() == EventSourceFromEvent(event)) {
      gfx::Point screen_location(event.location());
      views::View::ConvertPointToScreen(view, &screen_location);
      drag_controller_->Drag(screen_location);
    }
  }

  bool EndDrag(EndDragReason reason) {
    if (!drag_controller_.get())
      return false;
    bool started_drag = drag_controller_->started_drag();
    drag_controller_->EndDrag(reason);
    return started_drag;
  }

  // TabDragContext:
  views::View* AsView() override { return tab_strip_; }

  const views::View* AsView() const override { return tab_strip_; }

  Tab* GetTabAt(int i) const override { return tab_strip_->tab_at(i); }

  int GetIndexOf(const TabSlotView* view) const override {
    return tab_strip_->GetModelIndexOf(view);
  }

  int GetTabCount() const override { return tab_strip_->tab_count(); }

  bool IsTabPinned(const Tab* tab) const override {
    return tab_strip_->IsTabPinned(tab);
  }

  int GetPinnedTabCount() const override {
    return tab_strip_->GetPinnedTabCount();
  }

  TabGroupHeader* GetTabGroupHeader(
      const tab_groups::TabGroupId& group) const override {
    return tab_strip_->group_header(group);
  }

  TabStripModel* GetTabStripModel() override {
    return static_cast<BrowserTabStripController*>(
               tab_strip_->controller_.get())
        ->model();
  }

  base::Optional<int> GetActiveTouchIndex() const override {
    if (!tab_strip_->touch_layout_)
      return base::nullopt;
    return tab_strip_->touch_layout_->active_index();
  }

  TabDragController* GetDragController() override {
    return drag_controller_.get();
  }

  void OwnDragController(TabDragController* controller) override {
    // Typically, ReleaseDragController() and OwnDragController() calls are
    // paired via corresponding calls to TabDragController::Detach() and
    // TabDragController::Attach(). There is one exception to that rule: when a
    // drag might start, we create a TabDragController that is owned by the
    // potential source tabstrip in MaybeStartDrag(). If a drag actually starts,
    // we then call Attach() on the source tabstrip, but since the source
    // tabstrip already owns the TabDragController, so we don't need to do
    // anything.
    if (drag_controller_.get() != controller)
      drag_controller_.reset(controller);
  }

  void DestroyDragController() override {
    SetNewTabButtonVisible(true);
    drag_controller_.reset();
  }

  TabDragController* ReleaseDragController() override {
    return drag_controller_.release();
  }

  bool IsDragSessionActive() const override {
    return drag_controller_ != nullptr;
  }

  bool IsActiveDropTarget() const override {
    for (int i = 0; i < GetTabCount(); ++i) {
      const Tab* const tab = GetTabAt(i);
      if (tab->dragging())
        return true;
    }
    return false;
  }

  std::vector<int> GetTabXCoordinates() const override {
    std::vector<int> results;
    for (int i = 0; i < GetTabCount(); ++i)
      results.push_back(ideal_bounds(i).x());
    return results;
  }

  int GetActiveTabWidth() const override {
    return tab_strip_->GetActiveTabWidth();
  }

  int GetTabAreaWidth() const override { return tab_strip_->GetTabAreaWidth(); }

  int GetTabDragAreaWidth() const override {
    return tab_strip_->width() - tab_strip_->FrameGrabWidth();
  }

  int TabDragAreaBeginX() const override {
    return tab_strip_->GetMirroredXWithWidthInView(0, GetTabDragAreaWidth());
  }

  int TabDragAreaEndX() const override {
    return TabDragAreaBeginX() + GetTabDragAreaWidth();
  }

  int GetHorizontalDragThreshold() const override {
    constexpr int kHorizontalMoveThreshold = 16;  // DIPs.

    // Stacked tabs in touch mode don't shrink.
    if (tab_strip_->touch_layout_)
      return kHorizontalMoveThreshold;

    double ratio = double{tab_strip_->GetInactiveTabWidth()} /
                   TabStyle::GetStandardWidth();
    return gfx::ToRoundedInt(ratio * kHorizontalMoveThreshold);
  }

  int GetInsertionIndexForDraggedBounds(
      const gfx::Rect& dragged_bounds,
      bool attaching,
      int num_dragged_tabs,
      bool mouse_has_ever_moved_left,
      bool mouse_has_ever_moved_right,
      base::Optional<tab_groups::TabGroupId> group) const override {
    // If the strip has no tabs, the only position to insert at is 0.
    if (!GetTabCount())
      return 0;

    base::Optional<int> index;
    base::Optional<int> touch_index = GetActiveTouchIndex();
    if (touch_index) {
      index = GetInsertionIndexForDraggedBoundsStacked(
          dragged_bounds, mouse_has_ever_moved_left,
          mouse_has_ever_moved_right);
      if (index) {
        // Only move the tab to the left/right if the user actually moved the
        // mouse that way. This is necessary as tabs with stacked tabs
        // before/after them have multiple drag positions.
        if ((index < touch_index && !mouse_has_ever_moved_left) ||
            (index > touch_index && !mouse_has_ever_moved_right)) {
          index = *touch_index;
        }
      }
    } else {
      index = GetInsertionIndexFrom(dragged_bounds, 0, std::move(group));
    }
    if (!index) {
      const int last_tab_right = ideal_bounds(GetTabCount() - 1).right();
      index = (dragged_bounds.right() > last_tab_right) ? GetTabCount() : 0;
    }

    const Tab* last_visible_tab = tab_strip_->GetLastVisibleTab();
    int last_insertion_point =
        last_visible_tab ? (GetIndexOf(last_visible_tab) + 1) : 0;
    if (!attaching) {
      // We're not in the process of attaching, so clamp the insertion point to
      // keep it within the visible region.
      last_insertion_point =
          std::max(0, last_insertion_point - num_dragged_tabs);
    }

    // Ensure the first dragged tab always stays in the visible index range.
    return std::min(*index, last_insertion_point);
  }

  bool ShouldDragToNextStackedTab(
      const gfx::Rect& dragged_bounds,
      int index,
      bool mouse_has_ever_moved_right) const override {
    if (index + 1 >= GetTabCount() ||
        !tab_strip_->touch_layout_->IsStacked(index + 1) ||
        !mouse_has_ever_moved_right)
      return false;

    int active_x = ideal_bounds(index).x();
    int next_x = ideal_bounds(index + 1).x();
    int mid_x =
        std::min(next_x - kStackedDistance, active_x + (next_x - active_x) / 4);
    return GetDraggedX(dragged_bounds) >= mid_x;
  }

  bool ShouldDragToPreviousStackedTab(
      const gfx::Rect& dragged_bounds,
      int index,
      bool mouse_has_ever_moved_left) const override {
    if (index - 1 < tab_strip_->GetPinnedTabCount() ||
        !tab_strip_->touch_layout_->IsStacked(index - 1) ||
        !mouse_has_ever_moved_left)
      return false;

    int active_x = ideal_bounds(index).x();
    int previous_x = ideal_bounds(index - 1).x();
    int mid_x = std::max(previous_x + kStackedDistance,
                         active_x - (active_x - previous_x) / 4);
    return GetDraggedX(dragged_bounds) <= mid_x;
  }

  void DragActiveTabStacked(const std::vector<int>& initial_positions,
                            int delta) override {
    DCHECK_EQ(GetTabCount(), int{initial_positions.size()});
    SetIdealBoundsFromPositions(initial_positions);
    tab_strip_->touch_layout_->DragActiveTab(delta);
    tab_strip_->CompleteAnimationAndLayout();
  }

  std::vector<gfx::Rect> CalculateBoundsForDraggedViews(
      const std::vector<TabSlotView*>& views) override {
    DCHECK(!views.empty());

    std::vector<gfx::Rect> bounds;
    const int overlap = TabStyle::GetTabOverlap();
    int x = 0;
    for (const TabSlotView* view : views) {
      const int width = view->width();
      bounds.push_back(gfx::Rect(x, 0, width, view->height()));
      x += width - overlap;
    }

    return bounds;
  }

  void SetBoundsForDrag(const std::vector<TabSlotView*>& views,
                        const std::vector<gfx::Rect>& bounds) override {
    tab_strip_->StopAnimating(false);
    DCHECK_EQ(views.size(), bounds.size());
    for (size_t i = 0; i < views.size(); ++i)
      views[i]->SetBoundsRect(bounds[i]);
    // Reset the layout size as we've effectively laid out a different size.
    // This ensures a layout happens after the drag is done.
    tab_strip_->last_layout_size_ = gfx::Size();
  }

  void StartedDragging(const std::vector<TabSlotView*>& views) override {
    // Let the controller know that the user started dragging tabs.
    tab_strip_->controller_->OnStartedDragging();

    // Hide the new tab button immediately if we didn't originate the drag.
    if (!drag_controller_)
      SetNewTabButtonVisible(false);

    // Reset dragging state of existing tabs.
    for (int i = 0; i < GetTabCount(); ++i)
      GetTabAt(i)->set_dragging(false);

    for (size_t i = 0; i < views.size(); ++i) {
      views[i]->set_dragging(true);
      tab_strip_->bounds_animator_.StopAnimatingView(views[i]);
    }

    // Move the dragged tabs to their ideal bounds.
    tab_strip_->UpdateIdealBounds();

    // Sets the bounds of the dragged tabs.
    for (size_t i = 0; i < views.size(); ++i) {
      // Non-tabs, such as TabGroupHeaders, may also be dragging. Ignore these,
      // since they are positioned independently.
      if (views[i]->GetTabSlotViewType() != TabSlotView::ViewType::kTab)
        continue;

      int tab_data_index = GetIndexOf(views[i]);
      DCHECK_NE(TabStripModel::kNoTab, tab_data_index);
      views[i]->SetBoundsRect(ideal_bounds(tab_data_index));
    }
    tab_strip_->SetTabVisibility();
    tab_strip_->SchedulePaint();
  }

  void DraggedTabsDetached() override {
    // Let the controller know that the user is not dragging this tabstrip's
    // tabs anymore.
    tab_strip_->controller_->OnStoppedDragging();
    SetNewTabButtonVisible(true);
  }

  void StoppedDragging(const std::vector<TabSlotView*>& views,
                       const std::vector<int>& initial_positions,
                       bool move_only,
                       bool completed) override {
    // Let the controller know that the user stopped dragging tabs.
    tab_strip_->controller_->OnStoppedDragging();

    SetNewTabButtonVisible(true);
    if (move_only && tab_strip_->touch_layout_) {
      if (completed)
        tab_strip_->touch_layout_->SizeToFit();
      else
        SetIdealBoundsFromPositions(initial_positions);
    }
    bool is_first_view = true;
    for (size_t i = 0; i < views.size(); ++i)
      tab_strip_->StoppedDraggingView(views[i], &is_first_view);
  }

  void LayoutDraggedViewsAt(const std::vector<TabSlotView*>& views,
                            TabSlotView* source_view,
                            const gfx::Point& location,
                            bool initial_drag) override {
    // Immediately hide the new tab button if the last tab is being dragged.
    const Tab* last_visible_tab = tab_strip_->GetLastVisibleTab();
    if (last_visible_tab && last_visible_tab->dragging())
      SetNewTabButtonVisible(false);

    std::vector<gfx::Rect> bounds = CalculateBoundsForDraggedViews(views);
    DCHECK_EQ(views.size(), bounds.size());

    int active_tab_model_index = GetIndexOf(source_view);
    int active_tab_index = static_cast<int>(
        std::find(views.begin(), views.end(), source_view) - views.begin());
    for (size_t i = 0; i < views.size(); ++i) {
      TabSlotView* view = views[i];
      gfx::Rect new_bounds = bounds[i];
      new_bounds.Offset(location.x(), location.y());
      int consecutive_index =
          active_tab_model_index - (active_tab_index - static_cast<int>(i));
      // If this is the initial layout during a drag and the tabs aren't
      // consecutive animate the view into position. Do the same if the tab is
      // already animating (which means we previously caused it to animate).
      if ((initial_drag && GetIndexOf(views[i]) != consecutive_index) ||
          tab_strip_->bounds_animator_.IsAnimating(views[i])) {
        tab_strip_->bounds_animator_.SetTargetBounds(views[i], new_bounds);
      } else {
        view->SetBoundsRect(new_bounds);
      }
    }
    tab_strip_->SetTabVisibility();
  }

  // Forces the entire tabstrip to lay out.
  void ForceLayout() override {
    tab_strip_->InvalidateLayout();
    tab_strip_->CompleteAnimationAndLayout();
  }

 private:
  gfx::Rect ideal_bounds(int i) const { return tab_strip_->ideal_bounds(i); }

  void SetNewTabButtonVisible(bool visible) {
    tab_strip_->new_tab_button_->SetVisible(visible);
  }

  // Used by GetInsertionIndexForDraggedBounds() when the tabstrip is stacked.
  base::Optional<int> GetInsertionIndexForDraggedBoundsStacked(
      const gfx::Rect& dragged_bounds,
      bool mouse_has_ever_moved_left,
      bool mouse_has_ever_moved_right) const {
    int active_index = *GetActiveTouchIndex();
    // Search from the active index to the front of the tabstrip. Do this as
    // tabs overlap each other from the active index.
    base::Optional<int> index =
        GetInsertionIndexFromReversed(dragged_bounds, active_index);
    if (index != active_index)
      return index;
    if (!index)
      return GetInsertionIndexFrom(dragged_bounds, active_index + 1,
                                   base::nullopt);

    // The position to drag to corresponds to the active tab. If the
    // next/previous tab is stacked, then shorten the distance used to determine
    // insertion bounds. We do this as GetInsertionIndexFrom() uses the bounds
    // of the tabs. When tabs are stacked the next/previous tab is on top of the
    // tab.
    if (active_index + 1 < GetTabCount() &&
        tab_strip_->touch_layout_->IsStacked(active_index + 1)) {
      index = GetInsertionIndexFrom(dragged_bounds, active_index + 1,
                                    base::nullopt);
      if (!index && ShouldDragToNextStackedTab(dragged_bounds, active_index,
                                               mouse_has_ever_moved_right))
        index = active_index + 1;
      else if (index == -1)
        index = active_index;
    } else if (ShouldDragToPreviousStackedTab(dragged_bounds, active_index,
                                              mouse_has_ever_moved_left)) {
      index = active_index - 1;
    }
    return index;
  }

  // Determines the index to insert tabs at. |dragged_bounds| is the bounds of
  // the tab being dragged, |start| is the index of the tab to start looking
  // from, and |group| is the currently dragged group, if any. The search
  // proceeds to the end of the strip.
  base::Optional<int> GetInsertionIndexFrom(
      const gfx::Rect& dragged_bounds,
      int start,
      base::Optional<tab_groups::TabGroupId> group) const {
    const int last_tab = GetTabCount() - 1;
    const int dragged_x = GetDraggedX(dragged_bounds);
    if (start < 0 || start > last_tab || dragged_x < ideal_bounds(start).x() ||
        dragged_x > ideal_bounds(last_tab).right())
      return base::nullopt;

    base::Optional<int> insertion_index;
    for (int i = start; i <= last_tab; ++i) {
      if (dragged_x < ideal_bounds(i).CenterPoint().x()) {
        insertion_index = i;
        break;
      }
    }

    if (!insertion_index.has_value())
      return last_tab + 1;

    return GetInsertionIndexWithGroup(dragged_bounds, insertion_index.value(),
                                      std::move(group));
  }

  // Like GetInsertionIndexFrom(), but searches backwards from |start| to the
  // beginning of the strip.
  base::Optional<int> GetInsertionIndexFromReversed(
      const gfx::Rect& dragged_bounds,
      int start) const {
    const int dragged_x = GetDraggedX(dragged_bounds);
    if (start < 0 || start >= GetTabCount() ||
        dragged_x >= ideal_bounds(start).right() ||
        dragged_x < ideal_bounds(0).x())
      return base::nullopt;

    for (int i = start; i >= 0; --i) {
      if (dragged_x >= ideal_bounds(i).CenterPoint().x())
        return i + 1;
    }

    return 0;
  }

  // Determines the index to insert at, accounting for the dragging group and
  // other groups. |dragged_bounds| is the bounds of the tab being dragged,
  // |candidate_index| is the naive insertion index found via
  // GetInsertionIndexFrom, and |dragging_group| is the currently dragged
  // group, if any. This is distinct from the group membership of the dragging
  // tabs, and is only set when dragging by the group's header.
  int GetInsertionIndexWithGroup(
      const gfx::Rect& dragged_bounds,
      int candidate_index,
      base::Optional<tab_groups::TabGroupId> dragging_group) const {
    if (!dragging_group.has_value())
      return candidate_index;

    const std::vector<int> dragging_tabs =
        tab_strip_->controller()->ListTabsInGroup(dragging_group.value());
    base::Optional<tab_groups::TabGroupId> other_group =
        tab_strip_->tab_at(candidate_index)->group();

    // The other group will be the same as the dragging group if the user
    // hasn't dragged beyond the boundaries of the current "gap". In this
    // case, look ahead to where the dragging group would go, and sample
    // the group that's currently there.
    if (dragging_group == other_group) {
      // |dragging_tabs| can only be empty if dragging in from another window,
      // in which case |dragging_group| can't be the same as |other_group|.
      DCHECK(dragging_tabs.size() > 0);
      if (candidate_index <= dragging_tabs.front() ||
          dragging_tabs.back() >= GetTabCount() - 1)
        return dragging_tabs.front();

      other_group = tab_strip_->tab_at(dragging_tabs.back() + 1)->group();
    }

    if (!other_group.has_value())
      return candidate_index;

    const std::vector<int> other_tabs =
        tab_strip_->controller()->ListTabsInGroup(other_group.value());

    if (other_tabs.size() == 0)
      return candidate_index;

    // If the candidate index is in the middle of the other group, instead
    // return the nearest insertion index that is not in the other group.
    const int other_group_width =
        ideal_bounds(other_tabs.back()).right() -
        tab_strip_->group_header(other_group.value())->x();
    int left_insertion_index = other_tabs.front();
    if (dragging_tabs.size() > 0 && dragging_tabs.front() < other_tabs.front())
      left_insertion_index = dragging_tabs.front();

    if (GetDraggedX(dragged_bounds) <
        ideal_bounds(left_insertion_index).x() + other_group_width / 2)
      return left_insertion_index;
    return left_insertion_index + other_tabs.size();
  }

  // Sets the ideal bounds x-coordinates to |positions|.
  void SetIdealBoundsFromPositions(const std::vector<int>& positions) {
    if (static_cast<size_t>(GetTabCount()) != positions.size())
      return;

    for (int i = 0; i < GetTabCount(); ++i) {
      gfx::Rect bounds(ideal_bounds(i));
      bounds.set_x(positions[i]);
      tab_strip_->tabs_.set_ideal_bounds(i, bounds);
    }
  }

  TabStrip* const tab_strip_;

  // The controller for a drag initiated from a Tab. Valid for the lifetime of
  // the drag session.
  std::unique_ptr<TabDragController> drag_controller_;
};

///////////////////////////////////////////////////////////////////////////////
// TabStrip, public:

TabStrip::TabStrip(std::unique_ptr<TabStripController> controller)
    : controller_(std::move(controller)),
      layout_helper_(std::make_unique<TabStripLayoutHelper>(
          controller_.get(),
          base::BindRepeating(&TabStrip::tabs_view_model,
                              base::Unretained(this)),
          base::BindRepeating(&TabStrip::GetGroupHeaders,
                              base::Unretained(this)),
          base::BindRepeating(&TabStrip::LayoutToCurrentBounds,
                              base::Unretained(this)))),
      drag_context_(std::make_unique<TabDragContextImpl>(this)) {
  Init();
  SetEventTargeter(std::make_unique<views::ViewTargeter>(this));
  md_observer_.Add(MD::GetInstance());
}

TabStrip::~TabStrip() {
  // The animations may reference the tabs. Shut down the animation before we
  // delete the tabs.
  StopAnimating(false);

  // Disengage the drag controller before doing any additional cleanup. This
  // call can interact with child views so we can't reliably do it during member
  // destruction.
  drag_context_->DestroyDragController();

  // Make sure we unhook ourselves as a message loop observer so that we don't
  // crash in the case where the user closes the window after closing a tab
  // but before moving the mouse.
  RemoveMessageLoopObserver();

  new_tab_button_->RemoveObserver(this);

  hover_card_observer_.RemoveAll();

  // The children (tabs) may callback to us from their destructor. Delete them
  // so that if they call back we aren't in a weird state.
  RemoveAllChildViews(true);
}

// static
int TabStrip::GetSizeNeededForViews(const std::vector<TabSlotView*>& views) {
  int width = 0;
  for (const TabSlotView* view : views)
    width += view->width();
  if (!views.empty())
    width -= TabStyle::GetTabOverlap() * (views.size() - 1);
  return width;
}

void TabStrip::AddObserver(TabStripObserver* observer) {
  observers_.AddObserver(observer);
}

void TabStrip::RemoveObserver(TabStripObserver* observer) {
  observers_.RemoveObserver(observer);
}

void TabStrip::FrameColorsChanged() {
  for (int i = 0; i < tab_count(); ++i)
    tab_at(i)->FrameColorsChanged();
  new_tab_button_->FrameColorsChanged();
  UpdateContrastRatioValues();
  SchedulePaint();
}

void TabStrip::SetBackgroundOffset(int background_offset) {
  if (background_offset != background_offset_) {
    background_offset_ = background_offset;
    SchedulePaint();
  }
}

bool TabStrip::IsRectInWindowCaption(const gfx::Rect& rect) {
  // If there is no control at this location, the hit is in the caption area.
  const views::View* v = GetEventHandlerForRect(rect);
  if (v == this)
    return true;

  // When the window has a top drag handle, a thin strip at the top of inactive
  // tabs and the new tab button is treated as part of the window drag handle,
  // to increase draggability.  This region starts 1 DIP above the top of the
  // separator.
  const int drag_handle_extension = TabStyle::GetDragHandleExtension(height());

  // Disable drag handle extension when tab shapes are visible.
  bool extend_drag_handle = !controller_->IsFrameCondensed() &&
                            !controller_->EverHasVisibleBackgroundTabShapes();

  // A hit on the tab is not in the caption unless it is in the thin strip
  // mentioned above.
  const int tab_index = tabs_.GetIndexOfView(v);
  if (IsValidModelIndex(tab_index)) {
    Tab* tab = tab_at(tab_index);
    gfx::Rect tab_drag_handle = tab->GetMirroredBounds();
    tab_drag_handle.set_height(drag_handle_extension);
    return extend_drag_handle && !tab->IsActive() &&
           tab_drag_handle.Intersects(rect);
  }

  // Similarly, a hit in the new tab button is considered to be in the caption
  // if it's in this thin strip.
  gfx::Rect new_tab_button_drag_handle = new_tab_button_->GetMirroredBounds();
  new_tab_button_drag_handle.set_height(drag_handle_extension);
  if (extend_drag_handle && new_tab_button_drag_handle.Intersects(rect))
    return true;

  // Check to see if the rect intersects the non-button parts of the new tab
  // button. The button has a non-rectangular shape, so if it's not in the
  // visual portions of the button we treat it as a click to the caption.
  gfx::RectF rect_in_new_tab_coords_f(rect);
  View::ConvertRectToTarget(this, new_tab_button_, &rect_in_new_tab_coords_f);
  gfx::Rect rect_in_new_tab_coords =
      gfx::ToEnclosingRect(rect_in_new_tab_coords_f);
  return new_tab_button_->GetLocalBounds().Intersects(rect_in_new_tab_coords) &&
         !new_tab_button_->HitTestRect(rect_in_new_tab_coords);
}

bool TabStrip::IsPositionInWindowCaption(const gfx::Point& point) {
  return IsRectInWindowCaption(gfx::Rect(point, gfx::Size(1, 1)));
}

bool TabStrip::IsTabStripCloseable() const {
  return !drag_context_->IsDragSessionActive();
}

bool TabStrip::IsTabStripEditable() const {
  return !drag_context_->IsDragSessionActive() &&
         !drag_context_->IsActiveDropTarget();
}

bool TabStrip::IsTabCrashed(int tab_index) const {
  return tab_at(tab_index)->data().IsCrashed();
}

bool TabStrip::TabHasNetworkError(int tab_index) const {
  return tab_at(tab_index)->data().network_state == TabNetworkState::kError;
}

base::Optional<TabAlertState> TabStrip::GetTabAlertState(int tab_index) const {
  return tab_at(tab_index)->data().alert_state;
}

void TabStrip::UpdateLoadingAnimations(const base::TimeDelta& elapsed_time) {
  for (int i = 0; i < tab_count(); i++)
    tab_at(i)->StepLoadingAnimation(elapsed_time);
}

void TabStrip::SetStackedLayout(bool stacked_layout) {
  if (stacked_layout == stacked_layout_)
    return;

  stacked_layout_ = stacked_layout;
  SetResetToShrinkOnExit(false);
  SwapLayoutIfNecessary();

  // When transitioning to stacked try to keep the active tab from moving.
  const int active_index = controller_->GetActiveIndex();
  if (touch_layout_ && active_index != -1) {
    touch_layout_->SetActiveTabLocation(ideal_bounds(active_index).x());
    AnimateToIdealBounds();
  }

  for (int i = 0; i < tab_count(); ++i)
    tab_at(i)->Layout();
}

void TabStrip::AddTabAt(int model_index, TabRendererData data, bool is_active) {
  Tab* tab = new Tab(this);
  tab->set_context_menu_controller(&context_menu_controller_);
  tab->AddObserver(this);
  AddChildViewAt(tab, GetViewInsertionIndex(tab, base::nullopt, model_index));
  const bool pinned = data.pinned;
  tabs_.Add(tab, model_index);
  selected_tabs_.IncrementFrom(model_index);

  // Setting data must come after all state from the model has been updated
  // above for the tab. Accessibility, in particular, reacts to data changed
  // callbacks.
  tab->SetData(std::move(data));

  if (touch_layout_) {
    int add_types = 0;
    if (pinned)
      add_types |= StackedTabStripLayout::kAddTypePinned;
    if (is_active)
      add_types |= StackedTabStripLayout::kAddTypeActive;
    touch_layout_->AddTab(model_index, add_types,
                          UpdateIdealBoundsForPinnedTabs(nullptr));
  }

  // Don't animate the first tab, it looks weird, and don't animate anything
  // if the containing window isn't visible yet.
  if (tab_count() > 1 && GetWidget() && GetWidget()->IsVisible()) {
    StartInsertTabAnimation(model_index,
                            pinned ? TabPinned::kPinned : TabPinned::kUnpinned);
  } else {
    layout_helper_->InsertTabAtNoAnimation(
        model_index, tab,
        base::BindOnce(&TabStrip::OnTabCloseAnimationCompleted,
                       base::Unretained(this), base::Unretained(tab)),
        pinned ? TabPinned::kPinned : TabPinned::kUnpinned);
    CompleteAnimationAndLayout();
  }

  SwapLayoutIfNecessary();
  UpdateAccessibleTabIndices();

  for (TabStripObserver& observer : observers_)
    observer.OnTabAdded(model_index);

  // Stop dragging when a new tab is added and dragging a window. Doing
  // otherwise results in a confusing state if the user attempts to reattach. We
  // could allow this and make TabDragController update itself during the add,
  // but this comes up infrequently enough that it's not worth the complexity.
  //
  // At the start of AddTabAt() the model and tabs are out sync. Any queries to
  // find a tab given a model index can go off the end of |tabs_|. As such, it
  // is important that we complete the drag *after* adding the tab so that the
  // model and tabstrip are in sync.
  if (!drag_context_->IsMutating() && drag_context_->IsDraggingWindow())
    EndDrag(END_DRAG_COMPLETE);

  Profile* profile = controller()->GetProfile();
  if (profile) {
    if (profile->IsGuestSession())
      base::UmaHistogramCounts100("Tab.Count.Guest", tab_count());
    else if (profile->IsIncognitoProfile())
      base::UmaHistogramCounts100("Tab.Count.Incognito", tab_count());
  }
}

void TabStrip::MoveTab(int from_model_index,
                       int to_model_index,
                       TabRendererData data) {
  DCHECK_GT(tabs_.view_size(), 0);

  const Tab* last_tab = GetLastVisibleTab();

  Tab* moving_tab = tab_at(from_model_index);
  const bool pinned = data.pinned;
  moving_tab->SetData(std::move(data));

  ReorderChildView(
      moving_tab,
      GetViewInsertionIndex(moving_tab, from_model_index, to_model_index));

  if (touch_layout_) {
    tabs_.MoveViewOnly(from_model_index, to_model_index);
    int pinned_count = 0;
    const int start_x = UpdateIdealBoundsForPinnedTabs(&pinned_count);
    touch_layout_->MoveTab(from_model_index, to_model_index,
                           controller_->GetActiveIndex(), start_x,
                           pinned_count);
  } else {
    tabs_.Move(from_model_index, to_model_index);
  }
  selected_tabs_.Move(from_model_index, to_model_index, /*length=*/1);

  layout_helper_->MoveTab(moving_tab->group(), from_model_index,
                          to_model_index);
  layout_helper_->SetTabPinned(
      to_model_index, pinned ? TabPinned::kPinned : TabPinned::kUnpinned);
  StartMoveTabAnimation();
  if (TabDragController::IsAttachedTo(GetDragContext()) &&
      (last_tab != GetLastVisibleTab() || last_tab->dragging())) {
    new_tab_button_->SetVisible(false);
  }
  SwapLayoutIfNecessary();

  UpdateAccessibleTabIndices();

  for (TabStripObserver& observer : observers_)
    observer.OnTabMoved(from_model_index, to_model_index);
}

void TabStrip::RemoveTabAt(content::WebContents* contents,
                           int model_index,
                           bool was_active) {
  StartRemoveTabAnimation(model_index, was_active);

  SwapLayoutIfNecessary();

  UpdateAccessibleTabIndices();

  UpdateHoverCard(nullptr);

  for (TabStripObserver& observer : observers_)
    observer.OnTabRemoved(model_index);

  // Stop dragging when a new tab is removed and dragging a window. Doing
  // otherwise results in a confusing state if the user attempts to reattach. We
  // could allow this and make TabDragController update itself during the
  // remove operation, but this comes up infrequently enough that it's not worth
  // the complexity.
  //
  // At the start of RemoveTabAt() the model and tabs are out sync. Any queries
  // to find a tab given a model index can go off the end of |tabs_|. As such,
  // it is important that we complete the drag *after* removing the tab so that
  // the model and tabstrip are in sync.
  if (!drag_context_->IsMutating() && drag_context_->IsDraggingTab(contents))
    EndDrag(END_DRAG_COMPLETE);
}

void TabStrip::SetTabData(int model_index, TabRendererData data) {
  Tab* tab = tab_at(model_index);
  const bool pinned_state_changed = tab->data().pinned != data.pinned;
  tab->SetData(std::move(data));

  if (HoverCardIsShowingForTab(tab))
    UpdateHoverCard(tab);

  if (pinned_state_changed) {
    if (touch_layout_) {
      int pinned_tab_count = 0;
      int start_x = UpdateIdealBoundsForPinnedTabs(&pinned_tab_count);
      touch_layout_->SetXAndPinnedCount(start_x, pinned_tab_count);
    }

    layout_helper_->SetTabPinned(
        model_index, data.pinned ? TabPinned::kPinned : TabPinned::kUnpinned);
    if (GetWidget() && GetWidget()->IsVisible())
      StartPinnedTabAnimation();
    else
      CompleteAnimationAndLayout();
  }
  SwapLayoutIfNecessary();
}

void TabStrip::AddTabToGroup(base::Optional<tab_groups::TabGroupId> group,
                             int model_index) {
  tab_at(model_index)->set_group(std::move(group));
}

void TabStrip::OnGroupCreated(const tab_groups::TabGroupId& group) {
  std::unique_ptr<TabGroupViews> group_view =
      std::make_unique<TabGroupViews>(this, group);
  AddChildView(group_view->header());
  AddChildView(group_view->highlight());
  AddChildView(group_view->underline());
  layout_helper_->InsertGroupHeader(
      group, group_view->header(),
      base::BindOnce(&TabStrip::OnGroupCloseAnimationCompleted,
                     base::Unretained(this), group));
  group_views_[group] = std::move(group_view);
}

void TabStrip::OnGroupContentsChanged(const tab_groups::TabGroupId& group) {
  DCHECK(group_views_[group]);

  // The group header may be in the wrong place if the tab didn't actually
  // move in terms of model indices.
  OnGroupMoved(group);

  group_views_[group]->UpdateVisuals();
  UpdateIdealBounds();
  AnimateToIdealBounds();
}

void TabStrip::OnGroupVisualsChanged(const tab_groups::TabGroupId& group) {
  DCHECK(group_views_[group]);
  group_views_[group]->UpdateVisuals();
  // The group title may have changed size, so update bounds.
  UpdateIdealBounds();
  AnimateToIdealBounds();
}

void TabStrip::OnGroupMoved(const tab_groups::TabGroupId& group) {
  DCHECK(group_views_[group]);

  layout_helper_->UpdateGroupHeaderIndex(group);

  TabGroupHeader* group_header = group_views_[group]->header();
  const int first_tab = controller_->ListTabsInGroup(group).front();
  const int header_index = GetIndexOf(group_header);
  const int first_tab_index = GetIndexOf(tab_at(first_tab));

  // The header should be just before the first tab. If it isn't, reorder the
  // header such that it is. Note that the index to reorder to is different
  // depending on whether the header is before or after the tab, since the
  // header itself occupies an index.
  if (header_index < first_tab_index - 1)
    ReorderChildView(group_header, first_tab_index - 1);
  if (header_index > first_tab_index - 1)
    ReorderChildView(group_header, first_tab_index);
}

void TabStrip::OnGroupClosed(const tab_groups::TabGroupId& group) {
  bounds_animator_.StopAnimatingView(group_header(group));
  layout_helper_->RemoveGroupHeader(group);
  UpdateIdealBounds();
  AnimateToIdealBounds();
  // The group_views_ mapping is erased in OnGroupCloseAnimationCompleted().
}

void TabStrip::ShiftGroupLeft(const tab_groups::TabGroupId& group) {
  ShiftGroupRelative(group, -1);
}

void TabStrip::ShiftGroupRight(const tab_groups::TabGroupId& group) {
  ShiftGroupRelative(group, 1);
}

bool TabStrip::ShouldTabBeVisible(const Tab* tab) const {
  // Detached tabs should always be invisible (as they close).
  if (tab->detached())
    return false;

  // When stacking tabs, all tabs should always be visible.
  if (stacked_layout_)
    return true;

  // If the tab is currently clipped by the trailing edge of the strip, it
  // shouldn't be visible.
  const int right_edge = tab->bounds().right();
  const int tabstrip_right = tab->dragging()
                                 ? drag_context_->GetTabDragAreaWidth()
                                 : GetTabAreaWidth();
  if (right_edge > tabstrip_right)
    return false;

  // Non-clipped dragging tabs should always be visible.
  if (tab->dragging())
    return true;

  // Let all non-clipped closing tabs be visible.  These will probably finish
  // closing before the user changes the active tab, so there's little reason to
  // try and make the more complex logic below apply.
  if (tab->closing())
    return true;

  // Now we need to check whether the tab isn't currently clipped, but could
  // become clipped if we changed the active tab, widening either this tab or
  // the tabstrip portion before it.

  // Pinned tabs don't change size when activated, so any tab in the pinned tab
  // region is safe.
  if (tab->data().pinned)
    return true;

  // If the active tab is on or before this tab, we're safe.
  if (controller_->GetActiveIndex() <= GetModelIndexOf(tab))
    return true;

  // We need to check what would happen if the active tab were to move to this
  // tab or before.
  return (right_edge + GetActiveTabWidth() - GetInactiveTabWidth()) <=
         tabstrip_right;
}

bool TabStrip::ShouldDrawStrokes() const {
  // If the controller says we can't draw strokes, don't.
  if (!controller_->CanDrawStrokes())
    return false;

  // The tabstrip normally avoids strokes and relies on the active tab
  // contrasting sufficiently with the frame background.  When there isn't
  // enough contrast, fall back to a stroke.  Always compute the contrast ratio
  // against the active frame color, to avoid toggling the stroke on and off as
  // the window activation state changes.
  constexpr float kMinimumContrastRatioForOutlines = 1.3f;
  const SkColor background_color = GetTabBackgroundColor(
      TabActive::kActive, BrowserFrameActiveState::kActive);
  const SkColor frame_color =
      controller_->GetFrameColor(BrowserFrameActiveState::kActive);
  const float contrast_ratio =
      color_utils::GetContrastRatio(background_color, frame_color);
  if (contrast_ratio < kMinimumContrastRatioForOutlines)
    return true;

  // Don't want to have to run a full feature query every time this function is
  // called.
  static const bool tab_outlines_in_low_contrast =
      base::FeatureList::IsEnabled(features::kTabOutlinesInLowContrastThemes);
  if (tab_outlines_in_low_contrast) {
    constexpr float kMinimumAbsoluteContrastForOutlines = 0.2f;
    const float background_luminance =
        color_utils::GetRelativeLuminance(background_color);
    const float frame_luminance =
        color_utils::GetRelativeLuminance(frame_color);
    const float contrast_difference =
        std::fabs(background_luminance - frame_luminance);
    if (contrast_difference < kMinimumAbsoluteContrastForOutlines)
      return true;
  }

  return false;
}

void TabStrip::SetSelection(const ui::ListSelectionModel& new_selection) {
  if (selected_tabs_.active() != new_selection.active()) {
    if (selected_tabs_.active() >= 0)
      tab_at(selected_tabs_.active())->ActiveStateChanged();
    if (new_selection.active() >= 0)
      tab_at(new_selection.active())->ActiveStateChanged();
    layout_helper_->SetActiveTab(selected_tabs_.active(),
                                 new_selection.active());
  }

  if (touch_layout_) {
    touch_layout_->SetActiveIndex(new_selection.active());
    // Only start an animation if we need to. Otherwise clicking on an
    // unselected tab and dragging won't work because dragging is only allowed
    // if not animating.
    if (!views::ViewModelUtils::IsAtIdealBounds(tabs_))
      AnimateToIdealBounds();
    SchedulePaint();
  } else {
    if (GetActiveTabWidth() == GetInactiveTabWidth()) {
      // When tabs are wide enough, selecting a new tab cannot change the
      // ideal bounds, so only a repaint is necessary.
      SchedulePaint();
    } else if (bounds_animator_.IsAnimating()) {
      // The selection change will have modified the ideal bounds of the tabs
      // in |selected_tabs_| and |new_selection|.  We need to recompute.
      // Note: This is safe even if we're in the midst of mouse-based tab
      // closure--we won't expand the tabstrip back to the full window
      // width--because PrepareForCloseAt() will have set
      // |override_available_width_for_tabs_| already.
      UpdateIdealBounds();
      AnimateToIdealBounds();
    } else if (!layout_helper_->IsAnimating()) {
      // As in the animating case above, the selection change will have
      // affected the desired bounds of the tabs, but since we're not animating
      // we can just snap to the new bounds.
      CompleteAnimationAndLayout();
    }
  }

  // Use STLSetDifference to get the indices of elements newly selected
  // and no longer selected, since selected_indices() is always sorted.
  ui::ListSelectionModel::SelectedIndices no_longer_selected =
      base::STLSetDifference<ui::ListSelectionModel::SelectedIndices>(
          selected_tabs_.selected_indices(), new_selection.selected_indices());
  ui::ListSelectionModel::SelectedIndices newly_selected =
      base::STLSetDifference<ui::ListSelectionModel::SelectedIndices>(
          new_selection.selected_indices(), selected_tabs_.selected_indices());

  tab_at(new_selection.active())
      ->NotifyAccessibilityEvent(ax::mojom::Event::kSelection, true);
  selected_tabs_ = new_selection;

  UpdateHoverCard(nullptr);
  // The hover cards seen count is reset when the active tab is changed by any
  // event. Note TabStrip::SelectTab does not capture tab changes triggered by
  // the keyboard.
  if (base::FeatureList::IsEnabled(features::kTabHoverCards) && hover_card_)
    hover_card_->reset_hover_cards_seen_count();

  // Notify all tabs whose selected state changed.
  for (auto tab_index :
       base::STLSetUnion<ui::ListSelectionModel::SelectedIndices>(
           no_longer_selected, newly_selected)) {
    tab_at(tab_index)->SelectedStateChanged();
  }
}

void TabStrip::OnWidgetActivationChanged(views::Widget* widget, bool active) {
  if (active && selected_tabs_.active() >= 0) {
    // When the browser window is activated, fire a selection event on the
    // currently active tab, to help enable per-tab modes in assistive
    // technologies.
    tab_at(selected_tabs_.active())
        ->NotifyAccessibilityEvent(ax::mojom::Event::kSelection, true);
  }
}

void TabStrip::SetTabNeedsAttention(int model_index, bool attention) {
  tab_at(model_index)->SetTabNeedsAttention(attention);
}

int TabStrip::GetModelIndexOf(const TabSlotView* view) const {
  return tabs_.GetIndexOfView(view);
}

int TabStrip::GetModelCount() const {
  return controller_->GetCount();
}

bool TabStrip::IsValidModelIndex(int model_index) const {
  return controller_->IsValidIndex(model_index);
}

TabDragContext* TabStrip::GetDragContext() {
  return drag_context_.get();
}

int TabStrip::GetPinnedTabCount() const {
  return layout_helper_->GetPinnedTabCount();
}

bool TabStrip::IsAnimating() const {
  return bounds_animator_.IsAnimating() || layout_helper_->IsAnimating();
}

void TabStrip::StopAnimating(bool layout) {
  if (!IsAnimating())
    return;

  bounds_animator_.Cancel();
  layout_helper_->CompleteAnimations();

  if (layout)
    CompleteAnimationAndLayout();
}

base::Optional<int> TabStrip::GetFocusedTabIndex() const {
  for (int i = 0; i < tabs_.view_size(); ++i) {
    if (tabs_.view_at(i)->HasFocus())
      return i;
  }
  return base::nullopt;
}

const ui::ListSelectionModel& TabStrip::GetSelectionModel() const {
  return controller_->GetSelectionModel();
}

bool TabStrip::SupportsMultipleSelection() {
  // Currently we only allow single selection in touch layout mode.
  return touch_layout_ == nullptr;
}

bool TabStrip::ShouldHideCloseButtonForTab(Tab* tab) const {
  if (tab->IsActive())
    return false;
  return !!touch_layout_;
}

void TabStrip::SelectTab(Tab* tab, const ui::Event& event) {
  int model_index = GetModelIndexOf(tab);
  if (IsValidModelIndex(model_index)) {
    if (tab->group().has_value())
      base::RecordAction(base::UserMetricsAction("TabGroups_SwitchGroupedTab"));

    // Report histogram metrics for the number of tab hover cards seen before
    // a tab is selected by mouse press.
    if (base::FeatureList::IsEnabled(features::kTabHoverCards) && hover_card_ &&
        event.type() == ui::ET_MOUSE_PRESSED && !tab->IsActive()) {
      hover_card_->RecordHoverCardsSeenRatioMetric();
    }

    controller_->SelectTab(model_index, event);
  }
}

void TabStrip::ExtendSelectionTo(Tab* tab) {
  int model_index = GetModelIndexOf(tab);
  if (IsValidModelIndex(model_index))
    controller_->ExtendSelectionTo(model_index);
}

void TabStrip::ToggleSelected(Tab* tab) {
  int model_index = GetModelIndexOf(tab);
  if (IsValidModelIndex(model_index))
    controller_->ToggleSelected(model_index);
}

void TabStrip::AddSelectionFromAnchorTo(Tab* tab) {
  int model_index = GetModelIndexOf(tab);
  if (IsValidModelIndex(model_index))
    controller_->AddSelectionFromAnchorTo(model_index);
}

void TabStrip::CloseTab(Tab* tab, CloseTabSource source) {
  int model_index = GetModelIndexOf(tab);
  if (tab->closing()) {
    // If the tab is already closing, close the next tab. We do this so that the
    // user can rapidly close tabs by clicking the close button and not have
    // the animations interfere with that.
    std::vector<Tab*> all_tabs = layout_helper_->GetTabs();
    auto it = std::find(all_tabs.begin(), all_tabs.end(), tab);
    while (it < all_tabs.end() && (*it)->closing()) {
      it++;
    }

    model_index =
        it != all_tabs.end() ? GetModelIndexOf(*it) : TabStripModel::kNoTab;
  }

  if (!IsValidModelIndex(model_index))
    return;

  // If we're not allowed to close this tab for whatever reason, we should not
  // proceed.
  if (!controller_->BeforeCloseTab(model_index, source))
    return;

  if (!in_tab_close_ && IsAnimating()) {
    // Cancel any current animations. We do this as remove uses the current
    // ideal bounds and we need to know ideal bounds is in a good state.
    StopAnimating(true);
  }

  if (GetWidget()) {
    in_tab_close_ = true;
    resize_layout_timer_.Stop();
    if (source == CLOSE_TAB_FROM_TOUCH)
      StartResizeLayoutTabsFromTouchTimer();
    else
      AddMessageLoopObserver();
  }

  UpdateHoverCard(nullptr);
  if (tab->group().has_value())
    base::RecordAction(base::UserMetricsAction("CloseGroupedTab"));
  controller_->CloseTab(model_index, source);
}

void TabStrip::ShiftTabLeft(Tab* tab) {
  ShiftTabRelative(tab, ShiftOffset::kLeft);
}

void TabStrip::ShiftTabRight(Tab* tab) {
  ShiftTabRelative(tab, ShiftOffset::kRight);
}

void TabStrip::MoveTabFirst(Tab* tab) {
  if (tab->closing())
    return;

  const int start_index = GetModelIndexOf(tab);
  if (!IsValidModelIndex(start_index))
    return;

  int target_index = 0;
  if (!controller_->IsTabPinned(start_index)) {
    while (target_index < start_index && controller_->IsTabPinned(target_index))
      ++target_index;
  }

  if (!IsValidModelIndex(target_index))
    return;

  if (target_index == start_index) {
    // Even if the tab is already at the front, it may be able to "move" out of
    // its current group.
    if (tab->group().has_value())
      controller_->RemoveTabFromGroup(target_index);
    return;
  }

  controller_->MoveTab(start_index, target_index);

  // The tab may unintentionally land in the first group in the tab strip, so we
  // remove the group to ensure consistent behavior.
  if (tab->group().has_value())
    controller_->RemoveTabFromGroup(target_index);
}

void TabStrip::MoveTabLast(Tab* tab) {
  if (tab->closing())
    return;

  const int start_index = GetModelIndexOf(tab);
  if (!IsValidModelIndex(start_index))
    return;

  int target_index;
  if (controller_->IsTabPinned(start_index)) {
    int temp_index = start_index + 1;
    while (temp_index < tab_count() && controller_->IsTabPinned(temp_index))
      ++temp_index;
    target_index = temp_index - 1;
  } else {
    target_index = tab_count() - 1;
  }

  if (!IsValidModelIndex(target_index))
    return;

  if (target_index == start_index) {
    // Even if the tab is already at the back, it may be able to "move" out of
    // its current group.
    if (tab->group().has_value())
      controller_->RemoveTabFromGroup(target_index);
    return;
  }

  controller_->MoveTab(start_index, target_index);

  // The tab may unintentionally land in the last group in the tab strip, so we
  // remove the group to ensure consistent behavior.
  if (tab->group().has_value())
    controller_->RemoveTabFromGroup(target_index);
}

void TabStrip::ShowContextMenuForTab(Tab* tab,
                                     const gfx::Point& p,
                                     ui::MenuSourceType source_type) {
  controller_->ShowContextMenuForTab(tab, p, source_type);
}

bool TabStrip::IsActiveTab(const Tab* tab) const {
  int model_index = GetModelIndexOf(tab);
  return IsValidModelIndex(model_index) &&
         controller_->IsActiveTab(model_index);
}

bool TabStrip::IsTabSelected(const Tab* tab) const {
  int model_index = GetModelIndexOf(tab);
  return IsValidModelIndex(model_index) &&
         controller_->IsTabSelected(model_index);
}

bool TabStrip::IsTabPinned(const Tab* tab) const {
  if (tab->closing())
    return false;

  int model_index = GetModelIndexOf(tab);
  return IsValidModelIndex(model_index) &&
         controller_->IsTabPinned(model_index);
}

bool TabStrip::IsFirstVisibleTab(const Tab* tab) const {
  return GetModelIndexOf(tab) == 0;
}

bool TabStrip::IsLastVisibleTab(const Tab* tab) const {
  return GetLastVisibleTab() == tab;
}

bool TabStrip::IsFocusInTabs() const {
  return GetFocusManager() && Contains(GetFocusManager()->GetFocusedView()) &&
         GetFocusManager()->GetFocusedView() != new_tab_button_;
}

void TabStrip::MaybeStartDrag(
    TabSlotView* source,
    const ui::LocatedEvent& event,
    const ui::ListSelectionModel& original_selection) {
  // Don't accidentally start any drag operations during animations if the
  // mouse is down... during an animation tabs are being resized automatically,
  // so the View system can misinterpret this easily if the mouse is down that
  // the user is dragging.
  if (IsAnimating() || controller_->HasAvailableDragActions() == 0)
    return;

  // Check that the source is either a valid tab or a tab group header, which
  // are the only valid drag targets.
  if (!IsValidModelIndex(GetModelIndexOf(source))) {
    DCHECK_EQ(source->GetTabSlotViewType(),
              TabSlotView::ViewType::kTabGroupHeader);
  }

  drag_context_->MaybeStartDrag(source, event, original_selection);
}

void TabStrip::ContinueDrag(views::View* view, const ui::LocatedEvent& event) {
  drag_context_->ContinueDrag(view, event);
}

bool TabStrip::EndDrag(EndDragReason reason) {
  return drag_context_->EndDrag(reason);
}

Tab* TabStrip::GetTabAt(const gfx::Point& point) {
  views::View* view = GetEventHandlerForPoint(point);
  if (!view)
    return nullptr;  // No tab contains the point.

  // Walk up the view hierarchy until we find a tab, or the TabStrip.
  while (view && view != this && view->GetID() != VIEW_ID_TAB)
    view = view->parent();

  return view && view->GetID() == VIEW_ID_TAB ? static_cast<Tab*>(view)
                                              : nullptr;
}

const Tab* TabStrip::GetAdjacentTab(const Tab* tab, int offset) {
  int index = GetModelIndexOf(tab);
  if (index < 0)
    return nullptr;
  index += offset;
  return IsValidModelIndex(index) ? tab_at(index) : nullptr;
}

void TabStrip::OnMouseEventInTab(views::View* source,
                                 const ui::MouseEvent& event) {
  // Record time from cursor entering the tabstrip to first tap on a tab to
  // switch.
  if (mouse_entered_tabstrip_time_.has_value() &&
      event.type() == ui::ET_MOUSE_PRESSED &&
      !strcmp(source->GetClassName(), Tab::kViewClassName)) {
    UMA_HISTOGRAM_MEDIUM_TIMES(
        "TabStrip.TimeToSwitch",
        base::TimeTicks::Now() - mouse_entered_tabstrip_time_.value());
    mouse_entered_tabstrip_time_.reset();
  }
  UpdateStackedLayoutFromMouseEvent(source, event);
}

void TabStrip::UpdateHoverCard(Tab* tab) {
  if (!base::FeatureList::IsEnabled(features::kTabHoverCards))
    return;
  // We don't want to show a hover card for a tab while it is animating.
  if (tab &&
      (bounds_animator_.IsAnimating(tab) || layout_helper_->IsAnimating())) {
    return;
  }

  if (!hover_card_) {
    // There is nothing to be done if the hover card doesn't exist and we are
    // not trying to show it.
    if (!tab)
      return;
    hover_card_ = new TabHoverCardBubbleView(tab);
    hover_card_observer_.Add(hover_card_);
    if (GetWidget()) {
      hover_card_event_sniffer_ =
          std::make_unique<TabHoverCardEventSniffer>(hover_card_, this);
    }
  }
  if (tab)
    hover_card_->UpdateAndShow(tab);
  else
    hover_card_->FadeOutToHide();
}

bool TabStrip::HoverCardIsShowingForTab(Tab* tab) {
  if (!base::FeatureList::IsEnabled(features::kTabHoverCards))
    return false;

  return hover_card_ && hover_card_->GetWidget()->IsVisible() &&
         !hover_card_->IsFadingOut() &&
         hover_card_->GetDesiredAnchorView() == tab;
}

int TabStrip::GetBackgroundOffset() const {
  return background_offset_;
}

int TabStrip::GetStrokeThickness() const {
  return ShouldDrawStrokes() ? 1 : 0;
}

bool TabStrip::CanPaintThrobberToLayer() const {
  // Disable layer-painting of throbbers if dragging, if any tab animation is in
  // progress, or if stacked tabs are enabled. Also disable in fullscreen: when
  // "immersive" the tab strip could be sliding in or out; for other modes,
  // there's no tab strip.
  const bool dragging = drag_context_->IsDragStarted();
  const views::Widget* widget = GetWidget();
  return widget && !touch_layout_ && !dragging && !IsAnimating() &&
         !widget->IsFullscreen();
}

bool TabStrip::HasVisibleBackgroundTabShapes() const {
  return controller_->HasVisibleBackgroundTabShapes();
}

bool TabStrip::ShouldPaintAsActiveFrame() const {
  return controller_->ShouldPaintAsActiveFrame();
}

SkColor TabStrip::GetToolbarTopSeparatorColor() const {
  return controller_->GetToolbarTopSeparatorColor();
}

SkColor TabStrip::GetTabSeparatorColor() const {
  return separator_color_;
}

SkColor TabStrip::GetTabBackgroundColor(
    TabActive active,
    BrowserFrameActiveState active_state) const {
  const ui::ThemeProvider* tp = GetThemeProvider();
  if (!tp)
    return SK_ColorBLACK;

  if (active == TabActive::kActive)
    return tp->GetColor(ThemeProperties::COLOR_TOOLBAR);

  bool is_active_frame;
  if (active_state == BrowserFrameActiveState::kUseCurrent)
    is_active_frame = ShouldPaintAsActiveFrame();
  else
    is_active_frame = active_state == BrowserFrameActiveState::kActive;

  const int color_id = is_active_frame
                           ? ThemeProperties::COLOR_BACKGROUND_TAB
                           : ThemeProperties::COLOR_BACKGROUND_TAB_INACTIVE;
  // When the background tab color has not been customized, use the actual frame
  // color instead of COLOR_BACKGROUND_TAB.
  const SkColor frame = controller_->GetFrameColor(active_state);
  const SkColor background =
      tp->HasCustomColor(color_id)
          ? tp->GetColor(color_id)
          : color_utils::HSLShift(
                frame, tp->GetTint(ThemeProperties::TINT_BACKGROUND_TAB));

  return color_utils::GetResultingPaintColor(background, frame);
}

SkColor TabStrip::GetTabForegroundColor(TabActive active,
                                        SkColor background_color) const {
  const ui::ThemeProvider* tp = GetThemeProvider();
  if (!tp)
    return SK_ColorBLACK;

  const bool is_active_frame = ShouldPaintAsActiveFrame();

  // This color varies based on the tab and frame active states.
  int color_id = ThemeProperties::COLOR_TAB_TEXT;
  if (active != TabActive::kActive) {
    color_id = is_active_frame
                   ? ThemeProperties::COLOR_BACKGROUND_TAB_TEXT
                   : ThemeProperties::COLOR_BACKGROUND_TAB_TEXT_INACTIVE;
  }
  SkColor color = tp->GetColor(color_id);
  if (tp->HasCustomColor(color_id))
    return color;
  if ((color_id == ThemeProperties::COLOR_BACKGROUND_TAB_TEXT_INACTIVE) &&
      tp->HasCustomColor(ThemeProperties::COLOR_BACKGROUND_TAB_TEXT)) {
    // If a custom theme sets a background tab text color for active but not
    // inactive windows, generate the inactive color by blending the active one
    // at 75% as we do in the default theme.
    color = tp->GetColor(ThemeProperties::COLOR_BACKGROUND_TAB_TEXT);
  }

  if (!is_active_frame)
    color = color_utils::AlphaBlend(color, background_color, 0.75f);

  // To minimize any readability cost of custom system frame colors, try to make
  // the text reach the same contrast ratio that it would in the default theme.
  const SkColor target = color_utils::GetColorWithMaxContrast(background_color);
  // These contrast ratios should match the actual ratios in the default theme
  // colors when no system colors are involved, except for the inactive tab/
  // inactive frame case, which has been raised from 4.48 to 4.5 to meet
  // accessibility guidelines.
  constexpr float kContrast[2][2] = {{5.0f,     // Active tab, inactive frame
                                      10.46f},  // Active tab, active frame
                                     {4.5f,     // Inactive tab, inactive frame
                                      7.98f}};  // Inactive tab, active frame
  const float contrast =
      kContrast[active == TabActive::kActive ? 0 : 1][is_active_frame];
  return color_utils::BlendForMinContrast(color, background_color, target,
                                          contrast)
      .color;
}

// Returns the accessible tab name for the tab.
base::string16 TabStrip::GetAccessibleTabName(const Tab* tab) const {
  const int model_index = GetModelIndexOf(tab);
  return IsValidModelIndex(model_index) ? controller_->GetAccessibleTabName(tab)
                                        : base::string16();
}

base::Optional<int> TabStrip::GetCustomBackgroundId(
    BrowserFrameActiveState active_state) const {
  if (!TitlebarBackgroundIsTransparent())
    return controller_->GetCustomBackgroundId(active_state);

  constexpr int kBackgroundIdGlass = IDR_THEME_TAB_BACKGROUND_V;
  return GetThemeProvider()->HasCustomImage(kBackgroundIdGlass)
             ? base::make_optional(kBackgroundIdGlass)
             : base::nullopt;
}

gfx::Rect TabStrip::GetTabAnimationTargetBounds(const Tab* tab) {
  return bounds_animator_.GetTargetBounds(tab);
}

void TabStrip::MouseMovedOutOfHost() {
  ResizeLayoutTabs();
  if (reset_to_shrink_on_exit_) {
    reset_to_shrink_on_exit_ = false;
    SetStackedLayout(false);
    controller_->StackedLayoutMaybeChanged();
  }
}

float TabStrip::GetHoverOpacityForTab(float range_parameter) const {
  return gfx::Tween::FloatValueBetween(range_parameter, hover_opacity_min_,
                                       hover_opacity_max_);
}

float TabStrip::GetHoverOpacityForRadialHighlight() const {
  return radial_highlight_opacity_;
}

base::string16 TabStrip::GetGroupTitle(
    const tab_groups::TabGroupId& group) const {
  return controller_->GetGroupTitle(group);
}

tab_groups::TabGroupColorId TabStrip::GetGroupColorId(
    const tab_groups::TabGroupId& group) const {
  return controller_->GetGroupColorId(group);
}

SkColor TabStrip::GetPaintedGroupColor(
    const tab_groups::TabGroupColorId& color_id) const {
  const tab_groups::TabGroupColor color_data =
      tab_groups::GetTabGroupColorSet().at(color_id);

  const SkColor background = GetTabBackgroundColor(
      TabActive::kInactive, BrowserFrameActiveState::kUseCurrent);

  return color_utils::IsDark(background) ? color_data.dark_theme_color
                                         : color_data.light_theme_color;
}

void TabStrip::SetVisualDataForGroup(
    const tab_groups::TabGroupId& group,
    const tab_groups::TabGroupVisualData& visual_data) {
  controller_->SetVisualDataForGroup(group, visual_data);
}

void TabStrip::CloseAllTabsInGroup(const tab_groups::TabGroupId& group) {
  UpdateHoverCard(nullptr);

  std::vector<int> tabs = controller_->ListTabsInGroup(group);
  for (int i = tabs.size() - 1; i >= 0; i--) {
    controller_->CloseTab(tabs[i], CLOSE_TAB_FROM_MOUSE);
  }
}

void TabStrip::UngroupAllTabsInGroup(const tab_groups::TabGroupId& group) {
  UpdateHoverCard(nullptr);
  controller_->UngroupAllTabsInGroup(group);
}

void TabStrip::AddNewTabInGroup(const tab_groups::TabGroupId& group) {
  controller_->AddNewTabInGroup(group);
}

const Browser* TabStrip::GetBrowser() {
  return controller_->GetBrowser();
}
///////////////////////////////////////////////////////////////////////////////
// TabStrip, views::AccessiblePaneView overrides:

void TabStrip::Layout() {
  // Only do a layout if our size changed.
  if (last_layout_size_ == size())
    return;
  if (drag_context_->IsDragSessionActive())
    return;
  CompleteAnimationAndLayout();
}

void TabStrip::PaintChildren(const views::PaintInfo& paint_info) {
  // This is used to log to UMA. NO EARLY RETURNS!
  base::ElapsedTimer paint_timer;

  // The view order doesn't match the paint order (layout_helper_ contains the
  // view ordering).
  bool is_dragging = false;
  Tab* active_tab = nullptr;
  std::vector<Tab*> tabs_dragging;
  std::vector<Tab*> selected_and_hovered_tabs;

  // When background tab shapes are visible, as for hovered or selected tabs,
  // the paint order must be handled carefully to avoid Z-order errors, so
  // this code defers drawing such tabs until later.
  const auto paint_or_add_to_tabs = [&paint_info,
                                     &selected_and_hovered_tabs](Tab* tab) {
    if (tab->tab_style()->GetZValue() > 0.0) {
      selected_and_hovered_tabs.push_back(tab);
    } else {
      tab->Paint(paint_info);
    }
  };

  std::vector<Tab*> all_tabs = layout_helper_->GetTabs();

  int active_tab_index = -1;
  for (int i = all_tabs.size() - 1; i >= 0; --i) {
    Tab* tab = all_tabs[i];
    if (tab->dragging() && !stacked_layout_) {
      is_dragging = true;
      if (tab->IsActive()) {
        active_tab = tab;
        active_tab_index = i;
      } else {
        tabs_dragging.push_back(tab);
      }
    } else if (tab->IsActive()) {
      active_tab = tab;
      active_tab_index = i;
    } else if (!stacked_layout_) {
      paint_or_add_to_tabs(tab);
    }
  }

  // Draw from the left and then the right if we're in touch mode.
  if (stacked_layout_ && active_tab_index >= 0) {
    for (int i = 0; i < active_tab_index; ++i) {
      Tab* tab = all_tabs[i];
      tab->Paint(paint_info);
    }

    for (int i = all_tabs.size() - 1; i > active_tab_index; --i) {
      Tab* tab = all_tabs[i];
      tab->Paint(paint_info);
    }
  }

  std::stable_sort(selected_and_hovered_tabs.begin(),
                   selected_and_hovered_tabs.end(), [](Tab* tab1, Tab* tab2) {
                     return tab1->tab_style()->GetZValue() <
                            tab2->tab_style()->GetZValue();
                   });
  for (Tab* tab : selected_and_hovered_tabs)
    tab->Paint(paint_info);

  // Keep track of the dragging group if dragging by the group header, or
  // the current group if just dragging tabs into a group. At most one of these
  // will have a value, since a drag is either a group drag or a tab drag.
  base::Optional<tab_groups::TabGroupId> dragging_group = base::nullopt;
  base::Optional<tab_groups::TabGroupId> current_group = base::nullopt;

  // Paint group headers and underlines.
  for (const auto& group_view_pair : group_views_) {
    if (group_view_pair.second->header()->dragging()) {
      // If the whole group is dragging, defer painting both the header and the
      // underline, since they should appear above non-dragging tabs and groups.
      // Instead, just track the dragging group.
      dragging_group = group_view_pair.first;
    } else {
      group_view_pair.second->header()->Paint(paint_info);

      if (tabs_dragging.size() > 0 &&
          tabs_dragging[0]->group() == group_view_pair.first) {
        // If tabs are being dragged into a group, defer painting just the
        // underline, which should appear above non-active dragging tabs as well
        // as all non-dragging tabs and groups. Instead, just track the group
        // that the tabs are being dragged into.
        current_group = group_view_pair.first;
      } else {
        group_view_pair.second->underline()->Paint(paint_info);
      }
    }
  }

  // Always paint the active tab over all the inactive tabs.
  if (active_tab && !is_dragging)
    active_tab->Paint(paint_info);

  // Paint the New Tab button, unless it's being painted onto its own Layer.
  if (!new_tab_button_->layer())
    new_tab_button_->Paint(paint_info);

  // If dragging a group, paint the group highlight and header above all
  // non-dragging tabs and groups.
  if (dragging_group.has_value()) {
    group_views_[dragging_group.value()]->highlight()->Paint(paint_info);
    group_views_[dragging_group.value()]->header()->Paint(paint_info);
  }

  // Paint the dragged tabs.
  for (size_t i = 0; i < tabs_dragging.size(); ++i)
    tabs_dragging[i]->Paint(paint_info);

  // If dragging a group, or dragging tabs into a group, paint the group
  // underline above the dragging tabs. Otherwise, any non-active dragging tabs
  // will not get an underline.
  if (dragging_group.has_value())
    group_views_[dragging_group.value()]->underline()->Paint(paint_info);
  if (current_group.has_value())
    group_views_[current_group.value()]->underline()->Paint(paint_info);

  // If the active tab is being dragged, it goes last.
  if (active_tab && is_dragging)
    active_tab->Paint(paint_info);

  UMA_HISTOGRAM_CUSTOM_MICROSECONDS_TIMES(
      "TabStrip.PaintChildrenDuration", paint_timer.Elapsed(),
      base::TimeDelta::FromMicroseconds(1),
      base::TimeDelta::FromMicroseconds(10000), 50);
}

const char* TabStrip::GetClassName() const {
  static const char kViewClassName[] = "TabStrip";
  return kViewClassName;
}

gfx::Size TabStrip::CalculatePreferredSize() const {
  int needed_tab_width;
  if (touch_layout_ || adjust_layout_) {
    // For stacked tabs the minimum size is calculated as the size needed to
    // handle showing any number of tabs.
    needed_tab_width =
        GetStackableTabWidth() + (2 * kStackedPadding * kMaxStackedCount);
  } else {
    // Otherwise the minimum width is based on the actual number of tabs.
    const int pinned_tab_count = GetPinnedTabCount();
    needed_tab_width = pinned_tab_count * TabStyle::GetPinnedWidth();
    const int remaining_tab_count = tab_count() - pinned_tab_count;
    const int min_selected_width = TabStyleViews::GetMinimumActiveWidth();
    const int min_unselected_width = TabStyleViews::GetMinimumInactiveWidth();
    if (remaining_tab_count > 0) {
      needed_tab_width += min_selected_width +
                          ((remaining_tab_count - 1) * min_unselected_width);
    }

    const int overlap = TabStyle::GetTabOverlap();
    if (tab_count() > 1)
      needed_tab_width -= (tab_count() - 1) * overlap;

    // Don't let the tabstrip shrink smaller than is necessary to show one tab,
    // and don't force it to be larger than is necessary to show 20 tabs.
    const int largest_min_tab_width =
        min_selected_width + 19 * (min_unselected_width - overlap);
    needed_tab_width = std::min(std::max(needed_tab_width, min_selected_width),
                                largest_min_tab_width);
  }
  return gfx::Size(needed_tab_width + TabToNewTabButtonSpacing() +
                       new_tab_button_ideal_bounds_.width() + FrameGrabWidth(),
                   GetLayoutConstant(TAB_HEIGHT));
}

void TabStrip::GetAccessibleNodeData(ui::AXNodeData* node_data) {
  node_data->role = ax::mojom::Role::kTabList;
}

views::View* TabStrip::GetTooltipHandlerForPoint(const gfx::Point& point) {
  if (!HitTestPoint(point))
    return nullptr;

  if (!touch_layout_) {
    // Return any view that isn't a Tab or this TabStrip immediately. We don't
    // want to interfere.
    views::View* v = View::GetTooltipHandlerForPoint(point);
    if (v && v != this && strcmp(v->GetClassName(), Tab::kViewClassName))
      return v;

    views::View* tab = FindTabHitByPoint(point);
    if (tab)
      return tab;
  } else {
    if (new_tab_button_->GetVisible()) {
      views::View* view =
          ConvertPointToViewAndGetTooltipHandler(this, new_tab_button_, point);
      if (view)
        return view;
    }
    Tab* tab = FindTabForEvent(point);
    if (tab)
      return ConvertPointToViewAndGetTooltipHandler(this, tab, point);
  }
  return this;
}

void TabStrip::OnThemeChanged() {
  FrameColorsChanged();
}

views::View* TabStrip::GetDefaultFocusableChild() {
  int active = controller_->GetActiveIndex();
  return active != TabStripModel::kNoTab
             ? tab_at(active)
             : AccessiblePaneView::GetDefaultFocusableChild();
}

BrowserRootView::DropIndex TabStrip::GetDropIndex(
    const ui::DropTargetEvent& event) {
  // Force animations to stop, otherwise it makes the index calculation tricky.
  StopAnimating(true);

  // If the UI layout is right-to-left, we need to mirror the mouse
  // coordinates since we calculate the drop index based on the
  // original (and therefore non-mirrored) positions of the tabs.
  const int x = GetMirroredXInView(event.x());
  for (int i = 0; i < tab_count(); ++i) {
    Tab* tab = tab_at(i);
    const int tab_max_x = tab->x() + tab->width();

    // When hovering over the left or right quarter of a tab, the drop indicator
    // will point between tabs.
    const int hot_width = tab->width() / 4;

    if (x < tab_max_x) {
      if (x >= (tab_max_x - hot_width))
        return {i + 1, true};
      return {i, x < tab->x() + hot_width};
    }
  }

  // The drop isn't over a tab, add it to the end.
  return {tab_count(), true};
}

views::View* TabStrip::GetViewForDrop() {
  return this;
}

void TabStrip::HandleDragUpdate(
    const base::Optional<BrowserRootView::DropIndex>& index) {
  SetDropArrow(index);
}

void TabStrip::HandleDragExited() {
  SetDropArrow({});
}

///////////////////////////////////////////////////////////////////////////////
// TabStrip, private:

void TabStrip::Init() {
  SetID(VIEW_ID_TAB_STRIP);
  // So we get enter/exit on children to switch stacked layout on and off.
  set_notify_enter_exit_on_child(true);

  new_tab_button_ = new NewTabButton(this, this);
  new_tab_button_->SetTooltipText(
      l10n_util::GetStringUTF16(IDS_TOOLTIP_NEW_TAB));
  new_tab_button_->SetAccessibleName(
      l10n_util::GetStringUTF16(IDS_ACCNAME_NEWTAB));
  new_tab_button_->SetImageVerticalAlignment(views::ImageButton::ALIGN_BOTTOM);
  new_tab_button_->SetEventTargeter(
      std::make_unique<views::ViewTargeter>(new_tab_button_));
  AddChildView(new_tab_button_);
  new_tab_button_->AddObserver(this);

  UpdateNewTabButtonBorder();
  new_tab_button_ideal_bounds_.set_size(new_tab_button_->GetPreferredSize());

  if (g_drop_indicator_width == 0) {
    // Direction doesn't matter, both images are the same size.
    gfx::ImageSkia* drop_image = GetDropArrowImage(true);
    g_drop_indicator_width = drop_image->width();
    g_drop_indicator_height = drop_image->height();
  }

  UpdateContrastRatioValues();

  if (!gfx::Animation::ShouldRenderRichAnimation())
    bounds_animator_.SetAnimationDuration(base::TimeDelta());
}

std::map<tab_groups::TabGroupId, TabGroupHeader*> TabStrip::GetGroupHeaders() {
  std::map<tab_groups::TabGroupId, TabGroupHeader*> group_headers;
  for (const auto& group_view_pair : group_views_) {
    group_headers.insert(std::make_pair(group_view_pair.first,
                                        group_view_pair.second->header()));
  }
  return group_headers;
}

void TabStrip::StartInsertTabAnimation(int model_index, TabPinned pinned) {
  if (!bounds_animator_.IsAnimating() && !in_tab_close_) {
    layout_helper_->InsertTabAt(
        model_index, tab_at(model_index),
        base::BindOnce(&TabStrip::OnTabCloseAnimationCompleted,
                       base::Unretained(this),
                       base::Unretained(tab_at(model_index))),
        pinned);
  } else {
    // TODO(958173): Delete this branch once |TabStripLayoutHelper::animator_|
    // has taken over all animation responsibilities.
    layout_helper_->InsertTabAtNoAnimation(
        model_index, tab_at(model_index),
        base::BindOnce(&TabStrip::OnTabCloseAnimationCompleted,
                       base::Unretained(this),
                       base::Unretained(tab_at(model_index))),
        pinned);

    PrepareForAnimation();

    ExitTabClosingMode();

    UpdateIdealBounds();

    // Insert the tab just after the current right edge of the previous tab, if
    // any.
    gfx::Rect bounds = ideal_bounds(model_index);
    const int tab_overlap = TabStyle::GetTabOverlap();
    if (model_index > 0)
      bounds.set_x(tab_at(model_index - 1)->bounds().right() - tab_overlap);

    // Start at the width of the overlap in order to animate at the same speed
    // the surrounding tabs are moving, since at this width the subsequent tab
    // is naturally positioned at the same X coordinate.
    bounds.set_width(tab_overlap);

    // Animate in to the full width.
    tab_at(model_index)->SetBoundsRect(bounds);
    AnimateToIdealBounds();
  }
}

void TabStrip::StartRemoveTabAnimation(int model_index, bool was_active) {
  if (bounds_animator_.IsAnimating() || touch_layout_ ||
      !base::FeatureList::IsEnabled(features::kNewTabstripAnimation)) {
    StartFallbackRemoveTabAnimation(model_index, was_active);
    return;
  }

  // The tab formerly at |model_index| has already been removed from the model,
  // so |model_count| won't include it.
  const int model_count = GetModelCount();

  // If |in_tab_close_| is true then we want to animate the remaining tabs
  // (if there are any) differently in order to ensure that a useful tab will
  // end up under the cursor.
  if (in_tab_close_ && model_count > 0) {
    const int available_width =
        override_available_width_for_tabs_.value_or(GetTabAreaWidth());
    layout_helper_->EnterTabClosingMode(available_width);
  } else {
    layout_helper_->ExitTabClosingMode();
  }

  Tab* tab = tab_at(model_index);
  tab->SetClosing(true);

  const int old_x = ideal_bounds(model_index).x();
  RemoveTabFromViewModel(model_index);

  if (touch_layout_) {
    touch_layout_->RemoveTab(model_index,
                             UpdateIdealBoundsForPinnedTabs(nullptr), old_x);
  }

  layout_helper_->RemoveTab(model_index, tab);
}

void TabStrip::StartFallbackRemoveTabAnimation(int model_index,
                                               bool was_active) {
  // TODO(958173): Delete this once all animations have been migrated to the
  // new style.
  const int model_count = GetModelCount();
  const int tab_overlap = TabStyle::GetTabOverlap();
  if (in_tab_close_ && model_count > 0 && model_index != model_count) {
    // The user closed a tab other than the last tab. Set
    // override_available_width_for_tabs_ so that as the user closes tabs with
    // the mouse a tab continues to fall under the mouse.
    int next_active_index = controller_->GetActiveIndex();
    DCHECK(IsValidModelIndex(next_active_index));
    if (model_index <= next_active_index) {
      // At this point, model's internal state has already been updated.
      // |contents| has been detached from model and the active index has been
      // updated. But the tab for |contents| isn't removed yet. Thus, we need to
      // fix up next_active_index based on it.
      next_active_index++;
    }
    Tab* next_active_tab = tab_at(next_active_index);
    Tab* tab_being_removed = tab_at(model_index);

    UpdateIdealBounds();
    int size_delta = tab_being_removed->width();
    if (!tab_being_removed->data().pinned && was_active &&
        GetActiveTabWidth() > GetInactiveTabWidth()) {
      // When removing an active, non-pinned tab, an inactive tab will be made
      // active and thus given the active width. Thus the width being removed
      // from the strip is really the current width of whichever inactive tab
      // will be made active.
      size_delta = next_active_tab->width();
    }

    override_available_width_for_tabs_ =
        ideal_bounds(model_count).right() - size_delta + tab_overlap;
  }

  if (!touch_layout_)
    PrepareForAnimation();

  Tab* tab = tab_at(model_index);
  tab->SetClosing(true);

  int old_x = tabs_.ideal_bounds(model_index).x();
  RemoveTabFromViewModel(model_index);

  if (touch_layout_) {
    touch_layout_->RemoveTab(model_index,
                             UpdateIdealBoundsForPinnedTabs(nullptr), old_x);
  }

  // Destroy any tabs that |layout_helper_| is animating closed.
  if (layout_helper_->IsAnimating())
    layout_helper_->CompleteAnimations();

  layout_helper_->RemoveTabNoAnimation(model_index, tab);
  UpdateIdealBounds();
  // Don't destroy the tab that we just started animating closed.
  AnimateToIdealBounds(ClosingTabsBehavior::kTransferOwnership);

  // TODO(pkasting): When closing multiple tabs, we get repeated RemoveTabAt()
  // calls, each of which closes a new tab and thus generates different ideal
  // bounds.  We should update the animations of any other tabs that are
  // currently being closed to reflect the new ideal bounds, or else change from
  // removing one tab at a time to animating the removal of all tabs at once.

  // Compute the target bounds for animating this tab closed.  The tab's left
  // edge should stay joined to the right edge of the previous tab, if any.
  gfx::Rect tab_bounds = tab->bounds();
  tab_bounds.set_x((model_index > 0)
                       ? (ideal_bounds(model_index - 1).right() - tab_overlap)
                       : 0);

  // The tab should animate to the width of the overlap in order to close at the
  // same speed the surrounding tabs are moving, since at this width the
  // subsequent tab is naturally positioned at the same X coordinate.
  tab_bounds.set_width(tab_overlap);

  // Animate the tab closed.
  bounds_animator_.AnimateViewTo(
      tab, tab_bounds, std::make_unique<RemoveTabDelegate>(this, tab));

  // TODO(pkasting): The first part of this conditional doesn't really make
  // sense to me.  Why is each condition justified?
  if ((touch_layout_ || !in_tab_close_ || model_index == GetModelCount()) &&
      TabDragController::IsAttachedTo(GetDragContext())) {
    // Don't animate the new tab button when dragging tabs. Otherwise it looks
    // like the new tab button magically appears from beyond the end of the tab
    // strip.
    bounds_animator_.StopAnimatingView(new_tab_button_);
    new_tab_button_->SetBoundsRect(new_tab_button_ideal_bounds_);
  }
}

void TabStrip::StartMoveTabAnimation() {
  PrepareForAnimation();
  UpdateIdealBounds();
  AnimateToIdealBounds();
}

void TabStrip::AnimateToIdealBounds(ClosingTabsBehavior closing_tabs_behavior) {
  UpdateHoverCard(nullptr);
  // |bounds_animator_| and |layout_helper_| should not run
  // concurrently. |bounds_animator_| takes precedence, and can finish what the
  // other started.
  if (layout_helper_->IsAnimating()) {
    // Complete animations so |layout_helper_| will be in sync with
    // ideal bounds instead of the current bounds.
    switch (closing_tabs_behavior) {
      case ClosingTabsBehavior::kTransferOwnership:
        layout_helper_->CompleteAnimationsWithoutDestroyingTabs();
        break;
      case ClosingTabsBehavior::kDestroy:
        layout_helper_->CompleteAnimations();
        break;
    }
    // Take over tab closing mode now so that |bounds_animator| can use
    // |layout_helper_| to calculate ideal bounds without interference.
    override_available_width_for_tabs_ = layout_helper_->ExitTabClosingMode();
  }

  for (int i = 0; i < tab_count(); ++i) {
    // If the tab is being dragged manually, skip it.
    Tab* tab = tab_at(i);
    if (tab->dragging() && !bounds_animator_.IsAnimating(tab))
      continue;

    // Also skip tabs already being animated to the same ideal bounds.  Calling
    // AnimateViewTo() again restarts the animation, which changes the timing of
    // how the tab animates, leading to hitches.
    const gfx::Rect& target_bounds = ideal_bounds(i);
    if (bounds_animator_.GetTargetBounds(tab) == target_bounds)
      continue;

    // Set an animation delegate for the tab so it will clip appropriately.
    // Don't do this if dragging() is true.  In this case the tab was
    // previously being dragged and is now animating back to its ideal
    // bounds; it already has an associated ResetDraggingStateDelegate that
    // will reset this dragging state. Replacing this delegate would mean
    // this code would also need to reset the dragging state immediately,
    // and that could allow the new tab button to be drawn atop this tab.
    bounds_animator_.AnimateViewTo(
        tab, target_bounds,
        tab->dragging() ? nullptr
                        : std::make_unique<TabAnimationDelegate>(this, tab));
  }

  for (const auto& header_pair : group_views_) {
    bounds_animator_.AnimateViewTo(
        header_pair.second->header(),
        layout_helper_->group_header_ideal_bounds().at(header_pair.first));
  }

  if (bounds_animator_.GetTargetBounds(new_tab_button_) !=
      new_tab_button_ideal_bounds_) {
    bounds_animator_.AnimateViewTo(new_tab_button_,
                                   new_tab_button_ideal_bounds_);
  }
}

void TabStrip::ExitTabClosingMode() {
  in_tab_close_ = false;
  override_available_width_for_tabs_.reset();
  layout_helper_->ExitTabClosingMode();
}

bool TabStrip::ShouldHighlightCloseButtonAfterRemove() {
  return in_tab_close_;
}

int TabStrip::TabToNewTabButtonSpacing() const {
  // The new tab button contains built-in padding, and should be placed flush
  // against the trailing separator.
  return -TabStyle::GetTabInternalPadding().right();
}

int TabStrip::FrameGrabWidth() const {
  // The grab area is adjacent to the new tab button.  Treat the padding in the
  // new tab button as part of the grab area.
  constexpr int kApparentWidth = 50;
  return kApparentWidth - new_tab_button_->GetInsets().right();
}

bool TabStrip::TitlebarBackgroundIsTransparent() const {
#if defined(OS_WIN)
  // Windows 8+ uses transparent window contents (because the titlebar area is
  // drawn by the system and not Chrome), but the actual titlebar is opaque.
  if (base::win::GetVersion() >= base::win::Version::WIN8)
    return false;
#endif
  return GetWidget()->ShouldWindowContentsBeTransparent();
}

void TabStrip::CompleteAnimationAndLayout() {
  ExitTabClosingMode();

  // If |bounds_animator_| is running, it owns destroying tabs when close
  // animations complete.  Otherwise, |layout_helper_| does.
  if (bounds_animator_.IsAnimating()) {
    layout_helper_->CompleteAnimationsWithoutDestroyingTabs();
  } else {
    layout_helper_->CompleteAnimations();
  }
  LayoutToCurrentBounds();

  UpdateIdealBounds();
}

void TabStrip::LayoutToCurrentBounds() {
  last_layout_size_ = size();

  bounds_animator_.Cancel();

  SwapLayoutIfNecessary();
  if (touch_layout_) {
    touch_layout_->SetWidth(GetTabAreaWidth());
    UpdateIdealBounds();

    views::ViewModelUtils::SetViewBoundsToIdealBounds(tabs_);
    bounds_animator_.StopAnimatingView(new_tab_button_);
    new_tab_button_->SetBoundsRect(new_tab_button_ideal_bounds_);
  } else {
    const int available_width =
        override_available_width_for_tabs_.value_or(GetTabAreaWidth());
    int trailing_x = layout_helper_->LayoutTabs(available_width);

    gfx::Rect new_tab_button_current_bounds = new_tab_button_ideal_bounds_;
    new_tab_button_current_bounds.set_origin(gfx::Point(
        std::min(available_width, trailing_x) + TabToNewTabButtonSpacing(), 0));
    new_tab_button_->SetBoundsRect(new_tab_button_current_bounds);
  }

  SetTabVisibility();

  SchedulePaint();
}

void TabStrip::SetTabVisibility() {
  // We could probably be more efficient here by making use of the fact that the
  // tabstrip will always have any visible tabs, and then any invisible tabs, so
  // we could e.g. binary-search for the changeover point.  But since we have to
  // iterate through all the tabs to call SetVisible() anyway, it doesn't seem
  // worth it.
  for (Tab* tab : layout_helper_->GetTabs())
    tab->SetVisible(ShouldTabBeVisible(tab));
}

void TabStrip::UpdateAccessibleTabIndices() {
  const int num_tabs = tab_count();
  for (int i = 0; i < num_tabs; ++i)
    tab_at(i)->GetViewAccessibility().OverridePosInSet(i + 1, num_tabs);
}

int TabStrip::GetActiveTabWidth() const {
  return layout_helper_->active_tab_width();
}

int TabStrip::GetInactiveTabWidth() const {
  return layout_helper_->inactive_tab_width();
}

int TabStrip::GetTabAreaWidth() const {
  return drag_context_->GetTabDragAreaWidth() -
         new_tab_button_ideal_bounds_.width() - TabToNewTabButtonSpacing();
}

int TabStrip::GetNewTabButtonIdealX() const {
  const int trailing_x = tabs_.ideal_bounds(tab_count() - 1).right();
  // For non-stacked tabs the ideal bounds may go outside the bounds of the
  // tabstrip. Constrain the x-coordinate of the new tab button so that it is
  // always visible.
  return std::min(GetTabAreaWidth(), trailing_x) + TabToNewTabButtonSpacing();
}

const Tab* TabStrip::GetLastVisibleTab() const {
  for (int i = tab_count() - 1; i >= 0; --i) {
    const Tab* tab = tab_at(i);
    if (tab->GetVisible())
      return tab;
  }
  // While in normal use the tabstrip should always be wide enough to have at
  // least one visible tab, it can be zero-width in tests, meaning we get here.
  return nullptr;
}

int TabStrip::GetViewInsertionIndex(Tab* tab,
                                    base::Optional<int> from_model_index,
                                    int to_model_index) const {
  // If to_model_index is beyond the end of the tab strip, then the tab is newly
  // added to the end of the tab strip. In that case we can just return the last
  // TabSlotView view index, which should be at the sum of the number of tabs
  // and groups.
  if (to_model_index >= tab_count())
    return tab_count() + group_views_.size();

  // If there is no from_model_index, then the tab is newly added in the middle
  // of the tab strip. In that case we treat it as coming from the end of the
  // tab strip, since new views are ordered at the end by default.
  if (!from_model_index.has_value())
    from_model_index = tab_count();

  DCHECK_NE(to_model_index, from_model_index.value());

  // Since we don't have an absolute mapping from model index to view index, we
  // anchor on the last known view index at the given to_model_index.
  Tab* other_tab = tab_at(to_model_index);
  int other_view_index = GetIndexOf(other_tab);

  // When moving to the right, just use the anchor index because the tab will
  // replace that position in both the model and the view. This happens because
  // the tab itself occupies a lower index that the other tabs will shift into.
  if (to_model_index > from_model_index.value())
    return other_view_index;

  // When moving to the left, the tab may end up on either the left or right
  // side of a group header, depending on if it's in that group. This affects
  // its view index but not its model index, so we adjust the former only.
  if (other_tab->group().has_value() && other_tab->group() != tab->group())
    return other_view_index - 1;

  return other_view_index;
}

void TabStrip::RemoveTabFromViewModel(int index) {
  Tab* closing_tab = tab_at(index);
  bool closing_tab_was_active = closing_tab->IsActive();

  UpdateHoverCard(nullptr);

  // We still need to keep the tab alive until the remove tab animation
  // completes. Defer destroying it until then.
  tabs_.Remove(index);
  selected_tabs_.DecrementFrom(index);

  if (closing_tab_was_active)
    closing_tab->ActiveStateChanged();
}

void TabStrip::OnTabCloseAnimationCompleted(Tab* tab) {
  DCHECK(tab->closing());

  std::unique_ptr<Tab> deleter(tab);
  layout_helper_->OnTabDestroyed(tab);

  // Send the Container a message to simulate a mouse moved event at the current
  // mouse position. This tickles the Tab the mouse is currently over to show
  // the "hot" state of the close button.  Note that this is not required (and
  // indeed may crash!) for removes spawned by non-mouse closes and
  // drag-detaches.
  if (!GetDragContext()->IsDragSessionActive() &&
      ShouldHighlightCloseButtonAfterRemove()) {
    // The widget can apparently be null during shutdown.
    views::Widget* widget = GetWidget();
    if (widget)
      widget->SynthesizeMouseMoveEvent();
  }
}

void TabStrip::OnGroupCloseAnimationCompleted(
    const tab_groups::TabGroupId& group) {
  group_views_.erase(group);
  // TODO(crbug.com/905491): We might want to simulate a mouse move here, like
  // we do in OnTabCloseAnimationCompleted.
}

void TabStrip::StoppedDraggingView(TabSlotView* view, bool* is_first_view) {
  if (view &&
      view->GetTabSlotViewType() == TabSlotView::ViewType::kTabGroupHeader) {
    // Ensure all tab group UI is repainted, especially the dragging highlight.
    view->set_dragging(false);
    SchedulePaint();
    return;
  }

  int tab_data_index = GetModelIndexOf(view);
  if (tab_data_index == -1) {
    // The tab was removed before the drag completed. Don't do anything.
    return;
  }

  if (*is_first_view) {
    *is_first_view = false;
    PrepareForAnimation();

    // Animate the view back to its correct position.
    UpdateIdealBounds();
    AnimateToIdealBounds();
  }

  // Install a delegate to reset the dragging state when done. We have to leave
  // dragging true for the tab otherwise it'll draw beneath the new tab button.
  bounds_animator_.AnimateViewTo(view, ideal_bounds(tab_data_index),
                                 std::make_unique<ResetDraggingStateDelegate>(
                                     this, static_cast<Tab*>(view)));
}

void TabStrip::UpdateStackedLayoutFromMouseEvent(views::View* source,
                                                 const ui::MouseEvent& event) {
  if (!adjust_layout_)
    return;

// The following code attempts to switch to shrink (not stacked) layout when
// the mouse exits the tabstrip (or the mouse is pressed on a stacked tab) and
// to stacked layout when a touch device is used. This is made problematic by
// windows generating mouse move events that do not clearly indicate the move
// is the result of a touch device. This assumes a real mouse is used if
// |kMouseMoveCountBeforeConsiderReal| mouse move events are received within
// the time window |kMouseMoveTime|.  At the time we get a mouse press we know
// whether its from a touch device or not, but we don't layout then else
// everything shifts. Instead we wait for the release.
//
// TODO(sky): revisit this when touch events are really plumbed through.
#if !defined(OS_CHROMEOS)
  constexpr auto kMouseMoveTime = base::TimeDelta::FromMilliseconds(200);
  constexpr int kMouseMoveCountBeforeConsiderReal = 3;
#endif

  switch (event.type()) {
    case ui::ET_MOUSE_PRESSED:
      mouse_move_count_ = 0;
      last_mouse_move_time_ = base::TimeTicks();
      SetResetToShrinkOnExit((event.flags() & ui::EF_FROM_TOUCH) == 0);
      if (reset_to_shrink_on_exit_ && touch_layout_) {
        gfx::Point tab_strip_point(event.location());
        views::View::ConvertPointToTarget(source, this, &tab_strip_point);
        Tab* tab = FindTabForEvent(tab_strip_point);
        if (tab && touch_layout_->IsStacked(GetModelIndexOf(tab))) {
          SetStackedLayout(false);
          controller_->StackedLayoutMaybeChanged();
        }
      }
      break;

    case ui::ET_MOUSE_MOVED: {
#if defined(OS_CHROMEOS)
      // Ash does not synthesize mouse events from touch events.
      SetResetToShrinkOnExit(true);
#else
      gfx::Point location(event.location());
      ConvertPointToTarget(source, this, &location);
      if (location == last_mouse_move_location_)
        return;  // Ignore spurious moves.
      last_mouse_move_location_ = location;
      if ((event.flags() & ui::EF_FROM_TOUCH) ||
          (event.flags() & ui::EF_IS_SYNTHESIZED)) {
        last_mouse_move_time_ = base::TimeTicks();
      } else if ((base::TimeTicks::Now() - last_mouse_move_time_) >=
                 kMouseMoveTime) {
        mouse_move_count_ = 1;
        last_mouse_move_time_ = base::TimeTicks::Now();
      } else if (mouse_move_count_ < kMouseMoveCountBeforeConsiderReal) {
        ++mouse_move_count_;
      } else {
        SetResetToShrinkOnExit(true);
      }
#endif
      break;
    }

    case ui::ET_MOUSE_RELEASED: {
      gfx::Point location(event.location());
      ConvertPointToTarget(source, this, &location);
      last_mouse_move_location_ = location;
      mouse_move_count_ = 0;
      last_mouse_move_time_ = base::TimeTicks();
      if ((event.flags() & ui::EF_FROM_TOUCH) == ui::EF_FROM_TOUCH) {
        SetStackedLayout(true);
        controller_->StackedLayoutMaybeChanged();
      }
      break;
    }

    default:
      break;
  }
}

void TabStrip::UpdateContrastRatioValues() {
  // There may be no controller in unit tests, and the call to
  // GetTabBackgroundColor() below requires one, so bail early if it is absent.
  if (!controller_)
    return;

  const SkColor inactive_bg = GetTabBackgroundColor(
      TabActive::kInactive, BrowserFrameActiveState::kUseCurrent);
  const auto get_blend = [inactive_bg](SkColor target, float contrast) {
    return color_utils::BlendForMinContrast(inactive_bg, inactive_bg, target,
                                            contrast);
  };

  const SkColor active_bg = GetTabBackgroundColor(
      TabActive::kActive, BrowserFrameActiveState::kUseCurrent);
  const auto get_hover_opacity = [active_bg, &get_blend](float contrast) {
    return get_blend(active_bg, contrast).alpha / 255.0f;
  };

  // The contrast ratio for the hover effect on standard-width tabs.
  // In the default color scheme, this corresponds to a hover opacity of 0.4.
  constexpr float kStandardWidthContrast = 1.11f;
  hover_opacity_min_ = get_hover_opacity(kStandardWidthContrast);

  // The contrast ratio for the hover effect on min-width tabs.
  // In the default color scheme, this corresponds to a hover opacity of 0.65.
  constexpr float kMinWidthContrast = 1.19f;
  hover_opacity_max_ = get_hover_opacity(kMinWidthContrast);

  // The contrast ratio for the radial gradient effect on hovered tabs.
  // In the default color scheme, this corresponds to a hover opacity of 0.45.
  constexpr float kRadialGradientContrast = 1.13728f;
  radial_highlight_opacity_ = get_hover_opacity(kRadialGradientContrast);

  const SkColor inactive_fg =
      GetTabForegroundColor(TabActive::kInactive, inactive_bg);
  // The contrast ratio for the separator between inactive tabs.
  constexpr float kTabSeparatorContrast = 2.5f;
  separator_color_ = get_blend(inactive_fg, kTabSeparatorContrast).color;
}

void TabStrip::ShiftTabRelative(Tab* tab, int offset) {
  DCHECK(offset == ShiftOffset::kLeft || offset == ShiftOffset::kRight);
  const int start_index = GetModelIndexOf(tab);
  const int target_index = start_index + offset;

  if (!IsValidModelIndex(start_index))
    return;

  if (tab->closing())
    return;

  if (!IsValidModelIndex(target_index) ||
      controller_->IsTabPinned(start_index) !=
          controller_->IsTabPinned(target_index)) {
    // Even if we've reached the boundary of where the tab could go, it may
    // still be able to "move" out of its current group.
    if (tab->group().has_value())
      controller_->RemoveTabFromGroup(start_index);
    return;
  }

  // If the tab is at a group boundary, instead of actually moving the tab just
  // change its group membership.
  base::Optional<tab_groups::TabGroupId> target_group =
      tab_at(target_index)->group();
  if (tab->group() != target_group) {
    if (tab->group().has_value())
      controller_->RemoveTabFromGroup(start_index);
    else if (target_group.has_value())
      controller_->AddTabToGroup(start_index, target_group.value());

    return;
  }

  controller_->MoveTab(start_index, target_index);
}

void TabStrip::ShiftGroupRelative(const tab_groups::TabGroupId& group,
                                  int offset) {
  DCHECK(offset == ShiftOffset::kLeft || offset == ShiftOffset::kRight);
  std::vector<int> tabs_in_group = controller_->ListTabsInGroup(group);

  const int start_index = tabs_in_group.front();
  int target_index = start_index + offset;

  if (offset > 0)
    target_index += tabs_in_group.size() - 1;

  if (!IsValidModelIndex(start_index) || !IsValidModelIndex(target_index))
    return;

  // Avoid moving into the middle of another group by accounting for its size.
  base::Optional<tab_groups::TabGroupId> target_group =
      tab_at(target_index)->group();
  if (target_group.has_value()) {
    target_index +=
        offset *
        (controller_->ListTabsInGroup(target_group.value()).size() - 1);
  }

  if (!IsValidModelIndex(target_index))
    return;

  if (controller_->IsTabPinned(start_index) !=
      controller_->IsTabPinned(target_index))
    return;

  controller_->MoveGroup(group, target_index);
}

void TabStrip::ResizeLayoutTabs() {
  // We've been called back after the TabStrip has been emptied out (probably
  // just prior to the window being destroyed). We need to do nothing here or
  // else GetTabAt below will crash.
  if (tab_count() == 0)
    return;

  // It is critically important that this is unhooked here, otherwise we will
  // keep spying on messages forever.
  RemoveMessageLoopObserver();

  ExitTabClosingMode();
  int pinned_tab_count = GetPinnedTabCount();
  if (pinned_tab_count == tab_count()) {
    // Only pinned tabs, we know the tab widths won't have changed (all
    // pinned tabs have the same width), so there is nothing to do.
    return;
  }
  // Don't try and avoid layout based on tab sizes. If tabs are small enough
  // then the width of the active tab may not change, but other widths may
  // have. This is particularly important if we've overflowed (all tabs are at
  // the min).
  StartResizeLayoutAnimation();
}

void TabStrip::ResizeLayoutTabsFromTouch() {
  // Don't resize if the user is interacting with the tabstrip.
  if (!drag_context_->IsDragSessionActive())
    ResizeLayoutTabs();
  else
    StartResizeLayoutTabsFromTouchTimer();
}

void TabStrip::StartResizeLayoutTabsFromTouchTimer() {
  // Amount of time we delay before resizing after a close from a touch.
  constexpr auto kTouchResizeLayoutTime = base::TimeDelta::FromSeconds(2);

  resize_layout_timer_.Stop();
  resize_layout_timer_.Start(FROM_HERE, kTouchResizeLayoutTime, this,
                             &TabStrip::ResizeLayoutTabsFromTouch);
}

void TabStrip::AddMessageLoopObserver() {
  if (!mouse_watcher_) {
    constexpr int kTabStripAnimationVSlop = 40;
    mouse_watcher_ = std::make_unique<views::MouseWatcher>(
        std::make_unique<views::MouseWatcherViewHost>(
            this, gfx::Insets(0, 0, kTabStripAnimationVSlop, 0)),
        this);
  }
  mouse_watcher_->Start(GetWidget()->GetNativeWindow());
}

void TabStrip::RemoveMessageLoopObserver() {
  mouse_watcher_ = nullptr;
}

gfx::Rect TabStrip::GetDropBounds(int drop_index,
                                  bool drop_before,
                                  bool* is_beneath) {
  DCHECK_NE(drop_index, -1);

  Tab* tab = tab_at(std::min(drop_index, tab_count() - 1));
  int center_x = tab->x();
  const int width = tab->width();
  const int overlap = TabStyle::GetTabOverlap();
  if (drop_index < tab_count())
    center_x += drop_before ? (overlap / 2) : (width / 2);
  else
    center_x += width - (overlap / 2);

  // Mirror the center point if necessary.
  center_x = GetMirroredXInView(center_x);

  // Determine the screen bounds.
  gfx::Point drop_loc(center_x - g_drop_indicator_width / 2,
                      -g_drop_indicator_height);
  ConvertPointToScreen(this, &drop_loc);
  gfx::Rect drop_bounds(drop_loc.x(), drop_loc.y(), g_drop_indicator_width,
                        g_drop_indicator_height);

  // If the rect doesn't fit on the monitor, push the arrow to the bottom.
  display::Screen* screen = display::Screen::GetScreen();
  display::Display display = screen->GetDisplayMatching(drop_bounds);
  *is_beneath = !display.bounds().Contains(drop_bounds);
  if (*is_beneath)
    drop_bounds.Offset(0, drop_bounds.height() + height());

  return drop_bounds;
}

void TabStrip::SetDropArrow(
    const base::Optional<BrowserRootView::DropIndex>& index) {
  if (!index) {
    controller_->OnDropIndexUpdate(-1, false);
    drop_arrow_.reset();
    return;
  }

  // Let the controller know of the index update.
  controller_->OnDropIndexUpdate(index->value, index->drop_before);

  if (drop_arrow_ && (index == drop_arrow_->index()))
    return;

  bool is_beneath;
  gfx::Rect drop_bounds =
      GetDropBounds(index->value, index->drop_before, &is_beneath);

  if (!drop_arrow_) {
    drop_arrow_ = std::make_unique<DropArrow>(*index, !is_beneath, GetWidget());
  } else {
    drop_arrow_->set_index(*index);
    drop_arrow_->SetPointDown(!is_beneath);
  }

  // Reposition the window.
  drop_arrow_->SetWindowBounds(drop_bounds);
}

// static
gfx::ImageSkia* TabStrip::GetDropArrowImage(bool is_down) {
  return ui::ResourceBundle::GetSharedInstance().GetImageSkiaNamed(
      is_down ? IDR_TAB_DROP_DOWN : IDR_TAB_DROP_UP);
}

// TabStrip:TabContextMenuController:
// ----------------------------------------------------------

TabStrip::TabContextMenuController::TabContextMenuController(TabStrip* parent)
    : parent_(parent) {}

void TabStrip::TabContextMenuController::ShowContextMenuForViewImpl(
    views::View* source,
    const gfx::Point& point,
    ui::MenuSourceType source_type) {
  // We are only intended to be installed as a context-menu handler for tabs, so
  // this cast should be safe.
  DCHECK_EQ(Tab::kViewClassName, source->GetClassName());
  Tab* const tab = static_cast<Tab*>(source);
  if (tab->closing())
    return;
  parent_->controller()->ShowContextMenuForTab(tab, point, source_type);
}

// TabStrip:DropArrow:
// ----------------------------------------------------------

TabStrip::DropArrow::DropArrow(const BrowserRootView::DropIndex& index,
                               bool point_down,
                               views::Widget* context)
    : index_(index), point_down_(point_down) {
  arrow_view_ = new views::ImageView;
  arrow_view_->SetImage(GetDropArrowImage(point_down_));

  arrow_window_ = new views::Widget;
  views::Widget::InitParams params(views::Widget::InitParams::TYPE_POPUP);
  params.z_order = ui::ZOrderLevel::kFloatingUIElement;
  params.opacity = views::Widget::InitParams::WindowOpacity::kTranslucent;
  params.accept_events = false;
  params.bounds = gfx::Rect(g_drop_indicator_width, g_drop_indicator_height);
  params.context = context->GetNativeWindow();
  arrow_window_->Init(std::move(params));
  arrow_window_->SetContentsView(arrow_view_);
  scoped_observer_.Add(arrow_window_);

  arrow_window_->Show();
}

TabStrip::DropArrow::~DropArrow() {
  // Close eventually deletes the window, which deletes arrow_view too.
  if (arrow_window_)
    arrow_window_->Close();
}

void TabStrip::DropArrow::SetPointDown(bool down) {
  if (point_down_ == down)
    return;

  point_down_ = down;
  arrow_view_->SetImage(GetDropArrowImage(point_down_));
}

void TabStrip::DropArrow::SetWindowBounds(const gfx::Rect& bounds) {
  arrow_window_->SetBounds(bounds);
}

void TabStrip::DropArrow::OnWidgetDestroying(views::Widget* widget) {
  scoped_observer_.Remove(arrow_window_);
  arrow_window_ = nullptr;
}

///////////////////////////////////////////////////////////////////////////////

void TabStrip::PrepareForAnimation() {
  if (!drag_context_->IsDragSessionActive() &&
      !TabDragController::IsAttachedTo(GetDragContext())) {
    for (int i = 0; i < tab_count(); ++i)
      tab_at(i)->set_dragging(false);
  }
}

void TabStrip::UpdateIdealBounds() {
  if (tab_count() == 0)
    return;  // Should only happen during creation/destruction, ignore.

  if (!touch_layout_) {
    const int available_width =
        override_available_width_for_tabs_.value_or(GetTabAreaWidth());
    layout_helper_->UpdateIdealBounds(available_width);
  }

  new_tab_button_ideal_bounds_.set_origin(
      gfx::Point(GetNewTabButtonIdealX(), 0));
}

int TabStrip::UpdateIdealBoundsForPinnedTabs(int* first_non_pinned_index) {
  layout_helper_->UpdateIdealBoundsForPinnedTabs();
  if (first_non_pinned_index)
    *first_non_pinned_index = layout_helper_->first_non_pinned_tab_index();
  return layout_helper_->first_non_pinned_tab_x();
}

void TabStrip::StartResizeLayoutAnimation() {
  PrepareForAnimation();
  UpdateIdealBounds();
  AnimateToIdealBounds();
}

void TabStrip::StartPinnedTabAnimation() {
  ExitTabClosingMode();

  PrepareForAnimation();

  UpdateIdealBounds();
  AnimateToIdealBounds();
}

bool TabStrip::IsPointInTab(Tab* tab,
                            const gfx::Point& point_in_tabstrip_coords) {
  if (!tab->GetVisible())
    return false;
  gfx::Point point_in_tab_coords(point_in_tabstrip_coords);
  View::ConvertPointToTarget(this, tab, &point_in_tab_coords);
  return tab->HitTestPoint(point_in_tab_coords);
}

Tab* TabStrip::FindTabForEvent(const gfx::Point& point) {
  DCHECK(touch_layout_);
  int active_tab_index = touch_layout_->active_index();
  Tab* tab = FindTabForEventFrom(point, active_tab_index, -1);
  return tab ? tab : FindTabForEventFrom(point, active_tab_index + 1, 1);
}

Tab* TabStrip::FindTabForEventFrom(const gfx::Point& point,
                                   int start,
                                   int delta) {
  // |start| equals tab_count() when there are only pinned tabs.
  if (start == tab_count())
    start += delta;
  for (int i = start; i >= 0 && i < tab_count(); i += delta) {
    if (IsPointInTab(tab_at(i), point))
      return tab_at(i);
  }
  return nullptr;
}

Tab* TabStrip::FindTabHitByPoint(const gfx::Point& point) {
  // Check all tabs, even closing tabs. Mouse events need to reach closing tabs
  // for users to be able to rapidly middle-click close several tabs.
  std::vector<Tab*> all_tabs = layout_helper_->GetTabs();

  // The display order doesn't necessarily match the child order, so we iterate
  // in display order.
  for (size_t i = 0; i < all_tabs.size(); ++i) {
    // If we don't first exclude points outside the current tab, the code below
    // will return the wrong tab if the next tab is selected, the following tab
    // is active, and |point| is in the overlap region between the two.
    Tab* tab = all_tabs[i];
    if (!IsPointInTab(tab, point))
      continue;

    // Selected tabs render atop unselected ones, and active tabs render atop
    // everything.  Check whether the next tab renders atop this one and |point|
    // is in the overlap region.
    Tab* next_tab = i < (all_tabs.size() - 1) ? all_tabs[i + 1] : nullptr;
    if (next_tab &&
        (next_tab->IsActive() ||
         (next_tab->IsSelected() && !tab->IsSelected())) &&
        IsPointInTab(next_tab, point))
      return next_tab;

    // This is the topmost tab for this point.
    return tab;
  }

  return nullptr;
}

void TabStrip::SwapLayoutIfNecessary() {
  bool needs_touch = NeedsTouchLayout();
  bool using_touch = touch_layout_ != nullptr;
  if (needs_touch == using_touch)
    return;

  if (needs_touch) {
    const int overlap = TabStyle::GetTabOverlap();
    touch_layout_ = std::make_unique<StackedTabStripLayout>(
        gfx::Size(GetStackableTabWidth(), GetLayoutConstant(TAB_HEIGHT)),
        overlap, kStackedPadding, kMaxStackedCount, &tabs_);
    touch_layout_->SetWidth(GetTabAreaWidth());
    // This has to be after SetWidth() as SetWidth() is going to reset the
    // bounds of the pinned tabs (since StackedTabStripLayout doesn't yet know
    // how many pinned tabs there are).
    touch_layout_->SetXAndPinnedCount(UpdateIdealBoundsForPinnedTabs(nullptr),
                                      GetPinnedTabCount());
    touch_layout_->SetActiveIndex(controller_->GetActiveIndex());

    base::RecordAction(
        base::UserMetricsAction("StackedTab_EnteredStackedLayout"));
  } else {
    touch_layout_.reset();
  }
  PrepareForAnimation();
  UpdateIdealBounds();
  SetTabVisibility();
  AnimateToIdealBounds();
}

bool TabStrip::NeedsTouchLayout() const {
  if (!stacked_layout_)
    return false;

  const int pinned_tab_count = GetPinnedTabCount();
  const int normal_count = tab_count() - pinned_tab_count;
  if (normal_count <= 1)
    return false;

  const int tab_overlap = TabStyle::GetTabOverlap();
  const int normal_width =
      (GetStackableTabWidth() - tab_overlap) * normal_count + tab_overlap;
  const int pinned_width =
      std::max(0, pinned_tab_count * TabStyle::GetPinnedWidth() - tab_overlap);
  return normal_width > (GetTabAreaWidth() - pinned_width);
}

void TabStrip::SetResetToShrinkOnExit(bool value) {
  if (!adjust_layout_)
    return;

  // We have to be using stacked layout to reset out of it.
  value &= stacked_layout_;

  if (value == reset_to_shrink_on_exit_)
    return;

  reset_to_shrink_on_exit_ = value;
  // Add an observer so we know when the mouse moves out of the tabstrip.
  if (reset_to_shrink_on_exit_)
    AddMessageLoopObserver();
  else
    RemoveMessageLoopObserver();
}

void TabStrip::UpdateNewTabButtonBorder() {
  // The button is placed vertically exactly in the center of the tabstrip.
  const int extra_vertical_space = GetLayoutConstant(TAB_HEIGHT) -
                                   GetLayoutConstant(TABSTRIP_TOOLBAR_OVERLAP) -
                                   NewTabButton::kButtonSize.height();
  constexpr int kHorizontalInset = 8;
  new_tab_button_->SetBorder(views::CreateEmptyBorder(gfx::Insets(
      extra_vertical_space / 2, kHorizontalInset, 0, kHorizontalInset)));
}

void TabStrip::ButtonPressed(views::Button* sender, const ui::Event& event) {
  if (sender == new_tab_button_) {
    base::RecordAction(base::UserMetricsAction("NewTab_Button"));
    UMA_HISTOGRAM_ENUMERATION("Tab.NewTab", TabStripModel::NEW_TAB_BUTTON,
                              TabStripModel::NEW_TAB_ENUM_COUNT);
    if (event.IsMouseEvent()) {
      // Prevent the hover card from popping back in immediately. This forces a
      // normal fade-in.
      if (hover_card_)
        hover_card_->set_last_mouse_exit_timestamp(base::TimeTicks());

      const ui::MouseEvent& mouse = static_cast<const ui::MouseEvent&>(event);
      if (mouse.IsOnlyMiddleMouseButton()) {
        if (ui::Clipboard::IsSupportedClipboardBuffer(
                ui::ClipboardBuffer::kSelection)) {
          ui::Clipboard* clipboard = ui::Clipboard::GetForCurrentThread();
          CHECK(clipboard);
          base::string16 clipboard_text;
          clipboard->ReadText(ui::ClipboardBuffer::kSelection, &clipboard_text);
          if (!clipboard_text.empty())
            controller_->CreateNewTabWithLocation(clipboard_text);
        }
        return;
      }
    }

    controller_->CreateNewTab();
    if (event.type() == ui::ET_GESTURE_TAP)
      TouchUMA::RecordGestureAction(TouchUMA::kGestureNewTabTap);
  }
}

// Overridden to support automation. See automation_proxy_uitest.cc.
const views::View* TabStrip::GetViewByID(int view_id) const {
  if (tab_count() > 0) {
    if (view_id == VIEW_ID_TAB_LAST)
      return tab_at(tab_count() - 1);
    if ((view_id >= VIEW_ID_TAB_0) && (view_id < VIEW_ID_TAB_LAST)) {
      int index = view_id - VIEW_ID_TAB_0;
      return (index >= 0 && index < tab_count()) ? tab_at(index) : nullptr;
    }
  }

  return View::GetViewByID(view_id);
}

bool TabStrip::OnMousePressed(const ui::MouseEvent& event) {
  UpdateStackedLayoutFromMouseEvent(this, event);
  // We can't return true here, else clicking in an empty area won't drag the
  // window.
  return false;
}

bool TabStrip::OnMouseDragged(const ui::MouseEvent& event) {
  ContinueDrag(this, event);
  return true;
}

void TabStrip::OnMouseReleased(const ui::MouseEvent& event) {
  EndDrag(END_DRAG_COMPLETE);
  UpdateStackedLayoutFromMouseEvent(this, event);
}

void TabStrip::OnMouseCaptureLost() {
  EndDrag(END_DRAG_CAPTURE_LOST);
}

void TabStrip::OnMouseMoved(const ui::MouseEvent& event) {
  UpdateStackedLayoutFromMouseEvent(this, event);
}

void TabStrip::OnMouseEntered(const ui::MouseEvent& event) {
  mouse_entered_tabstrip_time_ = base::TimeTicks::Now();
  SetResetToShrinkOnExit(true);
}

void TabStrip::OnMouseExited(const ui::MouseEvent& event) {
  if (base::FeatureList::IsEnabled(features::kTabHoverCards) && hover_card_ &&
      hover_card_->IsVisible()) {
    hover_card_->set_last_mouse_exit_timestamp(base::TimeTicks::Now());
  }
  UpdateHoverCard(nullptr);
}

void TabStrip::AddedToWidget() {
  GetWidget()->AddObserver(this);
}

void TabStrip::RemovedFromWidget() {
  GetWidget()->RemoveObserver(this);
}

void TabStrip::OnGestureEvent(ui::GestureEvent* event) {
  SetResetToShrinkOnExit(false);
  switch (event->type()) {
    case ui::ET_GESTURE_SCROLL_END:
    case ui::ET_SCROLL_FLING_START:
    case ui::ET_GESTURE_END:
      EndDrag(END_DRAG_COMPLETE);
      if (adjust_layout_) {
        SetStackedLayout(true);
        controller_->StackedLayoutMaybeChanged();
      }
      break;

    case ui::ET_GESTURE_LONG_PRESS:
      drag_context_->SetMoveBehavior(TabDragController::REORDER);
      break;

    case ui::ET_GESTURE_LONG_TAP: {
      EndDrag(END_DRAG_CANCEL);
      gfx::Point local_point = event->location();
      Tab* tab = touch_layout_ ? FindTabForEvent(local_point)
                               : FindTabHitByPoint(local_point);
      if (tab) {
        ConvertPointToScreen(this, &local_point);
        controller_->ShowContextMenuForTab(tab, local_point,
                                           ui::MENU_SOURCE_TOUCH);
      }
      break;
    }

    case ui::ET_GESTURE_SCROLL_UPDATE:
      ContinueDrag(this, *event);
      break;

    case ui::ET_GESTURE_TAP_DOWN:
      EndDrag(END_DRAG_CANCEL);
      break;

    case ui::ET_GESTURE_TAP: {
      const int active_index = controller_->GetActiveIndex();
      DCHECK_NE(-1, active_index);
      Tab* active_tab = tab_at(active_index);
      TouchUMA::GestureActionType action = TouchUMA::kGestureTabNoSwitchTap;
      if (active_tab->tab_activated_with_last_tap_down())
        action = TouchUMA::kGestureTabSwitchTap;
      TouchUMA::RecordGestureAction(action);
      break;
    }

    default:
      break;
  }
  event->SetHandled();
}

views::View* TabStrip::TargetForRect(views::View* root, const gfx::Rect& rect) {
  CHECK_EQ(root, this);

  if (!views::UsePointBasedTargeting(rect))
    return views::ViewTargeterDelegate::TargetForRect(root, rect);
  const gfx::Point point(rect.CenterPoint());

  if (!touch_layout_) {
    // Return any view that isn't a Tab or this TabStrip immediately. We don't
    // want to interfere.
    views::View* v = views::ViewTargeterDelegate::TargetForRect(root, rect);
    if (v && v != this && strcmp(v->GetClassName(), Tab::kViewClassName))
      return v;

    views::View* tab = FindTabHitByPoint(point);
    if (tab)
      return tab;
  } else {
    if (new_tab_button_->GetVisible()) {
      views::View* view =
          ConvertPointToViewAndGetEventHandler(this, new_tab_button_, point);
      if (view)
        return view;
    }
    Tab* tab = FindTabForEvent(point);
    if (tab)
      return ConvertPointToViewAndGetEventHandler(this, tab, point);
  }
  return this;
}

void TabStrip::OnViewIsDeleting(views::View* observed_view) {
  if (observed_view == hover_card_) {
    hover_card_observer_.Remove(hover_card_);
    hover_card_event_sniffer_.reset();
    hover_card_ = nullptr;
  }
}

void TabStrip::OnViewFocused(views::View* observed_view) {
  if (observed_view == new_tab_button_)
    UpdateHoverCard(nullptr);
  int index = tabs_.GetIndexOfView(observed_view);
  if (index != -1)
    controller_->OnKeyboardFocusedTabChanged(index);
}

void TabStrip::OnViewBlurred(views::View* observed_view) {
  controller_->OnKeyboardFocusedTabChanged(base::nullopt);
}

void TabStrip::OnTouchUiChanged() {
  UpdateNewTabButtonBorder();
  new_tab_button_ideal_bounds_.set_size(new_tab_button_->GetPreferredSize());
  new_tab_button_->SetBoundsRect(new_tab_button_ideal_bounds_);
  StopAnimating(true);
  PreferredSizeChanged();
}
