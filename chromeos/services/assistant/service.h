// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMEOS_SERVICES_ASSISTANT_SERVICE_H_
#define CHROMEOS_SERVICES_ASSISTANT_SERVICE_H_

#include <memory>
#include <string>

#include "ash/public/cpp/ambient/ambient_mode_state.h"
#include "ash/public/cpp/session/session_activation_observer.h"
#include "ash/public/mojom/assistant_controller.mojom.h"
#include "base/callback.h"
#include "base/cancelable_callback.h"
#include "base/component_export.h"
#include "base/memory/weak_ptr.h"
#include "base/optional.h"
#include "base/scoped_observer.h"
#include "base/sequence_checker.h"
#include "base/single_thread_task_runner.h"
#include "base/time/time.h"
#include "chromeos/dbus/power/power_manager_client.h"
#include "chromeos/services/assistant/assistant_manager_service.h"
#include "chromeos/services/assistant/assistant_state_proxy.h"
#include "chromeos/services/assistant/public/mojom/assistant.mojom.h"
#include "chromeos/services/assistant/public/mojom/settings.mojom.h"
#include "components/account_id/account_id.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"

class GoogleServiceAuthError;
class PrefService;

namespace base {
class OneShotTimer;
}  // namespace base

namespace network {
class PendingSharedURLLoaderFactory;
}  // namespace network

namespace power_manager {
class PowerSupplyProperties;
}  // namespace power_manager

namespace signin {
class AccessTokenFetcher;
struct AccessTokenInfo;
class IdentityManager;
}  // namespace signin

