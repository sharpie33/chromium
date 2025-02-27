// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/dom_storage/dom_storage_context_wrapper.h"

#include <string>
#include <vector>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/location.h"
#include "base/memory/weak_ptr.h"
#include "base/sequenced_task_runner.h"
#include "base/single_thread_task_runner.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/post_task.h"
#include "base/threading/thread_task_runner_handle.h"
#include "build/build_config.h"
#include "components/services/storage/dom_storage/local_storage_impl.h"
#include "components/services/storage/public/cpp/constants.h"
#include "content/browser/dom_storage/session_storage_context_mojo.h"
#include "content/browser/dom_storage/session_storage_namespace_impl.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/browser/session_storage_usage_info.h"
#include "content/public/browser/storage_usage_info.h"
#include "content/public/common/content_client.h"
#include "content/public/common/content_features.h"
#include "content/public/common/content_switches.h"
#include "storage/browser/quota/special_storage_policy.h"
#include "third_party/blink/public/common/features.h"
#include "url/origin.h"

namespace content {
namespace {

const char kSessionStorageDirectory[] = "Session Storage";

void GotMojoSessionStorageUsage(
    scoped_refptr<base::SingleThreadTaskRunner> reply_task_runner,
    DOMStorageContext::GetSessionStorageUsageCallback callback,
    std::vector<SessionStorageUsageInfo> usage) {
  reply_task_runner->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), std::move(usage)));
}

void AdaptUsageInfo(
    DOMStorageContext::GetLocalStorageUsageCallback callback,
    std::vector<storage::mojom::LocalStorageUsageInfoPtr> usage) {
  std::vector<StorageUsageInfo> result;
  result.reserve(usage.size());
  for (const auto& info : usage) {
    result.emplace_back(info->origin, info->size_in_bytes,
                        info->last_modified_time);
  }
  std::move(callback).Run(result);
}

}  // namespace

class DOMStorageContextWrapper::StoragePolicyObserver
    : public storage::SpecialStoragePolicy::Observer {
 public:
  explicit StoragePolicyObserver(
      scoped_refptr<storage::SpecialStoragePolicy> storage_policy,
      scoped_refptr<DOMStorageContextWrapper> context_wrapper)
      : storage_policy_(std::move(storage_policy)),
        context_wrapper_(std::move(context_wrapper)) {
    storage_policy_->AddObserver(this);
  }

  StoragePolicyObserver(const StoragePolicyObserver&) = delete;
  StoragePolicyObserver& operator=(const StoragePolicyObserver&) = delete;

  ~StoragePolicyObserver() override {
    DCHECK(!context_wrapper_);
    storage_policy_->RemoveObserver(this);
  }

  void DidShutdownContextWrapper() { context_wrapper_.reset(); }

 private:
  // storage::SpecialStoragePolicy::Observer:
  void OnPolicyChanged() override {
    if (!context_wrapper_)
      return;

    base::PostTask(
        FROM_HERE, {BrowserThread::UI},
        base::BindOnce(&DOMStorageContextWrapper::OnStoragePolicyChanged,
                       context_wrapper_));
  }

  const scoped_refptr<storage::SpecialStoragePolicy> storage_policy_;
  scoped_refptr<DOMStorageContextWrapper> context_wrapper_;
};

scoped_refptr<DOMStorageContextWrapper> DOMStorageContextWrapper::Create(
    const base::FilePath& profile_path,
    const base::FilePath& local_partition_path,
    storage::SpecialStoragePolicy* special_storage_policy) {
  base::FilePath data_path;
  if (!profile_path.empty())
    data_path = profile_path.Append(local_partition_path);

  auto mojo_task_runner =
      base::CreateSingleThreadTaskRunner({BrowserThread::IO});

  // TODO(https://crbug.com/1000959): This should be bound in an instance of
  // the Storage Service. For now we bind it alone on the IO thread because
  // that's where the implementation has effectively lived for some time.
  mojo::Remote<storage::mojom::LocalStorageControl> local_storage_control;
  mojo_task_runner->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](const base::FilePath& storage_root,
             mojo::PendingReceiver<storage::mojom::LocalStorageControl>
                 receiver) {
            // Deletes itself on shutdown completion.
            new storage::LocalStorageImpl(
                storage_root,
                base::CreateSingleThreadTaskRunner({BrowserThread::IO}),
                base::CreateSequencedTaskRunner(
                    {base::ThreadPool(), base::MayBlock(),
                     base::TaskPriority::USER_BLOCKING,
                     base::TaskShutdownBehavior::BLOCK_SHUTDOWN}),
                std::move(receiver));
          },
          data_path, local_storage_control.BindNewPipeAndPassReceiver()));

  SessionStorageContextMojo* mojo_session_state = nullptr;
  mojo_session_state = new SessionStorageContextMojo(
      data_path,
      base::CreateSequencedTaskRunner(
          {base::MayBlock(), base::ThreadPool(),
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN}),
      mojo_task_runner,
#if defined(OS_ANDROID)
      // On Android there is no support for session storage restoring, and
      // since the restoring code is responsible for database cleanup, we must
      // manually delete the old database here before we open it.
      SessionStorageContextMojo::BackingMode::kClearDiskStateOnOpen,
#else
      profile_path.empty()
          ? SessionStorageContextMojo::BackingMode::kNoDisk
          : SessionStorageContextMojo::BackingMode::kRestoreDiskState,
#endif
      std::string(kSessionStorageDirectory));

  auto wrapper = base::WrapRefCounted(new DOMStorageContextWrapper(
      mojo_task_runner, mojo_session_state, std::move(local_storage_control),
      special_storage_policy));

  if (special_storage_policy) {
    // If there's a SpecialStoragePolicy, ensure the wrapper is observing it on
    // the IO thread and query the initial set of in-use origins ASAP.
    wrapper->storage_policy_observer_ =
        base::SequenceBound<StoragePolicyObserver>(
            base::CreateSequencedTaskRunner(BrowserThread::IO),
            base::WrapRefCounted(special_storage_policy), wrapper);

    wrapper->local_storage_control_->GetUsage(base::BindOnce(
        &DOMStorageContextWrapper::OnStartupUsageRetrieved, wrapper));
  }

  return wrapper;
}

