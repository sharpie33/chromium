// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/services/storage/dom_storage/session_storage_area_impl.h"

#include "base/bind.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/metrics/histogram_macros.h"
#include "components/services/storage/dom_storage/session_storage_data_map.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "third_party/leveldatabase/env_chromium.h"

namespace storage {

SessionStorageAreaImpl::SessionStorageAreaImpl(
    SessionStorageMetadata::NamespaceEntry namespace_entry,
    url::Origin origin,
    scoped_refptr<SessionStorageDataMap> data_map,
    RegisterNewAreaMap register_new_map_callback)
    : namespace_entry_(namespace_entry),
      origin_(std::move(origin)),
      shared_data_map_(std::move(data_map)),
      register_new_map_callback_(std::move(register_new_map_callback)) {}

SessionStorageAreaImpl::~SessionStorageAreaImpl() {
  if (receiver_.is_bound())
    shared_data_map_->RemoveBindingReference();
}

void SessionStorageAreaImpl::Bind(
    mojo::PendingReceiver<blink::mojom::StorageArea> receiver) {
  if (IsBound()) {
    receiver_.reset();
  } else {
    shared_data_map_->AddBindingReference();
  }
  receiver_.Bind(std::move(receiver));
  receiver_.set_disconnect_handler(base::BindOnce(
      &SessionStorageAreaImpl::OnConnectionError, base::Unretained(this)));
}

std::unique_ptr<SessionStorageAreaImpl> SessionStorageAreaImpl::Clone(
    SessionStorageMetadata::NamespaceEntry namespace_entry) {
  DCHECK(namespace_entry_ != namespace_entry);
  return base::WrapUnique(new SessionStorageAreaImpl(
      namespace_entry, origin_, shared_data_map_, register_new_map_callback_));
}

void SessionStorageAreaImpl::NotifyObserversAllDeleted() {
  for (auto& observer : observers_) {
    // Renderer process expects |source| to always be two newline separated
    // strings. Note that we don't bother checking if storage was actually
    // empty since that might require loading the map where we otherwise
    // wouldn't need to. A side-effect is that browser-initiated storage removal
    // may result in a redundant "clear" StorageEvent on an already-empty
    // StorageArea.
    observer->AllDeleted(/*was_nonempty=*/true, "\n");
  };
}

// blink::mojom::StorageArea:
void SessionStorageAreaImpl::AddObserver(
    mojo::PendingRemote<blink::mojom::StorageAreaObserver> observer) {
  observers_.Add(std::move(observer));
}

void SessionStorageAreaImpl::Put(
    const std::vector<uint8_t>& key,
    const std::vector<uint8_t>& value,
    const base::Optional<std::vector<uint8_t>>& client_old_value,
    const std::string& source,
    PutCallback callback) {
  DCHECK(IsBound());
  DCHECK_NE(0, shared_data_map_->map_data()->ReferenceCount());
  if (shared_data_map_->map_data()->ReferenceCount() > 1)
    CreateNewMap(NewMapType::FORKED, base::nullopt);
  shared_data_map_->storage_area()->Put(key, value, client_old_value, source,
                                        std::move(callback));
}

void SessionStorageAreaImpl::Delete(
    const std::vector<uint8_t>& key,
    const base::Optional<std::vector<uint8_t>>& client_old_value,
    const std::string& source,
    DeleteCallback callback) {
  DCHECK(IsBound());
  DCHECK_NE(0, shared_data_map_->map_data()->ReferenceCount());
  if (shared_data_map_->map_data()->ReferenceCount() > 1)
    CreateNewMap(NewMapType::FORKED, base::nullopt);
  shared_data_map_->storage_area()->Delete(key, client_old_value, source,
                                           std::move(callback));
}

void SessionStorageAreaImpl::DeleteAll(
    const std::string& source,
    mojo::PendingRemote<blink::mojom::StorageAreaObserver> new_observer,
    DeleteAllCallback callback) {
  // Note: This can be called by the Clear Browsing Data flow, and thus doesn't
  // have to be bound.
  if (shared_data_map_->map_data()->ReferenceCount() > 1) {
    CreateNewMap(NewMapType::EMPTY_FROM_DELETE_ALL, source);
    if (new_observer)
      AddObserver(std::move(new_observer));
    std::move(callback).Run(true);
    return;
  }
  shared_data_map_->storage_area()->DeleteAll(
      source, /*new_observer=*/mojo::NullRemote(),
      base::BindOnce(&SessionStorageAreaImpl::OnDeleteAllResult,
                     weak_ptr_factory_.GetWeakPtr(), std::move(new_observer),
                     std::move(callback)));
}

void SessionStorageAreaImpl::Get(const std::vector<uint8_t>& key,
                                 GetCallback callback) {
  DCHECK(IsBound());
  DCHECK_NE(0, shared_data_map_->map_data()->ReferenceCount());
  shared_data_map_->storage_area()->Get(key, std::move(callback));
}

void SessionStorageAreaImpl::GetAll(
    mojo::PendingRemote<blink::mojom::StorageAreaObserver> new_observer,
    GetAllCallback callback) {
  DCHECK(IsBound());
  DCHECK_NE(0, shared_data_map_->map_data()->ReferenceCount());
  shared_data_map_->storage_area()->GetAll(
      /*new_observer=*/mojo::NullRemote(),
      base::BindOnce(&SessionStorageAreaImpl::OnGetAllResult,
                     weak_ptr_factory_.GetWeakPtr(), std::move(new_observer),
                     std::move(callback)));
}

// Note: this can be called after invalidation of the |namespace_entry_|.
void SessionStorageAreaImpl::OnConnectionError() {
  shared_data_map_->RemoveBindingReference();
  // Make sure we totally unbind the receiver - this doesn't seem to happen
  // automatically on connection error. The bound status is used in the
  // destructor to know if |RemoveBindingReference| was already called.
  if (receiver_.is_bound())
    receiver_.reset();
}

void SessionStorageAreaImpl::OnGetAllResult(
    mojo::PendingRemote<blink::mojom::StorageAreaObserver> new_observer,
    GetAllCallback callback,
    std::vector<blink::mojom::KeyValuePtr> entries) {
  std::move(callback).Run(std::move(entries));
  if (new_observer)
    AddObserver(std::move(new_observer));
}

void SessionStorageAreaImpl::OnDeleteAllResult(
    mojo::PendingRemote<blink::mojom::StorageAreaObserver> new_observer,
    DeleteAllCallback callback,
    bool was_nonempty) {
  std::move(callback).Run(true);
  if (new_observer)
    AddObserver(std::move(new_observer));
}

void SessionStorageAreaImpl::CreateNewMap(
    NewMapType map_type,
    const base::Optional<std::string>& delete_all_source) {
  bool bound = IsBound();
  if (bound)
    shared_data_map_->RemoveBindingReference();
  switch (map_type) {
    case NewMapType::FORKED:
      shared_data_map_ = SessionStorageDataMap::CreateClone(
          shared_data_map_->listener(),
          register_new_map_callback_.Run(namespace_entry_, origin_),
          shared_data_map_);
      break;
    case NewMapType::EMPTY_FROM_DELETE_ALL: {
      // The code optimizes the 'delete all' for shared maps by just creating
      // a new map instead of forking. However, we still need the observers to
      // be correctly called. To do that, we manually call them here.
      shared_data_map_ = SessionStorageDataMap::CreateEmpty(
          shared_data_map_->listener(),
          register_new_map_callback_.Run(namespace_entry_, origin_),
          shared_data_map_->storage_area()->database());
      break;
    }
  }
  if (bound)
    shared_data_map_->AddBindingReference();
}

}  // namespace storage
