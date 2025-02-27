// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/views/focus/focus_manager.h"

#include <stddef.h>

#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/macros.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/icu_test_util.h"
#include "build/build_config.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/accelerators/test_accelerator_target.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/views/accessible_pane_view.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/buildflags.h"
#include "ui/views/focus/focus_manager_delegate.h"
#include "ui/views/focus/focus_manager_factory.h"
#include "ui/views/focus/widget_focus_manager.h"
#include "ui/views/test/focus_manager_test.h"
#include "ui/views/test/native_widget_factory.h"
#include "ui/views/test/test_platform_native_widget.h"
#include "ui/views/test/widget_test.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/widget/widget.h"

#if defined(USE_AURA)
#include "ui/aura/client/focus_client.h"
#include "ui/views/widget/native_widget_aura.h"
#endif  // USE_AURA

namespace views {

enum FocusTestEventType { ON_FOCUS = 0, ON_BLUR };

struct FocusTestEvent {
  FocusTestEventType type;
  int view_id;
  FocusManager::FocusChangeReason focus_change_reason;
};

class SimpleTestView : public View {
 public:
  SimpleTestView(std::vector<FocusTestEvent>* event_list, int view_id)
      : event_list_(event_list) {
    SetFocusBehavior(FocusBehavior::ALWAYS);
    SetID(view_id);
  }

  void OnFocus() override {
    event_list_->push_back({
        ON_FOCUS,
        GetID(),
        GetFocusManager()->focus_change_reason(),
    });
  }

  void OnBlur() override {
    event_list_->push_back({
        ON_BLUR,
        GetID(),
        GetFocusManager()->focus_change_reason(),
    });
  }

 private:
  std::vector<FocusTestEvent>* event_list_;

