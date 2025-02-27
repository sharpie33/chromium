// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/public/browser/permission_type.h"

#include "base/no_destructor.h"
#include "base/stl_util.h"
#include "build/build_config.h"
#include "third_party/blink/public/mojom/permissions/permission.mojom.h"

using blink::mojom::PermissionDescriptorPtr;
using blink::mojom::PermissionName;
using blink::mojom::PermissionStatus;

namespace content {

const std::vector<PermissionType>& GetAllPermissionTypes() {
  static const base::NoDestructor<std::vector<PermissionType>>
      kAllPermissionTypes([] {
        const int NUM_TYPES = static_cast<int>(PermissionType::NUM);
        std::vector<PermissionType> all_types;
        all_types.reserve(NUM_TYPES - 2);
        for (int i = 1; i < NUM_TYPES; ++i) {
          if (i == 2)  // Skip PUSH_MESSAGING.
            continue;
          all_types.push_back(static_cast<PermissionType>(i));
        }
        return all_types;
      }());
  return *kAllPermissionTypes;
}

base::Optional<PermissionType> PermissionDescriptorToPermissionType(
    const PermissionDescriptorPtr& descriptor) {
  switch (descriptor->name) {
    case PermissionName::GEOLOCATION:
      return PermissionType::GEOLOCATION;
    case PermissionName::NOTIFICATIONS:
      return PermissionType::NOTIFICATIONS;
    case PermissionName::MIDI: {
      if (descriptor->extension && descriptor->extension->is_midi() &&
          descriptor->extension->get_midi()->sysex) {
        return PermissionType::MIDI_SYSEX;
      }
      return PermissionType::MIDI;
    }
    case PermissionName::PROTECTED_MEDIA_IDENTIFIER:
#if defined(ENABLE_PROTECTED_MEDIA_IDENTIFIER_PERMISSION)
      return PermissionType::PROTECTED_MEDIA_IDENTIFIER;
#else
      NOTIMPLEMENTED();
      return base::nullopt;
#endif  // defined(ENABLE_PROTECTED_MEDIA_IDENTIFIER_PERMISSION)
    case PermissionName::DURABLE_STORAGE:
      return PermissionType::DURABLE_STORAGE;
    case PermissionName::AUDIO_CAPTURE:
      return PermissionType::AUDIO_CAPTURE;
    case PermissionName::VIDEO_CAPTURE:
      return PermissionType::VIDEO_CAPTURE;
    case PermissionName::BACKGROUND_SYNC:
      return PermissionType::BACKGROUND_SYNC;
    case PermissionName::SENSORS:
      return PermissionType::SENSORS;
    case PermissionName::ACCESSIBILITY_EVENTS:
      return PermissionType::ACCESSIBILITY_EVENTS;
    case PermissionName::CLIPBOARD_READ:
      return PermissionType::CLIPBOARD_READ_WRITE;
    case PermissionName::CLIPBOARD_WRITE: {
      if (descriptor->extension && descriptor->extension->is_clipboard() &&
          descriptor->extension->get_clipboard()->allowWithoutSanitization) {
        return PermissionType::CLIPBOARD_READ_WRITE;
      } else {
        return PermissionType::CLIPBOARD_SANITIZED_WRITE;
      }
    }
    case PermissionName::PAYMENT_HANDLER:
      return PermissionType::PAYMENT_HANDLER;
    case PermissionName::BACKGROUND_FETCH:
      return PermissionType::BACKGROUND_FETCH;
    case PermissionName::IDLE_DETECTION:
      return PermissionType::IDLE_DETECTION;
    case PermissionName::PERIODIC_BACKGROUND_SYNC:
      return PermissionType::PERIODIC_BACKGROUND_SYNC;
    case PermissionName::WAKE_LOCK:
      if (descriptor->extension && descriptor->extension->is_wake_lock()) {
        switch (descriptor->extension->get_wake_lock()->type) {
          case blink::mojom::WakeLockType::kScreen:
            return PermissionType::WAKE_LOCK_SCREEN;
            break;
          case blink::mojom::WakeLockType::kSystem:
            return PermissionType::WAKE_LOCK_SYSTEM;
            break;
          default:
            NOTREACHED();
            return base::nullopt;
        }
      }
      break;
    case PermissionName::NFC:
      return PermissionType::NFC;
  }

  NOTREACHED();
  return base::nullopt;
}

}  // namespace content
