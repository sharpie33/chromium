// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/location_bar/cookie_controls_bubble_view.h"

#include <memory>
#include "base/logging.h"
#include "base/metrics/user_metrics.h"
#include "base/metrics/user_metrics_action.h"
#include "base/strings/string16.h"
#include "chrome/browser/ui/tab_dialogs.h"
#include "chrome/browser/ui/views/accessibility/non_accessible_image_view.h"
#include "chrome/browser/ui/views/chrome_layout_provider.h"
#include "chrome/grit/generated_resources.h"
#include "chrome/grit/theme_resources.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/base/ui_base_types.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/background.h"
#include "ui/views/bubble/bubble_frame_view.h"
#include "ui/views/controls/image_view.h"
#include "ui/views/controls/link.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/widget/widget.h"

using base::UserMetricsAction;

namespace {

// Singleton instance of the cookie bubble. The cookie bubble can only be
// shown on the active browser window, so there is no case in which it will be
// shown twice at the same time.
static CookieControlsBubbleView* g_instance;

std::unique_ptr<views::TooltipIcon> CreateInfoIcon() {
  auto explanation_tooltip = std::make_unique<views::TooltipIcon>(
      l10n_util::GetStringUTF16(IDS_COOKIE_CONTROLS_HELP));
  explanation_tooltip->set_bubble_width(
      ChromeLayoutProvider::Get()->GetDistanceMetric(
          DISTANCE_BUBBLE_PREFERRED_WIDTH));
  explanation_tooltip->set_anchor_point_arrow(
      views::BubbleBorder::Arrow::TOP_RIGHT);
  return explanation_tooltip;
}

}  // namespace

// static
void CookieControlsBubbleView::ShowBubble(
    views::View* anchor_view,
    views::Button* highlighted_button,
    content::WebContents* web_contents,
    CookieControlsController* controller,
    CookieControlsController::Status status) {
  DCHECK(web_contents);
  if (g_instance)
    return;

  base::RecordAction(UserMetricsAction("CookieControls.Bubble.Opened"));
  g_instance =
      new CookieControlsBubbleView(anchor_view, web_contents, controller);
  g_instance->SetHighlightedButton(highlighted_button);
  views::Widget* bubble_widget =
      views::BubbleDialogDelegateView::CreateBubble(g_instance);
  controller->Update(web_contents);
  bubble_widget->Show();
}

// static
CookieControlsBubbleView* CookieControlsBubbleView::GetCookieBubble() {
  return g_instance;
}

void CookieControlsBubbleView::OnStatusChanged(
    CookieControlsController::Status new_status,
    int blocked_cookies) {
  if (status_ == new_status) {
    OnBlockedCookiesCountChanged(blocked_cookies);
    return;
  }
  if (new_status != CookieControlsController::Status::kEnabled)
    intermediate_step_ = IntermediateStep::kNone;
  status_ = new_status;
  blocked_cookies_ = blocked_cookies;
  UpdateUi();
}

void CookieControlsBubbleView::OnBlockedCookiesCountChanged(
    int blocked_cookies) {
  // The blocked cookie count changes quite frequently, so avoid unnecessary
  // UI updates if possible.
  if (blocked_cookies_ == blocked_cookies)
    return;

  blocked_cookies_ = blocked_cookies;
  GetBubbleFrameView()->UpdateWindowTitle();
}

CookieControlsBubbleView::CookieControlsBubbleView(
    views::View* anchor_view,
    content::WebContents* web_contents,
    CookieControlsController* controller)
    : LocationBarBubbleDelegateView(anchor_view, web_contents),
      controller_(controller) {
  controller_observer_.Add(controller);
  DialogDelegate::set_buttons(ui::DIALOG_BUTTON_NONE);
}

CookieControlsBubbleView::~CookieControlsBubbleView() = default;

