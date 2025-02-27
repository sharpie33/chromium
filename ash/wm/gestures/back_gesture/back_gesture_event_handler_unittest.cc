// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/wm/gestures/back_gesture/back_gesture_event_handler.h"

#include "ash/accelerators/accelerator_controller_impl.h"
#include "ash/app_list/test/app_list_test_helper.h"
#include "ash/app_list/views/app_list_view.h"
#include "ash/app_list/views/search_box_view.h"
#include "ash/display/screen_orientation_controller.h"
#include "ash/display/screen_orientation_controller_test_api.h"
#include "ash/home_screen/home_screen_controller.h"
#include "ash/public/cpp/ash_features.h"
#include "ash/screen_util.h"
#include "ash/session/session_controller_impl.h"
#include "ash/shell.h"
#include "ash/test/ash_test_base.h"
#include "ash/test_shell_delegate.h"
#include "ash/wm/overview/overview_controller.h"
#include "ash/wm/splitview/split_view_divider.h"
#include "ash/wm/tablet_mode/tablet_mode_controller_test_api.h"
#include "ash/wm/tablet_mode/tablet_mode_window_manager.h"
#include "ash/wm/window_state.h"
#include "ash/wm/window_util.h"
#include "ash/wm/wm_event.h"
#include "base/test/scoped_feature_list.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/accelerators/test_accelerator_target.h"
#include "ui/display/test/display_manager_test_api.h"

namespace ash {

class BackGestureEventHandlerTest : public AshTestBase {
 public:
  // Distance that swiping from left edge to let the affordance achieve
  // activated state.
  static constexpr int kSwipingDistanceForGoingBack = 80;

  BackGestureEventHandlerTest() = default;
  ~BackGestureEventHandlerTest() override = default;

  void SetUp() override {
    AshTestBase::SetUp();

    feature_list_.InitAndEnableFeature(features::kSwipingFromLeftEdgeToGoBack);
    top_window_ = CreateAppWindow(gfx::Rect(), AppType::BROWSER);
    TabletModeControllerTestApi().EnterTabletMode();
  }

  void TearDown() override {
    top_window_.reset();
    AshTestBase::TearDown();
  }

  void RegisterBackPressAndRelease(ui::TestAcceleratorTarget* back_press,
                                   ui::TestAcceleratorTarget* back_release) {
    AcceleratorControllerImpl* controller =
        Shell::Get()->accelerator_controller();

    // Register an accelerator that looks for back presses.
    ui::Accelerator accelerator_back_press(ui::VKEY_BROWSER_BACK, ui::EF_NONE);
    accelerator_back_press.set_key_state(ui::Accelerator::KeyState::PRESSED);
    controller->Register({accelerator_back_press}, back_press);

    // Register an accelerator that looks for back releases.
    ui::Accelerator accelerator_back_release(ui::VKEY_BROWSER_BACK,
                                             ui::EF_NONE);
    accelerator_back_release.set_key_state(ui::Accelerator::KeyState::RELEASED);
    controller->Register({accelerator_back_release}, back_release);
  }

  // Send touch event with |type| to the toplevel window event handler.
  void SendTouchEvent(const gfx::Point& position, ui::EventType type) {
    ui::TouchEvent event = ui::TouchEvent(
        type, position, base::TimeTicks::Now(),
        ui::PointerDetails(ui::EventPointerType::POINTER_TYPE_TOUCH,
                           /*pointer_id=*/5, /*radius_x=*/5.0f,
                           /*radius_y=*/5.0, /*force=*/1.0f));
    ui::Event::DispatcherApi(&event).set_target(top_window_.get());
    Shell::Get()->back_gesture_event_handler()->OnTouchEvent(&event);
  }

  aura::Window* top_window() { return top_window_.get(); }

 private:
  base::test::ScopedFeatureList feature_list_;
  std::unique_ptr<aura::Window> top_window_;

