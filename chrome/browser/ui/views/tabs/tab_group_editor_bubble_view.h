// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_TABS_TAB_GROUP_EDITOR_BUBBLE_VIEW_H_
#define CHROME_BROWSER_UI_VIEWS_TABS_TAB_GROUP_EDITOR_BUBBLE_VIEW_H_

#include "base/strings/string16.h"
#include "chrome/browser/ui/views/tabs/tab_group_header.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/controls/textfield/textfield_controller.h"

class TabController;

namespace gfx {
class Size;
}

namespace tab_groups {
enum class TabGroupColorId;
class TabGroupId;
}  // namespace tab_groups

class ColorPickerView;
class TabGroupHeader;

// A dialog for changing a tab group's visual parameters.
class TabGroupEditorBubbleView : public views::BubbleDialogDelegateView {
 public:
  static constexpr int TAB_GROUP_HEADER_CXMENU_NEW_TAB_IN_GROUP = 13;
  static constexpr int TAB_GROUP_HEADER_CXMENU_UNGROUP = 14;
  static constexpr int TAB_GROUP_HEADER_CXMENU_CLOSE_GROUP = 15;
  static constexpr int TAB_GROUP_HEADER_CXMENU_FEEDBACK = 16;

  // Shows the editor for |group|. Returns an *unowned* pointer to the
  // bubble's widget.
  static views::Widget* Show(TabGroupHeader* anchor_view,
                             TabController* tab_controller,
                             const tab_groups::TabGroupId& group);

  // views::BubbleDialogDelegateView:
  gfx::Size CalculatePreferredSize() const override;
  ui::ModalType GetModalType() const override;
  views::View* GetInitiallyFocusedView() override;

 private:
  TabGroupEditorBubbleView(TabGroupHeader* anchor_view,
                           TabController* tab_controller,
                           const tab_groups::TabGroupId& group);
  ~TabGroupEditorBubbleView() override;

  void UpdateGroup();

  SkColor background_color() const { return color(); }

  void OnBubbleClose();

  TabController* const tab_controller_;
  const tab_groups::TabGroupId group_;

  class TitleFieldController : public views::TextfieldController {
   public:
    explicit TitleFieldController(TabGroupEditorBubbleView* parent)
        : parent_(parent) {}
    ~TitleFieldController() override = default;

    // views::TextfieldController:
    void ContentsChanged(views::Textfield* sender,
                         const base::string16& new_contents) override;
    bool HandleKeyEvent(views::Textfield* sender,
                        const ui::KeyEvent& key_event) override;

   private:
    TabGroupEditorBubbleView* const parent_;
  };

  TitleFieldController title_field_controller_;

  class ButtonListener : public views::ButtonListener {
   public:
    explicit ButtonListener(TabController* tab_controller,
                            TabGroupHeader* anchor_view,
                            tab_groups::TabGroupId group);

    // views::ButtonListener:
    void ButtonPressed(views::Button* sender, const ui::Event& event) override;

   private:
    TabController* const tab_controller_;
    TabGroupHeader* anchor_view_;
    const tab_groups::TabGroupId group_;
  };

  ButtonListener button_listener_;

  views::Textfield* title_field_;

  std::vector<tab_groups::TabGroupColorId> color_ids_;
  std::vector<std::pair<SkColor, base::string16>> colors_;
  ColorPickerView* color_selector_;

  // Creates the set of tab group colors to display and returns the color that
  // is initially selected.
  SkColor InitColorSet();

  base::string16 title_at_opening_;
};

#endif  // CHROME_BROWSER_UI_VIEWS_TABS_TAB_GROUP_EDITOR_BUBBLE_VIEW_H_
