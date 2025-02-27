// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/services/multidevice_setup/host_verifier_impl.h"

#include <memory>
#include <string>
#include <utility>

#include "base/macros.h"
#include "base/test/simple_test_clock.h"
#include "base/timer/mock_timer.h"
#include "chromeos/components/multidevice/remote_device_test_util.h"
#include "chromeos/components/multidevice/software_feature.h"
#include "chromeos/components/multidevice/software_feature_state.h"
#include "chromeos/services/device_sync/proto/cryptauth_common.pb.h"
#include "chromeos/services/device_sync/public/cpp/fake_device_sync_client.h"
#include "chromeos/services/multidevice_setup/fake_host_backend_delegate.h"
#include "chromeos/services/multidevice_setup/fake_host_verifier.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace chromeos {

namespace multidevice_setup {

namespace {

const int64_t kTestTimeMs = 1500000000000;

constexpr const multidevice::SoftwareFeature kPotentialHostSoftwareFeatures[] =
    {multidevice::SoftwareFeature::kSmartLockHost,
     multidevice::SoftwareFeature::kInstantTetheringHost,
     multidevice::SoftwareFeature::kMessagesForWebHost};

const char kRetryTimestampPrefName[] =
    "multidevice_setup.current_retry_timestamp_ms";
const char kLastUsedTimeDeltaMsPrefName[] =
    "multidevice_setup.last_used_time_delta_ms";

const int64_t kFirstRetryDeltaMs = 10 * 60 * 1000;
const double kExponentialBackoffMultiplier = 1.5;

enum class HostState {
  // A device has not been marked as a BetterTogether host.
  kHostNotSet,

  // A device has been marked as a BetterTogether host, but that device has not
  // enabled any of its individual features yet.
  kHostSetButFeaturesDisabled,

