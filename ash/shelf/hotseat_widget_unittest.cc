// Copyright (c) 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/app_list/app_list_controller_impl.h"
#include "ash/app_list/test/app_list_test_helper.h"
#include "ash/focus_cycler.h"
#include "ash/home_screen/drag_window_from_shelf_controller_test_api.h"
#include "ash/public/cpp/ash_features.h"
#include "ash/public/cpp/test/assistant_test_api.h"
#include "ash/public/cpp/test/shell_test_api.h"
#include "ash/shelf/home_button.h"
#include "ash/shelf/shelf.h"
#include "ash/shelf/shelf_app_button.h"
#include "ash/shelf/shelf_focus_cycler.h"
#include "ash/shelf/shelf_layout_manager.h"
#include "ash/shelf/shelf_metrics.h"
#include "ash/shelf/shelf_navigation_widget.h"
#include "ash/shelf/shelf_test_util.h"
#include "ash/shelf/shelf_view.h"
#include "ash/shelf/shelf_view_test_api.h"
#include "ash/shelf/test/hotseat_state_watcher.h"
#include "ash/shelf/test/overview_animation_waiter.h"
#include "ash/shelf/test/shelf_layout_manager_test_base.h"
#include "ash/shell.h"
#include "ash/system/overview/overview_button_tray.h"
#include "ash/system/status_area_widget.h"
#include "ash/system/unified/unified_system_tray.h"
#include "ash/test/ash_test_base.h"
#include "ash/wm/overview/overview_controller.h"
#include "ash/wm/tablet_mode/tablet_mode_controller_test_api.h"
#include "ash/wm/window_state.h"
#include "base/test/metrics/histogram_tester.h"
#include "base/test/scoped_feature_list.h"
#include "chromeos/constants/chromeos_features.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/aura/client/aura_constants.h"
#include "ui/compositor/scoped_animation_duration_scale_mode.h"
#include "ui/events/gesture_detection/gesture_configuration.h"
#include "ui/events/test/event_generator.h"
#include "ui/wm/core/window_util.h"

namespace ash {

namespace {
ShelfWidget* GetShelfWidget() {
  return AshTestBase::GetPrimaryShelf()->shelf_widget();
}

ShelfLayoutManager* GetShelfLayoutManager() {
  return AshTestBase::GetPrimaryShelf()->shelf_layout_manager();
}
}  // namespace

class HotseatWidgetTest
    : public ShelfLayoutManagerTestBase,
      public testing::WithParamInterface<
          std::tuple<ShelfAutoHideBehavior, /*is_assistant_enabled*/ bool>> {
 public:
  HotseatWidgetTest()
      : ShelfLayoutManagerTestBase(
            base::test::TaskEnvironment::TimeSource::MOCK_TIME),
        shelf_auto_hide_behavior_(std::get<0>(GetParam())),
        is_assistant_enabled_(std::get<1>(GetParam())) {
    if (is_assistant_enabled_)
      assistant_test_api_ = AssistantTestApi::Create();
  }

  // testing::Test:
  void SetUp() override {
    ShelfLayoutManagerTestBase::SetUp();

    if (is_assistant_enabled_) {
      assistant_test_api_->SetAssistantEnabled(true);
      assistant_test_api_->GetAssistantState()->NotifyFeatureAllowed(
          mojom::AssistantAllowedState::ALLOWED);
      assistant_test_api_->GetAssistantState()->NotifyStatusChanged(
          mojom::AssistantState::READY);

      assistant_test_api_->WaitUntilIdle();
    }
  }

  ShelfAutoHideBehavior shelf_auto_hide_behavior() const {
    return shelf_auto_hide_behavior_;
  }
  bool is_assistant_enabled() const { return is_assistant_enabled_; }
  AssistantTestApi* assistant_test_api() { return assistant_test_api_.get(); }

  void ShowShelfAndLongPressHome() {
    if (shelf_auto_hide_behavior() == ShelfAutoHideBehavior::kAlways)
      SwipeUpOnShelf();

    views::View* home_button =
        GetPrimaryShelf()->navigation_widget()->GetHomeButton();
    auto center_point = home_button->GetBoundsInScreen().CenterPoint();

    GetEventGenerator()->set_current_screen_location(center_point);
    GetEventGenerator()->PressTouch();
    GetAppListTestHelper()->WaitUntilIdle();

    // Advance clock to make sure long press gesture is triggered.
    task_environment_->AdvanceClock(base::TimeDelta::FromSeconds(5));
    GetAppListTestHelper()->WaitUntilIdle();

    GetEventGenerator()->ReleaseTouch();
    GetAppListTestHelper()->WaitUntilIdle();
  }

 private:
  const ShelfAutoHideBehavior shelf_auto_hide_behavior_;
  const bool is_assistant_enabled_;
  std::unique_ptr<AssistantTestApi> assistant_test_api_;
};

// Counts the number of times the work area changes.
class DisplayWorkAreaChangeCounter : public display::DisplayObserver {
 public:
  DisplayWorkAreaChangeCounter() {
    Shell::Get()->display_manager()->AddObserver(this);
  }
  ~DisplayWorkAreaChangeCounter() override {
    Shell::Get()->display_manager()->RemoveObserver(this);
  }

  void OnDisplayMetricsChanged(const display::Display& display,
                               uint32_t metrics) override {
    if (metrics & display::DisplayObserver::DISPLAY_METRIC_WORK_AREA)
      work_area_change_count_++;
  }

  int count() const { return work_area_change_count_; }

 private:
  int work_area_change_count_ = 0;

  DISALLOW_COPY_AND_ASSIGN(DisplayWorkAreaChangeCounter);
};

// Watches the shelf for state changes.
class ShelfStateWatcher : public ShelfObserver {
 public:
  ShelfStateWatcher() { AshTestBase::GetPrimaryShelf()->AddObserver(this); }
  ~ShelfStateWatcher() override {
    AshTestBase::GetPrimaryShelf()->RemoveObserver(this);
  }
  void WillChangeVisibilityState(ShelfVisibilityState new_state) override {
    state_change_count_++;
  }
  int state_change_count() const { return state_change_count_; }

 private:
  int state_change_count_ = 0;
};

// Used to test the Hotseat, ScrollabeShelf, and DenseShelf features.
INSTANTIATE_TEST_SUITE_P(
    All,
    HotseatWidgetTest,
    testing::Combine(testing::Values(ShelfAutoHideBehavior::kNever,
                                     ShelfAutoHideBehavior::kAlways),
                     testing::Bool()));

TEST_P(HotseatWidgetTest, LongPressHomeWithoutAppWindow) {
  GetPrimaryShelf()->SetAutoHideBehavior(shelf_auto_hide_behavior());
  TabletModeControllerTestApi().EnterTabletMode();
  GetAppListTestHelper()->CheckVisibility(true);

  HotseatStateWatcher watcher(GetShelfLayoutManager());

  ShowShelfAndLongPressHome();
  GetAppListTestHelper()->CheckVisibility(true);

  EXPECT_EQ(
      is_assistant_enabled(),
      GetAppListTestHelper()->GetAppListView()->IsShowingEmbeddedAssistantUI());

  // Hotseat should not change when showing Assistant.
  watcher.CheckEqual({});
}

TEST_P(HotseatWidgetTest, LongPressHomeWithAppWindow) {
  GetPrimaryShelf()->SetAutoHideBehavior(shelf_auto_hide_behavior());
  TabletModeControllerTestApi().EnterTabletMode();
  GetAppListTestHelper()->CheckVisibility(true);

  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());

  GetAppListTestHelper()->CheckVisibility(false);

  HotseatStateWatcher watcher(GetShelfLayoutManager());

  ShowShelfAndLongPressHome();
  GetAppListTestHelper()->CheckVisibility(false);

  EXPECT_EQ(
      is_assistant_enabled(),
      GetAppListTestHelper()->GetAppListView()->IsShowingEmbeddedAssistantUI());

  std::vector<HotseatState> expected_state;
  if (shelf_auto_hide_behavior() == ShelfAutoHideBehavior::kAlways) {
    // |ShowShelfAndLongPressHome()| will bring up shelf so it will trigger one
    // hotseat state change.
    expected_state.push_back(HotseatState::kExtended);
  }
  watcher.CheckEqual(expected_state);
}

// Tests that closing a window which was opened prior to entering tablet mode
// results in a kShown hotseat.
TEST_P(HotseatWidgetTest, ClosingLastWindowInTabletMode) {
  GetPrimaryShelf()->SetAutoHideBehavior(shelf_auto_hide_behavior());
  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  // Activate the window and go to tablet mode.
  wm::ActivateWindow(window.get());
  TabletModeControllerTestApi().EnterTabletMode();

  // Close the window, the AppListView should be shown, and the hotseat should
  // be kShown.
  window->Hide();

  EXPECT_EQ(HotseatState::kShown, GetShelfLayoutManager()->hotseat_state());
  GetAppListTestHelper()->CheckVisibility(true);
}

// Tests that the hotseat is kShown when entering tablet mode with no windows.
TEST_P(HotseatWidgetTest, GoingToTabletModeNoWindows) {
  GetPrimaryShelf()->SetAutoHideBehavior(shelf_auto_hide_behavior());
  TabletModeControllerTestApi().EnterTabletMode();

  GetAppListTestHelper()->CheckVisibility(true);
  EXPECT_EQ(HotseatState::kShown, GetShelfLayoutManager()->hotseat_state());
}

// Tests that the hotseat is kHidden when entering tablet mode with a window.
TEST_P(HotseatWidgetTest, GoingToTabletModeWithWindows) {
  GetPrimaryShelf()->SetAutoHideBehavior(shelf_auto_hide_behavior());

  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  // Activate the window and go to tablet mode.
  wm::ActivateWindow(window.get());
  TabletModeControllerTestApi().EnterTabletMode();

  EXPECT_EQ(HotseatState::kHidden, GetShelfLayoutManager()->hotseat_state());
  GetAppListTestHelper()->CheckVisibility(false);
}