void CookieControlsBubbleView::UpdateUi() {
  if (status_ == CookieControlsController::Status::kDisabled) {
    CloseBubble();
    return;
  }

  GetBubbleFrameView()->UpdateWindowTitle();
  text_->SetVisible(false);
  show_cookies_link_->SetVisible(false);
  header_view_->SetVisible(false);

  if (intermediate_step_ == IntermediateStep::kTurnOffButton) {
    text_->SetVisible(true);
    text_->SetText(
        l10n_util::GetStringUTF16(IDS_COOKIE_CONTROLS_NOT_WORKING_DESCRIPTION));
    auto tooltip_icon = CreateInfoIcon();
    tooltip_observer_.Add(tooltip_icon.get());
    extra_view_ = SetExtraView(std::move(tooltip_icon));
    show_cookies_link_->SetVisible(true);
  } else if (status_ == CookieControlsController::Status::kEnabled) {
    header_view_->SetVisible(true);
    header_view_->SetImage(
        ui::ResourceBundle::GetSharedInstance().GetImageSkiaNamed(
            IDR_COOKIE_BLOCKING_ON_HEADER));
    text_->SetVisible(true);
    text_->SetText(
        l10n_util::GetStringUTF16(IDS_COOKIE_CONTROLS_BLOCKED_MESSAGE));
    auto link = std::make_unique<views::Link>(
        l10n_util::GetStringUTF16(IDS_COOKIE_CONTROLS_NOT_WORKING_TITLE));
    link->set_callback(
        base::BindRepeating(&CookieControlsBubbleView::NotWorkingLinkClicked,
                            base::Unretained(this)));
    extra_view_ = SetExtraView(std::move(link));
    blocked_cookies_.reset();
  } else {
    DCHECK_EQ(status_, CookieControlsController::Status::kDisabledForSite);
    header_view_->SetVisible(true);
    header_view_->SetImage(
        ui::ResourceBundle::GetSharedInstance().GetImageSkiaNamed(
            IDR_COOKIE_BLOCKING_OFF_HEADER));
    if (extra_view_)
      extra_view_->SetVisible(false);
  }

  DialogDelegate::set_button_label(
      ui::DIALOG_BUTTON_OK,
      intermediate_step_ == IntermediateStep::kTurnOffButton
          ? l10n_util::GetStringUTF16(IDS_COOKIE_CONTROLS_TURN_OFF_BUTTON)
          : l10n_util::GetStringUTF16(IDS_COOKIE_CONTROLS_TURN_ON_BUTTON));
  DialogDelegate::set_buttons(
      (intermediate_step_ == IntermediateStep::kTurnOffButton ||
       status_ == CookieControlsController::Status::kDisabledForSite)
          ? ui::DIALOG_BUTTON_OK
          : ui::DIALOG_BUTTON_NONE);
  DialogDelegate::set_accept_callback(base::BindOnce(
      &CookieControlsBubbleView::OnDialogAccepted, base::Unretained(this)));

  DialogModelChanged();
  Layout();

  // The show_disable_cookie_blocking_ui_ state has a different title
  // configuration. To avoid jumping UI, don't resize the bubble. This should be
  // safe as the bubble in this state has less content than in Enabled state.
  if (intermediate_step_ != IntermediateStep::kTurnOffButton)
    SizeToContents();
}

void CookieControlsBubbleView::CloseBubble() {
  // Widget's Close() is async, but we don't want to use cookie_bubble_ after
  // this. Additionally web_contents() may have been destroyed.
  g_instance = nullptr;
  LocationBarBubbleDelegateView::CloseBubble();
}

void CookieControlsBubbleView::Init() {
  const ChromeLayoutProvider* provider = ChromeLayoutProvider::Get();
  SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kVertical, gfx::Insets(),
      provider->GetDistanceMetric(views::DISTANCE_RELATED_CONTROL_VERTICAL)));

  auto text = std::make_unique<views::Label>(base::string16(),
                                             views::style::CONTEXT_LABEL,
                                             views::style::STYLE_SECONDARY);
  text->SetHorizontalAlignment(gfx::HorizontalAlignment::ALIGN_LEFT);
  text->SetMultiLine(true);
  text_ = AddChildView(std::move(text));

  auto cookie_link = std::make_unique<views::Link>(
      l10n_util::GetStringUTF16(IDS_BLOCKED_COOKIES_INFO));
  cookie_link->SetMultiLine(true);
  cookie_link->set_callback(
      base::BindRepeating(&CookieControlsBubbleView::ShowCookiesLinkClicked,
                          base::Unretained(this)));
  cookie_link->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  show_cookies_link_ = AddChildView(std::move(cookie_link));

  // TODO(crbug.com/1013092): The bubble should display a header view with full
  // width without having to tweak margins.
  gfx::Insets insets = margins();
  set_margins(gfx::Insets(insets.top(), 0, insets.bottom(), 0));
  SetBorder(views::CreateEmptyBorder(0, insets.left(), 0, insets.right()));
}