  // A device has been marked as a BetterTogether host, and that device has
  // enabled at least one of its individual features.
  kHostSetAndFeaturesEnabled
};

}  // namespace

class MultiDeviceSetupHostVerifierImplTest
    : public ::testing::TestWithParam<bool> {
 protected:
  MultiDeviceSetupHostVerifierImplTest()
      : test_device_(multidevice::CreateRemoteDeviceRefForTest()) {}
  ~MultiDeviceSetupHostVerifierImplTest() override = default;

  // testing::Test:
  void SetUp() override {
    if (GetParam())
      GetMutableRemoteDevice(test_device_)->instance_id.clear();

    fake_host_backend_delegate_ = std::make_unique<FakeHostBackendDelegate>();

    fake_device_sync_client_ =
        std::make_unique<device_sync::FakeDeviceSyncClient>();

    test_pref_service_ =
        std::make_unique<sync_preferences::TestingPrefServiceSyncable>();
    HostVerifierImpl::RegisterPrefs(test_pref_service_->registry());

    test_clock_ = std::make_unique<base::SimpleTestClock>();
    test_clock_->SetNow(base::Time::FromJavaTime(kTestTimeMs));
  }

  void TearDown() override {
    if (fake_observer_)
      host_verifier_->RemoveObserver(fake_observer_.get());
  }

  void CreateVerifier(HostState initial_host_state,
                      int64_t initial_timer_pref_value = 0,
                      int64_t initial_time_delta_pref_value = 0) {
    SetHostState(initial_host_state);
    test_pref_service_->SetInt64(kRetryTimestampPrefName,
                                 initial_timer_pref_value);
    test_pref_service_->SetInt64(kLastUsedTimeDeltaMsPrefName,
                                 initial_time_delta_pref_value);

    auto mock_retry_timer = std::make_unique<base::MockOneShotTimer>();
    mock_retry_timer_ = mock_retry_timer.get();

    auto mock_sync_timer = std::make_unique<base::MockOneShotTimer>();
    mock_sync_timer_ = mock_sync_timer.get();

    host_verifier_ = HostVerifierImpl::Factory::Get()->BuildInstance(
        fake_host_backend_delegate_.get(), fake_device_sync_client_.get(),
        test_pref_service_.get(), test_clock_.get(),
        std::move(mock_retry_timer), std::move(mock_sync_timer));

    fake_observer_ = std::make_unique<FakeHostVerifierObserver>();
    host_verifier_->AddObserver(fake_observer_.get());
  }

  void RemoveTestDeviceCryptoData() {
    GetMutableRemoteDevice(test_device_)->public_key.clear();
    GetMutableRemoteDevice(test_device_)->beacon_seeds.clear();
    GetMutableRemoteDevice(test_device_)->persistent_symmetric_key.clear();
  }

  void SetHostState(HostState host_state) {
    for (const auto& feature : kPotentialHostSoftwareFeatures) {
      GetMutableRemoteDevice(test_device_)->software_features[feature] =
          host_state == HostState::kHostSetAndFeaturesEnabled
              ? multidevice::SoftwareFeatureState::kEnabled
              : multidevice::SoftwareFeatureState::kSupported;
    }

    if (host_state == HostState::kHostNotSet)
      fake_host_backend_delegate_->NotifyHostChangedOnBackend(base::nullopt);
    else
      fake_host_backend_delegate_->NotifyHostChangedOnBackend(test_device_);

    fake_device_sync_client_->NotifyNewDevicesSynced();
  }

  void VerifyState(bool expected_is_verified,
                   size_t expected_num_verified_events,
                   int64_t expected_retry_timestamp_value,
                   int64_t expected_retry_delta_value) {
    EXPECT_EQ(expected_is_verified, host_verifier_->IsHostVerified());
    EXPECT_EQ(expected_num_verified_events,
              fake_observer_->num_host_verifications());
    EXPECT_EQ(expected_retry_timestamp_value,
              test_pref_service_->GetInt64(kRetryTimestampPrefName));
    EXPECT_EQ(expected_retry_delta_value,
              test_pref_service_->GetInt64(kLastUsedTimeDeltaMsPrefName));

    // If a retry timestamp is set, the timer should be running.
    EXPECT_EQ(expected_retry_timestamp_value != 0,
              mock_retry_timer_->IsRunning());
  }

  void InvokePendingDeviceNotificationCall(bool success) {
    if (test_device_.instance_id().empty()) {
      // Verify input parameters to FindEligibleDevices().
      EXPECT_EQ(multidevice::SoftwareFeature::kBetterTogetherHost,
                fake_device_sync_client_->find_eligible_devices_inputs_queue()
                    .front()
                    .software_feature);

      fake_device_sync_client_->InvokePendingFindEligibleDevicesCallback(
          success
              ? device_sync::mojom::NetworkRequestResult::kSuccess
              : device_sync::mojom::NetworkRequestResult::kInternalServerError,
          multidevice::RemoteDeviceRefList() /* eligible_devices */,
          multidevice::RemoteDeviceRefList() /* ineligible_devices */);
      return;
    }

    // Verify input parameters to NotifyDevices().
    EXPECT_EQ(std::vector<std::string>{test_device_.instance_id()},
              fake_device_sync_client_->notify_devices_inputs_queue()
                  .front()
                  .device_instance_ids);
    EXPECT_EQ(cryptauthv2::TargetService::DEVICE_SYNC,
              fake_device_sync_client_->notify_devices_inputs_queue()
                  .front()
                  .target_service);
    EXPECT_EQ(multidevice::SoftwareFeature::kBetterTogetherHost,
              fake_device_sync_client_->notify_devices_inputs_queue()
                  .front()
                  .feature);

    fake_device_sync_client_->InvokePendingNotifyDevicesCallback(
        success
            ? device_sync::mojom::NetworkRequestResult::kSuccess
            : device_sync::mojom::NetworkRequestResult::kInternalServerError);
  }

  void SimulateRetryTimePassing(const base::TimeDelta& delta,
                                bool simulate_timeout = false) {
    test_clock_->Advance(delta);

    if (simulate_timeout)
      mock_retry_timer_->Fire();
  }

  void FireSyncTimerAndVerifySyncOccurred() {
    EXPECT_TRUE(mock_sync_timer_->IsRunning());
    mock_sync_timer_->Fire();
    fake_device_sync_client_->InvokePendingForceSyncNowCallback(
        true /* success */);
    SetHostState(HostState::kHostSetAndFeaturesEnabled);
  }

  bool DoesTestDeviceHaveInstanceId() const {
    return !test_device_.instance_id().empty();
  }

  FakeHostBackendDelegate* fake_host_backend_delegate() {
    return fake_host_backend_delegate_.get();
  }

 private:
  multidevice::RemoteDeviceRef test_device_;

  std::unique_ptr<FakeHostVerifierObserver> fake_observer_;
  std::unique_ptr<FakeHostBackendDelegate> fake_host_backend_delegate_;
  std::unique_ptr<device_sync::FakeDeviceSyncClient> fake_device_sync_client_;
  std::unique_ptr<sync_preferences::TestingPrefServiceSyncable>
      test_pref_service_;
  std::unique_ptr<base::SimpleTestClock> test_clock_;
  base::MockOneShotTimer* mock_retry_timer_ = nullptr;
  base::MockOneShotTimer* mock_sync_timer_ = nullptr;

  std::unique_ptr<HostVerifier> host_verifier_;

  DISALLOW_COPY_AND_ASSIGN(MultiDeviceSetupHostVerifierImplTest);
};

TEST_P(MultiDeviceSetupHostVerifierImplTest, StartWithoutHost_SetAndVerify) {
  CreateVerifier(HostState::kHostNotSet);

  SetHostState(HostState::kHostSetButFeaturesDisabled);
  InvokePendingDeviceNotificationCall(true /* success */);
  VerifyState(
      false /* expected_is_verified */, 0u /* expected_num_verified_events */,
      kTestTimeMs + kFirstRetryDeltaMs /* expected_retry_timestamp_value */,
      kFirstRetryDeltaMs /* expected_retry_delta_value */);

  SimulateRetryTimePassing(base::TimeDelta::FromMinutes(1));
  SetHostState(HostState::kHostSetAndFeaturesEnabled);
  VerifyState(true /* expected_is_verified */,
              1u /* expected_num_verified_events */,
              0 /* expected_retry_timestamp_value */,
              0 /* expected_retry_delta_value */);
}

TEST_P(MultiDeviceSetupHostVerifierImplTest,
       StartWithoutHost_DeviceNotificationFails) {
  CreateVerifier(HostState::kHostNotSet);
  SetHostState(HostState::kHostSetButFeaturesDisabled);

  // If the device notification call fails, a retry should still be scheduled.
  InvokePendingDeviceNotificationCall(false /* success */);
  VerifyState(
      false /* expected_is_verified */, 0u /* expected_num_verified_events */,
      kTestTimeMs + kFirstRetryDeltaMs /* expected_retry_timestamp_value */,
      kFirstRetryDeltaMs /* expected_retry_delta_value */);
}

TEST_P(MultiDeviceSetupHostVerifierImplTest, SyncAfterDeviceNotification) {
  CreateVerifier(HostState::kHostNotSet);

  SetHostState(HostState::kHostSetButFeaturesDisabled);
  InvokePendingDeviceNotificationCall(true /* success */);
  VerifyState(
      false /* expected_is_verified */, 0u /* expected_num_verified_events */,
      kTestTimeMs + kFirstRetryDeltaMs /* expected_retry_timestamp_value */,
      kFirstRetryDeltaMs /* expected_retry_delta_value */);

  FireSyncTimerAndVerifySyncOccurred();
  VerifyState(true /* expected_is_verified */,
              1u /* expected_num_verified_events */,
              0 /* expected_retry_timestamp_value */,
              0 /* expected_retry_delta_value */);
}

TEST_P(MultiDeviceSetupHostVerifierImplTest, StartWithoutHost_Retry) {
  CreateVerifier(HostState::kHostNotSet);

  SetHostState(HostState::kHostSetButFeaturesDisabled);
  InvokePendingDeviceNotificationCall(true /* success */);
  VerifyState(
      false /* expected_is_verified */, 0u /* expected_num_verified_events */,
      kTestTimeMs + kFirstRetryDeltaMs /* expected_retry_timestamp_value */,
      kFirstRetryDeltaMs /* expected_retry_delta_value */);

  // Simulate enough time pasing to time out and retry.
  SimulateRetryTimePassing(
      base::TimeDelta::FromMilliseconds(kFirstRetryDeltaMs),
      true /* simulate_timeout */);
  InvokePendingDeviceNotificationCall(true /* success */);
  VerifyState(false /* expected_is_verified */,
              0u /* expected_num_verified_events */,
              kTestTimeMs + kFirstRetryDeltaMs +
                  kFirstRetryDeltaMs * kExponentialBackoffMultiplier
              /* expected_retry_timestamp_value */,
              kFirstRetryDeltaMs * kExponentialBackoffMultiplier
              /* expected_retry_delta_value */);

  // Simulate the next retry timeout passing.
  SimulateRetryTimePassing(
      base::TimeDelta::FromMilliseconds(kFirstRetryDeltaMs *
                                        kExponentialBackoffMultiplier),
      true /* simulate_timeout */);
  InvokePendingDeviceNotificationCall(true /* success */);
  VerifyState(false /* expected_is_verified */,
              0u /* expected_num_verified_events */,
              kTestTimeMs + kFirstRetryDeltaMs +
                  kFirstRetryDeltaMs * kExponentialBackoffMultiplier +
                  kFirstRetryDeltaMs * kExponentialBackoffMultiplier *
                      kExponentialBackoffMultiplier
              /* expected_retry_timestamp_value */,
              kFirstRetryDeltaMs * kExponentialBackoffMultiplier *
                  kExponentialBackoffMultiplier
              /* expected_retry_delta_value */);

  // Succeed.
  SetHostState(HostState::kHostSetAndFeaturesEnabled);
  VerifyState(true /* expected_is_verified */,
              1u /* expected_num_verified_events */,
              0 /* expected_retry_timestamp_value */,
              0 /* expected_retry_delta_value */);
}

TEST_P(MultiDeviceSetupHostVerifierImplTest,
       StartWithUnverifiedHost_NoInitialPrefs) {
  CreateVerifier(HostState::kHostSetButFeaturesDisabled);

  InvokePendingDeviceNotificationCall(true /* success */);
  VerifyState(
      false /* expected_is_verified */, 0u /* expected_num_verified_events */,
      kTestTimeMs + kFirstRetryDeltaMs /* expected_retry_timestamp_value */,
      kFirstRetryDeltaMs /* expected_retry_delta_value */);
}

TEST_P(MultiDeviceSetupHostVerifierImplTest,
       StartWithUnverifiedHost_InitialPrefs_HasNotPassedRetryTime) {
  // Simulate starting up the device to find that the retry timer is in 5
  // minutes.
  CreateVerifier(HostState::kHostSetButFeaturesDisabled,
                 kTestTimeMs + base::TimeDelta::FromMinutes(5).InMilliseconds()
                 /* initial_timer_pref_value */,
                 kFirstRetryDeltaMs /* initial_time_delta_pref_value */);

  SimulateRetryTimePassing(base::TimeDelta::FromMinutes(5),
                           true /* simulate_timeout */);
  InvokePendingDeviceNotificationCall(true /* success */);
  VerifyState(false /* expected_is_verified */,
              0u /* expected_num_verified_events */,
              kTestTimeMs + base::TimeDelta::FromMinutes(5).InMilliseconds() +
                  kFirstRetryDeltaMs * kExponentialBackoffMultiplier
              /* expected_retry_timestamp_value */,
              kFirstRetryDeltaMs * kExponentialBackoffMultiplier
              /* expected_retry_delta_value */);
}

TEST_P(MultiDeviceSetupHostVerifierImplTest,
       StartWithUnverifiedHost_InitialPrefs_AlreadyPassedRetryTime) {
  // Simulate starting up the device to find that the retry timer had already
  // fired 5 minutes ago.
  CreateVerifier(HostState::kHostSetButFeaturesDisabled,
                 kTestTimeMs - base::TimeDelta::FromMinutes(5).InMilliseconds()
                 /* initial_timer_pref_value */,
                 kFirstRetryDeltaMs /* initial_time_delta_pref_value */);

  InvokePendingDeviceNotificationCall(true /* success */);
  VerifyState(false /* expected_is_verified */,
              0u /* expected_num_verified_events */,
              kTestTimeMs - base::TimeDelta::FromMinutes(5).InMilliseconds() +
                  kFirstRetryDeltaMs * kExponentialBackoffMultiplier
              /* expected_retry_timestamp_value */,
              kFirstRetryDeltaMs * kExponentialBackoffMultiplier
              /* expected_retry_delta_value */);
}

TEST_P(MultiDeviceSetupHostVerifierImplTest,
       StartWithUnverifiedHost_InitialPrefs_AlreadyPassedMultipleRetryTimes) {
  // Simulate starting up the device to find that the retry timer had already
  // fired 20 minutes ago.
  CreateVerifier(HostState::kHostSetButFeaturesDisabled,
                 kTestTimeMs - base::TimeDelta::FromMinutes(20).InMilliseconds()
                 /* initial_timer_pref_value */,
                 kFirstRetryDeltaMs /* initial_time_delta_pref_value */);

  // Because the first delta is 10 minutes, the second delta is 10 * 1.5 = 15
  // minutes. In this case, that means that *two* previous timeouts were missed,
  // so the third one should be scheduled.
  InvokePendingDeviceNotificationCall(true /* success */);
  VerifyState(false /* expected_is_verified */,
              0u /* expected_num_verified_events */,
              kTestTimeMs - base::TimeDelta::FromMinutes(20).InMilliseconds() +
                  kFirstRetryDeltaMs * kExponentialBackoffMultiplier +
                  kFirstRetryDeltaMs * kExponentialBackoffMultiplier *
                      kExponentialBackoffMultiplier
              /* expected_retry_timestamp_value */,
              kFirstRetryDeltaMs * kExponentialBackoffMultiplier *
                  kExponentialBackoffMultiplier
              /* expected_retry_delta_value */);
}

TEST_P(MultiDeviceSetupHostVerifierImplTest,
       StartWithVerifiedHost_HostChanges) {
  CreateVerifier(HostState::kHostSetAndFeaturesEnabled);
  VerifyState(true /* expected_is_verified */,
              0u /* expected_num_verified_events */,
              0 /* expected_retry_timestamp_value */,
              0 /* expected_retry_delta_value */);

  SetHostState(HostState::kHostNotSet);
  VerifyState(false /* expected_is_verified */,
              0u /* expected_num_verified_events */,
              0 /* expected_retry_timestamp_value */,
              0 /* expected_retry_delta_value */);

  SetHostState(HostState::kHostSetButFeaturesDisabled);
  InvokePendingDeviceNotificationCall(true /* success */);
  VerifyState(
      false /* expected_is_verified */, 0u /* expected_num_verified_events */,
      kTestTimeMs + kFirstRetryDeltaMs /* expected_retry_timestamp_value */,
      kFirstRetryDeltaMs /* expected_retry_delta_value */);
}

TEST_P(MultiDeviceSetupHostVerifierImplTest,
       StartWithVerifiedHost_PendingRemoval) {
  CreateVerifier(HostState::kHostSetAndFeaturesEnabled);
  VerifyState(true /* expected_is_verified */,
              0u /* expected_num_verified_events */,
              0 /* expected_retry_timestamp_value */,
              0 /* expected_retry_delta_value */);

  fake_host_backend_delegate()->AttemptToSetMultiDeviceHostOnBackend(
      base::nullopt /* host_device */);
  VerifyState(false /* expected_is_verified */,
              0u /* expected_num_verified_events */,
              0 /* expected_retry_timestamp_value */,
              0 /* expected_retry_delta_value */);
}

TEST_P(MultiDeviceSetupHostVerifierImplTest, HostMissingCryptoData) {
  // Remove the host device's public key, persistent symmetric key, and beacon
  // seeds. Without any of these, the host is not considered verified.
  RemoveTestDeviceCryptoData();
  CreateVerifier(HostState::kHostSetAndFeaturesEnabled);
  InvokePendingDeviceNotificationCall(true /* success */);
  VerifyState(
      false /* expected_is_verified */, 0u /* expected_num_verified_events */,
      kTestTimeMs + kFirstRetryDeltaMs /* expected_retry_timestamp_value */,
      kFirstRetryDeltaMs /* expected_retry_delta_value */);
}

// Runs tests for a host device with and without an Instance ID.
// TODO(https://crbug.com/1019206): Remove when v1 DeviceSync is disabled, when
// all devices should have an Instance ID.
INSTANTIATE_TEST_SUITE_P(All,
                         MultiDeviceSetupHostVerifierImplTest,
                         ::testing::Bool());

}  // namespace multidevice_setup

}  // namespace chromeos
