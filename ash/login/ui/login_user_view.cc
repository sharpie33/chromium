// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/login/ui/login_user_view.h"

#include <memory>

#include "ash/login/ui/animated_rounded_image_view.h"
#include "ash/login/ui/hover_notifier.h"
#include "ash/login/ui/image_parser.h"
#include "ash/login/ui/login_button.h"
#include "ash/login/ui/non_accessible_view.h"
#include "ash/login/ui/user_switch_flip_animation.h"
#include "ash/login/ui/views_utils.h"
#include "ash/public/cpp/ash_constants.h"
#include "ash/public/cpp/login_constants.h"
#include "ash/public/cpp/session/user_info.h"
#include "ash/resources/vector_icons/vector_icons.h"
#include "ash/strings/grit/ash_strings.h"
#include "ash/system/user/rounded_image_view.h"
#include "base/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "components/user_manager/user_type.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/compositor/layer_animation_sequence.h"
#include "ui/compositor/layer_animator.h"
#include "ui/compositor/scoped_layer_animation_settings.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/gfx/text_elider.h"
#include "ui/views/controls/button/image_button.h"
#include "ui/views/controls/image_view.h"
#include "ui/views/controls/label.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/layout/grid_layout.h"
#include "ui/views/painter.h"

namespace ash {
namespace {

// Vertical spacing between icon, label, and authentication UI.
constexpr int kVerticalSpacingBetweenEntriesDp = 24;
// Horizontal spacing between username label and the dropdown icon.
constexpr int kDistanceBetweenUsernameAndDropdownDp = 8;
// Distance between user icon and the user label in small/extra-small layouts.
constexpr int kSmallManyDistanceFromUserIconToUserLabelDp = 16;

constexpr int kDropdownIconSizeDp = 28;

// Width/height of the user view. Ensures proper centering.
constexpr int kLargeUserViewWidthDp = 306;
constexpr int kLargeUserViewHeightDp = 346;
constexpr int kSmallUserViewWidthDp = 304;
constexpr int kExtraSmallUserViewWidthDp = 282;

// Width/height of the user image.
constexpr int kLargeUserImageSizeDp = 96;
constexpr int kSmallUserImageSizeDp = 74;
constexpr int kExtraSmallUserImageSizeDp = 60;

// Opacity for when the user view is active/focused and inactive.
constexpr float kOpaqueUserViewOpacity = 1.f;
constexpr float kTransparentUserViewOpacity = 0.63f;
constexpr float kUserFadeAnimationDurationMs = 180;

constexpr char kUserViewClassName[] = "UserView";
constexpr char kLoginUserImageClassName[] = "LoginUserImage";
constexpr char kLoginUserLabelClassName[] = "LoginUserLabel";

int GetImageSize(LoginDisplayStyle style) {
  switch (style) {
    case LoginDisplayStyle::kLarge:
      return kLargeUserImageSizeDp;
    case LoginDisplayStyle::kSmall:
      return kSmallUserImageSizeDp;
    case LoginDisplayStyle::kExtraSmall:
      return kExtraSmallUserImageSizeDp;
      break;
  }

  NOTREACHED();
  return kLargeUserImageSizeDp;
}

// An animation decoder which does not rescale based on the current image_scale.
class PassthroughAnimationDecoder
    : public AnimatedRoundedImageView::AnimationDecoder {
 public:
  PassthroughAnimationDecoder(const AnimationFrames& frames)
      : frames_(frames) {}
  ~PassthroughAnimationDecoder() override = default;

  // AnimatedRoundedImageView::AnimationDecoder:
  AnimationFrames Decode(float image_scale) override { return frames_; }

 private:
  AnimationFrames frames_;
  DISALLOW_COPY_AND_ASSIGN(PassthroughAnimationDecoder);
};

}  // namespace

// Renders a user's profile icon.
class LoginUserView::UserImage : public NonAccessibleView {
 public:
  UserImage(int size)
      : NonAccessibleView(kLoginUserImageClassName), size_(size) {
    SetLayoutManager(std::make_unique<views::FillLayout>());

    image_ = new AnimatedRoundedImageView(gfx::Size(size_, size_), size_ / 2);
    AddChildView(image_);
  }
  ~UserImage() override = default;