  DISALLOW_COPY_AND_ASSIGN(SimpleTestView);
};

// Tests that the appropriate Focus related methods are called when a View
// gets/loses focus.
TEST_F(FocusManagerTest, ViewFocusCallbacks) {
  std::vector<FocusTestEvent> event_list;
  const int kView1ID = 1;
  const int kView2ID = 2;

  SimpleTestView* view1 = new SimpleTestView(&event_list, kView1ID);
  SimpleTestView* view2 = new SimpleTestView(&event_list, kView2ID);
  GetContentsView()->AddChildView(view1);
  GetContentsView()->AddChildView(view2);

  view1->RequestFocus();
  ASSERT_EQ(1, static_cast<int>(event_list.size()));
  EXPECT_EQ(ON_FOCUS, event_list[0].type);
  EXPECT_EQ(kView1ID, event_list[0].view_id);
  EXPECT_EQ(FocusChangeReason::kDirectFocusChange,
            event_list[0].focus_change_reason);

  event_list.clear();
  view2->RequestFocus();
  ASSERT_EQ(2, static_cast<int>(event_list.size()));
  EXPECT_EQ(ON_BLUR, event_list[0].type);
  EXPECT_EQ(kView1ID, event_list[0].view_id);
  EXPECT_EQ(ON_FOCUS, event_list[1].type);
  EXPECT_EQ(kView2ID, event_list[1].view_id);
  EXPECT_EQ(FocusChangeReason::kDirectFocusChange,
            event_list[0].focus_change_reason);
  EXPECT_EQ(FocusChangeReason::kDirectFocusChange,
            event_list[1].focus_change_reason);

  event_list.clear();
  GetFocusManager()->ClearFocus();
  ASSERT_EQ(1, static_cast<int>(event_list.size()));
  EXPECT_EQ(ON_BLUR, event_list[0].type);
  EXPECT_EQ(kView2ID, event_list[0].view_id);
  EXPECT_EQ(FocusChangeReason::kDirectFocusChange,
            event_list[0].focus_change_reason);
}

TEST_F(FocusManagerTest, FocusChangeListener) {
  View* view1 = new View();
  view1->SetFocusBehavior(View::FocusBehavior::ALWAYS);
  View* view2 = new View();
  view2->SetFocusBehavior(View::FocusBehavior::ALWAYS);
  GetContentsView()->AddChildView(view1);
  GetContentsView()->AddChildView(view2);

  TestFocusChangeListener listener;
  AddFocusChangeListener(&listener);

  // Required for VS2010:
  // http://connect.microsoft.com/VisualStudio/feedback/details/520043/error-converting-from-null-to-a-pointer-type-in-std-pair
  views::View* null_view = nullptr;

  view1->RequestFocus();
  ASSERT_EQ(1, static_cast<int>(listener.focus_changes().size()));
  EXPECT_TRUE(listener.focus_changes()[0] == ViewPair(null_view, view1));
  listener.ClearFocusChanges();

  view2->RequestFocus();
  ASSERT_EQ(1, static_cast<int>(listener.focus_changes().size()));
  EXPECT_TRUE(listener.focus_changes()[0] == ViewPair(view1, view2));
  listener.ClearFocusChanges();

  GetFocusManager()->ClearFocus();
  ASSERT_EQ(1, static_cast<int>(listener.focus_changes().size()));
  EXPECT_TRUE(listener.focus_changes()[0] == ViewPair(view2, null_view));
}

TEST_F(FocusManagerTest, WidgetFocusChangeListener) {
  // First, ensure the simulator is aware of the Widget created in SetUp() being
  // currently active.
  test::WidgetTest::SimulateNativeActivate(GetWidget());

  TestWidgetFocusChangeListener widget_listener;
  AddWidgetFocusChangeListener(&widget_listener);

  Widget::InitParams params = CreateParams(Widget::InitParams::TYPE_WINDOW);
  params.ownership = views::Widget::InitParams::WIDGET_OWNS_NATIVE_WIDGET;
  params.bounds = gfx::Rect(10, 10, 100, 100);
  params.parent = GetWidget()->GetNativeView();

  std::unique_ptr<Widget> widget1(new Widget);
  widget1->Init(std::move(params));
  widget1->Show();

  std::unique_ptr<Widget> widget2(new Widget);
  widget2->Init(std::move(params));
  widget2->Show();

  widget_listener.ClearFocusChanges();
  gfx::NativeView native_view1 = widget1->GetNativeView();
  test::WidgetTest::SimulateNativeActivate(widget1.get());
  ASSERT_EQ(2u, widget_listener.focus_changes().size());
  EXPECT_EQ(gfx::kNullNativeView, widget_listener.focus_changes()[0]);
  EXPECT_EQ(native_view1, widget_listener.focus_changes()[1]);

  widget_listener.ClearFocusChanges();
  gfx::NativeView native_view2 = widget2->GetNativeView();
  test::WidgetTest::SimulateNativeActivate(widget2.get());
  ASSERT_EQ(2u, widget_listener.focus_changes().size());
  EXPECT_EQ(gfx::kNullNativeView, widget_listener.focus_changes()[0]);
  EXPECT_EQ(native_view2, widget_listener.focus_changes()[1]);
}

TEST_F(FocusManagerTest, CallsNormalAcceleratorTarget) {
  FocusManager* focus_manager = GetFocusManager();
  ui::Accelerator return_accelerator(ui::VKEY_RETURN, ui::EF_NONE);
  ui::Accelerator escape_accelerator(ui::VKEY_ESCAPE, ui::EF_NONE);

  ui::TestAcceleratorTarget return_target(true);
  ui::TestAcceleratorTarget escape_target(true);
  EXPECT_EQ(return_target.accelerator_count(), 0);
  EXPECT_EQ(escape_target.accelerator_count(), 0);

  // Register targets.
  focus_manager->RegisterAccelerator(return_accelerator,
                                     ui::AcceleratorManager::kNormalPriority,
                                     &return_target);
  focus_manager->RegisterAccelerator(escape_accelerator,
                                     ui::AcceleratorManager::kNormalPriority,
                                     &escape_target);

  // Hitting the return key.
  EXPECT_TRUE(focus_manager->ProcessAccelerator(return_accelerator));
  EXPECT_EQ(return_target.accelerator_count(), 1);
  EXPECT_EQ(escape_target.accelerator_count(), 0);

  // Hitting the escape key.
  EXPECT_TRUE(focus_manager->ProcessAccelerator(escape_accelerator));
  EXPECT_EQ(return_target.accelerator_count(), 1);
  EXPECT_EQ(escape_target.accelerator_count(), 1);

  // Register another target for the return key.
  ui::TestAcceleratorTarget return_target2(true);
  EXPECT_EQ(return_target2.accelerator_count(), 0);
  focus_manager->RegisterAccelerator(return_accelerator,
                                     ui::AcceleratorManager::kNormalPriority,
                                     &return_target2);

  // Hitting the return key; return_target2 has the priority.
  EXPECT_TRUE(focus_manager->ProcessAccelerator(return_accelerator));
  EXPECT_EQ(return_target.accelerator_count(), 1);
  EXPECT_EQ(return_target2.accelerator_count(), 1);

  // Register a target that does not process the accelerator event.
  ui::TestAcceleratorTarget return_target3(false);
  EXPECT_EQ(return_target3.accelerator_count(), 0);
  focus_manager->RegisterAccelerator(return_accelerator,
                                     ui::AcceleratorManager::kNormalPriority,
                                     &return_target3);
  // Hitting the return key.
  // Since the event handler of return_target3 returns false, return_target2
  // should be called too.
  EXPECT_TRUE(focus_manager->ProcessAccelerator(return_accelerator));
  EXPECT_EQ(return_target.accelerator_count(), 1);
  EXPECT_EQ(return_target2.accelerator_count(), 2);
  EXPECT_EQ(return_target3.accelerator_count(), 1);

  // Unregister return_target2.
  focus_manager->UnregisterAccelerator(return_accelerator, &return_target2);

  // Hitting the return key. return_target3 and return_target should be called.
  EXPECT_TRUE(focus_manager->ProcessAccelerator(return_accelerator));
  EXPECT_EQ(return_target.accelerator_count(), 2);
  EXPECT_EQ(return_target2.accelerator_count(), 2);
  EXPECT_EQ(return_target3.accelerator_count(), 2);

  // Unregister targets.
  focus_manager->UnregisterAccelerator(return_accelerator, &return_target);
  focus_manager->UnregisterAccelerator(return_accelerator, &return_target3);
  focus_manager->UnregisterAccelerator(escape_accelerator, &escape_target);

  // Hitting the return key and the escape key. Nothing should happen.
  EXPECT_FALSE(focus_manager->ProcessAccelerator(return_accelerator));
  EXPECT_EQ(return_target.accelerator_count(), 2);
  EXPECT_EQ(return_target2.accelerator_count(), 2);
  EXPECT_EQ(return_target3.accelerator_count(), 2);
  EXPECT_FALSE(focus_manager->ProcessAccelerator(escape_accelerator));
  EXPECT_EQ(escape_target.accelerator_count(), 1);
}

TEST_F(FocusManagerTest, HighPriorityHandlers) {
  FocusManager* focus_manager = GetFocusManager();
  ui::Accelerator escape_accelerator(ui::VKEY_ESCAPE, ui::EF_NONE);

  ui::TestAcceleratorTarget escape_target_high(true);
  ui::TestAcceleratorTarget escape_target_normal(true);
  EXPECT_EQ(escape_target_high.accelerator_count(), 0);
  EXPECT_EQ(escape_target_normal.accelerator_count(), 0);
  EXPECT_FALSE(focus_manager->HasPriorityHandler(escape_accelerator));

  // Register high priority target.
  focus_manager->RegisterAccelerator(escape_accelerator,
                                     ui::AcceleratorManager::kHighPriority,
                                     &escape_target_high);
  EXPECT_TRUE(focus_manager->HasPriorityHandler(escape_accelerator));

  // Hit the escape key.
  EXPECT_TRUE(focus_manager->ProcessAccelerator(escape_accelerator));
  EXPECT_EQ(escape_target_high.accelerator_count(), 1);
  EXPECT_EQ(escape_target_normal.accelerator_count(), 0);

  // Add a normal priority target and make sure it doesn't see the key.
  focus_manager->RegisterAccelerator(escape_accelerator,
                                     ui::AcceleratorManager::kNormalPriority,
                                     &escape_target_normal);

  // Checks if the correct target is registered (same as before, the high
  // priority one).
  EXPECT_TRUE(focus_manager->HasPriorityHandler(escape_accelerator));

  // Hit the escape key.
  EXPECT_TRUE(focus_manager->ProcessAccelerator(escape_accelerator));
  EXPECT_EQ(escape_target_high.accelerator_count(), 2);
  EXPECT_EQ(escape_target_normal.accelerator_count(), 0);

  // Unregister the high priority accelerator.
  focus_manager->UnregisterAccelerator(escape_accelerator, &escape_target_high);
  EXPECT_FALSE(focus_manager->HasPriorityHandler(escape_accelerator));

  // Hit the escape key.
  EXPECT_TRUE(focus_manager->ProcessAccelerator(escape_accelerator));
  EXPECT_EQ(escape_target_high.accelerator_count(), 2);
  EXPECT_EQ(escape_target_normal.accelerator_count(), 1);

  // Add the high priority target back and make sure it starts seeing the key.
  focus_manager->RegisterAccelerator(escape_accelerator,
                                     ui::AcceleratorManager::kHighPriority,
                                     &escape_target_high);
  EXPECT_TRUE(focus_manager->HasPriorityHandler(escape_accelerator));

  // Hit the escape key.
  EXPECT_TRUE(focus_manager->ProcessAccelerator(escape_accelerator));
  EXPECT_EQ(escape_target_high.accelerator_count(), 3);
  EXPECT_EQ(escape_target_normal.accelerator_count(), 1);

  // Unregister the normal priority accelerator.
  focus_manager->UnregisterAccelerator(escape_accelerator,
                                       &escape_target_normal);
  EXPECT_TRUE(focus_manager->HasPriorityHandler(escape_accelerator));

  // Hit the escape key.
  EXPECT_TRUE(focus_manager->ProcessAccelerator(escape_accelerator));
  EXPECT_EQ(escape_target_high.accelerator_count(), 4);
  EXPECT_EQ(escape_target_normal.accelerator_count(), 1);

  // Unregister the high priority accelerator.
  focus_manager->UnregisterAccelerator(escape_accelerator, &escape_target_high);
  EXPECT_FALSE(focus_manager->HasPriorityHandler(escape_accelerator));

  // Hit the escape key (no change, no targets registered).
  EXPECT_FALSE(focus_manager->ProcessAccelerator(escape_accelerator));
  EXPECT_EQ(escape_target_high.accelerator_count(), 4);
  EXPECT_EQ(escape_target_normal.accelerator_count(), 1);
}

TEST_F(FocusManagerTest, CallsEnabledAcceleratorTargetsOnly) {
  FocusManager* focus_manager = GetFocusManager();
  ui::Accelerator return_accelerator(ui::VKEY_RETURN, ui::EF_NONE);

  ui::TestAcceleratorTarget return_target1(true);
  ui::TestAcceleratorTarget return_target2(true);

  focus_manager->RegisterAccelerator(return_accelerator,
                                     ui::AcceleratorManager::kNormalPriority,
                                     &return_target1);
  focus_manager->RegisterAccelerator(return_accelerator,
                                     ui::AcceleratorManager::kNormalPriority,
                                     &return_target2);
  EXPECT_TRUE(focus_manager->ProcessAccelerator(return_accelerator));
  EXPECT_EQ(0, return_target1.accelerator_count());
  EXPECT_EQ(1, return_target2.accelerator_count());

  // If CanHandleAccelerators() return false, FocusManager shouldn't call
  // AcceleratorPressed().
  return_target2.set_can_handle_accelerators(false);
  EXPECT_TRUE(focus_manager->ProcessAccelerator(return_accelerator));
  EXPECT_EQ(1, return_target1.accelerator_count());
  EXPECT_EQ(1, return_target2.accelerator_count());

  // If no accelerator targets are enabled, ProcessAccelerator() should fail.
  return_target1.set_can_handle_accelerators(false);
  EXPECT_FALSE(focus_manager->ProcessAccelerator(return_accelerator));
  EXPECT_EQ(1, return_target1.accelerator_count());
  EXPECT_EQ(1, return_target2.accelerator_count());

  // Enabling the target again causes the accelerators to be processed again.
  return_target1.set_can_handle_accelerators(true);
  return_target2.set_can_handle_accelerators(true);
  EXPECT_TRUE(focus_manager->ProcessAccelerator(return_accelerator));
  EXPECT_EQ(1, return_target1.accelerator_count());
  EXPECT_EQ(2, return_target2.accelerator_count());
}

// Unregisters itself when its accelerator is invoked.
class SelfUnregisteringAcceleratorTarget : public ui::TestAcceleratorTarget {
 public:
  SelfUnregisteringAcceleratorTarget(const ui::Accelerator& accelerator,
                                     FocusManager* focus_manager)
      : accelerator_(accelerator), focus_manager_(focus_manager) {}

