// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stddef.h>
#include <stdint.h>

#include "base/bind.h"
#include "base/files/file_util.h"
#include "base/location.h"
#include "base/run_loop.h"
#include "base/single_thread_task_runner.h"
#include "base/stl_util.h"
#include "base/task/post_task.h"
#include "base/test/bind_test_util.h"
#include "base/test/scoped_feature_list.h"
#include "base/threading/thread.h"
#include "base/threading/thread_task_runner_handle.h"
#include "components/services/storage/dom_storage/async_dom_storage_database.h"
#include "components/services/storage/dom_storage/dom_storage_database.h"
#include "components/services/storage/dom_storage/local_storage_database.pb.h"
#include "components/services/storage/public/cpp/constants.h"
#include "content/browser/code_cache/generated_code_cache.h"
#include "content/browser/code_cache/generated_code_cache_context.h"
#include "content/browser/gpu/shader_cache_factory.h"
#include "content/browser/storage_partition_impl.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/generated_code_cache_settings.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/storage_usage_info.h"
#include "content/public/common/content_features.h"
#include "content/public/test/browser_task_environment.h"
#include "content/public/test/test_browser_context.h"
#include "content/public/test/test_utils.h"
#include "net/base/test_completion_callback.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_store.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/url_request_context_getter.h"
#include "ppapi/buildflags/buildflags.h"
#include "services/network/cookie_manager.h"
#include "storage/browser/quota/quota_manager.h"
#include "storage/browser/test/mock_quota_manager.h"
#include "storage/browser/test/mock_special_storage_policy.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/features.h"
#include "third_party/leveldatabase/env_chromium.h"

#if BUILDFLAG(ENABLE_PLUGINS)
#include "ppapi/shared_impl/ppapi_constants.h"  // nogncheck
#include "storage/browser/file_system/async_file_util.h"
#include "storage/browser/file_system/file_system_context.h"
#include "storage/browser/file_system/file_system_operation_context.h"
#include "storage/browser/file_system/isolated_context.h"
#include "storage/common/file_system/file_system_util.h"
#endif  // BUILDFLAG(ENABLE_PLUGINS)

using net::CanonicalCookie;
using CookieDeletionFilter = network::mojom::CookieDeletionFilter;
using CookieDeletionFilterPtr = network::mojom::CookieDeletionFilterPtr;

namespace content {
namespace {

const int kDefaultClientId = 42;
const char kCacheKey[] = "key";
const char kCacheValue[] = "cached value";

const char kTestOrigin1[] = "http://host1:1/";
const char kTestOrigin2[] = "http://host2:1/";
const char kTestOrigin3[] = "http://host3:1/";
const char kTestOriginDevTools[] = "devtools://abcdefghijklmnopqrstuvw/";
const char kTestURL[] = "http://host4/script.js";
const char kFilterURLForCodeCache[] = "http://host5/script.js";

#if BUILDFLAG(ENABLE_PLUGINS)
const char kWidevineCdmPluginId[] = "application_x-ppapi-widevine-cdm";
const char kClearKeyCdmPluginId[] = "application_x-ppapi-clearkey-cdm";
#endif  // BUILDFLAG(ENABLE_PLUGINS)

const blink::mojom::StorageType kTemporary =
    blink::mojom::StorageType::kTemporary;
const blink::mojom::StorageType kPersistent =
    blink::mojom::StorageType::kPersistent;

const storage::QuotaClient::ID kClientFile = storage::QuotaClient::kFileSystem;

const uint32_t kAllQuotaRemoveMask =
    StoragePartition::REMOVE_DATA_MASK_APPCACHE |
    StoragePartition::REMOVE_DATA_MASK_FILE_SYSTEMS |
    StoragePartition::REMOVE_DATA_MASK_INDEXEDDB |
    StoragePartition::REMOVE_DATA_MASK_WEBSQL;

// TODO(crbug.com/889590): Use helper for url::Origin creation from string.
url::Origin Origin1() {
  return url::Origin::Create(GURL(kTestOrigin1));
}
url::Origin Origin2() {
  return url::Origin::Create(GURL(kTestOrigin2));
}
url::Origin Origin3() {
  return url::Origin::Create(GURL(kTestOrigin3));
}
url::Origin OriginDevTools() {
  return url::Origin::Create(GURL(kTestOriginDevTools));
}
// TODO(https://crbug.com/1042727): Fix test GURL scoping and remove this getter
// function.
GURL ResourceUrl() {
  return GURL(kTestURL);
}
GURL FilterResourceURLForCodeCache() {
  return GURL(kFilterURLForCodeCache);
}

class AwaitCompletionHelper {
 public:
  AwaitCompletionHelper() : start_(false), already_quit_(false) {}
  virtual ~AwaitCompletionHelper() {}

  void BlockUntilNotified() {
    if (!already_quit_) {
      DCHECK(!start_);
      start_ = true;
      base::RunLoop().Run();
    } else {
      DCHECK(!start_);
      already_quit_ = false;
    }
  }

  void Notify() {
    if (start_) {
      DCHECK(!already_quit_);
      base::RunLoop::QuitCurrentWhenIdleDeprecated();
      start_ = false;
    } else {
      DCHECK(!already_quit_);
      already_quit_ = true;
    }
  }

 private:
  // Helps prevent from running message_loop, if the callback invoked
  // immediately.
  bool start_;
  bool already_quit_;

  DISALLOW_COPY_AND_ASSIGN(AwaitCompletionHelper);
};

class RemoveCookieTester {
 public:
  explicit RemoveCookieTester(StoragePartition* storage_partition)
      : get_cookie_success_(false), storage_partition_(storage_partition) {}

  // Returns true, if the given cookie exists in the cookie store.
  bool ContainsCookie() {
    get_cookie_success_ = false;
    storage_partition_->GetCookieManagerForBrowserProcess()->GetCookieList(
        Origin1().GetURL(), net::CookieOptions::MakeAllInclusive(),
        base::BindOnce(&RemoveCookieTester::GetCookieListCallback,
                       base::Unretained(this)));
    await_completion_.BlockUntilNotified();
    return get_cookie_success_;
  }

  void AddCookie() {
    CanonicalCookie::CookieInclusionStatus status;
    std::unique_ptr<net::CanonicalCookie> cc(net::CanonicalCookie::Create(
        Origin1().GetURL(), "A=1", base::Time::Now(),
        base::nullopt /* server_time */, &status));
    storage_partition_->GetCookieManagerForBrowserProcess()->SetCanonicalCookie(
        *cc, Origin1().scheme(), net::CookieOptions::MakeAllInclusive(),
        base::BindOnce(&RemoveCookieTester::SetCookieCallback,
                       base::Unretained(this)));
    await_completion_.BlockUntilNotified();
  }

 private:
  void GetCookieListCallback(const net::CookieStatusList& cookie_list,
                             const net::CookieStatusList& excluded_cookies) {
    std::string cookie_line =
        net::CanonicalCookie::BuildCookieLine(cookie_list);
    if (cookie_line == "A=1") {
      get_cookie_success_ = true;
    } else {
      EXPECT_EQ("", cookie_line);
      get_cookie_success_ = false;
    }
    await_completion_.Notify();
  }

  void SetCookieCallback(CanonicalCookie::CookieInclusionStatus result) {
    ASSERT_TRUE(result.IsInclude());
    await_completion_.Notify();
  }

  bool get_cookie_success_;
  AwaitCompletionHelper await_completion_;
  StoragePartition* storage_partition_;

  DISALLOW_COPY_AND_ASSIGN(RemoveCookieTester);
};

class RemoveLocalStorageTester {
 public:
  RemoveLocalStorageTester(content::BrowserTaskEnvironment* task_environment,
                           TestBrowserContext* browser_context)
      : task_environment_(task_environment),
        storage_partition_(
            BrowserContext::GetDefaultStoragePartition(browser_context)),
        dom_storage_context_(storage_partition_->GetDOMStorageContext()) {}

  ~RemoveLocalStorageTester() {
    // Tests which bring up a real Local Storage context need to shut it down
    // and wait for the database to be closed before terminating; otherwise the
    // TestBrowserContext may fail to delete its temp dir, and it will not be
    // happy about that.
    static_cast<DOMStorageContextWrapper*>(dom_storage_context_)->Shutdown();
    task_environment_->RunUntilIdle();
  }