void CookieControlsBubbleView::AddedToWidget() {
  auto header_view = std::make_unique<NonAccessibleImageView>();
  header_view_ = header_view.get();
  header_view_->SetBackground(views::CreateThemedSolidBackground(
      header_view_, ui::NativeTheme::kColorId_BubbleFooterBackground));
  GetBubbleFrameView()->SetHeaderView(std::move(header_view));
}

gfx::Size CookieControlsBubbleView::CalculatePreferredSize() const {
  // The total width of this view should always be identical to the width
  // of the header images.
  int width = ui::ResourceBundle::GetSharedInstance()
                  .GetImageSkiaNamed(IDR_COOKIE_BLOCKING_ON_HEADER)
                  ->width();
  return gfx::Size{width, GetHeightForWidth(width)};
}

base::string16 CookieControlsBubbleView::GetWindowTitle() const {
  switch (intermediate_step_) {
    case IntermediateStep::kTurnOffButton:
      return l10n_util::GetStringUTF16(IDS_COOKIE_CONTROLS_NOT_WORKING_TITLE);
    case IntermediateStep::kNone: {
      // Determine title based on status_ instead.
    }
  }
  switch (status_) {
    case CookieControlsController::Status::kEnabled:
      return l10n_util::GetPluralStringFUTF16(IDS_COOKIE_CONTROLS_DIALOG_TITLE,
                                              blocked_cookies_.value_or(0));
    case CookieControlsController::Status::kDisabledForSite:
      return l10n_util::GetStringUTF16(IDS_COOKIE_CONTROLS_DIALOG_TITLE_OFF);
    case CookieControlsController::Status::kUninitialized:
      return base::string16();
    case CookieControlsController::Status::kDisabled:
      NOTREACHED();
      return base::string16();
  }
}

bool CookieControlsBubbleView::ShouldShowWindowTitle() const {
  return true;
}

bool CookieControlsBubbleView::ShouldShowCloseButton() const {
  return true;
}

void CookieControlsBubbleView::WindowClosing() {
  // |cookie_bubble_| can be a new bubble by this point (as Close(); doesn't
  // call this right away). Only set to nullptr when it's this bubble.
  bool this_bubble = g_instance == this;
  if (this_bubble)
    g_instance = nullptr;

  controller_->OnBubbleUiClosing(web_contents());
}

void CookieControlsBubbleView::OnDialogAccepted() {
  if (intermediate_step_ == IntermediateStep::kTurnOffButton) {
    base::RecordAction(UserMetricsAction("CookieControls.Bubble.TurnOff"));
    controller_->OnCookieBlockingEnabledForSite(false);
  } else {
    DCHECK_EQ(status_, CookieControlsController::Status::kDisabledForSite);
    DCHECK_EQ(intermediate_step_, IntermediateStep::kNone);
    base::RecordAction(UserMetricsAction("CookieControls.Bubble.TurnOn"));
    controller_->OnCookieBlockingEnabledForSite(true);
  }
}

void CookieControlsBubbleView::ShowCookiesLinkClicked() {
  base::RecordAction(UserMetricsAction("CookieControls.Bubble.CookiesInUse"));
  TabDialogs::FromWebContents(web_contents())->ShowCollectedCookies();
  GetWidget()->Close();
}

void CookieControlsBubbleView::NotWorkingLinkClicked() {
  DCHECK_EQ(status_, CookieControlsController::Status::kEnabled);
  base::RecordAction(UserMetricsAction("CookieControls.Bubble.NotWorking"));
  // Don't go through the controller as this is an intermediary state that
  // is only relevant for the bubble UI.
  intermediate_step_ = IntermediateStep::kTurnOffButton;
  UpdateUi();
}

void CookieControlsBubbleView::OnTooltipBubbleShown(views::TooltipIcon* icon) {
  base::RecordAction(UserMetricsAction("CookieControls.Bubble.TooltipShown"));
}

void CookieControlsBubbleView::OnTooltipIconDestroying(
    views::TooltipIcon* icon) {
  tooltip_observer_.Remove(icon);
}