  // ui::TestAcceleratorTarget:
  bool AcceleratorPressed(const ui::Accelerator& accelerator) override {
    focus_manager_->UnregisterAccelerator(accelerator, this);
    return ui::TestAcceleratorTarget::AcceleratorPressed(accelerator);
  }

 private:
  ui::Accelerator accelerator_;
  FocusManager* focus_manager_;

  DISALLOW_COPY_AND_ASSIGN(SelfUnregisteringAcceleratorTarget);
};

TEST_F(FocusManagerTest, CallsSelfDeletingAcceleratorTarget) {
  FocusManager* focus_manager = GetFocusManager();
  ui::Accelerator return_accelerator(ui::VKEY_RETURN, ui::EF_NONE);
  SelfUnregisteringAcceleratorTarget target(return_accelerator, focus_manager);
  EXPECT_EQ(target.accelerator_count(), 0);

  // Register the target.
  focus_manager->RegisterAccelerator(
      return_accelerator, ui::AcceleratorManager::kNormalPriority, &target);

  // Hitting the return key. The target will be unregistered.
  EXPECT_TRUE(focus_manager->ProcessAccelerator(return_accelerator));
  EXPECT_EQ(target.accelerator_count(), 1);

  // Hitting the return key again; nothing should happen.
  EXPECT_FALSE(focus_manager->ProcessAccelerator(return_accelerator));
  EXPECT_EQ(target.accelerator_count(), 1);
}

TEST_F(FocusManagerTest, SuspendAccelerators) {
  const ui::KeyEvent event(ui::ET_KEY_PRESSED, ui::VKEY_RETURN, ui::EF_NONE);
  ui::Accelerator accelerator(event.key_code(), event.flags());
  ui::TestAcceleratorTarget target(true);
  FocusManager* focus_manager = GetFocusManager();
  focus_manager->RegisterAccelerator(
      accelerator, ui::AcceleratorManager::kNormalPriority, &target);

  focus_manager->set_shortcut_handling_suspended(true);
  EXPECT_TRUE(focus_manager->OnKeyEvent(event));
  EXPECT_EQ(0, target.accelerator_count());

  focus_manager->set_shortcut_handling_suspended(false);
  EXPECT_FALSE(focus_manager->OnKeyEvent(event));
  EXPECT_EQ(1, target.accelerator_count());
}

class FocusManagerDtorTest : public FocusManagerTest {
 protected:
  using DtorTrackVector = std::vector<std::string>;

  class FocusManagerDtorTracked : public FocusManager {
   public:
    FocusManagerDtorTracked(Widget* widget, DtorTrackVector* dtor_tracker)
        : FocusManager(widget, nullptr /* delegate */),
          dtor_tracker_(dtor_tracker) {}

    ~FocusManagerDtorTracked() override {
      dtor_tracker_->push_back("FocusManagerDtorTracked");
    }

    DtorTrackVector* dtor_tracker_;

   private:
    DISALLOW_COPY_AND_ASSIGN(FocusManagerDtorTracked);
  };

  class TestFocusManagerFactory : public FocusManagerFactory {
   public:
    explicit TestFocusManagerFactory(DtorTrackVector* dtor_tracker)
        : dtor_tracker_(dtor_tracker) {}
    ~TestFocusManagerFactory() override = default;

    std::unique_ptr<FocusManager> CreateFocusManager(Widget* widget) override {
      return std::make_unique<FocusManagerDtorTracked>(widget, dtor_tracker_);
    }

   private:
    DtorTrackVector* dtor_tracker_;

    DISALLOW_COPY_AND_ASSIGN(TestFocusManagerFactory);
  };