  BackGestureEventHandlerTest(const BackGestureEventHandlerTest&) = delete;
  BackGestureEventHandlerTest& operator=(const BackGestureEventHandlerTest&) =
      delete;
};

TEST_F(BackGestureEventHandlerTest, SwipingFromLeftEdgeToGoBack) {
  ui::TestAcceleratorTarget target_back_press, target_back_release;
  RegisterBackPressAndRelease(&target_back_press, &target_back_release);

  // Tests that swiping from the left less than |kSwipingDistanceForGoingBack|
  // should not go to previous page.
  ui::test::EventGenerator* generator = GetEventGenerator();
  const gfx::Point start(0, 100);
  generator->GestureScrollSequence(
      start, gfx::Point(kSwipingDistanceForGoingBack - 10, 100),
      base::TimeDelta::FromMilliseconds(100), 3);
  EXPECT_EQ(0, target_back_press.accelerator_count());
  EXPECT_EQ(0, target_back_release.accelerator_count());

  // Tests that swiping from the left more than |kSwipingDistanceForGoingBack|
  // should go to previous page.
  generator->GestureScrollSequence(
      start, gfx::Point(kSwipingDistanceForGoingBack + 10, 100),
      base::TimeDelta::FromMilliseconds(100), 3);
  EXPECT_EQ(1, target_back_press.accelerator_count());
  EXPECT_EQ(1, target_back_release.accelerator_count());
}

TEST_F(BackGestureEventHandlerTest, FlingFromLeftEdgeToGoBack) {
  ui::TestAcceleratorTarget target_back_press, target_back_release;
  RegisterBackPressAndRelease(&target_back_press, &target_back_release);

  // Tests that fling from the left with velocity smaller than
  // |kFlingVelocityForGoingBack| should not go to previous page.
  // Drag further than |touch_slop| in GestureDetector to trigger scroll
  // sequence. Note, |touch_slop| equals to 15.05, which is the value of
  // |max_touch_move_in_pixels_for_click_| + |kSlopEpsilon|. Generate the scroll
  // sequence with short duration and only one step for FLING scroll gestures.
  // X-velocity here will be 800 dips/seconds.
  ui::test::EventGenerator* generator = GetEventGenerator();
  generator->GestureScrollSequence(gfx::Point(0, 0), gfx::Point(16, 0),
                                   base::TimeDelta::FromMilliseconds(20),
                                   /*steps=*/1);
  EXPECT_EQ(0, target_back_press.accelerator_count());
  EXPECT_EQ(0, target_back_release.accelerator_count());

  // Tests that fling from the left with velocity larger than
  // |kFlingVelocityForGoingBack| should go to previous page. X-velocity here
  // will be 1600 dips/seconds.
  generator->GestureScrollSequence(gfx::Point(0, 0), gfx::Point(16, 0),
                                   base::TimeDelta::FromMilliseconds(1),
                                   /*steps=*/1);
  EXPECT_EQ(1, target_back_press.accelerator_count());
  EXPECT_EQ(1, target_back_release.accelerator_count());

  // Tests that fling from the left with velocity smaller than
  // |kFlingVelocityForGoingBack| but dragged further enough to trigger
  // activated affordance should still go back to previous page. X-velocity here
  // will be 800 dips/seconds and drag distance is 160, which is larger than
  // |kSwipingDistanceForGoingBack|.
  generator->GestureScrollSequence(gfx::Point(0, 0), gfx::Point(160, 0),
                                   base::TimeDelta::FromMilliseconds(200),
                                   /*steps=*/1);
  EXPECT_EQ(2, target_back_press.accelerator_count());
  EXPECT_EQ(2, target_back_release.accelerator_count());
}

TEST_F(BackGestureEventHandlerTest, GoBackInOverviewMode) {
  ui::TestAcceleratorTarget target_back_press, target_back_release;
  RegisterBackPressAndRelease(&target_back_press, &target_back_release);

  ash_test_helper()->test_shell_delegate()->SetCanGoBack(false);
  ASSERT_FALSE(WindowState::Get(top_window())->IsMinimized());
  ASSERT_TRUE(TabletModeWindowManager::ShouldMinimizeTopWindowOnBack());
  GetEventGenerator()->GestureScrollSequence(
      gfx::Point(0, 100), gfx::Point(kSwipingDistanceForGoingBack + 10, 100),
      base::TimeDelta::FromMilliseconds(100), 3);
  // Should trigger window minimize instead of go back.
  EXPECT_EQ(0, target_back_release.accelerator_count());
  EXPECT_TRUE(WindowState::Get(top_window())->IsMinimized());

  WindowState::Get(top_window())->Unminimize();
  ASSERT_FALSE(WindowState::Get(top_window())->IsMinimized());
  auto* shell = Shell::Get();
  shell->overview_controller()->StartOverview();
  ASSERT_TRUE(shell->overview_controller()->InOverviewSession());
  GetEventGenerator()->GestureScrollSequence(
      gfx::Point(0, 100), gfx::Point(kSwipingDistanceForGoingBack + 10, 100),
      base::TimeDelta::FromMilliseconds(100), 3);
  // Should trigger go back instead of minimize the window since it is in
  // overview mode.
  EXPECT_EQ(1, target_back_release.accelerator_count());
}

TEST_F(BackGestureEventHandlerTest, DonotStartGoingBack) {
  ui::TestAcceleratorTarget target_back_press, target_back_release;
  RegisterBackPressAndRelease(&target_back_press, &target_back_release);

  auto* shell = Shell::Get();
  ui::test::EventGenerator* generator = GetEventGenerator();
  const gfx::Point start(0, 100);

  // Should not go back if it is not in ACTIVE session.
  ASSERT_FALSE(shell->overview_controller()->InOverviewSession());
  ASSERT_FALSE(shell->home_screen_controller()->IsHomeScreenVisible());
  GetSessionControllerClient()->SetSessionState(
      session_manager::SessionState::LOCKED);
  generator->GestureScrollSequence(
      start, gfx::Point(kSwipingDistanceForGoingBack + 10, 100),
      base::TimeDelta::FromMilliseconds(100), 3);
  EXPECT_EQ(0, target_back_press.accelerator_count());
  EXPECT_EQ(0, target_back_release.accelerator_count());

  // Should not go back if home screen is visible and in |kFullscreenAllApps|
  // state.
  GetSessionControllerClient()->SetSessionState(
      session_manager::SessionState::ACTIVE);
  shell->home_screen_controller()->GoHome(GetPrimaryDisplay().id());
  ASSERT_TRUE(shell->home_screen_controller()->IsHomeScreenVisible());
  GetAppListTestHelper()->CheckState(AppListViewState::kFullscreenAllApps);
  generator->GestureScrollSequence(
      start, gfx::Point(kSwipingDistanceForGoingBack + 10, 100),
      base::TimeDelta::FromMilliseconds(100), 3);
  EXPECT_EQ(0, target_back_press.accelerator_count());
  EXPECT_EQ(0, target_back_release.accelerator_count());

  // Should exit |kFullscreenSearch| to enter |kFullscreenAllApps| state while
  // home screen search result page is opened.
  generator->GestureTapAt(GetAppListTestHelper()
                              ->GetAppListView()
                              ->search_box_view()
                              ->GetBoundsInScreen()
                              .CenterPoint());
  GetAppListTestHelper()->CheckState(AppListViewState::kFullscreenSearch);
  generator->GestureScrollSequence(
      start, gfx::Point(kSwipingDistanceForGoingBack + 10, 100),
      base::TimeDelta::FromMilliseconds(100), 3);
  EXPECT_EQ(1, target_back_release.accelerator_count());
  GetAppListTestHelper()->CheckState(AppListViewState::kFullscreenAllApps);
}

TEST_F(BackGestureEventHandlerTest, CancelOnScreenRotation) {
  UpdateDisplay("807x407");
  int64_t display_id = display::Screen::GetScreen()->GetPrimaryDisplay().id();
  display::DisplayManager* display_manager = Shell::Get()->display_manager();
  display::test::ScopedSetInternalDisplayId set_internal(display_manager,
                                                         display_id);
  ScreenOrientationControllerTestApi test_api(
      Shell::Get()->screen_orientation_controller());
  ui::TestAcceleratorTarget target_back_press, target_back_release;
  RegisterBackPressAndRelease(&target_back_press, &target_back_release);

  // Set the screen orientation to LANDSCAPE_PRIMARY.
  test_api.SetDisplayRotation(display::Display::ROTATE_0,
                              display::Display::RotationSource::ACTIVE);
  EXPECT_EQ(test_api.GetCurrentOrientation(),
            OrientationLockType::kLandscapePrimary);

  gfx::Point start(0, 100);
  gfx::Point update_and_end(200, 100);
  SendTouchEvent(start, ui::ET_TOUCH_PRESSED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_MOVED);
  // Rotate the screen by 270 degree during drag.
  test_api.SetDisplayRotation(display::Display::ROTATE_270,
                              display::Display::RotationSource::ACTIVE);
  EXPECT_EQ(test_api.GetCurrentOrientation(),
            OrientationLockType::kPortraitPrimary);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_RELEASED);
  // Left edge swipe back should be cancelled due to screen rotation, so the
  // fling event with velocity larger than |kFlingVelocityForGoingBack| above
  // will not trigger actual going back.
  EXPECT_EQ(0, target_back_press.accelerator_count());
  EXPECT_EQ(0, target_back_release.accelerator_count());
}