// The in-app Hotseat should not be hidden automatically when the shelf context
// menu shows (https://crbug.com/1020388).
TEST_P(HotseatWidgetTest, InAppShelfShowingContextMenu) {
  GetPrimaryShelf()->SetAutoHideBehavior(shelf_auto_hide_behavior());
  TabletModeControllerTestApi().EnterTabletMode();
  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());
  EXPECT_FALSE(Shell::Get()->app_list_controller()->IsVisible());

  ShelfTestUtil::AddAppShortcut("app_id", TYPE_PINNED_APP);

  // Swipe up on the shelf to show the hotseat.
  SwipeUpOnShelf();
  EXPECT_EQ(HotseatState::kExtended, GetShelfLayoutManager()->hotseat_state());

  ShelfViewTestAPI shelf_view_test_api(
      GetPrimaryShelf()->shelf_widget()->shelf_view_for_testing());
  ShelfAppButton* app_icon = shelf_view_test_api.GetButton(0);

  // Accelerate the generation of the long press event.
  ui::GestureConfiguration::GetInstance()->set_show_press_delay_in_ms(1);
  ui::GestureConfiguration::GetInstance()->set_long_press_time_in_ms(1);

  // Press the icon enough long time to generate the long press event.
  GetEventGenerator()->MoveTouch(app_icon->GetBoundsInScreen().CenterPoint());
  GetEventGenerator()->PressTouch();
  ui::GestureConfiguration* gesture_config =
      ui::GestureConfiguration::GetInstance();
  const int long_press_delay_ms = gesture_config->long_press_time_in_ms() +
                                  gesture_config->show_press_delay_in_ms();
  base::RunLoop run_loop;
  base::ThreadTaskRunnerHandle::Get()->PostDelayedTask(
      FROM_HERE, run_loop.QuitClosure(),
      base::TimeDelta::FromMilliseconds(long_press_delay_ms));
  run_loop.Run();
  GetEventGenerator()->ReleaseTouch();

  // Expects that the hotseat's state is kExntended.
  EXPECT_EQ(HotseatState::kExtended, GetShelfLayoutManager()->hotseat_state());

  // Ensures that the ink drop state is InkDropState::ACTIVATED before closing
  // the menu.
  app_icon->FireRippleActivationTimerForTest();
}

// Tests that a window that is created after going to tablet mode, then closed,
// results in a kShown hotseat.
TEST_P(HotseatWidgetTest, CloseLastWindowOpenedInTabletMode) {
  GetPrimaryShelf()->SetAutoHideBehavior(shelf_auto_hide_behavior());
  TabletModeControllerTestApi().EnterTabletMode();

  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  // Activate the window after entering tablet mode.
  wm::ActivateWindow(window.get());

  EXPECT_EQ(HotseatState::kHidden, GetShelfLayoutManager()->hotseat_state());
  GetAppListTestHelper()->CheckVisibility(false);

  // Hide the window, the hotseat should be kShown, and the home launcher should
  // be visible.
  window->Hide();

  EXPECT_EQ(HotseatState::kShown, GetShelfLayoutManager()->hotseat_state());
  GetAppListTestHelper()->CheckVisibility(true);
}

// Tests that swiping up on an autohidden shelf shows the hotseat, and swiping
// down hides it.
TEST_P(HotseatWidgetTest, ShowingAndHidingAutohiddenShelf) {
  if (shelf_auto_hide_behavior() != ShelfAutoHideBehavior::kAlways)
    return;

  GetPrimaryShelf()->SetAutoHideBehavior(ShelfAutoHideBehavior::kAlways);
  TabletModeControllerTestApi().EnterTabletMode();
  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());

  SwipeUpOnShelf();

  EXPECT_EQ(HotseatState::kExtended, GetShelfLayoutManager()->hotseat_state());
  EXPECT_EQ(SHELF_AUTO_HIDE_SHOWN, GetPrimaryShelf()->GetAutoHideState());

  SwipeDownOnShelf();

  EXPECT_EQ(HotseatState::kHidden, GetShelfLayoutManager()->hotseat_state());
  EXPECT_EQ(SHELF_AUTO_HIDE_HIDDEN, GetPrimaryShelf()->GetAutoHideState());

  // Swipe down again, nothing should change.
  SwipeDownOnShelf();

  EXPECT_EQ(HotseatState::kHidden, GetShelfLayoutManager()->hotseat_state());
  EXPECT_EQ(SHELF_AUTO_HIDE_HIDDEN, GetPrimaryShelf()->GetAutoHideState());
}

// Tests that swiping up on several places in the in-app shelf shows the
// hotseat (crbug.com/1016931).
TEST_P(HotseatWidgetTest, SwipeUpInAppShelfShowsHotseat) {
  TabletModeControllerTestApi().EnterTabletMode();
  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());

  base::HistogramTester histogram_tester;
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeDownToHide, 0);
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeUpToShow, 0);

  // Swipe up from the center of the shelf.
  SwipeUpOnShelf();
  EXPECT_EQ(HotseatState::kExtended, GetShelfLayoutManager()->hotseat_state());
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeDownToHide, 0);
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeUpToShow, 1);

  // Swipe down from the hotseat to hide it.
  gfx::Rect hotseat_bounds =
      GetShelfWidget()->hotseat_widget()->GetWindowBoundsInScreen();
  gfx::Point start = hotseat_bounds.top_center();
  gfx::Point end = start + gfx::Vector2d(0, 80);
  const base::TimeDelta kTimeDelta = base::TimeDelta::FromMilliseconds(100);
  const int kNumScrollSteps = 4;

  GetEventGenerator()->GestureScrollSequence(start, end, kTimeDelta,
                                             kNumScrollSteps);
  ASSERT_EQ(HotseatState::kHidden, GetShelfLayoutManager()->hotseat_state());

  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeDownToHide, 1);
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeUpToShow, 1);

  // Swipe up from the right part of the shelf (the system tray).
  start = GetShelfWidget()
              ->status_area_widget()
              ->GetWindowBoundsInScreen()
              .CenterPoint();
  end = start + gfx::Vector2d(0, -80);

  GetEventGenerator()->GestureScrollSequence(start, end, kTimeDelta,
                                             kNumScrollSteps);
  EXPECT_EQ(HotseatState::kExtended, GetShelfLayoutManager()->hotseat_state());

  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeDownToHide, 1);
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeUpToShow, 2);

  // Swipe down from the hotseat to hide it.
  start = hotseat_bounds.top_center();
  end = start + gfx::Vector2d(0, 80);

  GetEventGenerator()->GestureScrollSequence(start, end, kTimeDelta,
                                             kNumScrollSteps);
  ASSERT_EQ(HotseatState::kHidden, GetShelfLayoutManager()->hotseat_state());

  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeDownToHide, 2);
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeUpToShow, 2);

  // Swipe up from the left part of the shelf (the home/back button).
  start = GetShelfWidget()
              ->navigation_widget()
              ->GetWindowBoundsInScreen()
              .CenterPoint();
  end = start + gfx::Vector2d(0, -80);

  GetEventGenerator()->GestureScrollSequence(start, end, kTimeDelta,
                                             kNumScrollSteps);
  EXPECT_EQ(HotseatState::kExtended, GetShelfLayoutManager()->hotseat_state());

  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeDownToHide, 2);
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeUpToShow, 3);
}

// Tests that swiping up on the hotseat does nothing.
TEST_P(HotseatWidgetTest, SwipeUpOnHotseatBackgroundDoesNothing) {
  GetPrimaryShelf()->SetAutoHideBehavior(shelf_auto_hide_behavior());
  TabletModeControllerTestApi().EnterTabletMode();
  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());

  base::HistogramTester histogram_tester;
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeDownToHide, 0);
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeUpToShow, 0);

  // Swipe up on the shelf to show the hotseat.
  EXPECT_FALSE(Shell::Get()->app_list_controller()->IsVisible());

  SwipeUpOnShelf();

  EXPECT_EQ(HotseatState::kExtended, GetShelfLayoutManager()->hotseat_state());
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeDownToHide, 0);
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeUpToShow, 1);
  if (shelf_auto_hide_behavior() == ShelfAutoHideBehavior::kAlways)
    EXPECT_EQ(SHELF_AUTO_HIDE_SHOWN, GetPrimaryShelf()->GetAutoHideState());

  // Swipe up on the Hotseat (parent of ShelfView) does nothing.
  gfx::Point start(GetPrimaryShelf()
                       ->shelf_widget()
                       ->hotseat_widget()
                       ->GetWindowBoundsInScreen()
                       .top_center());
  const gfx::Point end(start + gfx::Vector2d(0, -300));
  const base::TimeDelta kTimeDelta = base::TimeDelta::FromMilliseconds(100);
  const int kNumScrollSteps = 4;
  GetEventGenerator()->GestureScrollSequence(start, end, kTimeDelta,
                                             kNumScrollSteps);

  EXPECT_FALSE(Shell::Get()->app_list_controller()->IsVisible());
  EXPECT_EQ(HotseatState::kExtended, GetShelfLayoutManager()->hotseat_state());
  if (shelf_auto_hide_behavior() == ShelfAutoHideBehavior::kAlways)
    EXPECT_EQ(SHELF_AUTO_HIDE_SHOWN, GetPrimaryShelf()->GetAutoHideState());
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeDownToHide, 0);
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeUpToShow, 1);
}