  class WindowDtorTracked : public Widget {
   public:
    explicit WindowDtorTracked(DtorTrackVector* dtor_tracker)
        : dtor_tracker_(dtor_tracker) {}

    ~WindowDtorTracked() override {
      dtor_tracker_->push_back("WindowDtorTracked");
    }

    DtorTrackVector* dtor_tracker_;
  };

  void SetUp() override {
    ViewsTestBase::SetUp();
    FocusManagerFactory::Install(new TestFocusManagerFactory(&dtor_tracker_));
    // Create WindowDtorTracked that uses FocusManagerDtorTracked.
    Widget* widget = new WindowDtorTracked(&dtor_tracker_);
    Widget::InitParams params;
    params.delegate = this;
    params.bounds = gfx::Rect(0, 0, 100, 100);
    widget->Init(std::move(params));

    tracked_focus_manager_ =
        static_cast<FocusManagerDtorTracked*>(GetFocusManager());
    widget->Show();
  }

  void TearDown() override {
    FocusManagerFactory::Install(nullptr);
    ViewsTestBase::TearDown();
  }

  FocusManager* tracked_focus_manager_;
  DtorTrackVector dtor_tracker_;
};

namespace {

class FocusInAboutToRequestFocusFromTabTraversalView : public View {
 public:
  FocusInAboutToRequestFocusFromTabTraversalView() = default;

  void set_view_to_focus(View* view) { view_to_focus_ = view; }

  void AboutToRequestFocusFromTabTraversal(bool reverse) override {
    view_to_focus_->RequestFocus();
  }