  // Returns true, if the given origin URL exists.
  bool DOMStorageExistsForOrigin(const url::Origin& origin) {
    GetLocalStorageUsage();
    await_completion_.BlockUntilNotified();
    for (size_t i = 0; i < infos_.size(); ++i) {
      if (origin == infos_[i].origin)
        return true;
    }
    return false;
  }

  void AddDOMStorageTestData() {
    // NOTE: Tests which call this method depend on implementation details of
    // how exactly the Local Storage subsystem stores persistent data.

    base::RunLoop open_loop;
    leveldb_env::Options options;
    options.create_if_missing = true;
    auto database = storage::AsyncDomStorageDatabase::OpenDirectory(
        std::move(options),
        storage_partition_->GetPath().Append(storage::kLocalStoragePath),
        storage::kLocalStorageLeveldbName, base::nullopt,
        base::ThreadTaskRunnerHandle::Get(),
        base::BindLambdaForTesting([&](leveldb::Status status) {
          ASSERT_TRUE(status.ok());
          open_loop.Quit();
        }));
    open_loop.Run();

    base::RunLoop populate_loop;
    database->database().PostTaskWithThisObject(
        FROM_HERE,
        base::BindLambdaForTesting([&](const storage::DomStorageDatabase& db) {
          PopulateDatabase(db);
          populate_loop.Quit();
        }));
    populate_loop.Run();

    // Ensure that this database is fully closed before returning.
    database.reset();
    task_environment_->RunUntilIdle();
  }

  static void PopulateDatabase(const storage::DomStorageDatabase& db) {
    storage::LocalStorageOriginMetaData data;
    std::map<std::vector<uint8_t>, std::vector<uint8_t>> entries;

    base::Time now = base::Time::Now();
    data.set_last_modified(now.ToInternalValue());
    data.set_size_bytes(16);
    ASSERT_TRUE(
        db.Put(CreateMetaDataKey(Origin1()),
               base::as_bytes(base::make_span(data.SerializeAsString())))
            .ok());
    ASSERT_TRUE(db.Put(CreateDataKey(Origin1()), {}).ok());

    base::Time one_day_ago = now - base::TimeDelta::FromDays(1);
    data.set_last_modified(one_day_ago.ToInternalValue());
    ASSERT_TRUE(
        db.Put(CreateMetaDataKey(Origin2()),
               base::as_bytes(base::make_span((data.SerializeAsString()))))
            .ok());
    ASSERT_TRUE(db.Put(CreateDataKey(Origin2()), {}).ok());

    base::Time sixty_days_ago = now - base::TimeDelta::FromDays(60);
    data.set_last_modified(sixty_days_ago.ToInternalValue());
    ASSERT_TRUE(
        db.Put(CreateMetaDataKey(Origin3()),
               base::as_bytes(base::make_span(data.SerializeAsString())))
            .ok());
    ASSERT_TRUE(db.Put(CreateDataKey(Origin3()), {}).ok());
  }

 private:
  static std::vector<uint8_t> CreateDataKey(const url::Origin& origin) {
    auto origin_str = origin.Serialize();
    std::vector<uint8_t> serialized_origin(origin_str.begin(),
                                           origin_str.end());
    std::vector<uint8_t> key = {'_'};
    key.insert(key.end(), serialized_origin.begin(), serialized_origin.end());
    key.push_back(0);
    key.push_back('X');
    return key;
  }

  static std::vector<uint8_t> CreateMetaDataKey(const url::Origin& origin) {
    const uint8_t kMetaPrefix[] = {'M', 'E', 'T', 'A', ':'};
    auto origin_str = origin.Serialize();
    std::vector<uint8_t> serialized_origin(origin_str.begin(),
                                           origin_str.end());
    std::vector<uint8_t> key;
    key.reserve(base::size(kMetaPrefix) + serialized_origin.size());
    key.insert(key.end(), kMetaPrefix, kMetaPrefix + base::size(kMetaPrefix));
    key.insert(key.end(), serialized_origin.begin(), serialized_origin.end());
    return key;
  }

  void GetLocalStorageUsage() {
    dom_storage_context_->GetLocalStorageUsage(
        base::BindOnce(&RemoveLocalStorageTester::OnGotLocalStorageUsage,
                       base::Unretained(this)));
  }

  void OnGotLocalStorageUsage(
      const std::vector<content::StorageUsageInfo>& infos) {
    infos_ = infos;
    await_completion_.Notify();
  }

  // We don't own these pointers.
  BrowserTaskEnvironment* const task_environment_;
  StoragePartition* const storage_partition_;
  DOMStorageContext* dom_storage_context_;

  std::vector<content::StorageUsageInfo> infos_;

  AwaitCompletionHelper await_completion_;

  DISALLOW_COPY_AND_ASSIGN(RemoveLocalStorageTester);
};

class RemoveCodeCacheTester {
 public:
  explicit RemoveCodeCacheTester(GeneratedCodeCacheContext* code_cache_context)
      : code_cache_context_(code_cache_context) {}

  enum Cache { kJs, kWebAssembly };

  bool ContainsEntry(Cache cache, GURL url, GURL origin_lock) {
    entry_exists_ = false;
    GeneratedCodeCache::ReadDataCallback callback = base::BindRepeating(
        &RemoveCodeCacheTester::FetchEntryCallback, base::Unretained(this));
    GetCache(cache)->FetchEntry(url, origin_lock, callback);
    await_completion_.BlockUntilNotified();
    return entry_exists_;
  }

  void AddEntry(Cache cache,
                GURL url,
                GURL origin_lock,
                const std::string& data) {
    std::vector<uint8_t> data_vector(data.begin(), data.end());
    GetCache(cache)->WriteEntry(url, origin_lock, base::Time::Now(),
                                data_vector);
    base::RunLoop().RunUntilIdle();
  }

  void SetLastUseTime(Cache cache,
                      GURL url,
                      GURL origin_lock,
                      base::Time time) {
    GetCache(cache)->SetLastUsedTimeForTest(
        url, origin_lock, time,
        base::BindRepeating(&RemoveCodeCacheTester::SetTimeCallback,
                            base::Unretained(this)));
    await_completion_.BlockUntilNotified();
  }

  std::string received_data() { return received_data_; }

 private:
  GeneratedCodeCache* GetCache(Cache cache) {
    if (cache == kJs)
      return code_cache_context_->generated_js_code_cache();
    else
      return code_cache_context_->generated_wasm_code_cache();
  }

  void FetchEntryCallback(const base::Time& response_time,
                          mojo_base::BigBuffer data) {
    if (!response_time.is_null()) {
      entry_exists_ = true;
      received_data_ = std::string(data.data(), data.data() + data.size());
    } else {
      entry_exists_ = false;
    }
    await_completion_.Notify();
  }

  void SetTimeCallback() { await_completion_.Notify(); }

  bool entry_exists_;
  AwaitCompletionHelper await_completion_;
  GeneratedCodeCacheContext* code_cache_context_;
  std::string received_data_;
  DISALLOW_COPY_AND_ASSIGN(RemoveCodeCacheTester);
};

#if BUILDFLAG(ENABLE_PLUGINS)
class RemovePluginPrivateDataTester {
 public:
  explicit RemovePluginPrivateDataTester(
      storage::FileSystemContext* filesystem_context)
      : filesystem_context_(filesystem_context) {}

  // Add some files to the PluginPrivateFileSystem. They are created as follows:
  //   Origin1() - ClearKey - 1 file - timestamp 10 days ago
  //   Origin2() - Widevine - 2 files - timestamps now and 60 days ago
  void AddPluginPrivateTestData() {
    base::Time now = base::Time::Now();
    base::Time ten_days_ago = now - base::TimeDelta::FromDays(10);
    base::Time sixty_days_ago = now - base::TimeDelta::FromDays(60);

    // Create a PluginPrivateFileSystem for ClearKey and add a single file
    // with a timestamp of 1 day ago.
    std::string clearkey_fsid =
        CreateFileSystem(kClearKeyCdmPluginId, Origin1().GetURL());
    clearkey_file_ = CreateFile(Origin1().GetURL(), clearkey_fsid, "foo");
    SetFileTimestamp(clearkey_file_, ten_days_ago);

    // Create a second PluginPrivateFileSystem for Widevine and add two files
    // with different times.
    std::string widevine_fsid =
        CreateFileSystem(kWidevineCdmPluginId, Origin2().GetURL());
    storage::FileSystemURL widevine_file1 =
        CreateFile(Origin2().GetURL(), widevine_fsid, "bar1");
    storage::FileSystemURL widevine_file2 =
        CreateFile(Origin2().GetURL(), widevine_fsid, "bar2");
    SetFileTimestamp(widevine_file1, now);
    SetFileTimestamp(widevine_file2, sixty_days_ago);
  }

