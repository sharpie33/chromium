// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_SERVICE_WORKER_SERVICE_WORKER_STORAGE_H_
#define CONTENT_BROWSER_SERVICE_WORKER_SERVICE_WORKER_STORAGE_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/containers/circular_deque.h"
#include "base/containers/flat_map.h"
#include "base/files/file_path.h"
#include "base/gtest_prod_util.h"
#include "base/macros.h"
#include "base/memory/weak_ptr.h"
#include "content/browser/service_worker/service_worker_database.h"
#include "content/browser/service_worker/service_worker_metrics.h"
#include "content/common/content_export.h"
#include "third_party/blink/public/common/service_worker/service_worker_status_code.h"
#include "url/gurl.h"

namespace base {
class SequencedTaskRunner;
}

namespace storage {
class QuotaManagerProxy;
class SpecialStoragePolicy;
}

namespace content {

class ServiceWorkerDiskCache;
class ServiceWorkerResponseMetadataWriter;
class ServiceWorkerResponseReader;
class ServiceWorkerResponseWriter;

namespace service_worker_storage_unittest {
class ServiceWorkerStorageTest;
class ServiceWorkerResourceStorageTest;
class ServiceWorkerResourceStorageDiskTest;
FORWARD_DECLARE_TEST(ServiceWorkerResourceStorageDiskTest, CleanupOnRestart);
FORWARD_DECLARE_TEST(ServiceWorkerResourceStorageDiskTest, DeleteAndStartOver);
FORWARD_DECLARE_TEST(ServiceWorkerResourceStorageDiskTest,
                     DeleteAndStartOver_UnrelatedFileExists);
FORWARD_DECLARE_TEST(ServiceWorkerResourceStorageDiskTest,
                     DeleteAndStartOver_OpenedFileExists);
}  // namespace service_worker_storage_unittest

// This class provides an interface to store and retrieve ServiceWorker
// registration data. The lifetime is equal to ServiceWorkerRegistry that is
// an owner of this class. When a storage operation fails, this is marked as
// disabled and all subsequent requests are aborted until the registry is
// restarted.
// TODO(crbug.com/1039200): Move some methods/fields to ServiceWorkerRegistry.
// See the toplevel description of ServiceWorkerRegistry.
class CONTENT_EXPORT ServiceWorkerStorage {
 public:
  using RegistrationList = std::vector<ServiceWorkerDatabase::RegistrationData>;
  using ResourceList = std::vector<ServiceWorkerDatabase::ResourceRecord>;
  using StatusCallback =
      base::OnceCallback<void(blink::ServiceWorkerStatusCode status)>;
  using FindRegistrationDataCallback = base::OnceCallback<void(
      std::unique_ptr<ServiceWorkerDatabase::RegistrationData> data,
      std::unique_ptr<ResourceList> resources,
      ServiceWorkerDatabase::Status status)>;
  using GetRegistrationsDataCallback = base::OnceCallback<void(
      blink::ServiceWorkerStatusCode status,
      std::unique_ptr<RegistrationList> registrations,
      std::unique_ptr<std::vector<ResourceList>> resource_lists)>;
  using GetAllRegistrationsCallback =
      base::OnceCallback<void(blink::ServiceWorkerStatusCode status,
                              std::unique_ptr<RegistrationList> registrations)>;
  using StoreRegistrationDataCallback = base::OnceCallback<void(
      blink::ServiceWorkerStatusCode status,
      int64_t deleted_version_id,
      const std::vector<int64_t>& newly_purgeable_resources)>;
  using DeleteRegistrationCallback = base::OnceCallback<void(
      blink::ServiceWorkerStatusCode status,
      int64_t deleted_version_id,
      const std::vector<int64_t>& newly_purgeable_resources)>;

  using ResponseWriterCreationCallback = base::OnceCallback<void(
      int64_t resource_id,
      std::unique_ptr<ServiceWorkerResponseWriter> response_writer)>;