 private:
  views::View* view_to_focus_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(FocusInAboutToRequestFocusFromTabTraversalView);
};
}  // namespace

// Verifies a focus change done during a call to
// AboutToRequestFocusFromTabTraversal() is honored.
TEST_F(FocusManagerTest, FocusInAboutToRequestFocusFromTabTraversal) {
  // Create 3 views focuses the 3 and advances to the second. The 2nd views
  // implementation of AboutToRequestFocusFromTabTraversal() focuses the first.
  views::View* v1 = new View;
  v1->SetFocusBehavior(View::FocusBehavior::ALWAYS);
  GetContentsView()->AddChildView(v1);

  FocusInAboutToRequestFocusFromTabTraversalView* v2 =
      new FocusInAboutToRequestFocusFromTabTraversalView;
  v2->SetFocusBehavior(View::FocusBehavior::ALWAYS);
  v2->set_view_to_focus(v1);
  GetContentsView()->AddChildView(v2);

  views::View* v3 = new View;
  v3->SetFocusBehavior(View::FocusBehavior::ALWAYS);
  GetContentsView()->AddChildView(v3);

  v3->RequestFocus();
  GetWidget()->GetFocusManager()->AdvanceFocus(true);
  EXPECT_TRUE(v1->HasFocus());
}

TEST_F(FocusManagerTest, RotatePaneFocus) {
  views::AccessiblePaneView* pane1 = new AccessiblePaneView();
  GetContentsView()->AddChildView(pane1);

  views::View* v1 = new View;
  v1->SetFocusBehavior(View::FocusBehavior::ALWAYS);
  pane1->AddChildView(v1);

  views::View* v2 = new View;
  v2->SetFocusBehavior(View::FocusBehavior::ALWAYS);
  pane1->AddChildView(v2);

  views::AccessiblePaneView* pane2 = new AccessiblePaneView();
  GetContentsView()->AddChildView(pane2);

  views::View* v3 = new View;
  v3->SetFocusBehavior(View::FocusBehavior::ALWAYS);
  pane2->AddChildView(v3);

  views::View* v4 = new View;
  v4->SetFocusBehavior(View::FocusBehavior::ALWAYS);
  pane2->AddChildView(v4);

  std::vector<views::View*> panes;
  panes.push_back(pane1);
  panes.push_back(pane2);
  SetAccessiblePanes(panes);

  FocusManager* focus_manager = GetWidget()->GetFocusManager();

  // Advance forwards. Focus should stay trapped within each pane.
  EXPECT_TRUE(focus_manager->RotatePaneFocus(FocusManager::kForward,
                                             FocusManager::kWrap));
  EXPECT_EQ(v1, focus_manager->GetFocusedView());
  focus_manager->AdvanceFocus(false);
  EXPECT_EQ(v2, focus_manager->GetFocusedView());
  focus_manager->AdvanceFocus(false);
  EXPECT_EQ(v1, focus_manager->GetFocusedView());

  EXPECT_TRUE(focus_manager->RotatePaneFocus(FocusManager::kForward,
                                             FocusManager::kWrap));
  EXPECT_EQ(v3, focus_manager->GetFocusedView());
  focus_manager->AdvanceFocus(false);
  EXPECT_EQ(v4, focus_manager->GetFocusedView());
  focus_manager->AdvanceFocus(false);
  EXPECT_EQ(v3, focus_manager->GetFocusedView());

  EXPECT_TRUE(focus_manager->RotatePaneFocus(FocusManager::kForward,
                                             FocusManager::kWrap));
  EXPECT_EQ(v1, focus_manager->GetFocusedView());

  // Advance backwards.
  EXPECT_TRUE(focus_manager->RotatePaneFocus(FocusManager::kBackward,
                                             FocusManager::kWrap));
  EXPECT_EQ(v3, focus_manager->GetFocusedView());

  EXPECT_TRUE(focus_manager->RotatePaneFocus(FocusManager::kBackward,
                                             FocusManager::kWrap));
  EXPECT_EQ(v1, focus_manager->GetFocusedView());

  // Advance without wrap. When it gets to the end of the list of
  // panes, RotatePaneFocus should return false but the current
  // focused view shouldn't change.
  EXPECT_TRUE(focus_manager->RotatePaneFocus(FocusManager::kForward,
                                             FocusManager::kNoWrap));
  EXPECT_EQ(v3, focus_manager->GetFocusedView());

  EXPECT_FALSE(focus_manager->RotatePaneFocus(FocusManager::kForward,
                                              FocusManager::kNoWrap));
  EXPECT_EQ(v3, focus_manager->GetFocusedView());
}

// Verifies the stored focus view tracks the focused view.
TEST_F(FocusManagerTest, ImplicitlyStoresFocus) {
  views::View* v1 = new View;
  v1->SetFocusBehavior(View::FocusBehavior::ALWAYS);
  GetContentsView()->AddChildView(v1);

  views::View* v2 = new View;
  v2->SetFocusBehavior(View::FocusBehavior::ALWAYS);
  GetContentsView()->AddChildView(v2);

  // Verify a focus request on |v1| implicitly updates the stored focus view.
  v1->RequestFocus();
  EXPECT_TRUE(v1->HasFocus());
  EXPECT_EQ(v1, GetWidget()->GetFocusManager()->GetStoredFocusView());

  // Verify a focus request on |v2| implicitly updates the stored focus view.
  v2->RequestFocus();
  EXPECT_TRUE(v2->HasFocus());
  EXPECT_EQ(v2, GetWidget()->GetFocusManager()->GetStoredFocusView());
}

namespace {

class FocusManagerArrowKeyTraversalTest
    : public FocusManagerTest,
      public testing::WithParamInterface<bool> {
 public:
  FocusManagerArrowKeyTraversalTest() = default;
  ~FocusManagerArrowKeyTraversalTest() override = default;

  // FocusManagerTest overrides:
  void SetUp() override {
    if (testing::UnitTest::GetInstance()->current_test_info()->value_param()) {
      is_rtl_ = GetParam();
      if (is_rtl_)
        base::i18n::SetICUDefaultLocale("he");
    }

    FocusManagerTest::SetUp();
    previous_arrow_key_traversal_enabled_ =
        FocusManager::arrow_key_traversal_enabled();
  }

  void TearDown() override {
    FocusManager::set_arrow_key_traversal_enabled(
        previous_arrow_key_traversal_enabled_);
    FocusManagerTest::TearDown();
  }

  bool is_rtl_ = false;

 private:
  // Restores the locale to default when the destructor is called.
  base::test::ScopedRestoreICUDefaultLocale restore_locale_;

  bool previous_arrow_key_traversal_enabled_ = false;

  DISALLOW_COPY_AND_ASSIGN(FocusManagerArrowKeyTraversalTest);
};

// Instantiate the Boolean which is used to toggle RTL in
// the parameterized tests.
INSTANTIATE_TEST_SUITE_P(All,
                         FocusManagerArrowKeyTraversalTest,
                         testing::Bool());

}  // namespace

TEST_P(FocusManagerArrowKeyTraversalTest, ArrowKeyTraversal) {
  FocusManager* focus_manager = GetFocusManager();
  const ui::KeyEvent left_key(ui::ET_KEY_PRESSED, ui::VKEY_LEFT, ui::EF_NONE);
  const ui::KeyEvent right_key(ui::ET_KEY_PRESSED, ui::VKEY_RIGHT, ui::EF_NONE);
  const ui::KeyEvent up_key(ui::ET_KEY_PRESSED, ui::VKEY_UP, ui::EF_NONE);
  const ui::KeyEvent down_key(ui::ET_KEY_PRESSED, ui::VKEY_DOWN, ui::EF_NONE);

  std::vector<views::View*> v;
  for (size_t i = 0; i < 2; ++i) {
    views::View* view = new View;
    view->SetFocusBehavior(View::FocusBehavior::ALWAYS);
    GetContentsView()->AddChildView(view);
    v.push_back(view);
  }

  // Arrow key traversal is off and arrow key does not change focus.
  FocusManager::set_arrow_key_traversal_enabled(false);
  v[0]->RequestFocus();
  focus_manager->OnKeyEvent(right_key);
  EXPECT_EQ(v[0], focus_manager->GetFocusedView());
  focus_manager->OnKeyEvent(left_key);
  EXPECT_EQ(v[0], focus_manager->GetFocusedView());
  focus_manager->OnKeyEvent(down_key);
  EXPECT_EQ(v[0], focus_manager->GetFocusedView());
  focus_manager->OnKeyEvent(up_key);
  EXPECT_EQ(v[0], focus_manager->GetFocusedView());

  // Turn on arrow key traversal.
  FocusManager::set_arrow_key_traversal_enabled(true);
  v[0]->RequestFocus();
  focus_manager->OnKeyEvent(is_rtl_ ? left_key : right_key);
  EXPECT_EQ(v[1], focus_manager->GetFocusedView());
  focus_manager->OnKeyEvent(is_rtl_ ? right_key : left_key);
  EXPECT_EQ(v[0], focus_manager->GetFocusedView());
  focus_manager->OnKeyEvent(down_key);
  EXPECT_EQ(v[1], focus_manager->GetFocusedView());
  focus_manager->OnKeyEvent(up_key);
  EXPECT_EQ(v[0], focus_manager->GetFocusedView());
}

TEST_F(FocusManagerTest, StoreFocusedView) {
  std::vector<FocusTestEvent> event_list;
  const int kView1ID = 1;
  SimpleTestView* view = new SimpleTestView(&event_list, kView1ID);

  // Add view to the view hierarchy and make it focusable.
  GetWidget()->GetRootView()->AddChildView(view);
  view->SetFocusBehavior(View::FocusBehavior::ALWAYS);

  GetFocusManager()->SetFocusedView(view);
  GetFocusManager()->StoreFocusedView(false);
  EXPECT_EQ(nullptr, GetFocusManager()->GetFocusedView());
  EXPECT_TRUE(GetFocusManager()->RestoreFocusedView());
  EXPECT_EQ(view, GetFocusManager()->GetStoredFocusView());
  ASSERT_EQ(3, static_cast<int>(event_list.size()));
  EXPECT_EQ(ON_FOCUS, event_list[0].type);
  EXPECT_EQ(kView1ID, event_list[0].view_id);
  EXPECT_EQ(FocusChangeReason::kDirectFocusChange,
            event_list[0].focus_change_reason);
  EXPECT_EQ(ON_BLUR, event_list[1].type);
  EXPECT_EQ(kView1ID, event_list[1].view_id);
  EXPECT_EQ(FocusChangeReason::kDirectFocusChange,
            event_list[1].focus_change_reason);
  EXPECT_EQ(ON_FOCUS, event_list[2].type);
  EXPECT_EQ(kView1ID, event_list[2].view_id);
  EXPECT_EQ(FocusChangeReason::kFocusRestore,
            event_list[2].focus_change_reason);

  // Repeat with |true|.
  event_list.clear();
  GetFocusManager()->SetFocusedView(view);
  GetFocusManager()->StoreFocusedView(true);
  EXPECT_EQ(nullptr, GetFocusManager()->GetFocusedView());
  EXPECT_TRUE(GetFocusManager()->RestoreFocusedView());
  EXPECT_EQ(view, GetFocusManager()->GetStoredFocusView());
  ASSERT_EQ(2, static_cast<int>(event_list.size()));
  EXPECT_EQ(ON_BLUR, event_list[0].type);
  EXPECT_EQ(kView1ID, event_list[0].view_id);
  EXPECT_EQ(FocusChangeReason::kDirectFocusChange,
            event_list[0].focus_change_reason);
  EXPECT_EQ(ON_FOCUS, event_list[1].type);
  EXPECT_EQ(kView1ID, event_list[1].view_id);
  EXPECT_EQ(FocusChangeReason::kFocusRestore,
            event_list[1].focus_change_reason);

  // Necessary for clean teardown.
  GetFocusManager()->ClearFocus();
}

#if defined(OS_MACOSX)
// Test that the correct view is restored if full keyboard access is changed.
TEST_F(FocusManagerTest, StoreFocusedViewFullKeyboardAccess) {
  View* view1 = new View;
  View* view2 = new View;
  View* view3 = new View;

  // Make view1 focusable in accessibility mode, view2 not focusable and view3
  // always focusable.
  view1->SetFocusBehavior(View::FocusBehavior::ACCESSIBLE_ONLY);
  view2->SetFocusBehavior(View::FocusBehavior::NEVER);
  view3->SetFocusBehavior(View::FocusBehavior::ALWAYS);

  // Add views to the view hierarchy
  GetWidget()->GetRootView()->AddChildView(view1);
  GetWidget()->GetRootView()->AddChildView(view2);
  GetWidget()->GetRootView()->AddChildView(view3);

  view1->RequestFocus();
  EXPECT_EQ(view1, GetFocusManager()->GetFocusedView());
  GetFocusManager()->StoreFocusedView(true);
  EXPECT_EQ(nullptr, GetFocusManager()->GetFocusedView());

  // Turn off full keyboard access mode and restore focused view. Since view1 is
  // no longer focusable, view3 should have focus.
  GetFocusManager()->SetKeyboardAccessible(false);
  EXPECT_FALSE(GetFocusManager()->RestoreFocusedView());
  EXPECT_EQ(view3, GetFocusManager()->GetFocusedView());

  GetFocusManager()->StoreFocusedView(false);
  EXPECT_EQ(nullptr, GetFocusManager()->GetFocusedView());

  // Turn on full keyboard access mode and restore focused view. Since view3 is
  // still focusable, view3 should have focus.
  GetFocusManager()->SetKeyboardAccessible(true);
  EXPECT_TRUE(GetFocusManager()->RestoreFocusedView());
  EXPECT_EQ(view3, GetFocusManager()->GetFocusedView());
}

// Test that View::RequestFocus() respects full keyboard access mode.
TEST_F(FocusManagerTest, RequestFocus) {
  View* view1 = new View();
  View* view2 = new View();

  // Make view1 always focusable, view2 only focusable in accessibility mode.
  view1->SetFocusBehavior(View::FocusBehavior::ALWAYS);
  view2->SetFocusBehavior(View::FocusBehavior::ACCESSIBLE_ONLY);

  // Adds views to the view hierarchy.
  GetWidget()->GetRootView()->AddChildView(view1);
  GetWidget()->GetRootView()->AddChildView(view2);

  // Verify view1 can always get focus via View::RequestFocus, while view2 can
  // only get focus in full keyboard accessibility mode.
  EXPECT_TRUE(GetFocusManager()->keyboard_accessible());
  view1->RequestFocus();
  EXPECT_EQ(view1, GetFocusManager()->GetFocusedView());
  view2->RequestFocus();
  EXPECT_EQ(view2, GetFocusManager()->GetFocusedView());

  // Toggle full keyboard accessibility.
  GetFocusManager()->SetKeyboardAccessible(false);

  GetFocusManager()->ClearFocus();
  EXPECT_NE(view1, GetFocusManager()->GetFocusedView());
  view1->RequestFocus();
  EXPECT_EQ(view1, GetFocusManager()->GetFocusedView());
  view2->RequestFocus();
  EXPECT_EQ(view1, GetFocusManager()->GetFocusedView());
}

#endif

namespace {

// Trivial WidgetDelegate implementation that allows setting return value of
// ShouldAdvanceFocusToTopLevelWidget().
class AdvanceFocusWidgetDelegate : public WidgetDelegate {
 public:
  explicit AdvanceFocusWidgetDelegate(Widget* widget)
      : widget_(widget), should_advance_focus_to_parent_(false) {}
  ~AdvanceFocusWidgetDelegate() override = default;

