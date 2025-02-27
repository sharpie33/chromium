// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/ash/assistant/assistant_client_impl.h"

#include <utility>

#include "ash/public/cpp/assistant/assistant_interface_binder.h"
#include "ash/public/cpp/network_config_service.h"
#include "chrome/browser/chrome_notification_types.h"
#include "chrome/browser/chromeos/assistant/assistant_util.h"
#include "chrome/browser/chromeos/profiles/profile_helper.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/signin/identity_manager_factory.h"
#include "chrome/browser/ui/ash/assistant/assistant_context_util.h"
#include "chrome/browser/ui/ash/assistant/assistant_image_downloader.h"
#include "chrome/browser/ui/ash/assistant/assistant_service_connection.h"
#include "chrome/browser/ui/ash/assistant/assistant_setup.h"
#include "chrome/browser/ui/ash/assistant/assistant_web_view_factory_impl.h"
#include "chrome/browser/ui/ash/assistant/conversation_starters_client_impl.h"
#include "chrome/browser/ui/ash/assistant/proactive_suggestions_client_impl.h"
#include "chromeos/constants/chromeos_features.h"
#include "chromeos/constants/chromeos_switches.h"
#include "chromeos/services/assistant/public/features.h"
#include "components/session_manager/core/session_manager.h"
#include "content/public/browser/audio_service.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/device_service.h"
#include "content/public/browser/media_session_service.h"
#include "content/public/browser/network_service_instance.h"
#include "content/public/browser/notification_registrar.h"
#include "content/public/browser/notification_service.h"
#include "content/public/browser/service_process_host.h"
#include "services/identity/public/mojom/identity_service.mojom.h"

AssistantClientImpl::AssistantClientImpl() {
  auto* session_manager = session_manager::SessionManager::Get();
  // AssistantClientImpl must be created before any user session is created.
  // Otherwise, it will not get OnUserProfileLoaded notification.
  DCHECK(session_manager->sessions().empty());
  session_manager->AddObserver(this);

  notification_registrar_.Add(this, chrome::NOTIFICATION_APP_TERMINATING,
                              content::NotificationService::AllSources());
}

AssistantClientImpl::~AssistantClientImpl() {
  session_manager::SessionManager::Get()->RemoveObserver(this);
  if (identity_manager_)
    identity_manager_->RemoveObserver(this);
}

void AssistantClientImpl::MaybeInit(Profile* profile) {
  if (assistant::IsAssistantAllowedForProfile(profile) !=
      ash::mojom::AssistantAllowedState::ALLOWED) {
    return;
  }

  if (!profile_) {
    profile_ = profile;
    identity_manager_ = IdentityManagerFactory::GetForProfile(profile_);
    DCHECK(identity_manager_);
    identity_manager_->AddObserver(this);
  }
  DCHECK_EQ(profile_, profile);

  if (initialized_)
    return;

  initialized_ = true;

  auto* service =
      AssistantServiceConnection::GetForProfile(profile_)->service();
  service->Init(client_receiver_.BindNewPipeAndPassRemote(),
                device_actions_.AddReceiver());

  assistant_image_downloader_ = std::make_unique<AssistantImageDownloader>();
  assistant_setup_ = std::make_unique<AssistantSetup>(service);
  assistant_web_view_factory_ =
      std::make_unique<AssistantWebViewFactoryImpl>(profile_);

  if (chromeos::assistant::features::IsConversationStartersV2Enabled()) {
    conversation_starters_client_ =
        std::make_unique<ConversationStartersClientImpl>(profile_);
  }

  if (chromeos::assistant::features::IsProactiveSuggestionsEnabled()) {
    proactive_suggestions_client_ =
        std::make_unique<ProactiveSuggestionsClientImpl>(profile_);
  }

  for (auto& receiver : pending_assistant_receivers_)
    service->BindAssistant(std::move(receiver));
  pending_assistant_receivers_.clear();
}

void AssistantClientImpl::MaybeStartAssistantOptInFlow() {
  if (!initialized_)
    return;

  assistant_setup_->MaybeStartAssistantOptInFlow();
}

void AssistantClientImpl::BindAssistant(
    mojo::PendingReceiver<chromeos::assistant::mojom::Assistant> receiver) {
  if (!initialized_) {
    pending_assistant_receivers_.push_back(std::move(receiver));
    return;
  }

  AssistantServiceConnection::GetForProfile(profile_)->service()->BindAssistant(
      std::move(receiver));
}

void AssistantClientImpl::Observe(int type,
                                  const content::NotificationSource& source,
                                  const content::NotificationDetails& details) {
  DCHECK_EQ(chrome::NOTIFICATION_APP_TERMINATING, type);
  if (!initialized_)
    return;

  AssistantServiceConnection::GetForProfile(profile_)->service()->Shutdown();
}

void AssistantClientImpl::OnAssistantStatusChanged(
    ash::mojom::AssistantState new_state) {
  ash::AssistantState::Get()->NotifyStatusChanged(new_state);
}

