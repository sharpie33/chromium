// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/test/permissions/permission_request_manager_test_api.h"

#include <memory>

#include "base/bind.h"
#include "chrome/browser/permissions/permission_request_impl.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"

#if defined(TOOLKIT_VIEWS)
#include "chrome/browser/ui/views/permission_bubble/permission_prompt_bubble_view.h"
#include "chrome/browser/ui/views/permission_bubble/permission_prompt_impl.h"
#include "ui/views/widget/widget.h"
#endif

namespace test {
namespace {

// Wraps a PermissionRequestImpl so that it can pass a closure to itself to the
// PermissionRequestImpl constructor. Without this wrapper, there's no way to
// handle all destruction paths.
class TestPermisisonRequestOwner {
 public:
  explicit TestPermisisonRequestOwner(ContentSettingsType type) {
    bool user_gesture = true;
    auto decided = [](ContentSetting) {};
    request_ = std::make_unique<PermissionRequestImpl>(
        GURL("https://example.com"), type, user_gesture, base::Bind(decided),
        base::Bind(&TestPermisisonRequestOwner::DeleteThis,
                   base::Unretained(this)));
  }

  PermissionRequestImpl* request() { return request_.get(); }

 private:
  void DeleteThis() { delete this; }

  std::unique_ptr<PermissionRequestImpl> request_;

  DISALLOW_COPY_AND_ASSIGN(TestPermisisonRequestOwner);
};

}  // namespace

PermissionRequestManagerTestApi::PermissionRequestManagerTestApi(
    PermissionRequestManager* manager)
    : manager_(manager) {}

PermissionRequestManagerTestApi::PermissionRequestManagerTestApi(
    Browser* browser)
    : PermissionRequestManagerTestApi(PermissionRequestManager::FromWebContents(
          browser->tab_strip_model()->GetActiveWebContents())) {}

void PermissionRequestManagerTestApi::AddSimpleRequest(
    ContentSettingsType type) {
  TestPermisisonRequestOwner* request_owner =
      new TestPermisisonRequestOwner(type);
  manager_->AddRequest(request_owner->request());
}

gfx::NativeWindow PermissionRequestManagerTestApi::GetPromptWindow() {
#if defined(TOOLKIT_VIEWS)
  PermissionPromptImpl* prompt =
      static_cast<PermissionPromptImpl*>(manager_->view_.get());
  return prompt ? prompt->prompt_bubble_for_testing()
                      ->GetWidget()
                      ->GetNativeWindow()
                : nullptr;
#else
  NOTIMPLEMENTED();
#endif
}

void PermissionRequestManagerTestApi::SimulateWebContentsDestroyed() {
  manager_->WebContentsDestroyed();
}

}  // namespace test