  void DeleteClearKeyTestData() { DeleteFile(clearkey_file_); }

  // Returns true, if the given origin exists in a PluginPrivateFileSystem.
  bool DataExistsForOrigin(const url::Origin& origin) {
    AwaitCompletionHelper await_completion;
    bool data_exists_for_origin = false;
    filesystem_context_->default_file_task_runner()->PostTask(
        FROM_HERE,
        base::BindOnce(&RemovePluginPrivateDataTester::
                           CheckIfDataExistsForOriginOnFileTaskRunner,
                       base::Unretained(this), origin, &data_exists_for_origin,
                       &await_completion));
    await_completion.BlockUntilNotified();
    return data_exists_for_origin;
  }

 private:
  // Creates a PluginPrivateFileSystem for the |plugin_name| and |origin|
  // provided. Returns the file system ID for the created
  // PluginPrivateFileSystem.
  std::string CreateFileSystem(const std::string& plugin_name,
                               const GURL& origin) {
    AwaitCompletionHelper await_completion;
    std::string fsid = storage::IsolatedContext::GetInstance()
                           ->RegisterFileSystemForVirtualPath(
                               storage::kFileSystemTypePluginPrivate,
                               ppapi::kPluginPrivateRootName, base::FilePath());
    EXPECT_TRUE(storage::ValidateIsolatedFileSystemId(fsid));
    filesystem_context_->OpenPluginPrivateFileSystem(
        origin, storage::kFileSystemTypePluginPrivate, fsid, plugin_name,
        storage::OPEN_FILE_SYSTEM_CREATE_IF_NONEXISTENT,
        base::BindOnce(&RemovePluginPrivateDataTester::OnFileSystemOpened,
                       base::Unretained(this), &await_completion));
    await_completion.BlockUntilNotified();
    return fsid;
  }

  // Creates a file named |file_name| in the PluginPrivateFileSystem identified
  // by |origin| and |fsid|. Returns the URL for the created file. The file
  // must not already exist or the test will fail.
  storage::FileSystemURL CreateFile(const GURL& origin,
                                    const std::string& fsid,
                                    const std::string& file_name) {
    AwaitCompletionHelper await_completion;
    std::string root = storage::GetIsolatedFileSystemRootURIString(
        origin, fsid, ppapi::kPluginPrivateRootName);
    storage::FileSystemURL file_url =
        filesystem_context_->CrackURL(GURL(root + file_name));
    storage::AsyncFileUtil* file_util = filesystem_context_->GetAsyncFileUtil(
        storage::kFileSystemTypePluginPrivate);
    std::unique_ptr<storage::FileSystemOperationContext> operation_context =
        std::make_unique<storage::FileSystemOperationContext>(
            filesystem_context_);
    operation_context->set_allowed_bytes_growth(
        storage::QuotaManager::kNoLimit);
    file_util->EnsureFileExists(
        std::move(operation_context), file_url,
        base::BindOnce(&RemovePluginPrivateDataTester::OnFileCreated,
                       base::Unretained(this), &await_completion));
    await_completion.BlockUntilNotified();
    return file_url;
  }

  void DeleteFile(storage::FileSystemURL file_url) {
    AwaitCompletionHelper await_completion;
    storage::AsyncFileUtil* file_util = filesystem_context_->GetAsyncFileUtil(
        storage::kFileSystemTypePluginPrivate);
    std::unique_ptr<storage::FileSystemOperationContext> operation_context =
        std::make_unique<storage::FileSystemOperationContext>(
            filesystem_context_);
    file_util->DeleteFile(
        std::move(operation_context), file_url,
        base::BindOnce(&RemovePluginPrivateDataTester::OnFileDeleted,
                       base::Unretained(this), &await_completion));
    await_completion.BlockUntilNotified();
  }

  // Sets the last_access_time and last_modified_time to |time_stamp| on the
  // file specified by |file_url|. The file must already exist.
  void SetFileTimestamp(const storage::FileSystemURL& file_url,
                        const base::Time& time_stamp) {
    AwaitCompletionHelper await_completion;
    storage::AsyncFileUtil* file_util = filesystem_context_->GetAsyncFileUtil(
        storage::kFileSystemTypePluginPrivate);
    std::unique_ptr<storage::FileSystemOperationContext> operation_context =
        std::make_unique<storage::FileSystemOperationContext>(
            filesystem_context_);
    file_util->Touch(
        std::move(operation_context), file_url, time_stamp, time_stamp,
        base::BindOnce(&RemovePluginPrivateDataTester::OnFileTouched,
                       base::Unretained(this), &await_completion));
    await_completion.BlockUntilNotified();
  }

  void OnFileSystemOpened(AwaitCompletionHelper* await_completion,
                          base::File::Error result) {
    EXPECT_EQ(base::File::FILE_OK, result) << base::File::ErrorToString(result);
    await_completion->Notify();
  }

  void OnFileCreated(AwaitCompletionHelper* await_completion,
                     base::File::Error result,
                     bool created) {
    EXPECT_EQ(base::File::FILE_OK, result) << base::File::ErrorToString(result);
    EXPECT_TRUE(created);
    await_completion->Notify();
  }

  void OnFileDeleted(AwaitCompletionHelper* await_completion,
                     base::File::Error result) {
    EXPECT_EQ(base::File::FILE_OK, result) << base::File::ErrorToString(result);
    await_completion->Notify();
  }

  void OnFileTouched(AwaitCompletionHelper* await_completion,
                     base::File::Error result) {
    EXPECT_EQ(base::File::FILE_OK, result) << base::File::ErrorToString(result);
    await_completion->Notify();
  }

  // If |origin| exists in the PluginPrivateFileSystem, set
  // |data_exists_for_origin| to true, false otherwise.
  void CheckIfDataExistsForOriginOnFileTaskRunner(
      const url::Origin& origin,
      bool* data_exists_for_origin,
      AwaitCompletionHelper* await_completion) {
    storage::FileSystemBackend* backend =
        filesystem_context_->GetFileSystemBackend(
            storage::kFileSystemTypePluginPrivate);
    storage::FileSystemQuotaUtil* quota_util = backend->GetQuotaUtil();

    // Determine the set of origins used.
    std::set<GURL> origins;
    quota_util->GetOriginsForTypeOnFileTaskRunner(
        storage::kFileSystemTypePluginPrivate, &origins);
    *data_exists_for_origin = origins.find(origin.GetURL()) != origins.end();

    // AwaitCompletionHelper and MessageLoop don't work on a
    // SequencedTaskRunner, so post a task on the IO thread.
    base::PostTask(FROM_HERE, {BrowserThread::IO},
                   base::BindOnce(&AwaitCompletionHelper::Notify,
                                  base::Unretained(await_completion)));
  }

  // We don't own this pointer.
  storage::FileSystemContext* filesystem_context_;

  // Keep track of the URL for the ClearKey file so that it can be written to
  // or deleted.
  storage::FileSystemURL clearkey_file_;