  using DatabaseStatusCallback =
      base::OnceCallback<void(ServiceWorkerDatabase::Status status)>;
  using GetUserDataInDBCallback =
      base::OnceCallback<void(const std::vector<std::string>& data,
                              ServiceWorkerDatabase::Status)>;
  using GetUserKeysAndDataInDBCallback = base::OnceCallback<void(
      const base::flat_map<std::string, std::string>& data_map,
      ServiceWorkerDatabase::Status)>;
  using GetUserDataForAllRegistrationsInDBCallback = base::OnceCallback<void(
      const std::vector<std::pair<int64_t, std::string>>& user_data,
      ServiceWorkerDatabase::Status)>;

  ~ServiceWorkerStorage();

  static blink::ServiceWorkerStatusCode DatabaseStatusToStatusCode(
      ServiceWorkerDatabase::Status status);

  // TODO(crbug.com/1039200): Stop passing ServiceWorkerRegistry once
  // ServiceWorkerRegistration dependencies are moved to ServiceWorkerRegistry.
  static std::unique_ptr<ServiceWorkerStorage> Create(
      const base::FilePath& user_data_directory,
      scoped_refptr<base::SequencedTaskRunner> database_task_runner,
      storage::QuotaManagerProxy* quota_manager_proxy,
      storage::SpecialStoragePolicy* special_storage_policy);

  // Used for DeleteAndStartOver. Creates new storage based on |old_storage|.
  static std::unique_ptr<ServiceWorkerStorage> Create(
      ServiceWorkerStorage* old_storage);

  // Reads stored registrations for |client_url| or |scope| or
  // |registration_id|. Returns blink::ServiceWorkerStatusCode::kOk with
  // non-null RegistrationData and ResourceList if registration is found, or
  // returns blink::ServiceWorkerStatusCode::kErrorNotFound if no matching
  // registration is found.
  void FindRegistrationForClientUrl(const GURL& client_url,
                                    FindRegistrationDataCallback callback);
  void FindRegistrationForScope(const GURL& scope,
                                FindRegistrationDataCallback callback);
  void FindRegistrationForId(int64_t registration_id,
                             const GURL& origin,
                             FindRegistrationDataCallback callback);
  void FindRegistrationForIdOnly(int64_t registration_id,
                                 FindRegistrationDataCallback callback);

  // Returns all stored registrations for a given origin.
  void GetRegistrationsForOrigin(const GURL& origin,
                                 GetRegistrationsDataCallback callback);

  // Returns all stored registrations.
  void GetAllRegistrations(GetAllRegistrationsCallback callback);

  // Stores |registration_data| and |resources| on persistent storage.
  void StoreRegistrationData(
      const ServiceWorkerDatabase::RegistrationData& registration_data,
      const ResourceList& resources,
      StoreRegistrationDataCallback callback);

  // Updates the state of the registration's stored version to active.
  void UpdateToActiveState(int64_t registration_id,
                           const GURL& origin,
                           DatabaseStatusCallback callback);

  // Updates the stored time to match the value of
  // registration->last_update_check().
  void UpdateLastUpdateCheckTime(int64_t registration_id,
                                 const GURL& origin,
                                 base::Time last_update_check_time,
                                 StatusCallback callback);

  // Updates the specified registration's navigation preload state in storage.
  // The caller is responsible for mutating the live registration's state.
  void UpdateNavigationPreloadEnabled(int64_t registration_id,
                                      const GURL& origin,
                                      bool enable,
                                      StatusCallback callback);
  void UpdateNavigationPreloadHeader(int64_t registration_id,
                                     const GURL& origin,
                                     const std::string& value,
                                     StatusCallback callback);

  // Deletes the registration specified by |registration_id|. This should be
  // called only from ServiceWorkerRegistry.
  void DeleteRegistration(int64_t registration_id,
                          const GURL& origin,
                          DeleteRegistrationCallback callback);

  // Removes traces of deleted data on disk.
  void PerformStorageCleanup(base::OnceClosure callback);

  // Creates a resource accessor. Never returns nullptr but an accessor may be
  // associated with the disabled disk cache if the storage is disabled.
  std::unique_ptr<ServiceWorkerResponseReader> CreateResponseReader(
      int64_t resource_id);
  std::unique_ptr<ServiceWorkerResponseWriter> CreateResponseWriter(
      int64_t resource_id);
  std::unique_ptr<ServiceWorkerResponseMetadataWriter>
  CreateResponseMetadataWriter(int64_t resource_id);