  void set_should_advance_focus_to_parent(bool value) {
    should_advance_focus_to_parent_ = value;
  }

  // WidgetDelegate:
  bool ShouldAdvanceFocusToTopLevelWidget() const override {
    return should_advance_focus_to_parent_;
  }

 private:
  // WidgetDelegate:
  const Widget* GetWidgetImpl() const override { return widget_; }

  Widget* widget_;
  bool should_advance_focus_to_parent_;

  DISALLOW_COPY_AND_ASSIGN(AdvanceFocusWidgetDelegate);
};

class TestBubbleDialogDelegateView : public BubbleDialogDelegateView {
 public:
  explicit TestBubbleDialogDelegateView(View* anchor)
      : BubbleDialogDelegateView(anchor, BubbleBorder::NONE) {
    DialogDelegate::set_buttons(ui::DIALOG_BUTTON_NONE);
  }
  ~TestBubbleDialogDelegateView() override = default;

  // If this is called, the bubble will be forced to use a NativeWidgetAura.
  // If not set, it might get a DesktopNativeWidgetAura depending on the
  // platform and other factors.
  void UseNativeWidgetAura() { use_native_widget_aura_ = true; }

  void OnBeforeBubbleWidgetInit(Widget::InitParams* params,
                                Widget* widget) const override {
#if defined(USE_AURA)
    if (use_native_widget_aura_) {
      params->native_widget =
          new test::TestPlatformNativeWidget<NativeWidgetAura>(widget, false,
                                                               nullptr);
    }
#endif  // USE_AURA
  }

 private:
  bool use_native_widget_aura_ = false;

