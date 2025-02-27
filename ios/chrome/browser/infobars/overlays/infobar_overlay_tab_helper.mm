// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ios/chrome/browser/infobars/overlays/infobar_overlay_tab_helper.h"

#include "base/logging.h"
#include "ios/chrome/browser/infobars/infobar_manager_impl.h"
#import "ios/chrome/browser/infobars/overlays/infobar_overlay_request_factory.h"
#import "ios/chrome/browser/infobars/overlays/infobar_overlay_request_inserter.h"
#include "ios/chrome/browser/overlays/public/overlay_request.h"
#import "ios/chrome/browser/overlays/public/overlay_request_queue.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

using infobars::InfoBar;
using infobars::InfoBarManager;

#pragma mark - InfobarOverlayTabHelper

WEB_STATE_USER_DATA_KEY_IMPL(InfobarOverlayTabHelper)

InfobarOverlayTabHelper::InfobarOverlayTabHelper(web::WebState* web_state)
    : request_inserter_(InfobarOverlayRequestInserter::FromWebState(web_state)),
      request_scheduler_(web_state, this) {}

InfobarOverlayTabHelper::~InfobarOverlayTabHelper() = default;

#pragma mark - InfobarOverlayTabHelper::OverlayRequestScheduler

InfobarOverlayTabHelper::OverlayRequestScheduler::OverlayRequestScheduler(
    web::WebState* web_state,
    InfobarOverlayTabHelper* tab_helper)
    : tab_helper_(tab_helper), scoped_observer_(this) {
  DCHECK(tab_helper_);
  InfoBarManager* manager = InfoBarManagerImpl::FromWebState(web_state);
  DCHECK(manager);
  scoped_observer_.Add(manager);
}

InfobarOverlayTabHelper::OverlayRequestScheduler::~OverlayRequestScheduler() =
    default;

void InfobarOverlayTabHelper::OverlayRequestScheduler::OnInfoBarAdded(
    InfoBar* infobar) {
  tab_helper_->request_inserter()->AddOverlayRequest(
      infobar, InfobarOverlayType::kBanner);
}

void InfobarOverlayTabHelper::OverlayRequestScheduler::OnManagerShuttingDown(
    InfoBarManager* manager) {
  scoped_observer_.Remove(manager);
}