  DISALLOW_COPY_AND_ASSIGN(RemovePluginPrivateDataTester);
};
#endif  // BUILDFLAG(ENABLE_PLUGINS)

bool IsWebSafeSchemeForTest(const std::string& scheme) {
  return scheme == url::kHttpScheme;
}

bool DoesOriginMatchForUnprotectedWeb(
    const url::Origin& origin,
    storage::SpecialStoragePolicy* special_storage_policy) {
  if (IsWebSafeSchemeForTest(origin.scheme()))
    return !special_storage_policy->IsStorageProtected(origin.GetURL());

  return false;
}

bool DoesOriginMatchForBothProtectedAndUnprotectedWeb(
    const url::Origin& origin,
    storage::SpecialStoragePolicy* special_storage_policy) {
  return true;
}

bool DoesOriginMatchUnprotected(
    const url::Origin& origin,
    storage::SpecialStoragePolicy* special_storage_policy) {
  return origin.scheme() != OriginDevTools().scheme();
}

void ClearQuotaData(content::StoragePartition* partition,
                    base::RunLoop* loop_to_quit) {
  partition->ClearData(
      kAllQuotaRemoveMask, StoragePartition::QUOTA_MANAGED_STORAGE_MASK_ALL,
      GURL(), base::Time(), base::Time::Max(), loop_to_quit->QuitClosure());
}

void ClearQuotaDataWithOriginMatcher(
    content::StoragePartition* partition,
    StoragePartition::OriginMatcherFunction origin_matcher,
    const base::Time delete_begin,
    base::RunLoop* loop_to_quit) {
  partition->ClearData(kAllQuotaRemoveMask,
                       StoragePartition::QUOTA_MANAGED_STORAGE_MASK_ALL,
                       std::move(origin_matcher), nullptr, false, delete_begin,
                       base::Time::Max(), loop_to_quit->QuitClosure());
}

void ClearQuotaDataForOrigin(content::StoragePartition* partition,
                             const GURL& remove_origin,
                             const base::Time delete_begin,
                             base::RunLoop* loop_to_quit) {
  partition->ClearData(kAllQuotaRemoveMask,
                       StoragePartition::QUOTA_MANAGED_STORAGE_MASK_ALL,
                       remove_origin, delete_begin, base::Time::Max(),
                       loop_to_quit->QuitClosure());
}

void ClearQuotaDataForNonPersistent(content::StoragePartition* partition,
                                    const base::Time delete_begin,
                                    base::RunLoop* loop_to_quit) {
  partition->ClearData(kAllQuotaRemoveMask,
                       ~StoragePartition::QUOTA_MANAGED_STORAGE_MASK_PERSISTENT,
                       GURL(), delete_begin, base::Time::Max(),
                       loop_to_quit->QuitClosure());
}

void ClearCookies(content::StoragePartition* partition,
                  const base::Time delete_begin,
                  const base::Time delete_end,
                  base::RunLoop* run_loop) {
  partition->ClearData(StoragePartition::REMOVE_DATA_MASK_COOKIES,
                       StoragePartition::QUOTA_MANAGED_STORAGE_MASK_ALL, GURL(),
                       delete_begin, delete_end, run_loop->QuitClosure());
}

void ClearCookiesMatchingInfo(content::StoragePartition* partition,
                              CookieDeletionFilterPtr delete_filter,
                              base::RunLoop* run_loop) {
  base::Time delete_begin;
  if (delete_filter->created_after_time.has_value())
    delete_begin = delete_filter->created_after_time.value();
  base::Time delete_end;
  if (delete_filter->created_before_time.has_value())
    delete_end = delete_filter->created_before_time.value();
  partition->ClearData(StoragePartition::REMOVE_DATA_MASK_COOKIES,
                       StoragePartition::QUOTA_MANAGED_STORAGE_MASK_ALL,
                       StoragePartition::OriginMatcherFunction(),
                       std::move(delete_filter), false, delete_begin,
                       delete_end, run_loop->QuitClosure());
}

void ClearStuff(uint32_t remove_mask,
                content::StoragePartition* partition,
                const base::Time delete_begin,
                const base::Time delete_end,
                StoragePartition::OriginMatcherFunction origin_matcher,
                base::RunLoop* run_loop) {
  partition->ClearData(remove_mask,
                       StoragePartition::QUOTA_MANAGED_STORAGE_MASK_ALL,
                       std::move(origin_matcher), nullptr, false, delete_begin,
                       delete_end, run_loop->QuitClosure());
}

void ClearData(content::StoragePartition* partition, base::RunLoop* run_loop) {
  base::Time time;
  partition->ClearData(StoragePartition::REMOVE_DATA_MASK_SHADER_CACHE,
                       StoragePartition::QUOTA_MANAGED_STORAGE_MASK_ALL, GURL(),
                       time, time, run_loop->QuitClosure());
}

void ClearCodeCache(content::StoragePartition* partition,
                    base::Time begin_time,
                    base::Time end_time,
                    base::RepeatingCallback<bool(const GURL&)> url_predicate,
                    base::RunLoop* run_loop) {
  partition->ClearCodeCaches(begin_time, end_time, url_predicate,
                             run_loop->QuitClosure());
}

bool FilterURL(const GURL& url) {
  if (url == FilterResourceURLForCodeCache())
    return true;
  return false;
}

#if BUILDFLAG(ENABLE_PLUGINS)
void ClearPluginPrivateData(content::StoragePartition* partition,
                            const GURL& storage_origin,
                            const base::Time delete_begin,
                            const base::Time delete_end,
                            base::RunLoop* run_loop) {
  partition->ClearData(
      StoragePartitionImpl::REMOVE_DATA_MASK_PLUGIN_PRIVATE_DATA,
      StoragePartition::QUOTA_MANAGED_STORAGE_MASK_ALL, storage_origin,
      delete_begin, delete_end, run_loop->QuitClosure());
}
#endif  // BUILDFLAG(ENABLE_PLUGINS)

bool FilterMatchesCookie(const CookieDeletionFilterPtr& filter,
                         const net::CanonicalCookie& cookie) {
  return network::DeletionFilterToInfo(filter.Clone()).Matches(cookie);
}

}  // namespace

class StoragePartitionImplTest : public testing::Test {
 public:
  StoragePartitionImplTest()
      : task_environment_(content::BrowserTaskEnvironment::IO_MAINLOOP),
        browser_context_(new TestBrowserContext()) {}

  MockQuotaManager* GetMockManager() {
    if (!quota_manager_.get()) {
      quota_manager_ = new MockQuotaManager(
          browser_context_->IsOffTheRecord(), browser_context_->GetPath(),
          base::CreateSingleThreadTaskRunner({BrowserThread::IO}).get(),
          browser_context_->GetSpecialStoragePolicy());
    }
    return quota_manager_.get();
  }

  TestBrowserContext* browser_context() { return browser_context_.get(); }

  content::BrowserTaskEnvironment* task_environment() {
    return &task_environment_;
  }

 private:
  content::BrowserTaskEnvironment task_environment_;
  std::unique_ptr<TestBrowserContext> browser_context_;
  scoped_refptr<MockQuotaManager> quota_manager_;

  DISALLOW_COPY_AND_ASSIGN(StoragePartitionImplTest);
};

class StoragePartitionShaderClearTest : public testing::Test {
 public:
  StoragePartitionShaderClearTest()
      : task_environment_(content::BrowserTaskEnvironment::IO_MAINLOOP),
        browser_context_(new TestBrowserContext()) {
    InitShaderCacheFactorySingleton(base::ThreadTaskRunnerHandle::Get());
    GetShaderCacheFactorySingleton()->SetCacheInfo(
        kDefaultClientId,
        BrowserContext::GetDefaultStoragePartition(browser_context())
            ->GetPath());
    cache_ = GetShaderCacheFactorySingleton()->Get(kDefaultClientId);
  }

  ~StoragePartitionShaderClearTest() override {
    cache_ = nullptr;
    GetShaderCacheFactorySingleton()->RemoveCacheInfo(kDefaultClientId);
  }

  void InitCache() {
    net::TestCompletionCallback available_cb;
    int rv = cache_->SetAvailableCallback(available_cb.callback());
    ASSERT_EQ(net::OK, available_cb.GetResult(rv));
    EXPECT_EQ(0, cache_->Size());

    cache_->Cache(kCacheKey, kCacheValue);

    net::TestCompletionCallback complete_cb;

    rv = cache_->SetCacheCompleteCallback(complete_cb.callback());
    ASSERT_EQ(net::OK, complete_cb.GetResult(rv));
  }

  size_t Size() { return cache_->Size(); }

  TestBrowserContext* browser_context() { return browser_context_.get(); }

 private:
  content::BrowserTaskEnvironment task_environment_;
  std::unique_ptr<TestBrowserContext> browser_context_;

  scoped_refptr<gpu::ShaderDiskCache> cache_;
};

// Tests ---------------------------------------------------------------------

TEST_F(StoragePartitionShaderClearTest, ClearShaderCache) {
  InitCache();
  EXPECT_EQ(1u, Size());

  base::RunLoop run_loop;
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(&ClearData,
                                BrowserContext::GetDefaultStoragePartition(
                                    browser_context()),
                                &run_loop));
  run_loop.Run();
  EXPECT_EQ(0u, Size());
}