// Tests that tapping an active window with an extended hotseat results in a
// hidden hotseat.
TEST_P(HotseatWidgetTest, TappingActiveWindowHidesHotseat) {
  GetPrimaryShelf()->SetAutoHideBehavior(shelf_auto_hide_behavior());
  TabletModeControllerTestApi().EnterTabletMode();
  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());

  base::HistogramTester histogram_tester;
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeDownToHide, 0);
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeUpToShow, 0);
  histogram_tester.ExpectBucketCount(
      kHotseatGestureHistogramName,
      InAppShelfGestures::kHotseatHiddenDueToInteractionOutsideOfShelf, 0);

  // Swipe up on the shelf to show the hotseat.
  SwipeUpOnShelf();

  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeDownToHide, 0);
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeUpToShow, 1);
  histogram_tester.ExpectBucketCount(
      kHotseatGestureHistogramName,
      InAppShelfGestures::kHotseatHiddenDueToInteractionOutsideOfShelf, 0);

  // Tap the shelf background, nothing should happen.
  gfx::Rect display_bounds =
      display::Screen::GetScreen()->GetPrimaryDisplay().bounds();
  gfx::Point tap_point = display_bounds.bottom_center();
  GetEventGenerator()->GestureTapAt(tap_point);

  EXPECT_EQ(HotseatState::kExtended, GetShelfLayoutManager()->hotseat_state());
  if (shelf_auto_hide_behavior() == ShelfAutoHideBehavior::kAlways)
    EXPECT_EQ(SHELF_AUTO_HIDE_SHOWN, GetPrimaryShelf()->GetAutoHideState());

  // Tap the active window, the hotseat should hide.
  tap_point.Offset(0, -200);
  GetEventGenerator()->GestureTapAt(tap_point);

  EXPECT_EQ(HotseatState::kHidden, GetShelfLayoutManager()->hotseat_state());
  if (shelf_auto_hide_behavior() == ShelfAutoHideBehavior::kAlways)
    EXPECT_EQ(SHELF_AUTO_HIDE_HIDDEN, GetPrimaryShelf()->GetAutoHideState());

  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeDownToHide, 0);
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeUpToShow, 1);
  histogram_tester.ExpectBucketCount(
      kHotseatGestureHistogramName,
      InAppShelfGestures::kHotseatHiddenDueToInteractionOutsideOfShelf, 1);
}

// Tests that gesture dragging an active window hides the hotseat.
TEST_P(HotseatWidgetTest, GestureDraggingActiveWindowHidesHotseat) {
  GetPrimaryShelf()->SetAutoHideBehavior(shelf_auto_hide_behavior());
  TabletModeControllerTestApi().EnterTabletMode();
  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());

  base::HistogramTester histogram_tester;
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeDownToHide, 0);
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeUpToShow, 0);

  // Swipe up on the shelf to show the hotseat.
  SwipeUpOnShelf();

  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeDownToHide, 0);
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeUpToShow, 1);

  if (shelf_auto_hide_behavior() == ShelfAutoHideBehavior::kAlways)
    EXPECT_EQ(SHELF_AUTO_HIDE_SHOWN, GetPrimaryShelf()->GetAutoHideState());

  // Gesture drag on the active window, the hotseat should hide.
  gfx::Rect display_bounds =
      display::Screen::GetScreen()->GetPrimaryDisplay().bounds();
  gfx::Point start = display_bounds.bottom_center();
  start.Offset(0, -200);
  gfx::Point end = start;
  end.Offset(0, -200);
  GetEventGenerator()->GestureScrollSequence(
      start, end, base::TimeDelta::FromMilliseconds(10), 4);

  EXPECT_EQ(HotseatState::kHidden, GetShelfLayoutManager()->hotseat_state());
  if (shelf_auto_hide_behavior() == ShelfAutoHideBehavior::kAlways)
    EXPECT_EQ(SHELF_AUTO_HIDE_HIDDEN, GetPrimaryShelf()->GetAutoHideState());

  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeDownToHide, 0);
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeUpToShow, 1);
}

// Tests that a swipe up on the shelf shows the hotseat while in split view.
TEST_P(HotseatWidgetTest, SwipeUpOnShelfShowsHotseatInSplitView) {
  TabletModeControllerTestApi().EnterTabletMode();
  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());

  base::HistogramTester histogram_tester;
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeDownToHide, 0);
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeUpToShow, 0);

  // Go into split view mode by first going into overview, and then snapping
  // the open window on one side.
  OverviewController* overview_controller = Shell::Get()->overview_controller();
  overview_controller->StartOverview();
  SplitViewController* split_view_controller =
      SplitViewController::Get(Shell::GetPrimaryRootWindow());
  split_view_controller->SnapWindow(window.get(), SplitViewController::LEFT);
  EXPECT_TRUE(split_view_controller->InSplitViewMode());

  // We should still be able to drag up the hotseat.
  SwipeUpOnShelf();
  EXPECT_EQ(HotseatState::kExtended, GetShelfLayoutManager()->hotseat_state());
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeDownToHide, 0);
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeUpToShow, 1);
}

// Tests that releasing the hotseat gesture below the threshold results in a
// kHidden hotseat when the shelf is shown.
TEST_P(HotseatWidgetTest, ReleasingSlowDragBelowThreshold) {
  GetPrimaryShelf()->SetAutoHideBehavior(ShelfAutoHideBehavior::kNever);
  TabletModeControllerTestApi().EnterTabletMode();
  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());

  base::HistogramTester histogram_tester;
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeDownToHide, 0);
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeUpToShow, 0);

  gfx::Rect display_bounds =
      display::Screen::GetScreen()->GetPrimaryDisplay().bounds();
  const gfx::Point start(display_bounds.bottom_center());
  const int hotseat_size = GetPrimaryShelf()
                               ->shelf_widget()
                               ->hotseat_widget()
                               ->GetWindowBoundsInScreen()
                               .height();
  const gfx::Point end(start + gfx::Vector2d(0, -hotseat_size / 2 + 1));
  const base::TimeDelta kTimeDelta = base::TimeDelta::FromMilliseconds(1000);
  const int kNumScrollSteps = 4;
  GetEventGenerator()->GestureScrollSequence(start, end, kTimeDelta,
                                             kNumScrollSteps);

  EXPECT_EQ(HotseatState::kHidden, GetShelfLayoutManager()->hotseat_state());
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeDownToHide, 0);
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeUpToShow, 0);
}

// Tests that releasing the hotseat gesture above the threshold results in a
// kExtended hotseat.
TEST_P(HotseatWidgetTest, ReleasingSlowDragAboveThreshold) {
  GetPrimaryShelf()->SetAutoHideBehavior(shelf_auto_hide_behavior());
  TabletModeControllerTestApi().EnterTabletMode();
  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());

  base::HistogramTester histogram_tester;
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeDownToHide, 0);
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeUpToShow, 0);

  gfx::Rect display_bounds =
      display::Screen::GetScreen()->GetPrimaryDisplay().bounds();
  const gfx::Point start(display_bounds.bottom_center());
  const int hotseat_size = GetPrimaryShelf()
                               ->shelf_widget()
                               ->hotseat_widget()
                               ->GetWindowBoundsInScreen()
                               .height();
  const gfx::Point end(start + gfx::Vector2d(0, -hotseat_size * 3.0f / 2.0f));
  const base::TimeDelta kTimeDelta = base::TimeDelta::FromMilliseconds(1000);
  const int kNumScrollSteps = 4;
  GetEventGenerator()->GestureScrollSequence(start, end, kTimeDelta,
                                             kNumScrollSteps);

  EXPECT_EQ(HotseatState::kExtended, GetShelfLayoutManager()->hotseat_state());
  if (shelf_auto_hide_behavior() == ShelfAutoHideBehavior::kAlways)
    EXPECT_EQ(SHELF_AUTO_HIDE_SHOWN, GetPrimaryShelf()->GetAutoHideState());
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeDownToHide, 0);
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeUpToShow, 1);
}

// Tests that showing overview after showing the hotseat results in only one
// animation, to |kExtended|.
TEST_P(HotseatWidgetTest, ShowingOverviewFromShownAnimatesOnce) {
  GetPrimaryShelf()->SetAutoHideBehavior(shelf_auto_hide_behavior());
  TabletModeControllerTestApi().EnterTabletMode();
  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());

  std::unique_ptr<HotseatStateWatcher> state_watcher_ =
      std::make_unique<HotseatStateWatcher>(GetShelfLayoutManager());
  SwipeUpOnShelf();
  ASSERT_EQ(HotseatState::kExtended, GetShelfLayoutManager()->hotseat_state());

  const gfx::Point overview_button_center = GetPrimaryShelf()
                                                ->status_area_widget()
                                                ->overview_button_tray()
                                                ->GetBoundsInScreen()
                                                .CenterPoint();
  GetEventGenerator()->GestureTapAt(overview_button_center);

  state_watcher_->CheckEqual({HotseatState::kExtended});
}

// Tests that the hotseat is not flush with the bottom of the screen when home
// launcher is showing.
TEST_P(HotseatWidgetTest, HotseatNotFlushWhenHomeLauncherShowing) {
  GetPrimaryShelf()->SetAutoHideBehavior(shelf_auto_hide_behavior());
  TabletModeControllerTestApi().EnterTabletMode();
  const int display_height =
      display::Screen::GetScreen()->GetPrimaryDisplay().bounds().height();
  const int hotseat_bottom = GetPrimaryShelf()
                                 ->shelf_widget()
                                 ->hotseat_widget()
                                 ->GetWindowBoundsInScreen()
                                 .bottom();
  EXPECT_LT(hotseat_bottom, display_height);
}

