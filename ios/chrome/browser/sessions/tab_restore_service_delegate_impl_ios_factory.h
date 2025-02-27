// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IOS_CHROME_BROWSER_SESSIONS_TAB_RESTORE_SERVICE_DELEGATE_IMPL_IOS_FACTORY_H_
#define IOS_CHROME_BROWSER_SESSIONS_TAB_RESTORE_SERVICE_DELEGATE_IMPL_IOS_FACTORY_H_

#include <memory>

#include "base/macros.h"
#include "base/no_destructor.h"
#include "components/keyed_service/ios/browser_state_keyed_service_factory.h"

class ChromeBrowserState;
class TabRestoreServiceDelegateImplIOS;

class TabRestoreServiceDelegateImplIOSFactory
    : public BrowserStateKeyedServiceFactory {
 public:
  static TabRestoreServiceDelegateImplIOS* GetForBrowserState(
      ChromeBrowserState* browser_state);

  static TabRestoreServiceDelegateImplIOSFactory* GetInstance();

 private:
  friend class base::NoDestructor<TabRestoreServiceDelegateImplIOSFactory>;

  TabRestoreServiceDelegateImplIOSFactory();
  ~TabRestoreServiceDelegateImplIOSFactory() override;

  // BrowserStateKeyedServiceFactory implementation.
  std::unique_ptr<KeyedService> BuildServiceInstanceFor(
      web::BrowserState* context) const override;

  DISALLOW_COPY_AND_ASSIGN(TabRestoreServiceDelegateImplIOSFactory);
};

#endif  // IOS_CHROME_BROWSER_SESSIONS_TAB_RESTORE_SERVICE_DELEGATE_IMPL_IOS_FACTORY_H_