DOMStorageContextWrapper::DOMStorageContextWrapper(
    scoped_refptr<base::SequencedTaskRunner> mojo_task_runner,
    SessionStorageContextMojo* mojo_session_storage_context,
    mojo::Remote<storage::mojom::LocalStorageControl> local_storage_control,
    storage::SpecialStoragePolicy* special_storage_policy)
    : mojo_session_state_(mojo_session_storage_context),
      mojo_task_runner_(std::move(mojo_task_runner)),
      local_storage_control_(std::move(local_storage_control)),
      storage_policy_(special_storage_policy) {
  memory_pressure_listener_.reset(new base::MemoryPressureListener(
      base::BindRepeating(&DOMStorageContextWrapper::OnMemoryPressure,
                          base::Unretained(this))));
}

DOMStorageContextWrapper::~DOMStorageContextWrapper() {
  DCHECK(!local_storage_control_)
      << "Shutdown should be called before destruction";
}

storage::mojom::LocalStorageControl*
DOMStorageContextWrapper::GetLocalStorageControl() {
  DCHECK(local_storage_control_);
  return local_storage_control_.get();
}

void DOMStorageContextWrapper::GetLocalStorageUsage(
    GetLocalStorageUsageCallback callback) {
  if (!local_storage_control_) {
    // Shutdown() has been called.
    std::move(callback).Run(std::vector<StorageUsageInfo>());
    return;
  }

  local_storage_control_->GetUsage(
      base::BindOnce(&AdaptUsageInfo, std::move(callback)));
}

void DOMStorageContextWrapper::GetSessionStorageUsage(
    GetSessionStorageUsageCallback callback) {
  if (!mojo_session_state_) {
    // Shutdown() has been called.
    std::move(callback).Run(std::vector<SessionStorageUsageInfo>());
    return;
  }
  // base::Unretained is safe here, because the mojo_session_state_ won't be
  // deleted until a ShutdownAndDelete task has been ran on the
  // mojo_task_runner_, and as soon as that task is posted,
  // mojo_session_state_ is set to null, preventing further tasks from being
  // queued.
  mojo_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&SessionStorageContextMojo::GetStorageUsage,
                     base::Unretained(mojo_session_state_),
                     base::BindOnce(&GotMojoSessionStorageUsage,
                                    base::ThreadTaskRunnerHandle::Get(),
                                    std::move(callback))));
}

void DOMStorageContextWrapper::DeleteLocalStorage(const url::Origin& origin,
                                                  base::OnceClosure callback) {
  DCHECK(callback);
  if (!local_storage_control_) {
    // Shutdown() has been called.
    std::move(callback).Run();
    return;
  }

  local_storage_control_->DeleteStorage(origin, std::move(callback));
}

void DOMStorageContextWrapper::PerformLocalStorageCleanup(
    base::OnceClosure callback) {
  DCHECK(callback);
  if (!local_storage_control_) {
    // Shutdown() has been called.
    std::move(callback).Run();
    return;
  }

  local_storage_control_->CleanUpStorage(std::move(callback));
}

