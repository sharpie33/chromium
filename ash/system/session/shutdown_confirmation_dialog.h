// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ASH_SYSTEM_SESSION_SHUTDOWN_CONFIRMATION_DIALOG_H_
#define ASH_SYSTEM_SESSION_SHUTDOWN_CONFIRMATION_DIALOG_H_

#include "base/callback.h"
#include "base/macros.h"
#include "base/strings/string16.h"
#include "ui/views/window/dialog_delegate.h"

namespace views {
class Label;
}

namespace ash {

// Defines a dialog for shutdown that require confirmation from users -
// more specifically for the situation where the subsequent boot is slow.
class ShutdownConfirmationDialog : public views::DialogDelegateView {
 public:
  ShutdownConfirmationDialog(int window_title_text_id,
                             int dialog_text_id,
                             base::OnceClosure on_accept_callback,
                             base::OnceClosure on_cancel_callback);
  ~ShutdownConfirmationDialog() override;

  // views::DialogDelegateView:
  bool Accept() override;
  bool Cancel() override;

  // views::WidgetDelegate:
  ui::ModalType GetModalType() const override;
  base::string16 GetWindowTitle() const override;
  bool ShouldShowCloseButton() const override;

  // views::View:
  gfx::Size CalculatePreferredSize() const override;

 private:
  const base::string16 window_title_;
  base::OnceClosure on_accept_callback_;
  base::OnceClosure on_cancel_callback_;
  views::Label* label_;

  DISALLOW_COPY_AND_ASSIGN(ShutdownConfirmationDialog);
};

}  // namespace ash

#endif  // ASH_SYSTEM_SESSION_SHUTDOWN_CONFIRMATION_DIALOG_H_