TEST_F(StoragePartitionImplTest, QuotaClientMaskGeneration) {
  EXPECT_EQ(storage::QuotaClient::kFileSystem,
            StoragePartitionImpl::GenerateQuotaClientMask(
                StoragePartition::REMOVE_DATA_MASK_FILE_SYSTEMS));
  EXPECT_EQ(storage::QuotaClient::kDatabase,
            StoragePartitionImpl::GenerateQuotaClientMask(
                StoragePartition::REMOVE_DATA_MASK_WEBSQL));
  EXPECT_EQ(storage::QuotaClient::kAppcache,
            StoragePartitionImpl::GenerateQuotaClientMask(
                StoragePartition::REMOVE_DATA_MASK_APPCACHE));
  EXPECT_EQ(storage::QuotaClient::kIndexedDatabase,
            StoragePartitionImpl::GenerateQuotaClientMask(
                StoragePartition::REMOVE_DATA_MASK_INDEXEDDB));
  EXPECT_EQ(storage::QuotaClient::kFileSystem |
                storage::QuotaClient::kDatabase |
                storage::QuotaClient::kAppcache |
                storage::QuotaClient::kIndexedDatabase,
            StoragePartitionImpl::GenerateQuotaClientMask(kAllQuotaRemoveMask));
}

void PopulateTestQuotaManagedPersistentData(MockQuotaManager* manager) {
  manager->AddOrigin(Origin2(), kPersistent, kClientFile, base::Time());
  manager->AddOrigin(Origin3(), kPersistent, kClientFile,
                     base::Time::Now() - base::TimeDelta::FromDays(1));

  EXPECT_FALSE(manager->OriginHasData(Origin1(), kPersistent, kClientFile));
  EXPECT_TRUE(manager->OriginHasData(Origin2(), kPersistent, kClientFile));
  EXPECT_TRUE(manager->OriginHasData(Origin3(), kPersistent, kClientFile));
}

void PopulateTestQuotaManagedTemporaryData(MockQuotaManager* manager) {
  manager->AddOrigin(Origin1(), kTemporary, kClientFile, base::Time::Now());
  manager->AddOrigin(Origin3(), kTemporary, kClientFile,
                     base::Time::Now() - base::TimeDelta::FromDays(1));

  EXPECT_TRUE(manager->OriginHasData(Origin1(), kTemporary, kClientFile));
  EXPECT_FALSE(manager->OriginHasData(Origin2(), kTemporary, kClientFile));
  EXPECT_TRUE(manager->OriginHasData(Origin3(), kTemporary, kClientFile));
}

void PopulateTestQuotaManagedData(MockQuotaManager* manager) {
  // Set up Origin1() with a temporary quota, Origin2() with a persistent
  // quota, and Origin3() with both. Origin1() is modified now, Origin2()
  // is modified at the beginning of time, and Origin3() is modified one day
  // ago.
  PopulateTestQuotaManagedPersistentData(manager);
  PopulateTestQuotaManagedTemporaryData(manager);
}

void PopulateTestQuotaManagedNonBrowsingData(MockQuotaManager* manager) {
  manager->AddOrigin(OriginDevTools(), kTemporary, kClientFile, base::Time());
  manager->AddOrigin(OriginDevTools(), kPersistent, kClientFile, base::Time());
}

TEST_F(StoragePartitionImplTest, RemoveQuotaManagedDataForeverBoth) {
  PopulateTestQuotaManagedData(GetMockManager());

  StoragePartitionImpl* partition = static_cast<StoragePartitionImpl*>(
      BrowserContext::GetDefaultStoragePartition(browser_context()));
  partition->OverrideQuotaManagerForTesting(GetMockManager());

  base::RunLoop run_loop;
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(&ClearQuotaData, partition, &run_loop));
  run_loop.Run();

  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin1(), kTemporary, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin2(), kTemporary, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin3(), kTemporary, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin1(), kPersistent, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin2(), kPersistent, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin3(), kPersistent, kClientFile));
}

TEST_F(StoragePartitionImplTest, RemoveQuotaManagedDataForeverOnlyTemporary) {
  PopulateTestQuotaManagedTemporaryData(GetMockManager());

  StoragePartitionImpl* partition = static_cast<StoragePartitionImpl*>(
      BrowserContext::GetDefaultStoragePartition(browser_context()));
  partition->OverrideQuotaManagerForTesting(GetMockManager());

  base::RunLoop run_loop;
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(&ClearQuotaData, partition, &run_loop));
  run_loop.Run();

  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin1(), kTemporary, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin2(), kTemporary, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin3(), kTemporary, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin1(), kPersistent, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin2(), kPersistent, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin3(), kPersistent, kClientFile));
}

TEST_F(StoragePartitionImplTest, RemoveQuotaManagedDataForeverOnlyPersistent) {
  PopulateTestQuotaManagedPersistentData(GetMockManager());

  StoragePartitionImpl* partition = static_cast<StoragePartitionImpl*>(
      BrowserContext::GetDefaultStoragePartition(browser_context()));
  partition->OverrideQuotaManagerForTesting(GetMockManager());

  base::RunLoop run_loop;
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(&ClearQuotaData, partition, &run_loop));
  run_loop.Run();

  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin1(), kTemporary, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin2(), kTemporary, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin3(), kTemporary, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin1(), kPersistent, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin2(), kPersistent, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin3(), kPersistent, kClientFile));
}

TEST_F(StoragePartitionImplTest, RemoveQuotaManagedDataForeverNeither) {
  StoragePartitionImpl* partition = static_cast<StoragePartitionImpl*>(
      BrowserContext::GetDefaultStoragePartition(browser_context()));
  partition->OverrideQuotaManagerForTesting(GetMockManager());

  base::RunLoop run_loop;
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(&ClearQuotaData, partition, &run_loop));
  run_loop.Run();

  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin1(), kTemporary, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin2(), kTemporary, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin3(), kTemporary, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin1(), kPersistent, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin2(), kPersistent, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin3(), kPersistent, kClientFile));
}

TEST_F(StoragePartitionImplTest, RemoveQuotaManagedDataForeverSpecificOrigin) {
  PopulateTestQuotaManagedData(GetMockManager());

  StoragePartitionImpl* partition = static_cast<StoragePartitionImpl*>(
      BrowserContext::GetDefaultStoragePartition(browser_context()));
  partition->OverrideQuotaManagerForTesting(GetMockManager());

  base::RunLoop run_loop;
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(&ClearQuotaDataForOrigin, partition,
                                Origin1().GetURL(), base::Time(), &run_loop));
  run_loop.Run();

  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin1(), kTemporary, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin2(), kTemporary, kClientFile));
  EXPECT_TRUE(
      GetMockManager()->OriginHasData(Origin3(), kTemporary, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin1(), kPersistent, kClientFile));
  EXPECT_TRUE(
      GetMockManager()->OriginHasData(Origin2(), kPersistent, kClientFile));
  EXPECT_TRUE(
      GetMockManager()->OriginHasData(Origin3(), kPersistent, kClientFile));
}

TEST_F(StoragePartitionImplTest, RemoveQuotaManagedDataForLastHour) {
  PopulateTestQuotaManagedData(GetMockManager());

  StoragePartitionImpl* partition = static_cast<StoragePartitionImpl*>(
      BrowserContext::GetDefaultStoragePartition(browser_context()));
  partition->OverrideQuotaManagerForTesting(GetMockManager());

  base::RunLoop run_loop;
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::BindOnce(&ClearQuotaDataForOrigin, partition, GURL(),
                     base::Time::Now() - base::TimeDelta::FromHours(1),
                     &run_loop));
  run_loop.Run();

  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin1(), kTemporary, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin2(), kTemporary, kClientFile));
  EXPECT_TRUE(
      GetMockManager()->OriginHasData(Origin3(), kTemporary, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin1(), kPersistent, kClientFile));
  EXPECT_TRUE(
      GetMockManager()->OriginHasData(Origin2(), kPersistent, kClientFile));
  EXPECT_TRUE(
      GetMockManager()->OriginHasData(Origin3(), kPersistent, kClientFile));
}