void AssistantClientImpl::RequestAssistantStructure(
    RequestAssistantStructureCallback callback) {
  RequestAssistantStructureForActiveBrowserWindow(std::move(callback));
}

void AssistantClientImpl::RequestAssistantController(
    mojo::PendingReceiver<chromeos::assistant::mojom::AssistantController>
        receiver) {
  ash::AssistantInterfaceBinder::GetInstance()->BindController(
      std::move(receiver));
}

void AssistantClientImpl::RequestAssistantAlarmTimerController(
    mojo::PendingReceiver<ash::mojom::AssistantAlarmTimerController> receiver) {
  ash::AssistantInterfaceBinder::GetInstance()->BindAlarmTimerController(
      std::move(receiver));
}

void AssistantClientImpl::RequestAssistantNotificationController(
    mojo::PendingReceiver<ash::mojom::AssistantNotificationController>
        receiver) {
  ash::AssistantInterfaceBinder::GetInstance()->BindNotificationController(
      std::move(receiver));
}

void AssistantClientImpl::RequestAssistantScreenContextController(
    mojo::PendingReceiver<ash::mojom::AssistantScreenContextController>
        receiver) {
  ash::AssistantInterfaceBinder::GetInstance()->BindScreenContextController(
      std::move(receiver));
}

void AssistantClientImpl::RequestAssistantVolumeControl(
    mojo::PendingReceiver<ash::mojom::AssistantVolumeControl> receiver) {
  ash::AssistantInterfaceBinder::GetInstance()->BindVolumeControl(
      std::move(receiver));
}

void AssistantClientImpl::RequestAssistantStateController(
    mojo::PendingReceiver<ash::mojom::AssistantStateController> receiver) {
  ash::AssistantInterfaceBinder::GetInstance()->BindStateController(
      std::move(receiver));
}

void AssistantClientImpl::RequestBatteryMonitor(
    mojo::PendingReceiver<device::mojom::BatteryMonitor> receiver) {
  content::GetDeviceService().BindBatteryMonitor(std::move(receiver));
}

void AssistantClientImpl::RequestWakeLockProvider(
    mojo::PendingReceiver<device::mojom::WakeLockProvider> receiver) {
  content::GetDeviceService().BindWakeLockProvider(std::move(receiver));
}

void AssistantClientImpl::RequestAudioStreamFactory(
    mojo::PendingReceiver<audio::mojom::StreamFactory> receiver) {
  content::GetAudioService().BindStreamFactory(std::move(receiver));
}

void AssistantClientImpl::RequestAudioDecoderFactory(
    mojo::PendingReceiver<
        chromeos::assistant::mojom::AssistantAudioDecoderFactory> receiver) {
  content::ServiceProcessHost::Launch(
      std::move(receiver),
      content::ServiceProcessHost::Options()
          .WithSandboxType(service_manager::SandboxType::kUtility)
          .WithDisplayName("Assistant Audio Decoder Service")
          .Pass());
}

void AssistantClientImpl::RequestIdentityAccessor(
    mojo::PendingReceiver<identity::mojom::IdentityAccessor> receiver) {
  identity::mojom::IdentityService* service = profile_->GetIdentityService();
  if (service)
    service->BindIdentityAccessor(std::move(receiver));
}

void AssistantClientImpl::RequestAudioFocusManager(
    mojo::PendingReceiver<media_session::mojom::AudioFocusManager> receiver) {
  content::GetMediaSessionService().BindAudioFocusManager(std::move(receiver));
}

void AssistantClientImpl::RequestMediaControllerManager(
    mojo::PendingReceiver<media_session::mojom::MediaControllerManager>
        receiver) {
  content::GetMediaSessionService().BindMediaControllerManager(
      std::move(receiver));
}

void AssistantClientImpl::RequestNetworkConfig(
    mojo::PendingReceiver<chromeos::network_config::mojom::CrosNetworkConfig>
        receiver) {
  ash::GetNetworkConfigService(std::move(receiver));
}

void AssistantClientImpl::OnExtendedAccountInfoUpdated(
    const AccountInfo& info) {
  if (initialized_)
    return;

  MaybeInit(profile_);
}

void AssistantClientImpl::OnUserProfileLoaded(const AccountId& account_id) {
  // Initialize Assistant when primary user profile is loaded so that it could
  // be used in post oobe steps. OnUserSessionStarted() is too late
  // because it happens after post oobe steps
  Profile* user_profile =
      chromeos::ProfileHelper::Get()->GetProfileByAccountId(account_id);
  if (!chromeos::ProfileHelper::IsPrimaryProfile(user_profile))
    return;

  MaybeInit(user_profile);
}

void AssistantClientImpl::OnUserSessionStarted(bool is_primary_user) {
  if (is_primary_user && !chromeos::switches::ShouldSkipOobePostLogin())
    MaybeStartAssistantOptInFlow();
}
