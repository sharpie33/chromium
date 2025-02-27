// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WEBLAYER_BROWSER_BROWSER_CONTEXT_IMPL_H_
#define WEBLAYER_BROWSER_BROWSER_CONTEXT_IMPL_H_

#include "base/files/file_path.h"
#include "build/build_config.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "weblayer/browser/download_manager_delegate_impl.h"
#include "weblayer/browser/ssl_host_state_delegate_impl.h"
#include "weblayer/public/profile.h"

class PrefRegistrySimple;
class PrefService;

namespace weblayer {
class ProfileImpl;
class ResourceContextImpl;

class BrowserContextImpl : public content::BrowserContext {
 public:
  BrowserContextImpl(ProfileImpl* profile_impl, const base::FilePath& path);
  ~BrowserContextImpl() override;
  BrowserContextImpl(const BrowserContextImpl&) = delete;
  BrowserContextImpl& operator=(const BrowserContextImpl&) = delete;

  static base::FilePath GetDefaultDownloadDirectory();

  // BrowserContext implementation:
#if !defined(OS_ANDROID)
  std::unique_ptr<content::ZoomLevelDelegate> CreateZoomLevelDelegate(
      const base::FilePath&) override;
#endif  // !defined(OS_ANDROID)
  base::FilePath GetPath() override;
  bool IsOffTheRecord() override;
  content::DownloadManagerDelegate* GetDownloadManagerDelegate() override;

  content::ResourceContext* GetResourceContext() override;
  content::BrowserPluginGuestManager* GetGuestManager() override;
  storage::SpecialStoragePolicy* GetSpecialStoragePolicy() override;
  content::PushMessagingService* GetPushMessagingService() override;
  content::StorageNotificationService* GetStorageNotificationService() override;
  content::SSLHostStateDelegate* GetSSLHostStateDelegate() override;
  content::PermissionControllerDelegate* GetPermissionControllerDelegate()
      override;
  content::ClientHintsControllerDelegate* GetClientHintsControllerDelegate()
      override;
  content::BackgroundFetchDelegate* GetBackgroundFetchDelegate() override;
  content::BackgroundSyncController* GetBackgroundSyncController() override;
  content::BrowsingDataRemoverDelegate* GetBrowsingDataRemoverDelegate()
      override;
  download::InProgressDownloadManager* RetriveInProgressDownloadManager()
      override;
  content::ContentIndexProvider* GetContentIndexProvider() override;

  ProfileImpl* profile_impl() const { return profile_impl_; }

 private:
  // Creates a simple in-memory pref service.
  // TODO(timvolodine): Investigate whether WebLayer needs persistent pref
  // service.
  void CreateUserPrefService();

  // Registers the preferences that WebLayer accesses.
  void RegisterPrefs(PrefRegistrySimple* pref_registry);

  ProfileImpl* const profile_impl_;
  base::FilePath path_;
  // ResourceContext needs to be deleted on the IO thread in general (and in
  // particular due to the destruction of the safebrowsing mojo interface
  // that has been added in ContentBrowserClient::ExposeInterfacesToRenderer
  // on IO thread, see crbug.com/1029317). Also this is similar to how Chrome
  // handles ProfileIOData.
  // TODO(timvolodine): consider a more general Profile shutdown/destruction
  // sequence for the IO/UI bits (crbug.com/1029879).
  std::unique_ptr<ResourceContextImpl, content::BrowserThread::DeleteOnIOThread>
      resource_context_;
  DownloadManagerDelegateImpl download_delegate_;
  SSLHostStateDelegateImpl ssl_host_state_delegate_;
  std::unique_ptr<PrefService> user_pref_service_;
  std::unique_ptr<content::PermissionControllerDelegate>
      permission_controller_delegate_;
};
}  // namespace weblayer

#endif  // WEBLAYER_BROWSER_BROWSER_CONTEXT_IMPL_H_