// Tests that home -> overview results in only one hotseat state change.
TEST_P(HotseatWidgetTest, HomeToOverviewChangesStateOnce) {
  GetPrimaryShelf()->SetAutoHideBehavior(shelf_auto_hide_behavior());
  TabletModeControllerTestApi().EnterTabletMode();

  // First, try with no windows open.
  const gfx::Point overview_button_center = GetPrimaryShelf()
                                                ->status_area_widget()
                                                ->overview_button_tray()
                                                ->GetBoundsInScreen()
                                                .CenterPoint();

  {
    HotseatStateWatcher watcher(GetShelfLayoutManager());
    OverviewAnimationWaiter waiter;
    GetEventGenerator()->GestureTapAt(overview_button_center);
    waiter.Wait();
    watcher.CheckEqual({HotseatState::kExtended});
  }

  // Open a window, then open the home launcher.
  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());
  if (shelf_auto_hide_behavior() == ShelfAutoHideBehavior::kAlways)
    SwipeUpOnShelf();
  views::View* home_button =
      GetPrimaryShelf()->navigation_widget()->GetHomeButton();
  GetEventGenerator()->GestureTapAt(
      home_button->GetBoundsInScreen().CenterPoint());
  GetAppListTestHelper()->CheckVisibility(true);
  // Activate overview and expect the hotseat only changes state to extended.
  {
    HotseatStateWatcher watcher(GetShelfLayoutManager());
    OverviewAnimationWaiter waiter;
    GetEventGenerator()->GestureTapAt(overview_button_center);
    waiter.Wait();

    watcher.CheckEqual({HotseatState::kExtended});
  }
}

// Tests that home -> in-app results in only one state change.
TEST_P(HotseatWidgetTest, HomeToInAppChangesStateOnce) {
  GetPrimaryShelf()->SetAutoHideBehavior(shelf_auto_hide_behavior());
  TabletModeControllerTestApi().EnterTabletMode();

  // Go to in-app, the hotseat should hide.
  HotseatStateWatcher watcher(GetShelfLayoutManager());
  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());

  watcher.CheckEqual({HotseatState::kHidden});
}

// Tests that in-app -> home via closing the only window, swiping from the
// bottom of the shelf, and tapping the home launcher button results in only one
// state change.
TEST_P(HotseatWidgetTest, InAppToHomeChangesStateOnce) {
  GetPrimaryShelf()->SetAutoHideBehavior(shelf_auto_hide_behavior());
  TabletModeControllerTestApi().EnterTabletMode();

  // Go to in-app with an extended hotseat.
  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());
  SwipeUpOnShelf();

  // Press the home button, the hotseat should transition directly to kShown.
  {
    HotseatStateWatcher watcher(GetShelfLayoutManager());
    views::View* home_button =
        GetPrimaryShelf()->navigation_widget()->GetHomeButton();
    GetEventGenerator()->GestureTapAt(
        home_button->GetBoundsInScreen().CenterPoint());

    watcher.CheckEqual({HotseatState::kShown});
  }
  // Go to in-app.
  window->Show();
  wm::ActivateWindow(window.get());

  // Extend the hotseat, then Swipe up to go home, the hotseat should transition
  // directly to kShown.
  SwipeUpOnShelf();
  {
    ui::ScopedAnimationDurationScaleMode regular_animations(
        ui::ScopedAnimationDurationScaleMode::NON_ZERO_DURATION);
    HotseatStateWatcher watcher(GetShelfLayoutManager());
    FlingUpOnShelf();
    watcher.CheckEqual({HotseatState::kShown});

    // Wait for the window animation to complete, and verify the hotseat state
    // remained kShown.
    ShellTestApi().WaitForWindowFinishAnimating(window.get());
    watcher.CheckEqual({HotseatState::kShown});
  }

  // Nothing left to test for autohidden shelf.
  if (shelf_auto_hide_behavior() == ShelfAutoHideBehavior::kAlways)
    return;

  // Go to in-app and do not extend the hotseat.
  window->Show();
  wm::ActivateWindow(window.get());

  // Press the home button, the hotseat should transition directly to kShown.
  {
    HotseatStateWatcher watcher(GetShelfLayoutManager());
    views::View* home_button =
        GetPrimaryShelf()->navigation_widget()->GetHomeButton();
    GetEventGenerator()->GestureTapAt(
        home_button->GetBoundsInScreen().CenterPoint());

    watcher.CheckEqual({HotseatState::kShown});
  }
}

// Tests that transitioning from overview to home while a transition from home
// to overview is still in progress ends up with hotseat in kShown state (and in
// app shelf not visible).
TEST_P(HotseatWidgetTest, HomeToOverviewAndBack) {
  GetPrimaryShelf()->SetAutoHideBehavior(shelf_auto_hide_behavior());
  TabletModeControllerTestApi().EnterTabletMode();

  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  WindowState::Get(window.get())->Minimize();

  // Start going to overview - hotseat should transition to extended state.
  HotseatStateWatcher watcher(GetShelfLayoutManager());
  {
    gfx::Point overview_button_center = GetPrimaryShelf()
                                            ->status_area_widget()
                                            ->overview_button_tray()
                                            ->GetBoundsInScreen()
                                            .CenterPoint();
    ui::ScopedAnimationDurationScaleMode regular_animations(
        ui::ScopedAnimationDurationScaleMode::NON_ZERO_DURATION);
    GetEventGenerator()->GestureTapAt(overview_button_center);
    watcher.CheckEqual({HotseatState::kExtended});
  }
  OverviewController* overview_controller = Shell::Get()->overview_controller();
  EXPECT_TRUE(overview_controller->InOverviewSession());

  views::View* home_button =
      GetPrimaryShelf()->navigation_widget()->GetHomeButton();
  GetEventGenerator()->GestureTapAt(
      home_button->GetBoundsInScreen().CenterPoint());

  GetAppListTestHelper()->CheckVisibility(true);
  EXPECT_FALSE(overview_controller->InOverviewSession());
  EXPECT_FALSE(ShelfConfig::Get()->is_in_app());

  watcher.CheckEqual({HotseatState::kExtended, HotseatState::kShown});
}

TEST_P(HotseatWidgetTest, InAppToOverviewAndBack) {
  GetPrimaryShelf()->SetAutoHideBehavior(shelf_auto_hide_behavior());
  TabletModeControllerTestApi().EnterTabletMode();

  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());

  // Start watching hotseat state before swipping up the shelf, so hotseat
  // change expectation match for both auto-hidden and always-shown shelf.
  HotseatStateWatcher watcher(GetShelfLayoutManager());

  // Make sure shelf (and overview button) are visible - this is moves the
  // hotseat into kExtended state.
  if (shelf_auto_hide_behavior() == ShelfAutoHideBehavior::kAlways)
    SwipeUpOnShelf();

  gfx::Point overview_button_center = GetPrimaryShelf()
                                          ->status_area_widget()
                                          ->overview_button_tray()
                                          ->GetBoundsInScreen()
                                          .CenterPoint();

  // Start going to overview - use non zero animation so transition is not
  // immediate.
  {
    ui::ScopedAnimationDurationScaleMode regular_animations(
        ui::ScopedAnimationDurationScaleMode::NON_ZERO_DURATION);
    GetEventGenerator()->GestureTapAt(overview_button_center);
  }

  OverviewController* overview_controller = Shell::Get()->overview_controller();
  EXPECT_TRUE(overview_controller->InOverviewSession());
  GetAppListTestHelper()->CheckVisibility(false);

  // Hotseat should be extended as overview is starting.
  watcher.CheckEqual({HotseatState::kExtended});

  // Tapping overview button again should go back to the app window.
  GetEventGenerator()->GestureTapAt(overview_button_center);
  EXPECT_FALSE(overview_controller->InOverviewSession());
  GetAppListTestHelper()->CheckVisibility(false);
  EXPECT_TRUE(ShelfConfig::Get()->is_in_app());

  // The hotseat is expected to be hidden.
  watcher.CheckEqual({HotseatState::kExtended, HotseatState::kHidden});
}

// Tests transition to home screen initiated while transition from app window to
// overview is in progress.
TEST_P(HotseatWidgetTest, GoHomeDuringInAppToOverviewTransition) {
  GetPrimaryShelf()->SetAutoHideBehavior(shelf_auto_hide_behavior());
  TabletModeControllerTestApi().EnterTabletMode();

  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());

  // Start watching hotseat state before swipping up the shelf, so hotseat
  // change expectation match for both auto-hidden and always-shown shelf.
  HotseatStateWatcher watcher(GetShelfLayoutManager());

  // Make sure shelf (and overview button) are visible - this is moves the
  // hotseat into kExtended state.
  if (shelf_auto_hide_behavior() == ShelfAutoHideBehavior::kAlways)
    SwipeUpOnShelf();

  gfx::Point overview_button_center = GetPrimaryShelf()
                                          ->status_area_widget()
                                          ->overview_button_tray()
                                          ->GetBoundsInScreen()
                                          .CenterPoint();

  // Start going to overview - use non zero animation so transition is not
  // immediate.
  {
    ui::ScopedAnimationDurationScaleMode regular_animations(
        ui::ScopedAnimationDurationScaleMode::NON_ZERO_DURATION);
    GetEventGenerator()->GestureTapAt(overview_button_center);
  }

  OverviewController* overview_controller = Shell::Get()->overview_controller();
  EXPECT_TRUE(overview_controller->InOverviewSession());
  GetAppListTestHelper()->CheckVisibility(false);

  // Hotseat should be extended as overview is starting.
  watcher.CheckEqual({HotseatState::kExtended});

  // Press home button - expect transition to home (with hotseat in kShown
  // state, and in app shelf hidden).
  views::View* home_button =
      GetPrimaryShelf()->navigation_widget()->GetHomeButton();
  GetEventGenerator()->GestureTapAt(
      home_button->GetBoundsInScreen().CenterPoint());

  GetAppListTestHelper()->CheckVisibility(true);
  EXPECT_FALSE(overview_controller->InOverviewSession());
  EXPECT_FALSE(ShelfConfig::Get()->is_in_app());

  watcher.CheckEqual({HotseatState::kExtended, HotseatState::kShown});
}