  void UpdateForUser(const LoginUserInfo& user) {
    // Set the initial image from |avatar| since we already have it available.
    // Then, decode the bytes via blink's PNG decoder and play any animated
    // frames if they are available.
    if (!user.basic_user_info.avatar.image.isNull())
      image_->SetImage(user.basic_user_info.avatar.image);

    // Decode the avatar using blink, as blink's PNG decoder supports APNG,
    // which is the format used for the animated avators.
    if (!user.basic_user_info.avatar.bytes.empty()) {
      DecodeAnimation(user.basic_user_info.avatar.bytes,
                      base::BindOnce(&LoginUserView::UserImage::OnImageDecoded,
                                     weak_factory_.GetWeakPtr()));
    }
  }

  void SetAnimationEnabled(bool enable) {
    animation_enabled_ = enable;
    image_->SetAnimationPlayback(
        animation_enabled_
            ? AnimatedRoundedImageView::Playback::kRepeat
            : AnimatedRoundedImageView::Playback::kFirstFrameOnly);
  }

 private:
  void OnImageDecoded(AnimationFrames animation) {
    // If there is only a single frame to display, show the existing avatar.
    if (animation.size() <= 1) {
      LOG_IF(ERROR, animation.empty()) << "Decoding user avatar failed";
      return;
    }

    image_->SetAnimationDecoder(
        std::make_unique<PassthroughAnimationDecoder>(animation),
        animation_enabled_
            ? AnimatedRoundedImageView::Playback::kRepeat
            : AnimatedRoundedImageView::Playback::kFirstFrameOnly);
  }

  AnimatedRoundedImageView* image_ = nullptr;
  int size_ = 0;
  bool animation_enabled_ = false;

  base::WeakPtrFactory<UserImage> weak_factory_{this};

  DISALLOW_COPY_AND_ASSIGN(UserImage);
};

// Shows the user's name.
class LoginUserView::UserLabel : public NonAccessibleView {
 public:
  UserLabel(LoginDisplayStyle style, int label_width)
      : NonAccessibleView(kLoginUserLabelClassName), label_width_(label_width) {
    SetLayoutManager(std::make_unique<views::FillLayout>());

    user_name_ = new views::Label();
    user_name_->SetEnabledColor(SK_ColorWHITE);
    user_name_->SetSubpixelRenderingEnabled(false);
    user_name_->SetAutoColorReadabilityEnabled(false);

    // TODO(jdufault): Figure out the correct font.
    const gfx::FontList& base_font_list = views::Label::GetDefaultFontList();

    switch (style) {
      case LoginDisplayStyle::kLarge:
        user_name_->SetFontList(base_font_list.Derive(
            11, gfx::Font::FontStyle::NORMAL, gfx::Font::Weight::LIGHT));
        break;
      case LoginDisplayStyle::kSmall:
        user_name_->SetFontList(base_font_list.Derive(
            8, gfx::Font::FontStyle::NORMAL, gfx::Font::Weight::LIGHT));
        break;
      case LoginDisplayStyle::kExtraSmall:
        // TODO(jdufault): match font against spec.
        user_name_->SetFontList(base_font_list.Derive(
            6, gfx::Font::FontStyle::NORMAL, gfx::Font::Weight::LIGHT));
        break;
    }

    AddChildView(user_name_);
  }
  ~UserLabel() override = default;

  void UpdateForUser(const LoginUserInfo& user) {
    std::string display_name = user.basic_user_info.display_name;
    // display_name can be empty in debug builds with stub users.
    if (display_name.empty())
      display_name = user.basic_user_info.display_email;

    user_name_->SetText(gfx::ElideText(base::UTF8ToUTF16(display_name),
                                       user_name_->font_list(), label_width_,
                                       gfx::ElideBehavior::ELIDE_TAIL));
  }

  const base::string16& displayed_name() const { return user_name_->GetText(); }

