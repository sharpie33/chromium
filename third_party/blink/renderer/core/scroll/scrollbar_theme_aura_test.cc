// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/scroll/scrollbar_theme_aura.h"

#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/renderer/core/scroll/scrollbar_test_suite.h"
#include "third_party/blink/renderer/platform/testing/testing_platform_support_with_mock_scheduler.h"

namespace blink {

using testing::Return;

namespace {

class ScrollbarThemeAuraButtonOverride final : public ScrollbarThemeAura {
 public:
  ScrollbarThemeAuraButtonOverride() : has_scrollbar_buttons_(true) {}

  void SetHasScrollbarButtons(bool value) { has_scrollbar_buttons_ = value; }

  bool HasScrollbarButtons(ScrollbarOrientation unused) const override {
    return has_scrollbar_buttons_;
  }

  int MinimumThumbLength(const Scrollbar& scrollbar) override {
    return ThumbThickness(scrollbar);
  }

 private:
  bool has_scrollbar_buttons_;
};

}  // namespace

class ScrollbarThemeAuraTest : public testing::Test {};

// Note that this helper only sends mouse events that are already handled on the
// compositor thread, to the scrollbar (i.e they will have the event modifier
// "kScrollbarManipulationHandledOnCompositorThread" set). The point of this
// exercise is to validate that the scrollbar parts invalidate as expected
// (since we still rely on the main thread for invalidation).
void SendEvent(Scrollbar* scrollbar,
               blink::WebInputEvent::Type type,
               gfx::PointF point) {
  const blink::WebMouseEvent web_mouse_event(
      type, point, point, blink::WebPointerProperties::Button::kLeft, 0,
      blink::WebInputEvent::kScrollbarManipulationHandledOnCompositorThread,
      base::TimeTicks::Now());
  switch (type) {
    case blink::WebInputEvent::kMouseDown:
      scrollbar->MouseDown(web_mouse_event);
      break;
    case blink::WebInputEvent::kMouseMove:
      scrollbar->MouseMoved(web_mouse_event);
      break;
    case blink::WebInputEvent::kMouseUp:
      scrollbar->MouseUp(web_mouse_event);
      break;
    default:
      // The rest are unhandled. Let the called know that this helper has not
      // yet implemented them.
      NOTIMPLEMENTED();
  }
}

TEST_F(ScrollbarThemeAuraTest, ButtonSizeHorizontal) {
  ScopedTestingPlatformSupport<TestingPlatformSupportWithMockScheduler>
      platform;

  MockScrollableArea* mock_scrollable_area = MockScrollableArea::Create();
  ScrollbarThemeAuraButtonOverride theme;
  Scrollbar* scrollbar = Scrollbar::CreateForTesting(
      mock_scrollable_area, kHorizontalScrollbar, kRegularScrollbar, &theme);

  IntRect scrollbar_size_normal_dimensions(11, 22, 444, 66);
  scrollbar->SetFrameRect(scrollbar_size_normal_dimensions);
  IntSize size1 = theme.ButtonSize(*scrollbar);
  EXPECT_EQ(66, size1.Width());
  EXPECT_EQ(66, size1.Height());

  IntRect scrollbar_size_squashed_dimensions(11, 22, 444, 666);
  scrollbar->SetFrameRect(scrollbar_size_squashed_dimensions);
  IntSize size2 = theme.ButtonSize(*scrollbar);
  EXPECT_EQ(222, size2.Width());
  EXPECT_EQ(666, size2.Height());

  ThreadState::Current()->CollectAllGarbageForTesting();
}

TEST_F(ScrollbarThemeAuraTest, ButtonSizeVertical) {
  ScopedTestingPlatformSupport<TestingPlatformSupportWithMockScheduler>
      platform;

  MockScrollableArea* mock_scrollable_area = MockScrollableArea::Create();
  ScrollbarThemeAuraButtonOverride theme;
  Scrollbar* scrollbar = Scrollbar::CreateForTesting(
      mock_scrollable_area, kVerticalScrollbar, kRegularScrollbar, &theme);

  IntRect scrollbar_size_normal_dimensions(11, 22, 44, 666);
  scrollbar->SetFrameRect(scrollbar_size_normal_dimensions);
  IntSize size1 = theme.ButtonSize(*scrollbar);
  EXPECT_EQ(44, size1.Width());
  EXPECT_EQ(44, size1.Height());

  IntRect scrollbar_size_squashed_dimensions(11, 22, 444, 666);
  scrollbar->SetFrameRect(scrollbar_size_squashed_dimensions);
  IntSize size2 = theme.ButtonSize(*scrollbar);
  EXPECT_EQ(444, size2.Width());
  EXPECT_EQ(333, size2.Height());

  ThreadState::Current()->CollectAllGarbageForTesting();
}

TEST_F(ScrollbarThemeAuraTest, NoButtonsReturnsSize0) {
  ScopedTestingPlatformSupport<TestingPlatformSupportWithMockScheduler>
      platform;

  MockScrollableArea* mock_scrollable_area = MockScrollableArea::Create();
  ScrollbarThemeAuraButtonOverride theme;
  Scrollbar* scrollbar = Scrollbar::CreateForTesting(
      mock_scrollable_area, kVerticalScrollbar, kRegularScrollbar, &theme);
  theme.SetHasScrollbarButtons(false);

  scrollbar->SetFrameRect(IntRect(1, 2, 3, 4));
  IntSize size = theme.ButtonSize(*scrollbar);
  EXPECT_EQ(0, size.Width());
  EXPECT_EQ(0, size.Height());

  ThreadState::Current()->CollectAllGarbageForTesting();
}

TEST_F(ScrollbarThemeAuraTest, ScrollbarPartsInvalidationTest) {
  ScopedTestingPlatformSupport<TestingPlatformSupportWithMockScheduler>
      platform;

  MockScrollableArea* mock_scrollable_area =
      MockScrollableArea::Create(ScrollOffset(0, 1000));
  ScrollbarThemeAuraButtonOverride theme;
  Scrollbar* scrollbar = Scrollbar::CreateForTesting(
      mock_scrollable_area, kVerticalScrollbar, kRegularScrollbar, &theme);
  ON_CALL(*mock_scrollable_area, VerticalScrollbar())
      .WillByDefault(Return(scrollbar));

  IntRect vertical_rect(1010, 0, 14, 768);
  scrollbar->SetFrameRect(vertical_rect);
  scrollbar->ClearThumbNeedsRepaint();
  scrollbar->ClearTrackNeedsRepaint();

  // Tests that mousedown on the thumb causes an invalidation.
  SendEvent(scrollbar, blink::WebInputEvent::kMouseMove, gfx::PointF(10, 20));
  SendEvent(scrollbar, blink::WebInputEvent::kMouseDown, gfx::PointF(10, 20));
  EXPECT_TRUE(scrollbar->ThumbNeedsRepaint());

  // Tests that mouseup on the thumb causes an invalidation.
  scrollbar->ClearThumbNeedsRepaint();
  SendEvent(scrollbar, blink::WebInputEvent::kMouseUp, gfx::PointF(10, 20));
  EXPECT_TRUE(scrollbar->ThumbNeedsRepaint());

  // Note that, since these tests run with the assumption that the compositor
  // thread has already handled scrolling, a "scroll" will be simulated by
  // calling SetScrollOffset. To check if the arrow was invalidated,
  // TrackNeedsRepaint needs to be used. TrackNeedsRepaint here means
  // "everything except the thumb needs to be repainted". The following verifies
  // that when the offset changes from 0 to a value > 0, an invalidation gets
  // triggered. At (0, 0) there is no upwards scroll available, so the arrow is
  // disabled. When we change the offset, it must be repainted to show available
  // scroll extent.
  EXPECT_FALSE(scrollbar->TrackNeedsRepaint());
  mock_scrollable_area->SetScrollOffset(
      ScrollOffset(0, 10),
      mojom::blink::ScrollIntoViewParams::Type::kCompositor);
  EXPECT_TRUE(scrollbar->TrackNeedsRepaint());

  // Tests that when the scroll offset changes from a value greater than 0 to a
  // value less than the max scroll offset, a track invalidation is *not*
  // triggered.
  scrollbar->ClearTrackNeedsRepaint();
  mock_scrollable_area->SetScrollOffset(
      ScrollOffset(0, 20),
      mojom::blink::ScrollIntoViewParams::Type::kCompositor);
  EXPECT_FALSE(scrollbar->TrackNeedsRepaint());

  // Tests that when the scroll offset changes to 0, a track invalidation is
  // gets triggered (for the arrow).
  scrollbar->ClearTrackNeedsRepaint();
  mock_scrollable_area->SetScrollOffset(
      ScrollOffset(0, 0),
      mojom::blink::ScrollIntoViewParams::Type::kCompositor);
  EXPECT_TRUE(scrollbar->TrackNeedsRepaint());

  // Tests that mousedown on the arrow causes an invalidation.
  scrollbar->ClearTrackNeedsRepaint();
  SendEvent(scrollbar, blink::WebInputEvent::kMouseMove, gfx::PointF(10, 760));
  SendEvent(scrollbar, blink::WebInputEvent::kMouseDown, gfx::PointF(10, 760));
  EXPECT_TRUE(scrollbar->TrackNeedsRepaint());

  // Tests that mouseup on the arrow causes an invalidation.
  scrollbar->ClearTrackNeedsRepaint();
  SendEvent(scrollbar, blink::WebInputEvent::kMouseUp, gfx::PointF(10, 760));
  EXPECT_TRUE(scrollbar->TrackNeedsRepaint());

  ThreadState::Current()->CollectAllGarbageForTesting();
}

}  // namespace blink