// Tests back gesture while in split view mode.
TEST_F(BackGestureEventHandlerTest, DragFromSplitViewDivider) {
  std::unique_ptr<aura::Window> window1 = CreateTestWindow();
  std::unique_ptr<aura::Window> window2 = CreateTestWindow();
  ui::TestAcceleratorTarget target_back_press, target_back_release;
  gfx::Rect display_bounds =
      screen_util::GetDisplayWorkAreaBoundsInScreenForActiveDeskContainer(
          window1.get());
  RegisterBackPressAndRelease(&target_back_press, &target_back_release);

  auto* split_view_controller =
      SplitViewController::Get(Shell::GetPrimaryRootWindow());
  split_view_controller->SnapWindow(window1.get(), SplitViewController::LEFT);
  split_view_controller->SnapWindow(window2.get(), SplitViewController::RIGHT);
  ASSERT_TRUE(split_view_controller->InSplitViewMode());
  ASSERT_EQ(SplitViewController::State::kBothSnapped,
            split_view_controller->state());

  gfx::Rect divider_bounds =
      split_view_controller->split_view_divider()->GetDividerBoundsInScreen(
          false);
  ui::test::EventGenerator* generator = GetEventGenerator();
  // Drag from the splitview divider's non-resizable area with larger than
  // |kSwipingDistanceForGoingBack| distance should trigger back gesture. The
  // snapped window should go to previous page and divider's position will not
  // be changed.
  gfx::Point start(divider_bounds.x(), 10);
  gfx::Point end(start.x() + kSwipingDistanceForGoingBack + 10, 10);
  EXPECT_GT(split_view_controller->divider_position(),
            0.33f * display_bounds.width());
  EXPECT_LE(split_view_controller->divider_position(),
            0.5f * display_bounds.width());
  generator->GestureScrollSequence(start, end,
                                   base::TimeDelta::FromMilliseconds(100), 3);
  EXPECT_EQ(SplitViewController::State::kBothSnapped,
            split_view_controller->state());
  EXPECT_EQ(1, target_back_press.accelerator_count());
  EXPECT_EQ(1, target_back_release.accelerator_count());
  EXPECT_GT(split_view_controller->divider_position(),
            0.33f * display_bounds.width());
  EXPECT_LE(split_view_controller->divider_position(),
            0.5f * display_bounds.width());

  // Drag from the divider's resizable area should trigger splitview resizing.
  // Divider's position will be changed and back gesture should not be
  // triggered.
  start = divider_bounds.CenterPoint();
  end = gfx::Point(0.67f * display_bounds.width(), start.y());
  generator->GestureScrollSequence(start, end,
                                   base::TimeDelta::FromMilliseconds(100), 3);
  EXPECT_EQ(1, target_back_press.accelerator_count());
  EXPECT_EQ(1, target_back_release.accelerator_count());
  EXPECT_GT(split_view_controller->divider_position(),
            0.5f * display_bounds.width());
  EXPECT_LE(split_view_controller->divider_position(),
            0.67f * display_bounds.width());
  split_view_controller->EndSplitView();
}