TEST_F(StoragePartitionImplTest, RemoveQuotaManagedDataForLastWeek) {
  PopulateTestQuotaManagedData(GetMockManager());

  base::RunLoop run_loop;
  StoragePartitionImpl* partition = static_cast<StoragePartitionImpl*>(
      BrowserContext::GetDefaultStoragePartition(browser_context()));
  partition->OverrideQuotaManagerForTesting(GetMockManager());
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::BindOnce(&ClearQuotaDataForNonPersistent, partition,
                     base::Time::Now() - base::TimeDelta::FromDays(7),
                     &run_loop));
  run_loop.Run();

  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin1(), kTemporary, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin2(), kTemporary, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin3(), kTemporary, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin1(), kPersistent, kClientFile));
  EXPECT_TRUE(
      GetMockManager()->OriginHasData(Origin2(), kPersistent, kClientFile));
  EXPECT_TRUE(
      GetMockManager()->OriginHasData(Origin3(), kPersistent, kClientFile));
}

TEST_F(StoragePartitionImplTest, RemoveQuotaManagedUnprotectedOrigins) {
  // Protect Origin1().
  scoped_refptr<MockSpecialStoragePolicy> mock_policy =
      new MockSpecialStoragePolicy;
  mock_policy->AddProtected(Origin1().GetURL());

  PopulateTestQuotaManagedData(GetMockManager());

  StoragePartitionImpl* partition = static_cast<StoragePartitionImpl*>(
      BrowserContext::GetDefaultStoragePartition(browser_context()));
  partition->OverrideQuotaManagerForTesting(GetMockManager());
  partition->OverrideSpecialStoragePolicyForTesting(mock_policy.get());

  base::RunLoop run_loop;
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::BindOnce(&ClearQuotaDataWithOriginMatcher, partition,
                     base::BindRepeating(&DoesOriginMatchForUnprotectedWeb),
                     base::Time(), &run_loop));
  run_loop.Run();

  EXPECT_TRUE(
      GetMockManager()->OriginHasData(Origin1(), kTemporary, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin2(), kTemporary, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin3(), kTemporary, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin1(), kPersistent, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin2(), kPersistent, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin3(), kPersistent, kClientFile));
}

TEST_F(StoragePartitionImplTest, RemoveQuotaManagedProtectedOrigins) {
  // Protect Origin1().
  scoped_refptr<MockSpecialStoragePolicy> mock_policy =
      new MockSpecialStoragePolicy;
  mock_policy->AddProtected(Origin1().GetURL());

  PopulateTestQuotaManagedData(GetMockManager());

  // Try to remove Origin1(). Expect success.
  base::RunLoop run_loop;
  StoragePartitionImpl* partition = static_cast<StoragePartitionImpl*>(
      BrowserContext::GetDefaultStoragePartition(browser_context()));
  partition->OverrideQuotaManagerForTesting(GetMockManager());
  partition->OverrideSpecialStoragePolicyForTesting(mock_policy.get());
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::BindOnce(&ClearQuotaDataWithOriginMatcher, partition,
                     base::BindRepeating(
                         &DoesOriginMatchForBothProtectedAndUnprotectedWeb),
                     base::Time(), &run_loop));
  run_loop.Run();

  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin1(), kTemporary, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin2(), kTemporary, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin3(), kTemporary, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin1(), kPersistent, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin2(), kPersistent, kClientFile));
  EXPECT_FALSE(
      GetMockManager()->OriginHasData(Origin3(), kPersistent, kClientFile));
}

TEST_F(StoragePartitionImplTest, RemoveQuotaManagedIgnoreDevTools) {
  PopulateTestQuotaManagedNonBrowsingData(GetMockManager());

  base::RunLoop run_loop;
  StoragePartitionImpl* partition = static_cast<StoragePartitionImpl*>(
      BrowserContext::GetDefaultStoragePartition(browser_context()));
  partition->OverrideQuotaManagerForTesting(GetMockManager());
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::BindOnce(&ClearQuotaDataWithOriginMatcher, partition,
                     base::BindRepeating(&DoesOriginMatchUnprotected),
                     base::Time(), &run_loop));
  run_loop.Run();

  // Check that devtools data isn't removed.
  EXPECT_TRUE(GetMockManager()->OriginHasData(OriginDevTools(), kTemporary,
                                              kClientFile));
  EXPECT_TRUE(GetMockManager()->OriginHasData(OriginDevTools(), kPersistent,
                                              kClientFile));
}

TEST_F(StoragePartitionImplTest, RemoveCookieForever) {
  StoragePartition* partition =
      BrowserContext::GetDefaultStoragePartition(browser_context());

  RemoveCookieTester tester(partition);
  tester.AddCookie();
  ASSERT_TRUE(tester.ContainsCookie());

  base::RunLoop run_loop;
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(&ClearCookies, partition, base::Time(),
                                base::Time::Max(), &run_loop));
  run_loop.Run();

  EXPECT_FALSE(tester.ContainsCookie());
}

TEST_F(StoragePartitionImplTest, RemoveCookieLastHour) {
  StoragePartition* partition =
      BrowserContext::GetDefaultStoragePartition(browser_context());

  RemoveCookieTester tester(partition);
  tester.AddCookie();
  ASSERT_TRUE(tester.ContainsCookie());

  base::Time an_hour_ago = base::Time::Now() - base::TimeDelta::FromHours(1);

  base::RunLoop run_loop;
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(&ClearCookies, partition, an_hour_ago,
                                base::Time::Max(), &run_loop));
  run_loop.Run();

  EXPECT_FALSE(tester.ContainsCookie());
}

TEST_F(StoragePartitionImplTest, RemoveCookieWithDeleteInfo) {
  StoragePartition* partition =
      BrowserContext::GetDefaultStoragePartition(browser_context());

  RemoveCookieTester tester(partition);
  tester.AddCookie();
  ASSERT_TRUE(tester.ContainsCookie());

  base::RunLoop run_loop2;
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(&ClearCookiesMatchingInfo, partition,
                                CookieDeletionFilter::New(), &run_loop2));
  run_loop2.RunUntilIdle();
  EXPECT_FALSE(tester.ContainsCookie());
}

TEST_F(StoragePartitionImplTest, RemoveUnprotectedLocalStorageForever) {
  // Protect Origin1().
  scoped_refptr<MockSpecialStoragePolicy> mock_policy =
      new MockSpecialStoragePolicy;
  mock_policy->AddProtected(Origin1().GetURL());

  RemoveLocalStorageTester tester(task_environment(), browser_context());

  tester.AddDOMStorageTestData();
  EXPECT_TRUE(tester.DOMStorageExistsForOrigin(Origin1()));
  EXPECT_TRUE(tester.DOMStorageExistsForOrigin(Origin2()));
  EXPECT_TRUE(tester.DOMStorageExistsForOrigin(Origin3()));

  StoragePartitionImpl* partition = static_cast<StoragePartitionImpl*>(
      BrowserContext::GetDefaultStoragePartition(browser_context()));
  partition->OverrideSpecialStoragePolicyForTesting(mock_policy.get());

  base::RunLoop run_loop;
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::BindOnce(
          &ClearStuff, StoragePartitionImpl::REMOVE_DATA_MASK_LOCAL_STORAGE,
          partition, base::Time(), base::Time::Max(),
          base::BindRepeating(&DoesOriginMatchForUnprotectedWeb), &run_loop));
  run_loop.Run();
  // ClearData only guarantees that tasks to delete data are scheduled when its
  // callback is invoked. It doesn't guarantee data has actually been cleared.
  // So run all scheduled tasks to make sure data is cleared.
  base::RunLoop().RunUntilIdle();

  EXPECT_TRUE(tester.DOMStorageExistsForOrigin(Origin1()));
  EXPECT_FALSE(tester.DOMStorageExistsForOrigin(Origin2()));
  EXPECT_FALSE(tester.DOMStorageExistsForOrigin(Origin3()));
}