// Tests that in-app -> overview results in only one state change with an
// autohidden shelf.
TEST_P(HotseatWidgetTest, InAppToOverviewChangesStateOnceAutohiddenShelf) {
  GetPrimaryShelf()->SetAutoHideBehavior(ShelfAutoHideBehavior::kAlways);
  TabletModeControllerTestApi().EnterTabletMode();

  // Test going to overview mode using the controller from an autohide hidden
  // shelf. Go to in-app.
  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());
  {
    HotseatStateWatcher watcher(GetShelfLayoutManager());
    // Enter overview by using the controller.
    OverviewAnimationWaiter waiter;
    Shell::Get()->overview_controller()->StartOverview();
    waiter.Wait();

    watcher.CheckEqual({HotseatState::kExtended});
  }
  {
    OverviewAnimationWaiter waiter;
    Shell::Get()->overview_controller()->EndOverview();
    waiter.Wait();
  }

  // Test in-app -> overview again with the autohide shown shelf.
  EXPECT_TRUE(ShelfConfig::Get()->is_in_app());
  EXPECT_EQ(ShelfAutoHideState::SHELF_AUTO_HIDE_HIDDEN,
            GetShelfLayoutManager()->auto_hide_state());
  SwipeUpOnShelf();
  {
    HotseatStateWatcher watcher(GetShelfLayoutManager());
    // Enter overview by using the controller.
    OverviewAnimationWaiter waiter;
    Shell::Get()->overview_controller()->StartOverview();
    waiter.Wait();

    watcher.CheckEqual({});
    EXPECT_EQ(HotseatState::kExtended,
              GetShelfLayoutManager()->hotseat_state());
  }
}

// Tests that going between Applist and overview in tablet mode with no windows
// results in no work area change.
TEST_P(HotseatWidgetTest,
       WorkAreaDoesNotUpdateAppListToFromOverviewWithNoWindow) {
  TabletModeControllerTestApi().EnterTabletMode();
  DisplayWorkAreaChangeCounter counter;

  {
    OverviewAnimationWaiter waiter;
    Shell::Get()->overview_controller()->StartOverview();
    waiter.Wait();
  }

  EXPECT_EQ(0, counter.count());

  {
    OverviewAnimationWaiter waiter;
    Shell::Get()->overview_controller()->EndOverview();
    waiter.Wait();
  }

  EXPECT_EQ(0, counter.count());
}

// Tests that switching between AppList and overview with a window results in no
// work area change.
TEST_P(HotseatWidgetTest,
       WorkAreaDoesNotUpdateAppListToFromOverviewWithWindow) {
  DisplayWorkAreaChangeCounter counter;
  TabletModeControllerTestApi().EnterTabletMode();
  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());
  ASSERT_EQ(1, counter.count());
  views::View* home_button =
      GetPrimaryShelf()->navigation_widget()->GetHomeButton();
  GetEventGenerator()->GestureTapAt(
      home_button->GetBoundsInScreen().CenterPoint());

  {
    OverviewAnimationWaiter waiter;
    gfx::Point overview_button_center = GetPrimaryShelf()
                                            ->status_area_widget()
                                            ->overview_button_tray()
                                            ->GetBoundsInScreen()
                                            .CenterPoint();
    GetEventGenerator()->GestureTapAt(overview_button_center);
    waiter.Wait();
  }

  EXPECT_EQ(1, counter.count());

  {
    OverviewAnimationWaiter waiter;
    // Overview button has moved a bit now that the shelf is in-app.
    gfx::Point overview_button_center = GetPrimaryShelf()
                                            ->status_area_widget()
                                            ->overview_button_tray()
                                            ->GetBoundsInScreen()
                                            .CenterPoint();
    GetEventGenerator()->GestureTapAt(overview_button_center);
    waiter.Wait();
  }

  EXPECT_EQ(1, counter.count());
}

// Tests that switching between AppList and an active window does not update the
// work area.
TEST_P(HotseatWidgetTest, WorkAreaDoesNotUpdateOpenWindowToFromAppList) {
  TabletModeControllerTestApi().EnterTabletMode();
  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());
  ASSERT_TRUE(ShelfConfig::Get()->is_in_app());

  // Go to the home launcher, work area should not update.
  DisplayWorkAreaChangeCounter counter;
  views::View* home_button =
      GetPrimaryShelf()->navigation_widget()->GetHomeButton();
  GetEventGenerator()->GestureTapAt(
      home_button->GetBoundsInScreen().CenterPoint());

  GetAppListTestHelper()->CheckVisibility(true);
  EXPECT_EQ(0, counter.count());

  // Go back to the window, work area should not update.
  wm::ActivateWindow(window.get());

  EXPECT_TRUE(ShelfConfig::Get()->is_in_app());
  EXPECT_EQ(0, counter.count());
}

// Tests that switching between overview and an active window does not update
// the work area.
TEST_P(HotseatWidgetTest, WorkAreaDoesNotUpdateOpenWindowToFromOverview) {
  TabletModeControllerTestApi().EnterTabletMode();
  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());
  ASSERT_TRUE(ShelfConfig::Get()->is_in_app());

  // Go to overview, there should not be a work area update.
  DisplayWorkAreaChangeCounter counter;
  {
    OverviewAnimationWaiter waiter;
    gfx::Point overview_button_center = GetPrimaryShelf()
                                            ->status_area_widget()
                                            ->overview_button_tray()
                                            ->GetBoundsInScreen()
                                            .CenterPoint();
    GetEventGenerator()->GestureTapAt(overview_button_center);
    waiter.Wait();
  }

  EXPECT_EQ(0, counter.count());

  // Go back to the app, there should not be a work area update.
  wm::ActivateWindow(window.get());

  EXPECT_TRUE(ShelfConfig::Get()->is_in_app());
  EXPECT_EQ(0, counter.count());
}

// Tests that the shelf opaque background is properly updated after a tablet
// mode transition with no apps.
TEST_P(HotseatWidgetTest, ShelfBackgroundNotVisibleInTabletModeNoApps) {
  TabletModeControllerTestApi().EnterTabletMode();

  EXPECT_FALSE(GetShelfWidget()->GetOpaqueBackground()->visible());
}

// Tests that the shelf opaque background is properly updated after a tablet
// mode transition with no apps with dense shelf.
TEST_P(HotseatWidgetTest, DenseShelfBackgroundNotVisibleInTabletModeNoApps) {
  UpdateDisplay("300x1000");
  TabletModeControllerTestApi().EnterTabletMode();

  EXPECT_FALSE(GetShelfWidget()->GetOpaqueBackground()->visible());
}

// Tests that the hotseat is extended if focused with a keyboard.
TEST_P(HotseatWidgetTest, ExtendHotseatIfFocusedWithKeyboard) {
  TabletModeControllerTestApi().EnterTabletMode();
  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());
  ASSERT_EQ(HotseatState::kHidden, GetShelfLayoutManager()->hotseat_state());

  // Focus the shelf. Hotseat should now show extended.
  GetPrimaryShelf()->shelf_focus_cycler()->FocusShelf(false /* last_element */);
  EXPECT_EQ(HotseatState::kExtended, GetShelfLayoutManager()->hotseat_state());

  // Focus the navigation widget. Hotseat should now hide, as it was
  // automatically extended by focusing it.
  GetPrimaryShelf()->shelf_focus_cycler()->FocusNavigation(
      false /* last_element */);
  EXPECT_EQ(HotseatState::kHidden, GetShelfLayoutManager()->hotseat_state());

  // Now swipe up to show the shelf and then focus it with the keyboard. Hotseat
  // should keep extended.
  SwipeUpOnShelf();
  GetPrimaryShelf()->shelf_focus_cycler()->FocusShelf(false /* last_element */);
  EXPECT_EQ(HotseatState::kExtended, GetShelfLayoutManager()->hotseat_state());

  // Now focus the navigation widget again. Hotseat should remain shown, as it
  // was manually extended.
  GetPrimaryShelf()->shelf_focus_cycler()->FocusNavigation(
      false /* last_element */);
  EXPECT_EQ(HotseatState::kExtended, GetShelfLayoutManager()->hotseat_state());
}

// Tests that if the hotseat was hidden while being focused, doing a traversal
// focus on the next element brings it up again.
TEST_P(HotseatWidgetTest, SwipeDownOnFocusedHotseat) {
  TabletModeControllerTestApi().EnterTabletMode();
  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());
  ShelfTestUtil::AddAppShortcut("app_id_1", TYPE_APP);
  ShelfTestUtil::AddAppShortcut("app_id_2", TYPE_APP);
  ASSERT_EQ(HotseatState::kHidden, GetShelfLayoutManager()->hotseat_state());

  // Focus the shelf, then swipe down on the shelf to hide it. Hotseat should be
  // hidden.
  GetPrimaryShelf()->shelf_focus_cycler()->FocusShelf(false /* last_element */);
  gfx::Rect hotseat_bounds =
      GetShelfWidget()->hotseat_widget()->GetWindowBoundsInScreen();
  gfx::Point start = hotseat_bounds.top_center();
  gfx::Point end = start + gfx::Vector2d(0, 80);
  GetEventGenerator()->GestureScrollSequence(
      start, end, base::TimeDelta::FromMilliseconds(100), 4 /*scroll_steps*/);
  EXPECT_EQ(HotseatState::kHidden, GetShelfLayoutManager()->hotseat_state());

  // Focus to the next element in the hotseat. The hotseat should show again.
  GetEventGenerator()->PressKey(ui::VKEY_TAB, 0);
  GetEventGenerator()->ReleaseKey(ui::VKEY_TAB, 0);
  EXPECT_EQ(HotseatState::kExtended, GetShelfLayoutManager()->hotseat_state());
}

