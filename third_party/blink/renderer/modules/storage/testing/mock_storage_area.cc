// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/storage/testing/mock_storage_area.h"

#include "base/bind.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"

namespace blink {

MockStorageArea::MockStorageArea() = default;
MockStorageArea::~MockStorageArea() = default;

mojo::PendingRemote<mojom::blink::StorageArea>
MockStorageArea::GetInterfaceRemote() {
  mojo::PendingRemote<mojom::blink::StorageArea> result;
  receivers_.Add(this, result.InitWithNewPipeAndPassReceiver());
  return result;
}

void MockStorageArea::AddObserver(
    mojo::PendingRemote<mojom::blink::StorageAreaObserver> observer) {
  ++observer_count_;
}

void MockStorageArea::Put(
    const Vector<uint8_t>& key,
    const Vector<uint8_t>& value,
    const base::Optional<Vector<uint8_t>>& client_old_value,
    const String& source,
    PutCallback callback) {
  observed_put_ = true;
  observed_key_ = key;
  observed_value_ = value;
  observed_source_ = source;
  std::move(callback).Run(true);
}

void MockStorageArea::Delete(
    const Vector<uint8_t>& key,
    const base::Optional<Vector<uint8_t>>& client_old_value,
    const String& source,
    DeleteCallback callback) {
  observed_delete_ = true;
  observed_key_ = key;
  observed_source_ = source;
  std::move(callback).Run(true);
}

void MockStorageArea::DeleteAll(
    const String& source,
    mojo::PendingRemote<mojom::blink::StorageAreaObserver> new_observer,
    DeleteAllCallback callback) {
  observed_delete_all_ = true;
  observed_source_ = source;
  ++observer_count_;
  std::move(callback).Run(true);
}

void MockStorageArea::Get(const Vector<uint8_t>& key, GetCallback callback) {
  NOTREACHED();
}

void MockStorageArea::GetAll(
    mojo::PendingRemote<mojom::blink::StorageAreaObserver> new_observer,
    GetAllCallback callback) {
  observed_get_all_ = true;
  ++observer_count_;
  std::move(callback).Run(std::move(get_all_return_values_));
}

}  // namespace blink