  // Assigns a new resource ID and creates a response writer associated with
  // the resource ID. If ID allocation fails, kInvalidServiceWorkerResourceId
  // and null writer are returned.
  // NOTE: Currently this method is synchronous but intentionally uses async
  // style because ServiceWorkerStorage will be accessed via mojo calls soon.
  // See crbug.com/1046335 for details.
  void CreateNewResponseWriter(ResponseWriterCreationCallback callback);

  // Adds |resource_id| to the set of resources that are in the disk cache
  // but not yet stored with a registration.
  void StoreUncommittedResourceId(int64_t resource_id,
                                  DatabaseStatusCallback callback);

  // Removes resource ids from uncommitted list, adds them to the purgeable list
  // and purges them.
  void DoomUncommittedResources(const std::set<int64_t>& resource_ids,
                                DatabaseStatusCallback callback);

  // Provide a storage mechanism to read/write arbitrary data associated with
  // a registration. Each registration has its own key namespace.
  // GetUserData responds OK only if all keys are found; otherwise NOT_FOUND,
  // and the callback's data will be empty.
  void GetUserData(int64_t registration_id,
                   const std::vector<std::string>& keys,
                   GetUserDataInDBCallback callback);
  // GetUserDataByKeyPrefix responds OK with a vector containing data rows that
  // had matching keys assuming the database was read successfully.
  void GetUserDataByKeyPrefix(int64_t registration_id,
                              const std::string& key_prefix,
                              GetUserDataInDBCallback callback);
  // GetUserKeysAndDataByKeyPrefix responds OK with a flat_map containing
  // matching keys and their data assuming the database was read successfully.
  // The map keys have |key_prefix| stripped from them.
  void GetUserKeysAndDataByKeyPrefix(int64_t registration_id,
                                     const std::string& key_prefix,
                                     GetUserKeysAndDataInDBCallback callback);

  // Stored data is deleted when the associated registraton is deleted.
  void StoreUserData(
      int64_t registration_id,
      const GURL& origin,
      const std::vector<std::pair<std::string, std::string>>& key_value_pairs,
      DatabaseStatusCallback callback);
  // Responds OK if all are successfully deleted or not found in the database.
  void ClearUserData(int64_t registration_id,
                     const std::vector<std::string>& keys,
                     DatabaseStatusCallback callback);
  // Responds OK if all are successfully deleted or not found in the database.
  // Neither |key_prefixes| nor the prefixes within can be empty.
  void ClearUserDataByKeyPrefixes(int64_t registration_id,
                                  const std::vector<std::string>& key_prefixes,
                                  DatabaseStatusCallback callback);
  // Responds with all registrations that have user data with a particular key,
  // as well as that user data.
  void GetUserDataForAllRegistrations(
      const std::string& key,
      GetUserDataForAllRegistrationsInDBCallback callback);
  // Responds with all registrations that have user data with a particular key,
  // as well as that user data.
  void GetUserDataForAllRegistrationsByKeyPrefix(
      const std::string& key_prefix,
      GetUserDataForAllRegistrationsInDBCallback callback);
  // Responds OK if all are successfully deleted or not found in the database.
  // |key_prefix| cannot be empty.
  void ClearUserDataForAllRegistrationsByKeyPrefix(
      const std::string& key_prefix,
      DatabaseStatusCallback callback);

  // Deletes the storage and starts over. This should be called only from
  // ServiceWorkerRegistry other than tests.
  void DeleteAndStartOver(StatusCallback callback);

  // Returns a new registration id which is guaranteed to be unique in the
  // storage. Returns blink::mojom::kInvalidServiceWorkerRegistrationId if the
  // storage is disabled.
  int64_t NewRegistrationId();

  // Returns a new version id which is guaranteed to be unique in the storage.
  // Returns kInvalidServiceWorkerVersionId if the storage is disabled.
  int64_t NewVersionId();