TEST_F(StoragePartitionImplTest, RemoveProtectedLocalStorageForever) {
  // Protect Origin1().
  scoped_refptr<MockSpecialStoragePolicy> mock_policy =
      new MockSpecialStoragePolicy;
  mock_policy->AddProtected(Origin1().GetURL());

  RemoveLocalStorageTester tester(task_environment(), browser_context());

  tester.AddDOMStorageTestData();
  EXPECT_TRUE(tester.DOMStorageExistsForOrigin(Origin1()));
  EXPECT_TRUE(tester.DOMStorageExistsForOrigin(Origin2()));
  EXPECT_TRUE(tester.DOMStorageExistsForOrigin(Origin3()));

  StoragePartitionImpl* partition = static_cast<StoragePartitionImpl*>(
      BrowserContext::GetDefaultStoragePartition(browser_context()));
  partition->OverrideSpecialStoragePolicyForTesting(mock_policy.get());

  base::RunLoop run_loop;
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::BindOnce(&ClearStuff,
                     StoragePartitionImpl::REMOVE_DATA_MASK_LOCAL_STORAGE,
                     partition, base::Time(), base::Time::Max(),
                     base::BindRepeating(
                         &DoesOriginMatchForBothProtectedAndUnprotectedWeb),
                     &run_loop));
  run_loop.Run();
  // ClearData only guarantees that tasks to delete data are scheduled when its
  // callback is invoked. It doesn't guarantee data has actually been cleared.
  // So run all scheduled tasks to make sure data is cleared.
  base::RunLoop().RunUntilIdle();

  // Even if Origin1() is protected, it will be deleted since we specify
  // ClearData to delete protected data.
  EXPECT_FALSE(tester.DOMStorageExistsForOrigin(Origin1()));
  EXPECT_FALSE(tester.DOMStorageExistsForOrigin(Origin2()));
  EXPECT_FALSE(tester.DOMStorageExistsForOrigin(Origin3()));
}

TEST_F(StoragePartitionImplTest, RemoveLocalStorageForLastWeek) {
  RemoveLocalStorageTester tester(task_environment(), browser_context());

  tester.AddDOMStorageTestData();
  EXPECT_TRUE(tester.DOMStorageExistsForOrigin(Origin1()));
  EXPECT_TRUE(tester.DOMStorageExistsForOrigin(Origin2()));
  EXPECT_TRUE(tester.DOMStorageExistsForOrigin(Origin3()));

  StoragePartitionImpl* partition = static_cast<StoragePartitionImpl*>(
      BrowserContext::GetDefaultStoragePartition(browser_context()));
  base::Time a_week_ago = base::Time::Now() - base::TimeDelta::FromDays(7);

  base::RunLoop run_loop;
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::BindOnce(&ClearStuff,
                     StoragePartitionImpl::REMOVE_DATA_MASK_LOCAL_STORAGE,
                     partition, a_week_ago, base::Time::Max(),
                     base::BindRepeating(
                         &DoesOriginMatchForBothProtectedAndUnprotectedWeb),
                     &run_loop));
  run_loop.Run();
  // ClearData only guarantees that tasks to delete data are scheduled when its
  // callback is invoked. It doesn't guarantee data has actually been cleared.
  // So run all scheduled tasks to make sure data is cleared.
  base::RunLoop().RunUntilIdle();

  // Origin1() and Origin2() do not have age more than a week.
  EXPECT_FALSE(tester.DOMStorageExistsForOrigin(Origin1()));
  EXPECT_FALSE(tester.DOMStorageExistsForOrigin(Origin2()));
  EXPECT_TRUE(tester.DOMStorageExistsForOrigin(Origin3()));
}

TEST_F(StoragePartitionImplTest, ClearCodeCache) {
  StoragePartitionImpl* partition = static_cast<StoragePartitionImpl*>(
      BrowserContext::GetDefaultStoragePartition(browser_context()));
  // Ensure code cache is initialized.
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(partition->GetGeneratedCodeCacheContext() != nullptr);

  RemoveCodeCacheTester tester(partition->GetGeneratedCodeCacheContext());

  GURL origin = GURL(kTestOrigin1);
  std::string data("SomeData");
  tester.AddEntry(RemoveCodeCacheTester::kJs, ResourceUrl(), origin, data);
  EXPECT_TRUE(
      tester.ContainsEntry(RemoveCodeCacheTester::kJs, ResourceUrl(), origin));
  EXPECT_EQ(tester.received_data(), data);

  base::RunLoop run_loop;
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::BindOnce(&ClearCodeCache, partition, base::Time(), base::Time(),
                     base::RepeatingCallback<bool(const GURL&)>(), &run_loop));
  run_loop.Run();

  EXPECT_FALSE(
      tester.ContainsEntry(RemoveCodeCacheTester::kJs, ResourceUrl(), origin));

  // Make sure there isn't a second invalid callback sitting in the queue.
  // (this used to be a bug).
  base::RunLoop().RunUntilIdle();
}

TEST_F(StoragePartitionImplTest, ClearCodeCacheSpecificURL) {
  StoragePartitionImpl* partition = static_cast<StoragePartitionImpl*>(
      BrowserContext::GetDefaultStoragePartition(browser_context()));
  // Ensure code cache is initialized.
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(partition->GetGeneratedCodeCacheContext() != nullptr);

  RemoveCodeCacheTester tester(partition->GetGeneratedCodeCacheContext());

  GURL origin = GURL(kTestOrigin1);
  std::string data("SomeData");
  tester.AddEntry(RemoveCodeCacheTester::kJs, ResourceUrl(), origin, data);
  tester.AddEntry(RemoveCodeCacheTester::kJs, FilterResourceURLForCodeCache(),
                  origin, data);
  EXPECT_TRUE(
      tester.ContainsEntry(RemoveCodeCacheTester::kJs, ResourceUrl(), origin));
  EXPECT_TRUE(tester.ContainsEntry(RemoveCodeCacheTester::kJs,
                                   FilterResourceURLForCodeCache(), origin));
  EXPECT_EQ(tester.received_data(), data);

  base::RunLoop run_loop;
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::BindOnce(&ClearCodeCache, partition, base::Time(), base::Time(),
                     base::BindRepeating(&FilterURL), &run_loop));
  run_loop.Run();

  EXPECT_TRUE(
      tester.ContainsEntry(RemoveCodeCacheTester::kJs, ResourceUrl(), origin));
  EXPECT_FALSE(tester.ContainsEntry(RemoveCodeCacheTester::kJs,
                                    FilterResourceURLForCodeCache(), origin));

  // Make sure there isn't a second invalid callback sitting in the queue.
  // (this used to be a bug).
  base::RunLoop().RunUntilIdle();
}

TEST_F(StoragePartitionImplTest, ClearCodeCacheDateRange) {
  StoragePartitionImpl* partition = static_cast<StoragePartitionImpl*>(
      BrowserContext::GetDefaultStoragePartition(browser_context()));
  // Ensure code cache is initialized.
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(partition->GetGeneratedCodeCacheContext() != nullptr);

  RemoveCodeCacheTester tester(partition->GetGeneratedCodeCacheContext());

  base::Time current_time = base::Time::NowFromSystemTime();
  base::Time out_of_range_time = current_time - base::TimeDelta::FromHours(3);
  base::Time begin_time = current_time - base::TimeDelta::FromHours(2);
  base::Time in_range_time = current_time - base::TimeDelta::FromHours(1);

  GURL origin = GURL(kTestOrigin1);
  std::string data("SomeData");
  tester.AddEntry(RemoveCodeCacheTester::kJs, ResourceUrl(), origin, data);
  EXPECT_TRUE(
      tester.ContainsEntry(RemoveCodeCacheTester::kJs, ResourceUrl(), origin));
  EXPECT_EQ(tester.received_data(), data);
  tester.SetLastUseTime(RemoveCodeCacheTester::kJs, ResourceUrl(), origin,
                        out_of_range_time);

  // Add a new entry.
  tester.AddEntry(RemoveCodeCacheTester::kJs, FilterResourceURLForCodeCache(),
                  origin, data);
  EXPECT_TRUE(tester.ContainsEntry(RemoveCodeCacheTester::kJs,
                                   FilterResourceURLForCodeCache(), origin));
  tester.SetLastUseTime(RemoveCodeCacheTester::kJs,
                        FilterResourceURLForCodeCache(), origin, in_range_time);

  base::RunLoop run_loop;
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::BindOnce(&ClearCodeCache, partition, begin_time, current_time,
                     base::BindRepeating(&FilterURL), &run_loop));
  run_loop.Run();

  EXPECT_TRUE(
      tester.ContainsEntry(RemoveCodeCacheTester::kJs, ResourceUrl(), origin));
  EXPECT_FALSE(tester.ContainsEntry(RemoveCodeCacheTester::kJs,
                                    FilterResourceURLForCodeCache(), origin));

  // Make sure there isn't a second invalid callback sitting in the queue.
  // (this used to be a bug).
  base::RunLoop().RunUntilIdle();
}