// Tests that in overview, we can still exit by clicking on the hotseat if the
// point is not on the visible area.
TEST_P(HotseatWidgetTest, ExitOverviewWithClickOnHotseat) {
  std::unique_ptr<aura::Window> window1 = AshTestBase::CreateTestWindow();
  ShelfTestUtil::AddAppShortcut("app_id_1", TYPE_APP);

  TabletModeControllerTestApi().EnterTabletMode();
  ASSERT_TRUE(TabletModeControllerTestApi().IsTabletModeStarted());
  ASSERT_FALSE(WindowState::Get(window1.get())->IsMinimized());

  // Enter overview, hotseat is visible. Choose the point to the farthest left.
  // This point will not be visible.
  auto* overview_controller = Shell::Get()->overview_controller();
  auto* hotseat_widget = GetShelfWidget()->hotseat_widget();
  overview_controller->StartOverview();
  ASSERT_TRUE(overview_controller->InOverviewSession());
  ASSERT_EQ(HotseatState::kExtended, GetShelfLayoutManager()->hotseat_state());
  gfx::Point far_left_point =
      hotseat_widget->GetWindowBoundsInScreen().left_center();

  // Tests that on clicking, we exit overview and all windows are minimized.
  GetEventGenerator()->set_current_screen_location(far_left_point);
  GetEventGenerator()->ClickLeftButton();
  EXPECT_EQ(HotseatState::kShown, GetShelfLayoutManager()->hotseat_state());
  EXPECT_TRUE(WindowState::Get(window1.get())->IsMinimized());
  EXPECT_FALSE(overview_controller->InOverviewSession());
}

// Hides the hotseat if the hotseat is in kExtendedMode and the system tray
// is about to show (see https://crbug.com/1028321).
TEST_P(HotseatWidgetTest, DismissHotseatWhenSystemTrayShows) {
  GetPrimaryShelf()->SetAutoHideBehavior(shelf_auto_hide_behavior());

  TabletModeControllerTestApi().EnterTabletMode();
  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());

  SwipeUpOnShelf();
  ASSERT_EQ(HotseatState::kExtended, GetShelfLayoutManager()->hotseat_state());

  // Activates the system tray when hotseat is in kExtended mode and waits for
  // the update in system tray to finish.
  StatusAreaWidget* status_area_widget = GetShelfWidget()->status_area_widget();
  const gfx::Point status_area_widget_center =
      status_area_widget->GetNativeView()->GetBoundsInScreen().CenterPoint();
  GetEventGenerator()->GestureTapAt(status_area_widget_center);
  base::RunLoop().RunUntilIdle();

  // Expects that the system tray shows and the hotseat is hidden.
  EXPECT_EQ(HotseatState::kHidden, GetShelfLayoutManager()->hotseat_state());
  EXPECT_TRUE(status_area_widget->unified_system_tray()->IsBubbleShown());

  // Early out since the remaining code is only meaningful for auto-hide shelf.
  if (GetPrimaryShelf()->auto_hide_behavior() !=
      ShelfAutoHideBehavior::kAlways) {
    return;
  }

  // Auto-hide shelf should show when opening the system tray.
  EXPECT_EQ(ShelfAutoHideState::SHELF_AUTO_HIDE_SHOWN,
            GetShelfLayoutManager()->auto_hide_state());

  // Auto-hide shelf should hide when closing the system tray.
  GetEventGenerator()->GestureTapAt(status_area_widget_center);

  // Waits for the system tray to be closed.
  base::RunLoop().RunUntilIdle();

  EXPECT_EQ(ShelfAutoHideState::SHELF_AUTO_HIDE_HIDDEN,
            GetShelfLayoutManager()->auto_hide_state());
}

// Tests that the work area updates once each when going to/from tablet mode
// with no windows open.
TEST_P(HotseatWidgetTest, WorkAreaUpdatesClamshellToFromHomeLauncherNoWindows) {
  DisplayWorkAreaChangeCounter counter;
  TabletModeControllerTestApi().EnterTabletMode();

  EXPECT_EQ(1, counter.count());

  TabletModeControllerTestApi().LeaveTabletMode();

  EXPECT_EQ(2, counter.count());
}

// Tests that the work area changes just once when opening a window in tablet
// mode.
TEST_P(HotseatWidgetTest, OpenWindowInTabletModeChangesWorkArea) {
  DisplayWorkAreaChangeCounter counter;
  TabletModeControllerTestApi().EnterTabletMode();
  ASSERT_EQ(1, counter.count());

  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());

  EXPECT_EQ(1, counter.count());
}

// Tests that going to and from tablet mode with an open window results in a
// work area change.
TEST_P(HotseatWidgetTest, ToFromTabletModeWithWindowChangesWorkArea) {
  DisplayWorkAreaChangeCounter counter;
  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());

  TabletModeControllerTestApi().EnterTabletMode();
  EXPECT_EQ(1, counter.count());

  TabletModeControllerTestApi().LeaveTabletMode();
  EXPECT_EQ(2, counter.count());
}

// Tests that the hotseat is flush with the bottom of the screen when in
// clamshell mode and the shelf is oriented on the bottom.
TEST_P(HotseatWidgetTest, HotseatFlushWithScreenBottomInClamshell) {
  GetPrimaryShelf()->SetAutoHideBehavior(shelf_auto_hide_behavior());
  const int display_height =
      display::Screen::GetScreen()->GetPrimaryDisplay().bounds().height();
  const int hotseat_bottom = GetPrimaryShelf()
                                 ->shelf_widget()
                                 ->hotseat_widget()
                                 ->GetWindowBoundsInScreen()
                                 .bottom();
  EXPECT_EQ(hotseat_bottom, display_height);
}

// Tests that when hotseat and drag-window-to-overview features are both
// enabled, HomeLauncherGestureHandler can receive and process events properly.
TEST_P(HotseatWidgetTest, DragActiveWindowInTabletMode) {
  base::test::ScopedFeatureList scoped_features;
  scoped_features.InitAndEnableFeature(
      features::kDragFromShelfToHomeOrOverview);

  GetPrimaryShelf()->SetAutoHideBehavior(shelf_auto_hide_behavior());
  TabletModeControllerTestApi().EnterTabletMode();
  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());

  // Swipe up to bring up the hotseat first.
  SwipeUpOnShelf();
  ASSERT_EQ(HotseatState::kExtended, GetShelfLayoutManager()->hotseat_state());

  // Now swipe up again to start drag the active window.
  ui::test::EventGenerator* generator = GetEventGenerator();
  const gfx::Rect bottom_shelf_bounds =
      GetShelfWidget()->GetWindowBoundsInScreen();
  generator->MoveMouseTo(bottom_shelf_bounds.CenterPoint());
  generator->PressTouch();
  EXPECT_TRUE(window->layer()->transform().IsIdentity());

  // Drag upward, test the window transform changes.
  const gfx::Rect display_bounds =
      display::Screen::GetScreen()->GetPrimaryDisplay().bounds();
  generator->MoveTouch(display_bounds.CenterPoint());
  const gfx::Transform upward_transform = window->layer()->transform();
  EXPECT_FALSE(upward_transform.IsIdentity());
  // Drag downwad, test the window tranfrom changes.
  generator->MoveTouch(display_bounds.bottom_center());
  const gfx::Transform downward_transform = window->layer()->transform();
  EXPECT_NE(upward_transform, downward_transform);

  generator->ReleaseTouch();
  EXPECT_TRUE(window->layer()->transform().IsIdentity());
}

// Tests that when hotseat and drag-window-to-overview features are both
// enabled, hotseat is not extended after dragging a window to overview, and
// then activating the window.
TEST_P(HotseatWidgetTest, ExitingOvervieHidesHotseat) {
  base::test::ScopedFeatureList scoped_features;
  scoped_features.InitAndEnableFeature(
      features::kDragFromShelfToHomeOrOverview);

  const ShelfAutoHideBehavior auto_hide_behavior = shelf_auto_hide_behavior();
  GetPrimaryShelf()->SetAutoHideBehavior(auto_hide_behavior);
  TabletModeControllerTestApi().EnterTabletMode();

  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());

  // If the shelf is auto-hidden, swipe up to bring up shelf and hotseat first
  // (otherwise, the window drag to overview will not be handled).
  if (auto_hide_behavior == ShelfAutoHideBehavior::kAlways) {
    SwipeUpOnShelf();
    ASSERT_EQ(HotseatState::kExtended,
              GetShelfLayoutManager()->hotseat_state());
  }

  // Swipe up to start dragging the active window.
  const gfx::Rect bottom_shelf_bounds =
      GetShelfWidget()->GetWindowBoundsInScreen();
  StartScroll(bottom_shelf_bounds.CenterPoint());

  // Drag upward, to the center of the screen, and release (this should enter
  // the overview).
  const gfx::Rect display_bounds =
      display::Screen::GetScreen()->GetPrimaryDisplay().bounds();
  UpdateScroll(display_bounds.CenterPoint().y() -
               bottom_shelf_bounds.CenterPoint().y());
  // Small scroll update, to simulate the user holding the pointer.
  UpdateScroll(2);
  DragWindowFromShelfController* window_drag_controller =
      GetShelfLayoutManager()->window_drag_controller_for_testing();
  ASSERT_TRUE(window_drag_controller);
  DragWindowFromShelfControllerTestApi test_api;
  test_api.WaitUntilOverviewIsShown(window_drag_controller);
  EndScroll(/*is_fling=*/false, 0.f);

  OverviewController* overview_controller = Shell::Get()->overview_controller();
  EXPECT_EQ(HotseatState::kExtended, GetShelfLayoutManager()->hotseat_state());
  EXPECT_TRUE(overview_controller->InOverviewSession());

  // Activate the window - the overview session should exit, and hotseat should
  // be hidden.
  wm::ActivateWindow(window.get());
  EXPECT_FALSE(overview_controller->InOverviewSession());
  EXPECT_EQ(HotseatState::kHidden, GetShelfLayoutManager()->hotseat_state());
}

