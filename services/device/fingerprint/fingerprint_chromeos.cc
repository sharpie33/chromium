// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/device/fingerprint/fingerprint_chromeos.h"

#include <string.h>

#include "base/bind.h"
#include "chromeos/dbus/biod/biod_client.h"
#include "dbus/object_path.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "services/device/fingerprint/fingerprint.h"
#include "services/device/public/mojom/fingerprint.mojom.h"

namespace device {

namespace {

chromeos::BiodClient* GetBiodClient() {
  return chromeos::BiodClient::Get();
}

// Helper functions to convert between dbus and mojo types. The dbus type comes
// from code imported from cros, so it is hard to use the mojo type there. Since
// the dbus type is imported, there are DEPs restrictions w.r.t. using it across
// the entire code-base. Chrome code outside of the interop layer with dbus
// exclusively uses the mojo type.
device::mojom::BiometricType ToMojom(biod::BiometricType type) {
  switch (type) {
    case biod::BIOMETRIC_TYPE_UNKNOWN:
      return device::mojom::BiometricType::UNKNOWN;
    case biod::BIOMETRIC_TYPE_FINGERPRINT:
      return device::mojom::BiometricType::FINGERPRINT;
    case biod::BIOMETRIC_TYPE_MAX:
      return device::mojom::BiometricType::kMaxValue;
  }
  NOTREACHED();
  return device::mojom::BiometricType::UNKNOWN;
}
device::mojom::ScanResult ToMojom(biod::ScanResult type) {
  switch (type) {
    case biod::SCAN_RESULT_SUCCESS:
      return device::mojom::ScanResult::SUCCESS;
    case biod::SCAN_RESULT_PARTIAL:
      return device::mojom::ScanResult::PARTIAL;
    case biod::SCAN_RESULT_INSUFFICIENT:
      return device::mojom::ScanResult::INSUFFICIENT;
    case biod::SCAN_RESULT_SENSOR_DIRTY:
      return device::mojom::ScanResult::SENSOR_DIRTY;
    case biod::SCAN_RESULT_TOO_SLOW:
      return device::mojom::ScanResult::TOO_SLOW;
    case biod::SCAN_RESULT_TOO_FAST:
      return device::mojom::ScanResult::TOO_FAST;
    case biod::SCAN_RESULT_IMMOBILE:
      return device::mojom::ScanResult::IMMOBILE;
    case biod::SCAN_RESULT_MAX:
      return device::mojom::ScanResult::kMaxValue;
  }
  NOTREACHED();
  return device::mojom::ScanResult::INSUFFICIENT;
}

}  // namespace

FingerprintChromeOS::FingerprintChromeOS() {
  CHECK(GetBiodClient());
  GetBiodClient()->AddObserver(this);
}

FingerprintChromeOS::~FingerprintChromeOS() {
  GetBiodClient()->RemoveObserver(this);
  if (opened_session_ == FingerprintSession::ENROLL) {
    GetBiodClient()->CancelEnrollSession(
        chromeos::EmptyVoidDBusMethodCallback());
  } else if (opened_session_ == FingerprintSession::AUTH) {
    GetBiodClient()->EndAuthSession(chromeos::EmptyVoidDBusMethodCallback());
  }
}

void FingerprintChromeOS::GetRecordsForUser(
    const std::string& user_id,
    GetRecordsForUserCallback callback) {
  get_records_pending_requests_.push(base::BindOnce(
      &FingerprintChromeOS::RunGetRecordsForUser,
      weak_ptr_factory_.GetWeakPtr(), user_id, std::move(callback)));
  if (is_request_running_)
    return;

  is_request_running_ = true;
  StartNextRequest();
}

void FingerprintChromeOS::RunGetRecordsForUser(
    const std::string& user_id,
    GetRecordsForUserCallback callback) {
  GetBiodClient()->GetRecordsForUser(
      user_id,
      base::BindOnce(&FingerprintChromeOS::OnGetRecordsForUser,
                     weak_ptr_factory_.GetWeakPtr(), std::move(callback)));
}

void FingerprintChromeOS::StartEnrollSession(const std::string& user_id,
                                             const std::string& label) {
  if (opened_session_ == FingerprintSession::ENROLL)
    return;

  GetBiodClient()->EndAuthSession(
      base::BindOnce(&FingerprintChromeOS::OnCloseAuthSessionForEnroll,
                     weak_ptr_factory_.GetWeakPtr(), user_id, label));
}

void FingerprintChromeOS::OnCloseAuthSessionForEnroll(
    const std::string& user_id,
    const std::string& label,
    bool result) {
  if (!result)
    return;

  GetBiodClient()->StartEnrollSession(
      user_id, label,
      base::BindOnce(&FingerprintChromeOS::OnStartEnrollSession,
                     weak_ptr_factory_.GetWeakPtr()));
}

void FingerprintChromeOS::CancelCurrentEnrollSession(
    CancelCurrentEnrollSessionCallback callback) {
  if (opened_session_ == FingerprintSession::ENROLL) {
    GetBiodClient()->CancelEnrollSession(std::move(callback));
    opened_session_ = FingerprintSession::NONE;
  } else {
    std::move(callback).Run(true);
  }
}

void FingerprintChromeOS::RequestRecordLabel(
    const std::string& record_path,
    RequestRecordLabelCallback callback) {
  GetBiodClient()->RequestRecordLabel(
      dbus::ObjectPath(record_path),
      base::AdaptCallbackForRepeating(std::move(callback)));
}

void FingerprintChromeOS::SetRecordLabel(const std::string& new_label,
                                         const std::string& record_path,
                                         SetRecordLabelCallback callback) {
  GetBiodClient()->SetRecordLabel(dbus::ObjectPath(record_path), new_label,
                                  std::move(callback));
}

void FingerprintChromeOS::RemoveRecord(const std::string& record_path,
                                       RemoveRecordCallback callback) {
  GetBiodClient()->RemoveRecord(dbus::ObjectPath(record_path),
                                std::move(callback));
}

void FingerprintChromeOS::StartAuthSession() {
  if (opened_session_ == FingerprintSession::AUTH)
    return;

  if (opened_session_ == FingerprintSession::ENROLL) {
    GetBiodClient()->CancelEnrollSession(
        base::BindRepeating(&FingerprintChromeOS::OnCloseEnrollSessionForAuth,
                            weak_ptr_factory_.GetWeakPtr()));
  } else {
    GetBiodClient()->StartAuthSession(
        base::BindOnce(&FingerprintChromeOS::OnStartAuthSession,
                       weak_ptr_factory_.GetWeakPtr()));
  }
}

void FingerprintChromeOS::OnCloseEnrollSessionForAuth(bool result) {
  if (!result)
    return;

  GetBiodClient()->StartAuthSession(
      base::BindOnce(&FingerprintChromeOS::OnStartAuthSession,
                     weak_ptr_factory_.GetWeakPtr()));
}

void FingerprintChromeOS::EndCurrentAuthSession(
    EndCurrentAuthSessionCallback callback) {
  if (opened_session_ == FingerprintSession::AUTH) {
    GetBiodClient()->EndAuthSession(std::move(callback));
    opened_session_ = FingerprintSession::NONE;
  } else {
    std::move(callback).Run(true);
  }
}

void FingerprintChromeOS::DestroyAllRecords(
    DestroyAllRecordsCallback callback) {
  GetBiodClient()->DestroyAllRecords(std::move(callback));
}

void FingerprintChromeOS::RequestType(RequestTypeCallback callback) {
  GetBiodClient()->RequestType(base::BindOnce(
      [](RequestTypeCallback callback, biod::BiometricType type) {
        std::move(callback).Run(ToMojom(type));
      },
      std::move(callback)));
}

void FingerprintChromeOS::AddFingerprintObserver(
    mojo::PendingRemote<mojom::FingerprintObserver> pending_observer) {
  mojo::Remote<mojom::FingerprintObserver> observer(
      std::move(pending_observer));
  observer.set_disconnect_handler(
      base::BindOnce(&FingerprintChromeOS::OnFingerprintObserverDisconnected,
                     base::Unretained(this), observer.get()));
  observers_.push_back(std::move(observer));
}

void FingerprintChromeOS::BiodServiceRestarted() {
  opened_session_ = FingerprintSession::NONE;
  for (auto& observer : observers_)
    observer->OnRestarted();
}

void FingerprintChromeOS::BiodEnrollScanDoneReceived(
    biod::ScanResult scan_result,
    bool enroll_session_complete,
    int percent_complete) {
  if (enroll_session_complete)
    opened_session_ = FingerprintSession::NONE;

  for (auto& observer : observers_) {
    observer->OnEnrollScanDone(ToMojom(scan_result), enroll_session_complete,
                               percent_complete);
  }
}

void FingerprintChromeOS::BiodAuthScanDoneReceived(
    biod::ScanResult scan_result,
    const chromeos::AuthScanMatches& matches) {
  // Convert ObjectPath to string, since mojom doesn't know definition of
  // dbus ObjectPath.
  std::vector<std::pair<std::string, std::vector<std::string>>> entries;
  for (auto& item : matches) {
    std::vector<std::string> paths;
    for (auto& object_path : item.second) {
      paths.push_back(object_path.value());
    }
    entries.emplace_back(std::move(item.first), std::move(paths));
  }

  auto casted_scan_result = static_cast<device::mojom::ScanResult>(scan_result);
  CHECK(device::mojom::IsKnownEnumValue(casted_scan_result));

  for (auto& observer : observers_) {
    observer->OnAuthScanDone(
        casted_scan_result,
        base::flat_map<std::string, std::vector<std::string>>(
            std::move(entries)));
  }
}

void FingerprintChromeOS::BiodSessionFailedReceived() {
  for (auto& observer : observers_)
    observer->OnSessionFailed();
}

void FingerprintChromeOS::OnFingerprintObserverDisconnected(
    mojom::FingerprintObserver* observer) {
  for (auto item = observers_.begin(); item != observers_.end(); ++item) {
    if (item->get() == observer) {
      observers_.erase(item);
      break;
    }
  }
}

void FingerprintChromeOS::OnStartEnrollSession(
    const dbus::ObjectPath& enroll_path) {
  if (enroll_path.IsValid()) {
    DCHECK_NE(opened_session_, FingerprintSession::ENROLL);
    opened_session_ = FingerprintSession::ENROLL;
  }
}

void FingerprintChromeOS::OnStartAuthSession(
    const dbus::ObjectPath& auth_path) {
  if (auth_path.IsValid()) {
    DCHECK_NE(opened_session_, FingerprintSession::AUTH);
    opened_session_ = FingerprintSession::AUTH;
  }
}

void FingerprintChromeOS::OnGetRecordsForUser(
    GetRecordsForUserCallback callback,
    const std::vector<dbus::ObjectPath>& records) {
  if (records.size() == 0) {
    std::move(callback).Run({base::flat_map<std::string, std::string>()});
    StartNextRequest();
    return;
  }

  DCHECK(!on_get_records_);
  on_get_records_ = std::move(callback);

  for (auto& record : records) {
    GetBiodClient()->RequestRecordLabel(
        record,
        base::BindOnce(&FingerprintChromeOS::OnGetLabelFromRecordPath,
                       weak_ptr_factory_.GetWeakPtr(), records.size(), record));
  }
}

void FingerprintChromeOS::OnGetLabelFromRecordPath(
    size_t num_records,
    const dbus::ObjectPath& record_path,
    const std::string& label) {
  records_path_to_label_[record_path.value()] = label;
  if (records_path_to_label_.size() == num_records) {
    DCHECK(on_get_records_);
    std::move(on_get_records_).Run(records_path_to_label_);
    StartNextRequest();
  }
}

void FingerprintChromeOS::StartNextRequest() {
  records_path_to_label_.clear();

  // All the pending requests complete, toggle |is_request_running_|.
  if (get_records_pending_requests_.empty()) {
    is_request_running_ = false;
    return;
  }

  // Current request completes, start running next request.
  std::move(get_records_pending_requests_.front()).Run();
  get_records_pending_requests_.pop();
}

// static
void Fingerprint::Create(
    mojo::PendingReceiver<device::mojom::Fingerprint> receiver) {
  mojo::MakeSelfOwnedReceiver(std::make_unique<FingerprintChromeOS>(),
                              std::move(receiver));
}

}  // namespace device
