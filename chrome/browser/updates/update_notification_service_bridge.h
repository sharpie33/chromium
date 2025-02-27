// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UPDATES_UPDATE_NOTIFICATION_SERVICE_BRIDGE_H_
#define CHROME_BROWSER_UPDATES_UPDATE_NOTIFICATION_SERVICE_BRIDGE_H_

#include <memory>

#include "base/macros.h"
#include "base/optional.h"
#include "base/time/time.h"

namespace updates {

class UpdateNotificationServiceBridge {
 public:
  // Create the default UpdateNotificationServiceBridge.
  static std::unique_ptr<UpdateNotificationServiceBridge> Create();

  // Updates and persists |timestamp| in Android SharedPreferences.
  virtual void UpdateLastShownTimeStamp(base::Time timestamp);

  // Returns persisted timestamp of last shown notification from Android
  // SharedPreferences. Return nullopt if there is no data.
  virtual base::Optional<base::Time> GetLastShownTimeStamp();

  // Updates and persists |interval| in Android SharedPreferences.
  virtual void UpdateThrottleInterval(base::TimeDelta interval);

  // Returns persisted interval that might be throttled from Android
  // SharedPreferences. Return nullopt if there is no data.
  virtual base::Optional<base::TimeDelta> GetThrottleInterval();

  // Updates and persists |count| in Android SharedPreferences.
  virtual void UpdateUserDismissCount(int count);

  // Returns persisted count from Android SharedPreferences.
  virtual int GetUserDismissCount();

  // Launches Chrome activity after user clicked the notification. Launching
  // behavior may be different which depends on |state|.
  virtual void LaunchChromeActivity(int state);

  virtual ~UpdateNotificationServiceBridge() = default;

 protected:
  UpdateNotificationServiceBridge() = default;

 private:
  DISALLOW_COPY_AND_ASSIGN(UpdateNotificationServiceBridge);
};

}  // namespace updates

#endif  // CHROME_BROWSER_UPDATES_UPDATE_NOTIFICATION_SERVICE_BRIDGE_H_