// Tests that failing to drag the maximized window to overview mode results in
// an extended hotseat.
TEST_P(HotseatWidgetTest, FailingOverviewDragResultsInExtendedHotseat) {
  base::test::ScopedFeatureList scoped_features;
  scoped_features.InitAndEnableFeature(
      features::kDragFromShelfToHomeOrOverview);

  const ShelfAutoHideBehavior auto_hide_behavior = shelf_auto_hide_behavior();
  GetPrimaryShelf()->SetAutoHideBehavior(auto_hide_behavior);
  TabletModeControllerTestApi().EnterTabletMode();

  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());

  // If the shelf is auto-hidden, swipe up to bring up shelf and hotseat first
  // (otherwise, the window drag to overview will not be handled).
  if (auto_hide_behavior == ShelfAutoHideBehavior::kAlways) {
    SwipeUpOnShelf();
    ASSERT_EQ(HotseatState::kExtended,
              GetShelfLayoutManager()->hotseat_state());
  }

  // Swipe up to start dragging the active window.
  const gfx::Rect bottom_shelf_bounds =
      GetShelfWidget()->GetWindowBoundsInScreen();
  StartScroll(bottom_shelf_bounds.top_center());

  // Drag upward, a bit past the hotseat extended height but not enough to go to
  // overview.
  const int extended_hotseat_distance_from_top_of_shelf =
      ShelfConfig::Get()->hotseat_bottom_padding() +
      ShelfConfig::Get()->hotseat_size();
  UpdateScroll(-extended_hotseat_distance_from_top_of_shelf - 30);
  EndScroll(/*is_fling=*/false, 0.f);

  ASSERT_FALSE(Shell::Get()->overview_controller()->InOverviewSession());
  EXPECT_EQ(HotseatState::kExtended, GetShelfLayoutManager()->hotseat_state());
}

// Tests that hotseat remains in extended state while in overview mode when
// flinging the shelf up or down.
TEST_P(HotseatWidgetTest, SwipeOnHotseatInOverview) {
  GetPrimaryShelf()->SetAutoHideBehavior(shelf_auto_hide_behavior());
  TabletModeControllerTestApi().EnterTabletMode();

  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());

  OverviewController* overview_controller = Shell::Get()->overview_controller();
  overview_controller->StartOverview();

  Shelf* const shelf = GetPrimaryShelf();

  SwipeUpOnShelf();

  EXPECT_TRUE(overview_controller->InOverviewSession());
  EXPECT_EQ(HotseatState::kExtended, GetShelfLayoutManager()->hotseat_state());
  if (shelf_auto_hide_behavior() == ShelfAutoHideBehavior::kAlways) {
    EXPECT_EQ(SHELF_AUTO_HIDE, shelf->GetVisibilityState());
    EXPECT_EQ(SHELF_AUTO_HIDE_SHOWN, shelf->GetAutoHideState());
  } else {
    EXPECT_EQ(SHELF_VISIBLE, shelf->GetVisibilityState());
  }

  // Drag from the hotseat to the bezel, the hotseat should remain in extended
  // state.
  DragHotseatDownToBezel();

  EXPECT_TRUE(overview_controller->InOverviewSession());
  EXPECT_EQ(HotseatState::kExtended, GetShelfLayoutManager()->hotseat_state());
  if (shelf_auto_hide_behavior() == ShelfAutoHideBehavior::kAlways) {
    EXPECT_EQ(SHELF_AUTO_HIDE, shelf->GetVisibilityState());
    EXPECT_EQ(SHELF_AUTO_HIDE_SHOWN, shelf->GetAutoHideState());
  } else {
    EXPECT_EQ(SHELF_VISIBLE, shelf->GetVisibilityState());
  }

  SwipeUpOnShelf();

  EXPECT_TRUE(overview_controller->InOverviewSession());
  EXPECT_EQ(HotseatState::kExtended, GetShelfLayoutManager()->hotseat_state());
  if (shelf_auto_hide_behavior() == ShelfAutoHideBehavior::kAlways) {
    EXPECT_EQ(SHELF_AUTO_HIDE, shelf->GetVisibilityState());
    EXPECT_EQ(SHELF_AUTO_HIDE_SHOWN, shelf->GetAutoHideState());
  } else {
    EXPECT_EQ(SHELF_VISIBLE, shelf->GetVisibilityState());
  }
}

TEST_P(HotseatWidgetTest, SwipeOnHotseatInSplitViewWithOverview) {
  Shelf* const shelf = GetPrimaryShelf();
  shelf->SetAutoHideBehavior(shelf_auto_hide_behavior());
  TabletModeControllerTestApi().EnterTabletMode();

  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());

  OverviewController* overview_controller = Shell::Get()->overview_controller();
  overview_controller->StartOverview();

  SplitViewController* split_view_controller =
      SplitViewController::Get(Shell::GetPrimaryRootWindow());
  split_view_controller->SnapWindow(window.get(), SplitViewController::LEFT);

  SwipeUpOnShelf();

  EXPECT_TRUE(split_view_controller->InSplitViewMode());
  EXPECT_TRUE(overview_controller->InOverviewSession());
  EXPECT_EQ(HotseatState::kExtended, GetShelfLayoutManager()->hotseat_state());
  if (shelf_auto_hide_behavior() == ShelfAutoHideBehavior::kAlways) {
    EXPECT_EQ(SHELF_AUTO_HIDE, shelf->GetVisibilityState());
    EXPECT_EQ(SHELF_AUTO_HIDE_SHOWN, shelf->GetAutoHideState());
  } else {
    EXPECT_EQ(SHELF_VISIBLE, shelf->GetVisibilityState());
  }

  DragHotseatDownToBezel();

  EXPECT_TRUE(split_view_controller->InSplitViewMode());
  EXPECT_TRUE(overview_controller->InOverviewSession());
  EXPECT_EQ(HotseatState::kHidden, GetShelfLayoutManager()->hotseat_state());
  if (shelf_auto_hide_behavior() == ShelfAutoHideBehavior::kAlways) {
    EXPECT_EQ(SHELF_AUTO_HIDE, shelf->GetVisibilityState());
    EXPECT_EQ(SHELF_AUTO_HIDE_SHOWN, shelf->GetAutoHideState());
  } else {
    EXPECT_EQ(SHELF_VISIBLE, shelf->GetVisibilityState());
  }

  SwipeUpOnShelf();

  EXPECT_TRUE(split_view_controller->InSplitViewMode());
  EXPECT_TRUE(overview_controller->InOverviewSession());
  EXPECT_EQ(HotseatState::kExtended, GetShelfLayoutManager()->hotseat_state());
  if (shelf_auto_hide_behavior() == ShelfAutoHideBehavior::kAlways) {
    EXPECT_EQ(SHELF_AUTO_HIDE, shelf->GetVisibilityState());
    EXPECT_EQ(SHELF_AUTO_HIDE_SHOWN, shelf->GetAutoHideState());
  } else {
    EXPECT_EQ(SHELF_VISIBLE, shelf->GetVisibilityState());
  }
}

TEST_P(HotseatWidgetTest, SwipeOnHotseatInSplitView) {
  Shelf* const shelf = GetPrimaryShelf();
  shelf->SetAutoHideBehavior(shelf_auto_hide_behavior());
  TabletModeControllerTestApi().EnterTabletMode();

  std::unique_ptr<aura::Window> window1 =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  std::unique_ptr<aura::Window> window2 =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window1.get());

  SplitViewController* split_view_controller =
      SplitViewController::Get(Shell::GetPrimaryRootWindow());
  split_view_controller->SnapWindow(window1.get(), SplitViewController::LEFT);
  split_view_controller->SnapWindow(window2.get(), SplitViewController::RIGHT);
  EXPECT_TRUE(split_view_controller->InSplitViewMode());

  SwipeUpOnShelf();

  EXPECT_TRUE(split_view_controller->InSplitViewMode());
  EXPECT_EQ(HotseatState::kExtended, GetShelfLayoutManager()->hotseat_state());
  if (shelf_auto_hide_behavior() == ShelfAutoHideBehavior::kAlways) {
    EXPECT_EQ(SHELF_AUTO_HIDE, shelf->GetVisibilityState());
    EXPECT_EQ(SHELF_AUTO_HIDE_SHOWN, shelf->GetAutoHideState());
  } else {
    EXPECT_EQ(SHELF_VISIBLE, shelf->GetVisibilityState());
  }

  DragHotseatDownToBezel();

  EXPECT_TRUE(split_view_controller->InSplitViewMode());
  EXPECT_EQ(HotseatState::kHidden, GetShelfLayoutManager()->hotseat_state());
  if (shelf_auto_hide_behavior() == ShelfAutoHideBehavior::kAlways) {
    EXPECT_EQ(SHELF_AUTO_HIDE, shelf->GetVisibilityState());
    EXPECT_EQ(SHELF_AUTO_HIDE_HIDDEN, shelf->GetAutoHideState());
  } else {
    EXPECT_EQ(SHELF_VISIBLE, shelf->GetVisibilityState());
  }

  SwipeUpOnShelf();

  EXPECT_TRUE(split_view_controller->InSplitViewMode());
  EXPECT_EQ(HotseatState::kExtended, GetShelfLayoutManager()->hotseat_state());
  if (shelf_auto_hide_behavior() == ShelfAutoHideBehavior::kAlways) {
    EXPECT_EQ(SHELF_AUTO_HIDE, shelf->GetVisibilityState());
    EXPECT_EQ(SHELF_AUTO_HIDE_SHOWN, shelf->GetAutoHideState());
  } else {
    EXPECT_EQ(SHELF_VISIBLE, shelf->GetVisibilityState());
  }
}