// Tests that in different screen orientations should always activate the
// snapped window in splitview that is underneath the finger. And should be the
// snapped window that is underneath to go back to the previous page.
TEST_F(BackGestureEventHandlerTest, BackInSplitViewMode) {
  int64_t display_id = display::Screen::GetScreen()->GetPrimaryDisplay().id();
  display::DisplayManager* display_manager = Shell::Get()->display_manager();
  display::test::ScopedSetInternalDisplayId set_internal(display_manager,
                                                         display_id);
  ScreenOrientationControllerTestApi test_api(
      Shell::Get()->screen_orientation_controller());
  ui::TestAcceleratorTarget target_back_press, target_back_release;
  RegisterBackPressAndRelease(&target_back_press, &target_back_release);

  std::unique_ptr<aura::Window> left_window = CreateTestWindow();
  std::unique_ptr<aura::Window> right_window = CreateTestWindow();
  auto* split_view_controller =
      SplitViewController::Get(Shell::GetPrimaryRootWindow());
  split_view_controller->SnapWindow(left_window.get(),
                                    SplitViewController::LEFT);
  split_view_controller->SnapWindow(right_window.get(),
                                    SplitViewController::RIGHT);

  // Set the screen orientation to LANDSCAPE_PRIMARY.
  test_api.SetDisplayRotation(display::Display::ROTATE_0,
                              display::Display::RotationSource::ACTIVE);
  EXPECT_EQ(test_api.GetCurrentOrientation(),
            OrientationLockType::kLandscapePrimary);

  ASSERT_EQ(right_window.get(), window_util::GetActiveWindow());
  gfx::Point start(0, 10);
  gfx::Point update_and_end(kSwipingDistanceForGoingBack + 10, 10);
  SendTouchEvent(start, ui::ET_TOUCH_PRESSED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_MOVED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_RELEASED);
  // Swiping from the left of the display in LandscapePrimary further than
  // |kSwipingDistanceForGoingBack| should activate the physically left snapped
  // window, which is |left_window| and it should go back to the previous page.
  EXPECT_EQ(left_window.get(), window_util::GetActiveWindow());
  EXPECT_EQ(1, target_back_press.accelerator_count());
  EXPECT_EQ(1, target_back_release.accelerator_count());

  gfx::Rect divider_bounds =
      split_view_controller->split_view_divider()->GetDividerBoundsInScreen(
          false);
  start = gfx::Point(divider_bounds.x(), 10);
  update_and_end =
      gfx::Point(divider_bounds.x() + kSwipingDistanceForGoingBack + 10, 10);
  SendTouchEvent(start, ui::ET_TOUCH_PRESSED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_MOVED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_RELEASED);
  // Swiping from the split view divider in LandscapePrimary further than
  // |kSwipingDistanceForGoingBack| should activate the physically right snapped
  // window, which is |right_window| and it should go back to the previous page.
  EXPECT_EQ(right_window.get(), window_util::GetActiveWindow());
  EXPECT_EQ(2, target_back_press.accelerator_count());
  EXPECT_EQ(2, target_back_release.accelerator_count());

  // Rotate the screen by 180 degree.
  test_api.SetDisplayRotation(display::Display::ROTATE_180,
                              display::Display::RotationSource::ACTIVE);
  EXPECT_EQ(test_api.GetCurrentOrientation(),
            OrientationLockType::kLandscapeSecondary);

  SendTouchEvent(start, ui::ET_TOUCH_PRESSED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_MOVED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_RELEASED);
  // Swiping from the split view divider in LandscapeSecondary further than
  // |kSwipingDistanceForGoingBack| should activate the physically right snapped
  // window, which is |left_window| and it should go back to the previous page.
  EXPECT_EQ(left_window.get(), window_util::GetActiveWindow());
  EXPECT_EQ(3, target_back_press.accelerator_count());
  EXPECT_EQ(3, target_back_release.accelerator_count());

  start = gfx::Point(0, 10);
  update_and_end = gfx::Point(kSwipingDistanceForGoingBack + 10, 10);
  SendTouchEvent(start, ui::ET_TOUCH_PRESSED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_MOVED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_RELEASED);
  // Swiping from the left of the display in LandscapeSecondary further than
  // |kSwipingDistanceForGoingBack| should activate the physically left snapped
  // window, which is |right_window| and it should go back to the previous page.
  EXPECT_EQ(right_window.get(), window_util::GetActiveWindow());
  EXPECT_EQ(4, target_back_press.accelerator_count());
  EXPECT_EQ(4, target_back_release.accelerator_count());

  // Rotate the screen by 270 degree.
  test_api.SetDisplayRotation(display::Display::ROTATE_270,
                              display::Display::RotationSource::ACTIVE);
  EXPECT_EQ(test_api.GetCurrentOrientation(),
            OrientationLockType::kPortraitPrimary);

  SendTouchEvent(start, ui::ET_TOUCH_PRESSED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_MOVED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_RELEASED);
  // Swiping from the left of the top half of the display in PortraitPrimary
  // further than |kSwipingDistanceForGoingBack| should activate the physically
  // top snapped window, which is |right_window|, and it should go back to the
  // previous page.
  EXPECT_EQ(left_window.get(), window_util::GetActiveWindow());
  EXPECT_EQ(5, target_back_press.accelerator_count());
  EXPECT_EQ(5, target_back_release.accelerator_count());

  divider_bounds =
      split_view_controller->split_view_divider()->GetDividerBoundsInScreen(
          false);
  start = gfx::Point(0, divider_bounds.bottom() + 10);
  update_and_end = gfx::Point(kSwipingDistanceForGoingBack + 10, start.y());
  SendTouchEvent(start, ui::ET_TOUCH_PRESSED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_MOVED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_RELEASED);
  // Swiping from the left of the bottom half of the display in PortraitPrimary
  // further than |kSwipingDistanceForGoingBack| should activate the physically
  // bottom snapped window, which is |right_window|, and it should go back to
  // the previous page.
  EXPECT_EQ(right_window.get(), window_util::GetActiveWindow());
  EXPECT_EQ(6, target_back_press.accelerator_count());
  EXPECT_EQ(6, target_back_release.accelerator_count());

  // Rotate the screen by 90 degree.
  test_api.SetDisplayRotation(display::Display::ROTATE_90,
                              display::Display::RotationSource::ACTIVE);
  EXPECT_EQ(test_api.GetCurrentOrientation(),
            OrientationLockType::kPortraitSecondary);

  SendTouchEvent(start, ui::ET_TOUCH_PRESSED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_MOVED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_RELEASED);
  // Swiping from the left of the bottom half of the display in
  // PortraitSecondary further than |kSwipingDistanceForGoingBack| should
  // activate the physically bottom snapped window, which is |left_window|, and
  // it should go back to the previous page.
  EXPECT_EQ(left_window.get(), window_util::GetActiveWindow());
  EXPECT_EQ(7, target_back_press.accelerator_count());
  EXPECT_EQ(7, target_back_release.accelerator_count());

  start = gfx::Point(0, 10);
  update_and_end = gfx::Point(kSwipingDistanceForGoingBack + 10, 10);
  SendTouchEvent(start, ui::ET_TOUCH_PRESSED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_MOVED);
  SendTouchEvent(update_and_end, ui::ET_TOUCH_RELEASED);
  // Swiping from the left of the top half of the display in PortraitSecondary
  // further than |kSwipingDistanceForGoingBack| should activate the physically
  // top snapped window, which is |right_window| and it should go back to the
  // previous page.
  EXPECT_EQ(right_window.get(), window_util::GetActiveWindow());
  EXPECT_EQ(8, target_back_press.accelerator_count());
  EXPECT_EQ(8, target_back_release.accelerator_count());
}