  // Returns a new resource id which is guaranteed to be unique in the storage.
  // Returns ServiceWorkerConsts::kInvalidServiceWorkerResourceId if the storage
  // is disabled.
  int64_t NewResourceId();

  void Disable();
  bool IsDisabled() const;

  // Schedules deleting |resources| from the disk cache and removing their keys
  // as purgeable resources from the service worker database. It's OK to call
  // this for resources that don't have purgeable resource keys, like
  // uncommitted resources, as long as the caller does its own cleanup to remove
  // the uncommitted resource keys.
  void PurgeResources(const ResourceList& resources);
  void PurgeResources(const std::vector<int64_t>& resource_ids);
  void PurgeResources(const std::set<int64_t>& resource_ids);

  void LazyInitializeForTest();

  void SetPurgingCompleteCallbackForTest(base::OnceClosure callback);

 private:
  friend class service_worker_storage_unittest::ServiceWorkerStorageTest;
  friend class service_worker_storage_unittest::
      ServiceWorkerResourceStorageTest;
  FRIEND_TEST_ALL_PREFIXES(
      service_worker_storage_unittest::ServiceWorkerResourceStorageDiskTest,
      CleanupOnRestart);
  FRIEND_TEST_ALL_PREFIXES(
      service_worker_storage_unittest::ServiceWorkerResourceStorageDiskTest,
      DeleteAndStartOver);
  FRIEND_TEST_ALL_PREFIXES(
      service_worker_storage_unittest::ServiceWorkerResourceStorageDiskTest,
      DeleteAndStartOver_UnrelatedFileExists);
  FRIEND_TEST_ALL_PREFIXES(
      service_worker_storage_unittest::ServiceWorkerResourceStorageDiskTest,
      DeleteAndStartOver_OpenedFileExists);

  struct InitialData {
    int64_t next_registration_id;
    int64_t next_version_id;
    int64_t next_resource_id;
    std::set<GURL> origins;

    InitialData();
    ~InitialData();
  };

  // Because there are too many params for base::Bind to wrap a closure around.
  struct DidDeleteRegistrationParams {
    int64_t registration_id;
    GURL origin;
    DeleteRegistrationCallback callback;

    DidDeleteRegistrationParams(int64_t registration_id,
                                GURL origin,
                                DeleteRegistrationCallback callback);
    ~DidDeleteRegistrationParams();
  };

  enum class OriginState {
    // Registrations may exist at this origin. It cannot be deleted.
    kKeep,
    // No registrations exist at this origin. It can be deleted.
    kDelete
  };

  using InitializeCallback =
      base::OnceCallback<void(std::unique_ptr<InitialData> data,
                              ServiceWorkerDatabase::Status status)>;
  using WriteRegistrationCallback = base::OnceCallback<void(
      const GURL& origin,
      const ServiceWorkerDatabase::RegistrationData& deleted_version_data,
      const std::vector<int64_t>& newly_purgeable_resources,
      ServiceWorkerDatabase::Status status)>;
  using DeleteRegistrationInDBCallback = base::OnceCallback<void(
      OriginState origin_state,
      const ServiceWorkerDatabase::RegistrationData& deleted_version_data,
      const std::vector<int64_t>& newly_purgeable_resources,
      ServiceWorkerDatabase::Status status)>;
  using FindInDBCallback = base::OnceCallback<void(
      std::unique_ptr<ServiceWorkerDatabase::RegistrationData> data,
      std::unique_ptr<ResourceList> resources,
      ServiceWorkerDatabase::Status status)>;
  using GetResourcesCallback =
      base::OnceCallback<void(const std::vector<int64_t>& resource_ids,
                              ServiceWorkerDatabase::Status status)>;

  ServiceWorkerStorage(
      const base::FilePath& user_data_directory,
      scoped_refptr<base::SequencedTaskRunner> database_task_runner,
      storage::QuotaManagerProxy* quota_manager_proxy,
      storage::SpecialStoragePolicy* special_storage_policy);

  base::FilePath GetDatabasePath();
  base::FilePath GetDiskCachePath();