  DISALLOW_COPY_AND_ASSIGN(TestBubbleDialogDelegateView);
};

}  // namespace

// Verifies focus wrapping happens in the same widget.
TEST_F(FocusManagerTest, AdvanceFocusStaysInWidget) {
  // Add |widget_view| as a child of the Widget.
  View* widget_view = new View;
  widget_view->SetFocusBehavior(View::FocusBehavior::ALWAYS);
  widget_view->SetBounds(20, 0, 20, 20);
  GetContentsView()->AddChildView(widget_view);

  // Create a widget with two views, focus the second.
  std::unique_ptr<AdvanceFocusWidgetDelegate> delegate;
  Widget::InitParams params = CreateParams(Widget::InitParams::TYPE_WINDOW);
  params.ownership = views::Widget::InitParams::WIDGET_OWNS_NATIVE_WIDGET;
  params.child = true;
  params.bounds = gfx::Rect(10, 10, 100, 100);
  params.parent = GetWidget()->GetNativeView();
  Widget child_widget;
  delegate = std::make_unique<AdvanceFocusWidgetDelegate>(&child_widget);
  params.delegate = delegate.get();
  child_widget.Init(std::move(params));
  View* view1 = new View;
  view1->SetFocusBehavior(View::FocusBehavior::ALWAYS);
  view1->SetBounds(0, 0, 20, 20);
  View* view2 = new View;
  view2->SetFocusBehavior(View::FocusBehavior::ALWAYS);
  view2->SetBounds(20, 0, 20, 20);
  child_widget.client_view()->AddChildView(view1);
  child_widget.client_view()->AddChildView(view2);
  child_widget.Show();
  view2->RequestFocus();
  EXPECT_EQ(view2, GetFocusManager()->GetFocusedView());

  // Advance focus backwards, which should focus the first.
  GetFocusManager()->AdvanceFocus(false);
  EXPECT_EQ(view1, GetFocusManager()->GetFocusedView());

  // Focus forward to |view2|.
  GetFocusManager()->AdvanceFocus(true);
  EXPECT_EQ(view2, GetFocusManager()->GetFocusedView());

  // And forward again, wrapping back to |view1|.
  GetFocusManager()->AdvanceFocus(true);
  EXPECT_EQ(view1, GetFocusManager()->GetFocusedView());

  // Allow focus to go to the parent, and focus backwards which should now move
  // up |widget_view| (in the parent).
  delegate->set_should_advance_focus_to_parent(true);
  GetFocusManager()->AdvanceFocus(true);
  EXPECT_EQ(widget_view, GetFocusManager()->GetFocusedView());
}

TEST_F(FocusManagerTest, NavigateIntoAnchoredDialog) {
  // The parent Widget has four focusable views. A child widget dialog has
  // two focusable views, and it's anchored to the 3rd parent view. Ensure
  // that focus traverses into the anchored dialog after the 3rd parent
  // view, and then back to the 4th parent view.

  View* parent1 = new View();
  View* parent2 = new View();
  View* parent3 = new View();
  View* parent4 = new View();

  parent1->SetFocusBehavior(View::FocusBehavior::ALWAYS);
  parent2->SetFocusBehavior(View::FocusBehavior::ALWAYS);
  parent3->SetFocusBehavior(View::FocusBehavior::ALWAYS);
  parent4->SetFocusBehavior(View::FocusBehavior::ALWAYS);

  GetWidget()->GetRootView()->AddChildView(parent1);
  GetWidget()->GetRootView()->AddChildView(parent2);
  GetWidget()->GetRootView()->AddChildView(parent3);
  GetWidget()->GetRootView()->AddChildView(parent4);

  // Add an unfocusable child view to the dialog anchor view. This is a
  // regression test that makes sure focus is able to navigate past unfocusable
  // children and try to go into the anchored dialog. |kAnchoredDialogKey| was
  // previously not checked if a recursive search to find a focusable child view
  // was attempted (and failed), so the dialog would previously be skipped.
  parent3->AddChildView(new View());

  BubbleDialogDelegateView* bubble_delegate =
      new TestBubbleDialogDelegateView(parent3);
  test::WidgetTest::WidgetAutoclosePtr bubble_widget(
      BubbleDialogDelegateView::CreateBubble(bubble_delegate));
  bubble_widget->SetFocusTraversableParent(
      bubble_delegate->anchor_widget()->GetFocusTraversable());

  bubble_widget->SetFocusTraversableParentView(parent3);
  View* child1 = new View();
  View* child2 = new View();
  child1->SetFocusBehavior(View::FocusBehavior::ALWAYS);
  child2->SetFocusBehavior(View::FocusBehavior::ALWAYS);
  bubble_widget->GetRootView()->AddChildView(child1);
  bubble_widget->GetRootView()->AddChildView(child2);
  bubble_delegate->set_close_on_deactivate(false);
  bubble_widget->Show();

  parent1->RequestFocus();

  // Navigate forwards
  GetWidget()->GetFocusManager()->AdvanceFocus(false);
  EXPECT_TRUE(parent2->HasFocus());
  GetWidget()->GetFocusManager()->AdvanceFocus(false);
  EXPECT_TRUE(parent3->HasFocus());
  GetWidget()->GetFocusManager()->AdvanceFocus(false);
  EXPECT_TRUE(child1->HasFocus());
  bubble_widget->GetFocusManager()->AdvanceFocus(false);
  EXPECT_TRUE(child2->HasFocus());
  bubble_widget->GetFocusManager()->AdvanceFocus(false);
  EXPECT_TRUE(parent4->HasFocus());

  // Navigate backwards
  GetWidget()->GetFocusManager()->AdvanceFocus(true);
  EXPECT_TRUE(child2->HasFocus());
  bubble_widget->GetFocusManager()->AdvanceFocus(true);
  EXPECT_TRUE(child1->HasFocus());
  bubble_widget->GetFocusManager()->AdvanceFocus(true);
  EXPECT_TRUE(parent3->HasFocus());
}

TEST_F(FocusManagerTest, AnchoredDialogOnContainerView) {
  // The parent Widget has four focusable views, with the middle two views
  // inside of a non-focusable grouping View. A child widget dialog has
  // two focusable views, and it's anchored to the group View. Ensure
  // that focus traverses into the anchored dialog after the 3rd parent
  // view, and then back to the 4th parent view.

  View* parent1 = new View();
  View* parent2 = new View();
  View* parent3 = new View();
  View* parent4 = new View();
  View* parent_group = new View();

  parent1->SetFocusBehavior(View::FocusBehavior::ALWAYS);
  parent2->SetFocusBehavior(View::FocusBehavior::ALWAYS);
  parent3->SetFocusBehavior(View::FocusBehavior::ALWAYS);
  parent4->SetFocusBehavior(View::FocusBehavior::ALWAYS);

  GetWidget()->GetRootView()->AddChildView(parent1);
  GetWidget()->GetRootView()->AddChildView(parent_group);
  parent_group->AddChildView(parent2);
  parent_group->AddChildView(parent3);
  GetWidget()->GetRootView()->AddChildView(parent4);

  BubbleDialogDelegateView* bubble_delegate =
      new TestBubbleDialogDelegateView(parent_group);
  test::WidgetTest::WidgetAutoclosePtr bubble_widget(
      BubbleDialogDelegateView::CreateBubble(bubble_delegate));
  bubble_widget->SetFocusTraversableParent(
      bubble_delegate->anchor_widget()->GetFocusTraversable());
  bubble_widget->SetFocusTraversableParentView(parent_group);
  View* child1 = new View();
  View* child2 = new View();
  child1->SetFocusBehavior(View::FocusBehavior::ALWAYS);
  child2->SetFocusBehavior(View::FocusBehavior::ALWAYS);
  bubble_widget->GetRootView()->AddChildView(child1);
  bubble_widget->GetRootView()->AddChildView(child2);
  bubble_delegate->set_close_on_deactivate(false);
  bubble_widget->Show();

  parent1->RequestFocus();

  // Navigate forwards
  GetWidget()->GetFocusManager()->AdvanceFocus(false);
  EXPECT_TRUE(parent2->HasFocus());
  GetWidget()->GetFocusManager()->AdvanceFocus(false);
  EXPECT_TRUE(parent3->HasFocus());
  GetWidget()->GetFocusManager()->AdvanceFocus(false);
  EXPECT_TRUE(child1->HasFocus());
  bubble_widget->GetFocusManager()->AdvanceFocus(false);
  EXPECT_TRUE(child2->HasFocus());
  bubble_widget->GetFocusManager()->AdvanceFocus(false);
  EXPECT_TRUE(parent4->HasFocus());

  // Navigate backwards
  GetWidget()->GetFocusManager()->AdvanceFocus(true);
  EXPECT_TRUE(child2->HasFocus());
  bubble_widget->GetFocusManager()->AdvanceFocus(true);
  EXPECT_TRUE(child1->HasFocus());
  bubble_widget->GetFocusManager()->AdvanceFocus(true);
  EXPECT_TRUE(parent3->HasFocus());
}

// Checks that focus traverses from a View to a bubble anchored at that View
// when in a pane.
TEST_F(FocusManagerTest, AnchoredDialogInPane) {
  // Set up a focusable view (to which we will anchor our bubble) inside an
  // AccessiblePaneView.
  View* root_view = GetWidget()->GetRootView();
  AccessiblePaneView* pane =
      root_view->AddChildView(std::make_unique<AccessiblePaneView>());
  View* anchor = pane->AddChildView(std::make_unique<View>());
  anchor->SetFocusBehavior(View::FocusBehavior::ALWAYS);

  BubbleDialogDelegateView* bubble = new TestBubbleDialogDelegateView(anchor);
  test::WidgetTest::WidgetAutoclosePtr bubble_widget(
      BubbleDialogDelegateView::CreateBubble(bubble));
  bubble_widget->SetFocusTraversableParent(
      bubble->anchor_widget()->GetFocusTraversable());
  bubble_widget->SetFocusTraversableParentView(anchor);
  bubble->set_close_on_deactivate(false);
  bubble_widget->Show();

  // We need a focusable view inside our bubble to check that focus traverses
  // in.
  View* bubble_child = bubble->AddChildView(std::make_unique<View>());
  bubble_child->SetFocusBehavior(View::FocusBehavior::ALWAYS);

  // Verify that, when in pane focus mode, focus advances from the anchor view
  // to inside the bubble.
  pane->SetPaneFocus(anchor);
  EXPECT_TRUE(anchor->HasFocus());
  GetWidget()->GetFocusManager()->AdvanceFocus(false);
  EXPECT_TRUE(bubble_child->HasFocus());
}

#if BUILDFLAG(ENABLE_DESKTOP_AURA)
// This test is specifically for the permutation where the main widget is a
// DesktopNativeWidgetAura and the bubble is a NativeWidgetAura. When focus
// moves back from the bubble to the parent widget, ensure that the DNWA's aura
// window is focused.
class DesktopWidgetFocusManagerTest : public FocusManagerTest {
 public:
  DesktopWidgetFocusManagerTest() = default;
  ~DesktopWidgetFocusManagerTest() override = default;