void DOMStorageContextWrapper::DeleteSessionStorage(
    const SessionStorageUsageInfo& usage_info,
    base::OnceClosure callback) {
  if (!mojo_session_state_) {
    // Shutdown() has been called.
    std::move(callback).Run();
  }
  // base::Unretained is safe here, because the mojo_session_state_ won't be
  // deleted until a ShutdownAndDelete task has been ran on the
  // mojo_task_runner_, and as soon as that task is posted,
  // mojo_session_state_ is set to null, preventing further tasks from being
  // queued.
  mojo_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&SessionStorageContextMojo::DeleteStorage,
                                base::Unretained(mojo_session_state_),
                                url::Origin::Create(usage_info.origin),
                                usage_info.namespace_id, std::move(callback)));
}

void DOMStorageContextWrapper::PerformSessionStorageCleanup(
    base::OnceClosure callback) {
  DCHECK(callback);
  if (!mojo_session_state_) {
    // Shutdown() has been called.
    std::move(callback).Run();
  }
  // base::Unretained is safe here, because the mojo_session_state_ won't be
  // deleted until a ShutdownAndDelete task has been ran on the
  // mojo_task_runner_, and as soon as that task is posted,
  // mojo_session_state_ is set to null, preventing further tasks from being
  // queued.
  mojo_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&SessionStorageContextMojo::PerformStorageCleanup,
                     base::Unretained(mojo_session_state_),
                     std::move(callback)));
}

scoped_refptr<SessionStorageNamespace>
DOMStorageContextWrapper::RecreateSessionStorage(
    const std::string& namespace_id) {
  return SessionStorageNamespaceImpl::Create(this, namespace_id);
}

void DOMStorageContextWrapper::StartScavengingUnusedSessionStorage() {
  if (!mojo_session_state_) {
    // Shutdown() has been called.
    return;
  }
  // base::Unretained is safe here, because the mojo_session_state_ won't be
  // deleted until a ShutdownAndDelete task has been ran on the
  // mojo_task_runner_, and as soon as that task is posted,
  // mojo_session_state_ is set to null, preventing further tasks from being
  // queued.
  mojo_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&SessionStorageContextMojo::ScavengeUnusedNamespaces,
                     base::Unretained(mojo_session_state_),
                     base::OnceClosure()));
}

void DOMStorageContextWrapper::SetForceKeepSessionState() {
  if (!local_storage_control_) {
    // Shutdown() has been called.
    return;
  }

  local_storage_control_->ForceKeepSessionState();
}

void DOMStorageContextWrapper::Shutdown() {
  // Signals the implementation to perform shutdown operations.
  local_storage_control_.reset();

  if (mojo_session_state_) {
    mojo_task_runner_->PostTask(
        FROM_HERE, base::BindOnce(&SessionStorageContextMojo::ShutdownAndDelete,
                                  base::Unretained(mojo_session_state_)));
    mojo_session_state_ = nullptr;
  }
  memory_pressure_listener_.reset();

  if (storage_policy_observer_) {
    // Make sure the observer drops its reference to |this|.
    storage_policy_observer_.Post(
        FROM_HERE, &StoragePolicyObserver::DidShutdownContextWrapper);
  }
}

void DOMStorageContextWrapper::Flush() {
  if (local_storage_control_)
    local_storage_control_->Flush(base::NullCallback());

  if (mojo_session_state_) {
    // base::Unretained is safe here, because the mojo_session_state_ won't be
    // deleted until a ShutdownAndDelete task has been ran on the
    // mojo_task_runner_, and as soon as that task is posted,
    // mojo_session_state_ is set to null, preventing further tasks from being
    // queued.
    mojo_task_runner_->PostTask(
        FROM_HERE, base::BindOnce(&SessionStorageContextMojo::Flush,
                                  base::Unretained(mojo_session_state_)));
  }
}

void DOMStorageContextWrapper::OpenLocalStorage(
    const url::Origin& origin,
    mojo::PendingReceiver<blink::mojom::StorageArea> receiver) {
  DCHECK(local_storage_control_);
  local_storage_control_->BindStorageArea(origin, std::move(receiver));
  if (storage_policy_) {
    EnsureLocalStorageOriginIsTracked(origin);
    OnStoragePolicyChanged();
  }
}

void DOMStorageContextWrapper::OpenSessionStorage(
    int process_id,
    const std::string& namespace_id,
    mojo::ReportBadMessageCallback bad_message_callback,
    mojo::PendingReceiver<blink::mojom::SessionStorageNamespace> receiver) {
  DCHECK(mojo_session_state_);
  // The bad message callback must be called on the same sequenced task runner
  // as the binding set. It cannot be called from our own mojo task runner.
  auto wrapped_bad_message_callback = base::BindOnce(
      [](mojo::ReportBadMessageCallback bad_message_callback,
         scoped_refptr<base::SequencedTaskRunner> bindings_runner,
         const std::string& error) {
        bindings_runner->PostTask(
            FROM_HERE, base::BindOnce(std::move(bad_message_callback), error));
      },
      std::move(bad_message_callback), base::SequencedTaskRunnerHandle::Get());
  // base::Unretained is safe here, because the mojo_state_ won't be deleted
  // until a ShutdownAndDelete task has been ran on the mojo_task_runner_, and
  // as soon as that task is posted, mojo_state_ is set to null, preventing
  // further tasks from being queued.
  mojo_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&SessionStorageContextMojo::OpenSessionStorage,
                     base::Unretained(mojo_session_state_), process_id,
                     namespace_id, std::move(wrapped_bad_message_callback),
                     std::move(receiver)));
}