  void LazyInitialize(base::OnceClosure callback);
  void DidReadInitialData(std::unique_ptr<InitialData> data,
                          ServiceWorkerDatabase::Status status);
  void DidGetRegistrationsForOrigin(
      GetRegistrationsDataCallback callback,
      std::unique_ptr<RegistrationList> registrations,
      std::unique_ptr<std::vector<ResourceList>> resource_lists,
      ServiceWorkerDatabase::Status status);
  void DidGetAllRegistrations(
      GetAllRegistrationsCallback callback,
      std::unique_ptr<RegistrationList> registration_data_list,
      ServiceWorkerDatabase::Status status);
  void DidStoreRegistrationData(
      StoreRegistrationDataCallback callback,
      const ServiceWorkerDatabase::RegistrationData& new_version,
      const GURL& origin,
      const ServiceWorkerDatabase::RegistrationData& deleted_version,
      const std::vector<int64_t>& newly_purgeable_resources,
      ServiceWorkerDatabase::Status status);
  void DidDeleteRegistration(
      std::unique_ptr<DidDeleteRegistrationParams> params,
      OriginState origin_state,
      const ServiceWorkerDatabase::RegistrationData& deleted_version,
      const std::vector<int64_t>& newly_purgeable_resources,
      ServiceWorkerDatabase::Status status);

  // Lazy disk_cache getter.
  ServiceWorkerDiskCache* disk_cache();
  void InitializeDiskCache();
  void OnDiskCacheInitialized(int rv);

  void StartPurgingResources(const std::set<int64_t>& resource_ids);
  void StartPurgingResources(const std::vector<int64_t>& resource_ids);
  void StartPurgingResources(const ResourceList& resources);
  void ContinuePurgingResources();
  void PurgeResource(int64_t id);
  void OnResourcePurged(int64_t id, int rv);

  // Deletes purgeable and uncommitted resources left over from the previous
  // browser session. This must be called once per session before any database
  // operation that may mutate the purgeable or uncommitted resource lists.
  void DeleteStaleResources();
  void DidCollectStaleResources(const std::vector<int64_t>& stale_resource_ids,
                                ServiceWorkerDatabase::Status status);

  void ClearSessionOnlyOrigins();

  // Static cross-thread helpers.
  static void CollectStaleResourcesFromDB(
      ServiceWorkerDatabase* database,
      scoped_refptr<base::SequencedTaskRunner> original_task_runner,
      GetResourcesCallback callback);
  static void ReadInitialDataFromDB(
      ServiceWorkerDatabase* database,
      scoped_refptr<base::SequencedTaskRunner> original_task_runner,
      InitializeCallback callback);
  static void DeleteRegistrationFromDB(
      ServiceWorkerDatabase* database,
      scoped_refptr<base::SequencedTaskRunner> original_task_runner,
      int64_t registration_id,
      const GURL& origin,
      DeleteRegistrationInDBCallback callback);
  static void WriteRegistrationInDB(
      ServiceWorkerDatabase* database,
      scoped_refptr<base::SequencedTaskRunner> original_task_runner,
      const ServiceWorkerDatabase::RegistrationData& registration,
      const ResourceList& resources,
      WriteRegistrationCallback callback);
  static void FindForClientUrlInDB(
      ServiceWorkerDatabase* database,
      scoped_refptr<base::SequencedTaskRunner> original_task_runner,
      const GURL& client_url,
      FindInDBCallback callback);
  static void FindForScopeInDB(
      ServiceWorkerDatabase* database,
      scoped_refptr<base::SequencedTaskRunner> original_task_runner,
      const GURL& scope,
      FindInDBCallback callback);
  static void FindForIdInDB(
      ServiceWorkerDatabase* database,
      scoped_refptr<base::SequencedTaskRunner> original_task_runner,
      int64_t registration_id,
      const GURL& origin,
      FindInDBCallback callback);
  static void FindForIdOnlyInDB(
      ServiceWorkerDatabase* database,
      scoped_refptr<base::SequencedTaskRunner> original_task_runner,
      int64_t registration_id,
      FindInDBCallback callback);
  static void GetUserDataInDB(
      ServiceWorkerDatabase* database,
      scoped_refptr<base::SequencedTaskRunner> original_task_runner,
      int64_t registration_id,
      const std::vector<std::string>& keys,
      GetUserDataInDBCallback callback);
  static void GetUserDataByKeyPrefixInDB(
      ServiceWorkerDatabase* database,
      scoped_refptr<base::SequencedTaskRunner> original_task_runner,
      int64_t registration_id,
      const std::string& key_prefix,
      GetUserDataInDBCallback callback);
  static void GetUserKeysAndDataByKeyPrefixInDB(
      ServiceWorkerDatabase* database,
      scoped_refptr<base::SequencedTaskRunner> original_task_runner,
      int64_t registration_id,
      const std::string& key_prefix,
      GetUserKeysAndDataInDBCallback callback);
  static void GetUserDataForAllRegistrationsInDB(
      ServiceWorkerDatabase* database,
      scoped_refptr<base::SequencedTaskRunner> original_task_runner,
      const std::string& key,
      GetUserDataForAllRegistrationsInDBCallback callback);
  static void GetUserDataForAllRegistrationsByKeyPrefixInDB(
      ServiceWorkerDatabase* database,
      scoped_refptr<base::SequencedTaskRunner> original_task_runner,
      const std::string& key_prefix,
      GetUserDataForAllRegistrationsInDBCallback callback);
  static void DeleteAllDataForOriginsFromDB(
      ServiceWorkerDatabase* database,
      const std::set<GURL>& origins);
  static void PerformStorageCleanupInDB(ServiceWorkerDatabase* database);

