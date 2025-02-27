// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_VR_WEBXR_PERMISSION_CONTEXT_H_
#define CHROME_BROWSER_VR_WEBXR_PERMISSION_CONTEXT_H_

#include "base/macros.h"
#include "chrome/browser/permissions/permission_context_base.h"
#include "components/content_settings/core/common/content_settings_types.h"

class WebXrPermissionContext : public PermissionContextBase {
 public:
  WebXrPermissionContext(Profile* profile,
                         ContentSettingsType content_settings_type);

  ~WebXrPermissionContext() override;

 private:
  // PermissionContextBase:
  bool IsRestrictedToSecureOrigins() const override;

  ContentSettingsType content_settings_type_;

  DISALLOW_COPY_AND_ASSIGN(WebXrPermissionContext);
};

#endif  // CHROME_BROWSER_VR_WEBXR_PERMISSION_CONTEXT_H_