// Tests the back gesture behavior on a fullscreen'ed window.
TEST_F(BackGestureEventHandlerTest, FullscreenedWindow) {
  ui::TestAcceleratorTarget target_back_press, target_back_release;
  RegisterBackPressAndRelease(&target_back_press, &target_back_release);

  WindowState* window_state = WindowState::Get(top_window());
  const WMEvent fullscreen_event(WM_EVENT_TOGGLE_FULLSCREEN);
  window_state->OnWMEvent(&fullscreen_event);
  EXPECT_TRUE(window_state->IsFullscreen());

  ui::test::EventGenerator* generator = GetEventGenerator();
  const gfx::Point start(0, 100);
  generator->GestureScrollSequence(
      start, gfx::Point(kSwipingDistanceForGoingBack + 10, 100),
      base::TimeDelta::FromMilliseconds(100), 3);
  // First back gesture should let the window exit fullscreen mode instead of
  // triggering go back.
  EXPECT_FALSE(window_state->IsFullscreen());
  EXPECT_EQ(0, target_back_press.accelerator_count());
  EXPECT_EQ(0, target_back_release.accelerator_count());

  generator->GestureScrollSequence(
      start, gfx::Point(kSwipingDistanceForGoingBack + 10, 100),
      base::TimeDelta::FromMilliseconds(100), 3);
  // Second back gesture should trigger go back.
  EXPECT_EQ(1, target_back_press.accelerator_count());
  EXPECT_EQ(1, target_back_release.accelerator_count());
}

}  // namespace ash