  // Posted by the underlying cache implementation after it finishes making
  // disk changes upon its destruction.
  void DiskCacheImplDoneWithDisk();
  void DidDeleteDatabase(StatusCallback callback,
                         ServiceWorkerDatabase::Status status);
  // Posted when we finish deleting the cache directory.
  void DidDeleteDiskCache(StatusCallback callback, bool result);

  // Origins having registations.
  std::set<GURL> registered_origins_;

  // Pending database tasks waiting for initialization.
  std::vector<base::OnceClosure> pending_tasks_;

  int64_t next_registration_id_;
  int64_t next_version_id_;
  int64_t next_resource_id_;

  enum State {
    STORAGE_STATE_UNINITIALIZED,
    STORAGE_STATE_INITIALIZING,
    STORAGE_STATE_INITIALIZED,
    STORAGE_STATE_DISABLED,
  };
  State state_;

  // non-null between when DeleteAndStartOver() is called and when the
  // underlying disk cache stops using the disk.
  StatusCallback delete_and_start_over_callback_;

  // This is set when we know that a call to Disable() will result in
  // DiskCacheImplDoneWithDisk() eventually called. This might not happen
  // for many reasons:
  // 1) A previous call to Disable() may have already triggered that.
  // 2) We may be using a memory backend.
  // 3) |disk_cache_| might not have been created yet.
  // ... so it's easier to keep track of the case when it will happen.
  bool expecting_done_with_disk_on_disable_;

  base::FilePath user_data_directory_;

  // |database_| is only accessed using |database_task_runner_|.
  std::unique_ptr<ServiceWorkerDatabase> database_;
  scoped_refptr<base::SequencedTaskRunner> database_task_runner_;

  scoped_refptr<storage::QuotaManagerProxy> quota_manager_proxy_;
  scoped_refptr<storage::SpecialStoragePolicy> special_storage_policy_;

  std::unique_ptr<ServiceWorkerDiskCache> disk_cache_;

  base::circular_deque<int64_t> purgeable_resource_ids_;
  bool is_purge_pending_;
  bool has_checked_for_stale_resources_;
  base::OnceClosure purging_complete_callback_for_test_;

  base::WeakPtrFactory<ServiceWorkerStorage> weak_factory_{this};

  DISALLOW_COPY_AND_ASSIGN(ServiceWorkerStorage);
};

}  // namespace content

#endif  // CONTENT_BROWSER_SERVICE_WORKER_SERVICE_WORKER_STORAGE_H_
