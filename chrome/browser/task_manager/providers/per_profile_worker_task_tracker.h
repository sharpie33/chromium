// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_TASK_MANAGER_PROVIDERS_PER_PROFILE_WORKER_TASK_TRACKER_H_
#define CHROME_BROWSER_TASK_MANAGER_PROVIDERS_PER_PROFILE_WORKER_TASK_TRACKER_H_

#include <memory>

#include "base/containers/flat_map.h"
#include "base/scoped_observer.h"
#include "chrome/browser/task_manager/providers/task.h"
#include "content/public/browser/dedicated_worker_service.h"
#include "content/public/browser/service_worker_context.h"
#include "content/public/browser/service_worker_context_observer.h"
#include "content/public/browser/shared_worker_service.h"

class Profile;

namespace task_manager {

class WorkerTask;
class WorkerTaskProvider;

// This is a helper class owned by WorkerTaskProvider to track all workers
// associated with a single profile. It manages the WorkerTasks and sends
// lifetime notifications to the WorkerTaskProvider.
class PerProfileWorkerTaskTracker
    : public content::DedicatedWorkerService::Observer,
      public content::SharedWorkerService::Observer,
      public content::ServiceWorkerContextObserver {
 public:
  PerProfileWorkerTaskTracker(WorkerTaskProvider* worker_task_provider,
                              Profile* profile);

  ~PerProfileWorkerTaskTracker() override;

  PerProfileWorkerTaskTracker(const PerProfileWorkerTaskTracker&) = delete;
  PerProfileWorkerTaskTracker& operator=(const PerProfileWorkerTaskTracker&) =
      delete;

  // content::DedicatedWorkerService::Observer:
  void OnWorkerStarted(
      content::DedicatedWorkerId dedicated_worker_id,
      int worker_process_id,
      content::GlobalFrameRoutingId ancestor_render_frame_host_id) override;
  void OnBeforeWorkerTerminated(
      content::DedicatedWorkerId dedicated_worker_id,
      content::GlobalFrameRoutingId ancestor_render_frame_host_id) override;

  // content::SharedWorkerService::Observer:
  void OnWorkerStarted(const content::SharedWorkerInstance& instance,
                       int worker_process_id,
                       const base::UnguessableToken& dev_tools_token) override;
  void OnBeforeWorkerTerminated(
      const content::SharedWorkerInstance& instance) override;
  void OnClientAdded(
      const content::SharedWorkerInstance& instance,
      content::GlobalFrameRoutingId render_frame_host_id) override {}
  void OnClientRemoved(
      const content::SharedWorkerInstance& instance,
      content::GlobalFrameRoutingId render_frame_host_id) override {}

  // content::ServiceWorkerContextObserver:
  void OnVersionStartedRunning(
      int64_t version_id,
      const content::ServiceWorkerRunningInfo& running_info) override;
  void OnVersionStoppedRunning(int64_t version_id) override;

 private:
  // Creates a WorkerTask and inserts it into |out_worker_tasks|. Then it
  // notifies the |worker_task_provider_| about the new Task.
  //
  // Note that this function is templated because each worker type uses a
  // different type as its ID.
  template <typename WorkerId>
  void CreateWorkerTask(
      const WorkerId& worker_id,
      Task::Type task_type,
      int worker_process_id,
      const GURL& script_url,
      base::flat_map<WorkerId, std::unique_ptr<WorkerTask>>* out_worker_tasks);

  // Deletes an existing WorkerTask from |out_worker_tasks| and notifies
  // |worker_task_provider_| about the deletion of the task.
  template <typename WorkerId>
  void DeleteWorkerTask(
      const WorkerId& worker_id,
      base::flat_map<WorkerId, std::unique_ptr<WorkerTask>>* out_worker_tasks);

  // The provider that gets notified when a WorkerTask is created/deleted.
  WorkerTaskProvider* const worker_task_provider_;  // Owner.

  // For dedicated workers:
  ScopedObserver<content::DedicatedWorkerService,
                 content::DedicatedWorkerService::Observer>
      scoped_dedicated_worker_service_observer_{this};

  base::flat_map<content::DedicatedWorkerId, std::unique_ptr<WorkerTask>>
      dedicated_worker_tasks_;

  // For shared workers:
  ScopedObserver<content::SharedWorkerService,
                 content::SharedWorkerService::Observer>
      scoped_shared_worker_service_observer_{this};

  base::flat_map<content::SharedWorkerInstance, std::unique_ptr<WorkerTask>>
      shared_worker_tasks_;

  // For service workers:
  ScopedObserver<content::ServiceWorkerContext,
                 content::ServiceWorkerContextObserver>
      scoped_service_worker_context_observer_{this};

  base::flat_map<int64_t /*version_id*/, std::unique_ptr<WorkerTask>>
      service_worker_tasks_;
};

}  // namespace task_manager

#endif  // CHROME_BROWSER_TASK_MANAGER_PROVIDERS_PER_PROFILE_WORKER_TASK_TRACKER_H_