  // FocusManagerTest:
  void SetUp() override {
    set_native_widget_type(NativeWidgetType::kDesktop);
    FocusManagerTest::SetUp();
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(DesktopWidgetFocusManagerTest);
};

TEST_F(DesktopWidgetFocusManagerTest, AnchoredDialogInDesktopNativeWidgetAura) {
  Widget widget;
  Widget::InitParams params = CreateParams(Widget::InitParams::TYPE_WINDOW);
  params.ownership = Widget::InitParams::WIDGET_OWNS_NATIVE_WIDGET;
  params.bounds = gfx::Rect(0, 0, 1024, 768);
  widget.Init(std::move(params));
  widget.Show();
  widget.Activate();

  View* parent1 = new View();
  View* parent2 = new View();

  parent1->SetFocusBehavior(View::FocusBehavior::ALWAYS);
  parent2->SetFocusBehavior(View::FocusBehavior::ALWAYS);

  widget.GetRootView()->AddChildView(parent1);
  widget.GetRootView()->AddChildView(parent2);

  TestBubbleDialogDelegateView* bubble_delegate =
      new TestBubbleDialogDelegateView(parent2);
  bubble_delegate->UseNativeWidgetAura();
  test::WidgetTest::WidgetAutoclosePtr bubble_widget(
      BubbleDialogDelegateView::CreateBubble(bubble_delegate));
  bubble_widget->SetFocusTraversableParent(
      bubble_delegate->anchor_widget()->GetFocusTraversable());
  bubble_widget->SetFocusTraversableParentView(parent2);
  View* child = new View();
  child->SetFocusBehavior(View::FocusBehavior::ALWAYS);
  bubble_widget->GetRootView()->AddChildView(child);
  bubble_delegate->set_close_on_deactivate(false);
  bubble_widget->Show();

  widget.Activate();
  parent1->RequestFocus();
  base::RunLoop().RunUntilIdle();

  // Initially the outer widget's window is focused.
  aura::client::FocusClient* focus_client =
      aura::client::GetFocusClient(widget.GetNativeView());
  ASSERT_EQ(widget.GetNativeView(), focus_client->GetFocusedWindow());

  // Navigate forwards
  widget.GetFocusManager()->AdvanceFocus(false);
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(parent2->HasFocus());
  widget.GetFocusManager()->AdvanceFocus(false);
  EXPECT_TRUE(child->HasFocus());

  // Now the bubble widget's window is focused.
  ASSERT_NE(widget.GetNativeView(), focus_client->GetFocusedWindow());
  ASSERT_EQ(bubble_widget->GetNativeView(), focus_client->GetFocusedWindow());

  // Navigate backwards
  bubble_widget->GetFocusManager()->AdvanceFocus(true);
  EXPECT_TRUE(parent2->HasFocus());

  // Finally, the outer widget's window should be focused again.
  ASSERT_EQ(widget.GetNativeView(), focus_client->GetFocusedWindow());
}
#endif

// Ensures graceful failure if there is a focus cycle.
TEST_F(FocusManagerTest, HandlesFocusCycles) {
  // Create two side-by-side views.
  View* root_view = GetWidget()->GetRootView();
  View* left = root_view->AddChildView(std::make_unique<View>());
  View* right = root_view->AddChildView(std::make_unique<View>());

  // Create a cycle where the left view is focusable and the right isn't.
  left->SetFocusBehavior(View::FocusBehavior::ALWAYS);
  right->SetFocusBehavior(View::FocusBehavior::NEVER);
  left->SetNextFocusableView(right);
  right->SetNextFocusableView(left);

  // Set focus on the left view then make it unfocusable, which both advances
  // focus and ensures there's no candidate for focusing.
  left->RequestFocus();
  EXPECT_TRUE(left->HasFocus());
  left->SetFocusBehavior(View::FocusBehavior::NEVER);

  // At this point, we didn't crash. Just as a sanity check, ensure neither of
  // our views were incorrectly focused.
  EXPECT_FALSE(left->HasFocus());
  EXPECT_FALSE(right->HasFocus());

  // Now test focusing in reverse.
  GetFocusManager()->SetFocusedView(right);
  EXPECT_TRUE(right->HasFocus());
  GetFocusManager()->AdvanceFocus(true);

  // We don't check whether |right| has focus since if no focusable view is
  // found, AdvanceFocus() doesn't clear focus.
  EXPECT_FALSE(left->HasFocus());
}

}  // namespace views