 private:
  views::Label* user_name_ = nullptr;
  const int label_width_;

  DISALLOW_COPY_AND_ASSIGN(UserLabel);
};

// A button embedded inside of LoginUserView, which is activated whenever the
// user taps anywhere in the LoginUserView. Previously, LoginUserView was a
// views::Button, but this breaks ChromeVox as it does not expect buttons to
// have any children (ie, the dropdown button).
class LoginUserView::TapButton : public views::Button {
 public:
  explicit TapButton(LoginUserView* parent)
      : views::Button(parent), parent_(parent) {}
  ~TapButton() override = default;

  // views::Button:
  void OnFocus() override {
    views::Button::OnFocus();
    parent_->UpdateOpacity();
  }
  void OnBlur() override {
    views::Button::OnBlur();
    parent_->UpdateOpacity();
  }

 private:
  LoginUserView* const parent_;

  DISALLOW_COPY_AND_ASSIGN(TapButton);
};

// LoginUserView is defined after LoginUserView::UserLabel so it can access the
// class members.

LoginUserView::TestApi::TestApi(LoginUserView* view) : view_(view) {}

LoginUserView::TestApi::~TestApi() = default;

LoginDisplayStyle LoginUserView::TestApi::display_style() const {
  return view_->display_style_;
}

const base::string16& LoginUserView::TestApi::displayed_name() const {
  return view_->user_label_->displayed_name();
}

views::View* LoginUserView::TestApi::user_label() const {
  return view_->user_label_;
}

views::View* LoginUserView::TestApi::tap_button() const {
  return view_->tap_button_;
}

views::View* LoginUserView::TestApi::dropdown() const {
  return view_->dropdown_;
}

LoginBaseBubbleView* LoginUserView::TestApi::menu() const {
  return view_->menu_;
}

bool LoginUserView::TestApi::is_opaque() const {
  return view_->is_opaque_;
}

// static
int LoginUserView::WidthForLayoutStyle(LoginDisplayStyle style) {
  switch (style) {
    case LoginDisplayStyle::kLarge:
      return kLargeUserViewWidthDp;
    case LoginDisplayStyle::kSmall:
      return kSmallUserViewWidthDp;
    case LoginDisplayStyle::kExtraSmall:
      return kExtraSmallUserViewWidthDp;
  }

  NOTREACHED();
  return 0;
}

LoginUserView::LoginUserView(
    LoginDisplayStyle style,
    bool show_dropdown,
    // We keep show_domain variable - even if it useless for the moment -
    // as it will be useful to implement account / profile level management.
    // Note that it could be managed by a separate entity, different from
    // device level management (indicated in the bottom).
    bool show_domain,
    const OnTap& on_tap,
    const OnRemoveWarningShown& on_remove_warning_shown,
    const OnRemove& on_remove)
    : on_tap_(on_tap),
      on_remove_warning_shown_(on_remove_warning_shown),
      on_remove_(on_remove),
      display_style_(style) {
  // show_dropdown can only be true when the user view is rendering in large
  // mode.
  DCHECK(!show_dropdown || style == LoginDisplayStyle::kLarge);
  DCHECK(!show_domain || style == LoginDisplayStyle::kLarge);
  // |on_remove_warning_shown| and |on_remove| is only available iff
  // |show_dropdown| is true.
  DCHECK(show_dropdown == !!on_remove_warning_shown);
  DCHECK(show_dropdown == !!on_remove);

  user_image_ = new UserImage(GetImageSize(style));
  int label_width =
      WidthForLayoutStyle(style) -
      2 * (kDistanceBetweenUsernameAndDropdownDp + kDropdownIconSizeDp);
  user_label_ = new UserLabel(style, label_width);
  if (show_dropdown) {
    dropdown_ = new LoginButton(this);
    dropdown_->set_has_ink_drop_action_on_click(false);
    dropdown_->SetPreferredSize(
        gfx::Size(kDropdownIconSizeDp, kDropdownIconSizeDp));
    dropdown_->SetImage(
        views::Button::STATE_NORMAL,
        gfx::CreateVectorIcon(kLockScreenDropdownIcon, gfx::kGoogleGrey200));
    dropdown_->SetFocusBehavior(FocusBehavior::ALWAYS);
  }
  tap_button_ = new TapButton(this);
  SetTapEnabled(true);

  switch (style) {
    case LoginDisplayStyle::kLarge:
      SetLargeLayout();
      break;
    case LoginDisplayStyle::kSmall:
    case LoginDisplayStyle::kExtraSmall:
      SetSmallishLayout();
      break;
  }

  // Layer rendering is needed for animation. We apply animations to child views
  // separately to reduce overdraw.
  auto setup_layer = [](views::View* view) {
    view->SetPaintToLayer();
    view->layer()->SetFillsBoundsOpaquely(false);
    view->layer()->SetOpacity(kTransparentUserViewOpacity);
    view->layer()->GetAnimator()->set_preemption_strategy(
        ui::LayerAnimator::PreemptionStrategy::
            IMMEDIATELY_ANIMATE_TO_NEW_TARGET);
  };
  setup_layer(user_image_);
  setup_layer(user_label_);
  if (dropdown_)
    setup_layer(dropdown_);

  hover_notifier_ = std::make_unique<HoverNotifier>(
      this,
      base::BindRepeating(&LoginUserView::OnHover, base::Unretained(this)));
}

LoginUserView::~LoginUserView() = default;

void LoginUserView::UpdateForUser(const LoginUserInfo& user, bool animate) {
  current_user_ = user;

  if (menu_ && menu_->parent()) {
    menu_->parent()->RemoveChildView(menu_);
    delete menu_;
  }

  menu_ = new LoginUserMenuView(
      base::UTF8ToUTF16(current_user_.basic_user_info.display_name),
      base::UTF8ToUTF16(current_user_.basic_user_info.display_email),
      current_user_.basic_user_info.type, current_user_.is_device_owner,
      dropdown_ /*anchor_view*/, dropdown_ /*bubble_opener*/,
      current_user_.can_remove /*show_remove_user*/, on_remove_warning_shown_,
      on_remove_);
  menu_->SetVisible(false);

  if (animate) {
    // Stop any existing animation.
    user_image_->layer()->GetAnimator()->StopAnimating();

    // Create the image flip animation.
    auto image_transition = std::make_unique<UserSwitchFlipAnimation>(
        user_image_->width(), 0 /*start_degrees*/, 90 /*midpoint_degrees*/,
        180 /*end_degrees*/,
        base::TimeDelta::FromMilliseconds(
            login_constants::kChangeUserAnimationDurationMs),
        gfx::Tween::Type::EASE_OUT,
        base::BindOnce(&LoginUserView::UpdateCurrentUserState,
                       base::Unretained(this)));
    auto* image_sequence =
        new ui::LayerAnimationSequence(std::move(image_transition));
    user_image_->layer()->GetAnimator()->StartAnimation(image_sequence);

    // Create opacity fade animation, which applies to the entire element.
    bool is_opaque = this->is_opaque_;
    auto make_opacity_sequence = [is_opaque]() {
      auto make_opacity_element = [](float target_opacity) {
        auto element = ui::LayerAnimationElement::CreateOpacityElement(
            target_opacity,
            base::TimeDelta::FromMilliseconds(
                login_constants::kChangeUserAnimationDurationMs / 2.0f));
        element->set_tween_type(gfx::Tween::Type::EASE_OUT);
        return element;
      };

      auto* opacity_sequence = new ui::LayerAnimationSequence();
      opacity_sequence->AddElement(make_opacity_element(0 /*target_opacity*/));
      opacity_sequence->AddElement(make_opacity_element(
          is_opaque ? kOpaqueUserViewOpacity
                    : kTransparentUserViewOpacity /*target_opacity*/));
      return opacity_sequence;
    };
    user_image_->layer()->GetAnimator()->StartAnimation(
        make_opacity_sequence());
    user_label_->layer()->GetAnimator()->StartAnimation(
        make_opacity_sequence());
    if (dropdown_) {
      dropdown_->layer()->GetAnimator()->StartAnimation(
          make_opacity_sequence());
    }
  } else {
    // Do not animate, so directly update to the current user.
    UpdateCurrentUserState();
  }
}

void LoginUserView::SetForceOpaque(bool force_opaque) {
  force_opaque_ = force_opaque;
  UpdateOpacity();
}

void LoginUserView::SetTapEnabled(bool enabled) {
  tap_button_->SetFocusBehavior(enabled ? FocusBehavior::ALWAYS
                                        : FocusBehavior::NEVER);
}

const char* LoginUserView::GetClassName() const {
  return kUserViewClassName;
}

gfx::Size LoginUserView::CalculatePreferredSize() const {
  switch (display_style_) {
    case LoginDisplayStyle::kLarge:
      return gfx::Size(kLargeUserViewWidthDp, kLargeUserViewHeightDp);
    case LoginDisplayStyle::kSmall:
      return gfx::Size(kSmallUserViewWidthDp, kSmallUserImageSizeDp);
    case LoginDisplayStyle::kExtraSmall:
      return gfx::Size(kExtraSmallUserViewWidthDp, kExtraSmallUserImageSizeDp);
  }

  NOTREACHED();
  return gfx::Size();
}

void LoginUserView::Layout() {
  views::View::Layout();
  tap_button_->SetBoundsRect(GetLocalBounds());
}

void LoginUserView::RequestFocus() {
  tap_button_->RequestFocus();
}

void LoginUserView::ButtonPressed(views::Button* sender,
                                  const ui::Event& event) {
  // Handle click on the dropdown arrow.
  if (sender == dropdown_) {
    DCHECK(dropdown_);
    DCHECK(menu_);

    // If menu is showing, just close it
    if (menu_->GetVisible()) {
      menu_->Hide();
      return;
    }

    bool opener_focused =
        menu_->GetBubbleOpener() && menu_->GetBubbleOpener()->HasFocus();

    if (!menu_->parent())
      login_views_utils::GetBubbleContainer(this)->AddChildView(menu_);

    // Reset state in case the remove-user button was clicked once previously.
    menu_->ResetState();
    menu_->Show();

    // If the menu was opened by pressing Enter on the focused dropdown, focus
    // should automatically go to the remove-user button (for keyboard
    // accessibility).
    if (opener_focused)
      menu_->RequestFocus();

    return;
  }

  // Run generic on_tap handler for any other click.
  on_tap_.Run();
}

void LoginUserView::OnHover(bool has_hover) {
  UpdateOpacity();
}

void LoginUserView::UpdateCurrentUserState() {
  auto email = base::UTF8ToUTF16(current_user_.basic_user_info.display_email);
  tap_button_->SetAccessibleName(email);
  if (dropdown_) {
    dropdown_->SetAccessibleName(l10n_util::GetStringFUTF16(
        IDS_ASH_LOGIN_POD_MENU_BUTTON_ACCESSIBLE_NAME, email));
  }

  user_image_->UpdateForUser(current_user_);
  user_label_->UpdateForUser(current_user_);
  Layout();
}

void LoginUserView::UpdateOpacity() {
  bool was_opaque = is_opaque_;
  is_opaque_ =
      force_opaque_ || tap_button_->IsMouseHovered() || tap_button_->HasFocus();
  if (was_opaque == is_opaque_)
    return;

  // Animate to new opacity.
  auto build_settings = [](views::View* view)
      -> std::unique_ptr<ui::ScopedLayerAnimationSettings> {
    auto settings = std::make_unique<ui::ScopedLayerAnimationSettings>(
        view->layer()->GetAnimator());
    settings->SetTransitionDuration(
        base::TimeDelta::FromMilliseconds(kUserFadeAnimationDurationMs));
    settings->SetTweenType(gfx::Tween::Type::EASE_IN_OUT);
    return settings;
  };
  std::unique_ptr<ui::ScopedLayerAnimationSettings> user_image_settings =
      build_settings(user_image_);
  std::unique_ptr<ui::ScopedLayerAnimationSettings> user_label_settings =
      build_settings(user_label_);
  float target_opacity =
      is_opaque_ ? kOpaqueUserViewOpacity : kTransparentUserViewOpacity;
  user_image_->layer()->SetOpacity(target_opacity);
  user_label_->layer()->SetOpacity(target_opacity);
  if (dropdown_) {
    std::unique_ptr<ui::ScopedLayerAnimationSettings> dropdown_settings =
        build_settings(dropdown_);
    dropdown_->layer()->SetOpacity(target_opacity);
  }

  // Animate avatar only if we are opaque.
  user_image_->SetAnimationEnabled(is_opaque_);
}

void LoginUserView::SetLargeLayout() {
  // Add views in tabbing order; they are rendered in a different order below.
  AddChildView(user_image_);
  AddChildView(user_label_);
  AddChildView(tap_button_);
  if (dropdown_)
    AddChildView(dropdown_);

  // Use views::GridLayout instead of views::BoxLayout because views::BoxLayout
  // lays out children according to the view->children order.
  views::GridLayout* layout =
      SetLayoutManager(std::make_unique<views::GridLayout>());

  constexpr int kImageColumnId = 0;
  constexpr int kLabelDropdownColumnId = 1;
  constexpr int kLabelDomainColumnId = 2;

  {
    views::ColumnSet* image = layout->AddColumnSet(kImageColumnId);
    image->AddColumn(views::GridLayout::CENTER, views::GridLayout::CENTER,
                     1 /*resize_percent*/, views::GridLayout::USE_PREF,
                     0 /*fixed_width*/, 0 /*min_width*/);
  }

  {
    views::ColumnSet* label_dropdown =
        layout->AddColumnSet(kLabelDropdownColumnId);
    label_dropdown->AddPaddingColumn(1.0f /*resize_percent*/, 0 /*width*/);
    if (dropdown_) {
      label_dropdown->AddPaddingColumn(
          0 /*resize_percent*/, dropdown_->GetPreferredSize().width() +
                                    kDistanceBetweenUsernameAndDropdownDp);
    }
    label_dropdown->AddColumn(views::GridLayout::CENTER,
                              views::GridLayout::CENTER, 0 /*resize_percent*/,
                              views::GridLayout::USE_PREF, 0 /*fixed_width*/,
                              0 /*min_width*/);
    if (dropdown_) {
      label_dropdown->AddPaddingColumn(0 /*resize_percent*/,
                                       kDistanceBetweenUsernameAndDropdownDp);
      label_dropdown->AddColumn(views::GridLayout::CENTER,
                                views::GridLayout::CENTER, 0 /*resize_percent*/,
                                views::GridLayout::USE_PREF, 0 /*fixed_width*/,
                                0 /*min_width*/);
    }
    label_dropdown->AddPaddingColumn(1.0f /*resize_percent*/, 0 /*width*/);
  }

  {
    views::ColumnSet* label_domain = layout->AddColumnSet(kLabelDomainColumnId);
    label_domain->AddColumn(views::GridLayout::CENTER,
                            views::GridLayout::CENTER, 1 /*resize_percent*/,
                            views::GridLayout::USE_PREF, 0 /*fixed_width*/,
                            0 /*min_width*/);
  }

  auto add_padding = [&](int amount) {
    layout->AddPaddingRow(0 /*vertical_resize*/, amount /*size*/);
  };

  // Add views in rendering order.
  // Image
  layout->StartRow(0 /*vertical_resize*/, kImageColumnId);
  layout->AddExistingView(user_image_);

  add_padding(kVerticalSpacingBetweenEntriesDp);

  // Label/dropdown.
  layout->StartRow(0 /*vertical_resize*/, kLabelDropdownColumnId);
  layout->AddExistingView(user_label_);
  if (dropdown_)
    layout->AddExistingView(dropdown_);
}

void LoginUserView::SetSmallishLayout() {
  SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kHorizontal, gfx::Insets(),
      kSmallManyDistanceFromUserIconToUserLabelDp));

  AddChildView(user_image_);
  AddChildView(user_label_);
  AddChildView(tap_button_);
}

}  // namespace ash