namespace chromeos {
namespace assistant {

class AssistantSettingsManager;
class ServiceContext;

// |AssistantManagerService|'s state won't update if it's currently in the
// process of starting up. This is the delay before we will try to update
// |AssistantManagerService| again.
constexpr auto kUpdateAssistantManagerDelay = base::TimeDelta::FromSeconds(1);

class COMPONENT_EXPORT(ASSISTANT_SERVICE) Service
    : public mojom::AssistantService,
      public chromeos::PowerManagerClient::Observer,
      public ash::SessionActivationObserver,
      public ash::AssistantStateObserver,
      public AssistantManagerService::CommunicationErrorObserver,
      public AssistantManagerService::StateObserver,
      public ash::AmbientModeStateObserver {
 public:
  Service(mojo::PendingReceiver<mojom::AssistantService> receiver,
          std::unique_ptr<network::PendingSharedURLLoaderFactory>
              pending_url_loader_factory,
          signin::IdentityManager* identity_manager,
          PrefService* profile_prefs);
  ~Service() override;

  // Allows tests to override the AssistantSettingsManager bound by the service.
  static void OverrideSettingsManagerForTesting(
      AssistantSettingsManager* manager);
  // Allows tests to override the S3 server URI used by the service.
  // The caller must ensure the memory passed in remains valid.
  // This override can be removed by passing in a nullptr.
  // Note: This would look nicer if it was a class method and not static,
  // but unfortunately this must be called before |Service| tries to create the
  // |AssistantManagerService|, which happens really soon after the service
  // itself is created, so we do not have time in our tests to grab a handle
  // to |Service| and set this before it is too late.
  static void OverrideS3ServerUriForTesting(const char* uri);

  void SetAssistantManagerServiceForTesting(
      std::unique_ptr<AssistantManagerService> assistant_manager_service);

  AssistantStateProxy* GetAssistantStateProxyForTesting();

 private:
  friend class AssistantServiceTest;

  class Context;

  // mojom::AssistantService overrides
  void Init(mojo::PendingRemote<mojom::Client> client,
            mojo::PendingRemote<mojom::DeviceActions> device_actions) override;
  void BindAssistant(mojo::PendingReceiver<mojom::Assistant> receiver) override;
  void BindSettingsManager(
      mojo::PendingReceiver<mojom::AssistantSettingsManager> receiver) override;
  void Shutdown() override;

  // chromeos::PowerManagerClient::Observer overrides:
  void PowerChanged(const power_manager::PowerSupplyProperties& prop) override;
  void SuspendDone(const base::TimeDelta& sleep_duration) override;

  // ash::SessionActivationObserver overrides:
  void OnSessionActivated(bool activated) override;
  void OnLockStateChanged(bool locked) override;

  // ash::AssistantStateObserver overrides:
  void OnAssistantConsentStatusChanged(int consent_status) override;
  void OnAssistantHotwordAlwaysOn(bool hotword_always_on) override;
  void OnAssistantSettingsEnabled(bool enabled) override;
  void OnAssistantHotwordEnabled(bool enabled) override;
  void OnLocaleChanged(const std::string& locale) override;
  void OnArcPlayStoreEnabledChanged(bool enabled) override;
  void OnLockedFullScreenStateChanged(bool enabled) override;

  // AssistantManagerService::CommunicationErrorObserver overrides:
  void OnCommunicationError(
      AssistantManagerService::CommunicationErrorType error_type) override;

  // AssistantManagerService::StateObserver overrides:
  void OnStateChanged(AssistantManagerService::State new_state) override;

  // ash::AmbientModeStateObserver overrides:
  void OnAmbientModeEnabled(bool enabled) override;

  void UpdateAssistantManagerState();

  void RequestAccessToken();

  void GetUnconsentedPrimaryAccountInfoCallback(
      const base::Optional<CoreAccountId>& account_id,
      const base::Optional<std::string>& gaia,
      const base::Optional<std::string>& email,
      const identity::AccountState& account_state);

  void GetAccessTokenCallback(GoogleServiceAuthError error,
                              signin::AccessTokenInfo access_token_info);
  void RetryRefreshToken();

  void CreateAssistantManagerService();
  std::unique_ptr<AssistantManagerService>
  CreateAndReturnAssistantManagerService();

  void FinalizeAssistantManagerService();

  void StopAssistantManagerService();

  void AddAshSessionObserver();

  void UpdateListeningState();

  ServiceContext* context() { return context_.get(); }

  // Returns the "actual" hotword status. In addition to the hotword pref, this
  // method also take power status into account if dsp support is not available
  // for the device.
  bool ShouldEnableHotword();

  mojo::Receiver<mojom::AssistantService> receiver_;
  mojo::ReceiverSet<mojom::Assistant> assistant_receivers_;

  bool observing_ash_session_ = false;
  mojo::Remote<mojom::Client> client_;
  mojo::Remote<mojom::DeviceActions> device_actions_;

  signin::IdentityManager* const identity_manager_;

  AccountId account_id_;
  std::unique_ptr<AssistantManagerService> assistant_manager_service_;
  std::unique_ptr<base::OneShotTimer> token_refresh_timer_;
  int token_refresh_error_backoff_factor = 1;
  scoped_refptr<base::SequencedTaskRunner> main_task_runner_;
  ScopedObserver<chromeos::PowerManagerClient,
                 chromeos::PowerManagerClient::Observer>
      power_manager_observer_{this};

  // Whether the current user session is active.
  bool session_active_ = false;
  // Whether the lock screen is on.
  bool locked_ = false;
  // Whether the power source is connected.
  bool power_source_connected_ = false;

  // The value passed into |SetAssistantManagerServiceForTesting|.
  // Will be moved into |assistant_manager_service_| when the service is
  // supposed to be created.
  std::unique_ptr<AssistantManagerService>
      assistant_manager_service_for_testing_ = nullptr;

  base::Optional<std::string> access_token_;

  mojo::Remote<mojom::AssistantController> assistant_controller_;

  mojo::Remote<ash::mojom::AssistantAlarmTimerController>
      assistant_alarm_timer_controller_;
  mojo::Remote<ash::mojom::AssistantNotificationController>
      assistant_notification_controller_;
  mojo::Remote<ash::mojom::AssistantScreenContextController>
      assistant_screen_context_controller_;
  AssistantStateProxy assistant_state_;

  // |ServiceContext| object passed to child classes so they can access some of
  // our functionality without depending on us.
  std::unique_ptr<ServiceContext> context_;

  // non-null until |assistant_manager_service_| is created.
  std::unique_ptr<network::PendingSharedURLLoaderFactory>
      pending_url_loader_factory_;

  // User profile preferences.
  PrefService* const profile_prefs_;

  base::CancelableOnceClosure update_assistant_manager_callback_;

  std::unique_ptr<signin::AccessTokenFetcher> access_token_fetcher_;

  SEQUENCE_CHECKER(sequence_checker_);

  base::WeakPtrFactory<Service> weak_ptr_factory_{this};

  DISALLOW_COPY_AND_ASSIGN(Service);
};

}  // namespace assistant
}  // namespace chromeos

#endif  // CHROMEOS_SERVICES_ASSISTANT_SERVICE_H_