scoped_refptr<SessionStorageNamespaceImpl>
DOMStorageContextWrapper::MaybeGetExistingNamespace(
    const std::string& namespace_id) const {
  base::AutoLock lock(alive_namespaces_lock_);
  auto it = alive_namespaces_.find(namespace_id);
  return (it != alive_namespaces_.end()) ? it->second : nullptr;
}

void DOMStorageContextWrapper::AddNamespace(
    const std::string& namespace_id,
    SessionStorageNamespaceImpl* session_namespace) {
  base::AutoLock lock(alive_namespaces_lock_);
  DCHECK(alive_namespaces_.find(namespace_id) == alive_namespaces_.end());
  alive_namespaces_[namespace_id] = session_namespace;
}

void DOMStorageContextWrapper::RemoveNamespace(
    const std::string& namespace_id) {
  base::AutoLock lock(alive_namespaces_lock_);
  DCHECK(alive_namespaces_.find(namespace_id) != alive_namespaces_.end());
  alive_namespaces_.erase(namespace_id);
}

void DOMStorageContextWrapper::OnMemoryPressure(
    base::MemoryPressureListener::MemoryPressureLevel memory_pressure_level) {
  PurgeOption purge_option = PURGE_UNOPENED;
  if (memory_pressure_level ==
      base::MemoryPressureListener::MEMORY_PRESSURE_LEVEL_CRITICAL) {
    purge_option = PURGE_AGGRESSIVE;
  }
  PurgeMemory(purge_option);
}

void DOMStorageContextWrapper::PurgeMemory(PurgeOption purge_option) {
  if (!local_storage_control_) {
    // Shutdown was called.
    return;
  }

  if (purge_option == PURGE_AGGRESSIVE) {
    local_storage_control_->PurgeMemory();

    // base::Unretained is safe here, because the mojo_session_state_ won't be
    // deleted until a ShutdownAndDelete task has been ran on the
    // mojo_task_runner_, and as soon as that task is posted,
    // mojo_session_state_ is set to null, preventing further tasks from being
    // queued.
    mojo_task_runner_->PostTask(
        FROM_HERE, base::BindOnce(&SessionStorageContextMojo::PurgeMemory,
                                  base::Unretained(mojo_session_state_)));
  }
}

void DOMStorageContextWrapper::OnStartupUsageRetrieved(
    std::vector<storage::mojom::LocalStorageUsageInfoPtr> usage) {
  for (const auto& info : usage)
    EnsureLocalStorageOriginIsTracked(info->origin);
  OnStoragePolicyChanged();
}

void DOMStorageContextWrapper::EnsureLocalStorageOriginIsTracked(
    const url::Origin& origin) {
  DCHECK(storage_policy_);
  auto it = local_storage_origins_.find(origin);
  if (it == local_storage_origins_.end())
    local_storage_origins_[origin] = {};
}

void DOMStorageContextWrapper::OnStoragePolicyChanged() {
  if (!local_storage_control_)
    return;

  // Scan for any relevant changes to policy regarding origins we know we're
  // managing.
  std::vector<storage::mojom::LocalStoragePolicyUpdatePtr> policy_updates;
  for (auto& entry : local_storage_origins_) {
    const url::Origin& origin = entry.first;
    LocalStorageOriginState& state = entry.second;
    state.should_purge_on_shutdown = ShouldPurgeLocalStorageOnShutdown(origin);
    if (state.should_purge_on_shutdown != state.will_purge_on_shutdown) {
      state.will_purge_on_shutdown = state.should_purge_on_shutdown;
      policy_updates.push_back(storage::mojom::LocalStoragePolicyUpdate::New(
          origin, state.should_purge_on_shutdown));
    }
  }

  if (!policy_updates.empty())
    local_storage_control_->ApplyPolicyUpdates(std::move(policy_updates));
}

bool DOMStorageContextWrapper::ShouldPurgeLocalStorageOnShutdown(
    const url::Origin& origin) {
  if (!storage_policy_)
    return false;
  return storage_policy_->IsStorageSessionOnly(origin.GetURL()) &&
         !storage_policy_->IsStorageProtected(origin.GetURL());
}

}  // namespace content