// Tests that swiping downward, towards the bezel, from a variety of points
// results in hiding the hotseat.
TEST_P(HotseatWidgetTest, HotseatHidesWhenSwipedToBezel) {
  // Go to in-app shelf and extend the hotseat.
  TabletModeControllerTestApi().EnterTabletMode();
  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());
  SwipeUpOnShelf();

  // Drag from the hotseat to the bezel, the hotseat should hide.
  DragHotseatDownToBezel();
  EXPECT_EQ(HotseatState::kHidden, GetShelfLayoutManager()->hotseat_state());

  // Reset the hotseat and swipe from the center of the hotseat, it should hide.
  SwipeUpOnShelf();

  gfx::Rect shelf_widget_bounds = GetShelfWidget()->GetWindowBoundsInScreen();
  gfx::Rect hotseat_bounds =
      GetShelfWidget()->hotseat_widget()->GetWindowBoundsInScreen();
  gfx::Point start = hotseat_bounds.CenterPoint();
  const gfx::Point end =
      gfx::Point(shelf_widget_bounds.x() + shelf_widget_bounds.width() / 2,
                 shelf_widget_bounds.bottom() + 1);
  const base::TimeDelta kTimeDelta = base::TimeDelta::FromMilliseconds(100);
  const int kNumScrollSteps = 4;

  GetEventGenerator()->GestureScrollSequence(start, end, kTimeDelta,
                                             kNumScrollSteps);

  EXPECT_EQ(HotseatState::kHidden, GetShelfLayoutManager()->hotseat_state());

  // Reset the hotseat and swipe from the bottom of the hotseat, it should hide.
  SwipeUpOnShelf();

  start = hotseat_bounds.bottom_center();
  start.Offset(0, -1);
  GetEventGenerator()->GestureScrollSequence(start, end, kTimeDelta,
                                             kNumScrollSteps);

  EXPECT_EQ(HotseatState::kHidden, GetShelfLayoutManager()->hotseat_state());

  // Reset the hotseat and swipe from the center of the in-app shelf, it should
  // hide.
  SwipeUpOnShelf();

  start = shelf_widget_bounds.CenterPoint();

  GetEventGenerator()->GestureScrollSequence(start, end, kTimeDelta,
                                             kNumScrollSteps);

  EXPECT_EQ(HotseatState::kHidden, GetShelfLayoutManager()->hotseat_state());

  // Reset the hotseat and swipe from the bottom of the in-app shelf, it should
  // hide.
  SwipeUpOnShelf();

  EXPECT_EQ(HotseatState::kExtended, GetShelfLayoutManager()->hotseat_state());
  start = shelf_widget_bounds.bottom_center();
  // The first few events which get sent to ShelfLayoutManager are
  // ui::ET_TAP_DOWN, and ui::ET_GESTURE_START. After a few px we get
  // ui::ET_GESTURE_SCROLL_UPDATE. Add 6 px of slop to get the first events out
  // of the way, and 1 extra px to ensure we are not on the bottom edge of the
  // display.
  start.Offset(0, -7);

  GetEventGenerator()->GestureScrollSequence(start, end, kTimeDelta,
                                             kNumScrollSteps);

  EXPECT_EQ(HotseatState::kHidden, GetShelfLayoutManager()->hotseat_state());
}

// Tests that flinging up the in-app shelf should show the hotseat.
TEST_P(HotseatWidgetTest, FlingUpHotseatWithShortFling) {
  TabletModeControllerTestApi().EnterTabletMode();
  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());
  GetAppListTestHelper()->CheckVisibility(false);

  base::HistogramTester histogram_tester;
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeUpToShow, 0);

  // Scrolls the hotseat by a distance not sufficuent to trigger the action of
  // entering home screen from the in-app shelf.
  gfx::Rect display_bounds =
      display::Screen::GetScreen()->GetPrimaryDisplay().bounds();
  const gfx::Point start(display_bounds.bottom_center());
  const gfx::Point end(start + gfx::Vector2d(0, -20));

  const int fling_speed =
      DragWindowFromShelfController::kVelocityToHomeScreenThreshold + 1;
  const int scroll_steps = 20;
  base::TimeDelta scroll_time =
      GetEventGenerator()->CalculateScrollDurationForFlingVelocity(
          start, end, fling_speed, scroll_steps);
  GetEventGenerator()->GestureScrollSequence(start, end, scroll_time,
                                             scroll_steps);
  base::RunLoop().RunUntilIdle();

  EXPECT_EQ(HotseatState::kExtended, GetShelfLayoutManager()->hotseat_state());
  GetAppListTestHelper()->CheckVisibility(false);
  histogram_tester.ExpectBucketCount(kHotseatGestureHistogramName,
                                     InAppShelfGestures::kSwipeUpToShow, 1);
}

// Tests that flinging up the in-app shelf should show the home launcher if the
// gesture distance is long enough.
TEST_P(HotseatWidgetTest, FlingUpHotseatWithLongFling) {
  TabletModeControllerTestApi().EnterTabletMode();
  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());
  GetAppListTestHelper()->CheckVisibility(false);

  base::HistogramTester histogram_tester;
  histogram_tester.ExpectBucketCount(
      kHotseatGestureHistogramName,
      InAppShelfGestures::kFlingUpToShowHomeScreen, 0);

  // Scrolls the hotseat by the sufficient distance to trigger the action of
  // entering home screen from the in-app shelf.
  gfx::Rect display_bounds =
      display::Screen::GetScreen()->GetPrimaryDisplay().bounds();
  const gfx::Point start(display_bounds.bottom_center());
  const gfx::Point end(start + gfx::Vector2d(0, -200));

  const int fling_speed =
      DragWindowFromShelfController::kVelocityToHomeScreenThreshold + 1;
  const int scroll_steps = 20;
  base::TimeDelta scroll_time =
      GetEventGenerator()->CalculateScrollDurationForFlingVelocity(
          start, end, fling_speed, scroll_steps);
  GetEventGenerator()->GestureScrollSequence(start, end, scroll_time,
                                             scroll_steps);
  base::RunLoop().RunUntilIdle();

  EXPECT_EQ(HotseatState::kShown, GetShelfLayoutManager()->hotseat_state());
  GetAppListTestHelper()->CheckVisibility(true);
  histogram_tester.ExpectBucketCount(
      kHotseatGestureHistogramName,
      InAppShelfGestures::kFlingUpToShowHomeScreen, 1);
}

// Tests that UpdateVisibilityState is ignored during a shelf drag. This
// prevents drag from getting interrupted.
TEST_P(HotseatWidgetTest, NoVisibilityStateUpdateDuringDrag) {
  // Autohide the shelf, then start a shelf drag.
  GetPrimaryShelf()->SetAutoHideBehavior(ShelfAutoHideBehavior::kAlways);
  std::unique_ptr<aura::Window> window1 =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window1.get());
  ASSERT_EQ(SHELF_AUTO_HIDE_HIDDEN, GetPrimaryShelf()->GetAutoHideState());

  // Drag the autohidden shelf up a bit, then open a new window and activate it
  // during the drag. The shelf state should not change.
  gfx::Point start_drag = GetVisibleShelfWidgetBoundsInScreen().top_center();
  GetEventGenerator()->set_current_screen_location(start_drag);
  GetEventGenerator()->PressTouch();
  GetEventGenerator()->MoveTouchBy(0, -2);
  auto shelf_state_watcher = std::make_unique<ShelfStateWatcher>();
  std::unique_ptr<aura::Window> window2 =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));

  wm::ActivateWindow(window2.get());
  window2->SetBounds(gfx::Rect(0, 0, 200, 200));

  EXPECT_EQ(0, shelf_state_watcher->state_change_count());
}

// Tests that popups don't activate the hotseat. (crbug.com/1018266)
TEST_P(HotseatWidgetTest, HotseatRemainsHiddenIfPopupLaunched) {
  // Go to in-app shelf and extend the hotseat.
  TabletModeControllerTestApi().EnterTabletMode();
  std::unique_ptr<aura::Window> window =
      AshTestBase::CreateTestWindow(gfx::Rect(0, 0, 400, 400));
  wm::ActivateWindow(window.get());
  SwipeUpOnShelf();
  EXPECT_EQ(HotseatState::kExtended, GetShelfLayoutManager()->hotseat_state());

  // Hide hotseat by clicking outside its bounds.
  gfx::Rect hotseat_bounds =
      GetShelfWidget()->hotseat_widget()->GetWindowBoundsInScreen();
  gfx::Point start = hotseat_bounds.top_center();
  GetEventGenerator()->GestureTapAt(gfx::Point(start.x() + 1, start.y() - 1));
  EXPECT_EQ(HotseatState::kHidden, GetShelfLayoutManager()->hotseat_state());

  // Create a popup window and wait until all actions finish. The hotseat should
  // remain hidden.
  aura::Window* window_2 = CreateTestWindowInParent(window.get());
  window_2->SetBounds(gfx::Rect(201, 0, 100, 100));
  window_2->SetProperty(aura::client::kShowStateKey, ui::SHOW_STATE_NORMAL);
  window_2->Show();
  GetAppListTestHelper()->WaitUntilIdle();
  EXPECT_EQ(HotseatState::kHidden, GetShelfLayoutManager()->hotseat_state());
}

}  // namespace ash