TEST_F(StoragePartitionImplTest, ClearWasmCodeCache) {
  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(blink::features::kWasmCodeCache);
  ASSERT_TRUE(base::FeatureList::IsEnabled(blink::features::kWasmCodeCache));

  StoragePartitionImpl* partition = static_cast<StoragePartitionImpl*>(
      BrowserContext::GetDefaultStoragePartition(browser_context()));
  // Ensure code cache is initialized.
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(partition->GetGeneratedCodeCacheContext() != nullptr);

  RemoveCodeCacheTester tester(partition->GetGeneratedCodeCacheContext());

  GURL origin = GURL(kTestOrigin1);
  std::string data("SomeData.wasm");
  tester.AddEntry(RemoveCodeCacheTester::kWebAssembly, ResourceUrl(), origin,
                  data);
  EXPECT_TRUE(tester.ContainsEntry(RemoveCodeCacheTester::kWebAssembly,
                                   ResourceUrl(), origin));
  EXPECT_EQ(tester.received_data(), data);

  base::RunLoop run_loop;
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::BindOnce(&ClearCodeCache, partition, base::Time(), base::Time(),
                     base::RepeatingCallback<bool(const GURL&)>(), &run_loop));
  run_loop.Run();

  EXPECT_FALSE(tester.ContainsEntry(RemoveCodeCacheTester::kWebAssembly,
                                    ResourceUrl(), origin));

  // Make sure there isn't a second invalid callback sitting in the queue.
  // (this used to be a bug).
  base::RunLoop().RunUntilIdle();
}

TEST_F(StoragePartitionImplTest, ClearCodeCacheIncognito) {
  browser_context()->set_is_off_the_record(true);

  StoragePartitionImpl* partition = static_cast<StoragePartitionImpl*>(
      BrowserContext::GetDefaultStoragePartition(browser_context()));
  base::RunLoop().RunUntilIdle();
  // We should not create GeneratedCodeCacheContext for off the record mode.
  EXPECT_EQ(nullptr, partition->GetGeneratedCodeCacheContext());

  base::RunLoop run_loop;
  // This shouldn't crash.
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::BindOnce(&ClearCodeCache, partition, base::Time(), base::Time(),
                     base::RepeatingCallback<bool(const GURL&)>(), &run_loop));
  run_loop.Run();
}

#if BUILDFLAG(ENABLE_PLUGINS)
TEST_F(StoragePartitionImplTest, RemovePluginPrivateDataForever) {
  StoragePartitionImpl* partition = static_cast<StoragePartitionImpl*>(
      BrowserContext::GetDefaultStoragePartition(browser_context()));

  RemovePluginPrivateDataTester tester(partition->GetFileSystemContext());
  tester.AddPluginPrivateTestData();
  EXPECT_TRUE(tester.DataExistsForOrigin(Origin1()));
  EXPECT_TRUE(tester.DataExistsForOrigin(Origin2()));

  base::RunLoop run_loop;
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(&ClearPluginPrivateData, partition, GURL(),
                                base::Time(), base::Time::Max(), &run_loop));
  run_loop.Run();

  EXPECT_FALSE(tester.DataExistsForOrigin(Origin1()));
  EXPECT_FALSE(tester.DataExistsForOrigin(Origin2()));
}

TEST_F(StoragePartitionImplTest, RemovePluginPrivateDataLastWeek) {
  StoragePartitionImpl* partition = static_cast<StoragePartitionImpl*>(
      BrowserContext::GetDefaultStoragePartition(browser_context()));
  base::Time a_week_ago = base::Time::Now() - base::TimeDelta::FromDays(7);

  RemovePluginPrivateDataTester tester(partition->GetFileSystemContext());
  tester.AddPluginPrivateTestData();
  EXPECT_TRUE(tester.DataExistsForOrigin(Origin1()));
  EXPECT_TRUE(tester.DataExistsForOrigin(Origin2()));

  base::RunLoop run_loop;
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(&ClearPluginPrivateData, partition, GURL(),
                                a_week_ago, base::Time::Max(), &run_loop));
  run_loop.Run();

  // Origin1 has 1 file from 10 days ago, so it should remain around.
  // Origin2 has a current file, so it should be removed (even though the
  // second file is much older).
  EXPECT_TRUE(tester.DataExistsForOrigin(Origin1()));
  EXPECT_FALSE(tester.DataExistsForOrigin(Origin2()));
}

TEST_F(StoragePartitionImplTest, RemovePluginPrivateDataForOrigin) {
  StoragePartitionImpl* partition = static_cast<StoragePartitionImpl*>(
      BrowserContext::GetDefaultStoragePartition(browser_context()));

  RemovePluginPrivateDataTester tester(partition->GetFileSystemContext());
  tester.AddPluginPrivateTestData();
  EXPECT_TRUE(tester.DataExistsForOrigin(Origin1()));
  EXPECT_TRUE(tester.DataExistsForOrigin(Origin2()));

  base::RunLoop run_loop;
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::BindOnce(&ClearPluginPrivateData, partition, Origin1().GetURL(),
                     base::Time(), base::Time::Max(), &run_loop));
  run_loop.Run();

  // Only Origin1 should be deleted.
  EXPECT_FALSE(tester.DataExistsForOrigin(Origin1()));
  EXPECT_TRUE(tester.DataExistsForOrigin(Origin2()));
}

TEST_F(StoragePartitionImplTest, RemovePluginPrivateDataAfterDeletion) {
  StoragePartitionImpl* partition = static_cast<StoragePartitionImpl*>(
      BrowserContext::GetDefaultStoragePartition(browser_context()));

  RemovePluginPrivateDataTester tester(partition->GetFileSystemContext());
  tester.AddPluginPrivateTestData();
  EXPECT_TRUE(tester.DataExistsForOrigin(Origin1()));
  EXPECT_TRUE(tester.DataExistsForOrigin(Origin2()));

  // Delete the single file saved for |Origin1()|. This does not remove the
  // origin from the list of Origins. However, ClearPluginPrivateData() will
  // remove it.
  tester.DeleteClearKeyTestData();
  EXPECT_TRUE(tester.DataExistsForOrigin(Origin1()));
  EXPECT_TRUE(tester.DataExistsForOrigin(Origin2()));

  base::RunLoop run_loop;
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(&ClearPluginPrivateData, partition, GURL(),
                                base::Time(), base::Time::Max(), &run_loop));
  run_loop.Run();

  EXPECT_FALSE(tester.DataExistsForOrigin(Origin1()));
  EXPECT_FALSE(tester.DataExistsForOrigin(Origin2()));
}
#endif  // BUILDFLAG(ENABLE_PLUGINS)

TEST(StoragePartitionImplStaticTest, CreatePredicateForHostCookies) {
  GURL url("http://www.example.com/");
  GURL url2("https://www.example.com/");
  GURL url3("https://www.google.com/");

  base::Optional<base::Time> server_time = base::nullopt;
  CookieDeletionFilterPtr deletion_filter = CookieDeletionFilter::New();
  deletion_filter->host_name = url.host();

  base::Time now = base::Time::Now();
  std::vector<std::unique_ptr<CanonicalCookie>> valid_cookies;
  valid_cookies.push_back(
      CanonicalCookie::Create(url, "A=B", now, server_time));
  valid_cookies.push_back(
      CanonicalCookie::Create(url, "C=F", now, server_time));
  // We should match a different scheme with the same host.
  valid_cookies.push_back(
      CanonicalCookie::Create(url2, "A=B", now, server_time));

  std::vector<std::unique_ptr<CanonicalCookie>> invalid_cookies;
  // We don't match domain cookies.
  invalid_cookies.push_back(CanonicalCookie::Create(
      url2, "A=B;domain=.example.com", now, server_time));
  invalid_cookies.push_back(
      CanonicalCookie::Create(url3, "A=B", now, server_time));

  for (const auto& cookie : valid_cookies) {
    EXPECT_TRUE(FilterMatchesCookie(deletion_filter, *cookie))
        << cookie->DebugString();
  }
  for (const auto& cookie : invalid_cookies) {
    EXPECT_FALSE(FilterMatchesCookie(deletion_filter, *cookie))
        << cookie->DebugString();
  }
}

}  // namespace content
