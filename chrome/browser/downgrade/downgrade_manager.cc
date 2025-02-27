// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/downgrade/downgrade_manager.h"

#include <algorithm>
#include <iterator>
#include <utility>

#include "base/bind.h"
#include "base/callback.h"
#include "base/command_line.h"
#include "base/enterprise_util.h"
#include "base/feature_list.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/metrics/histogram_functions.h"
#include "base/optional.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/syslog_logging.h"
#include "base/task/post_task.h"
#include "base/version.h"
#include "build/build_config.h"
#include "chrome/browser/browser_features.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/downgrade/downgrade_utils.h"
#include "chrome/browser/downgrade/snapshot_manager.h"
#include "chrome/browser/downgrade/user_data_downgrade.h"
#include "chrome/browser/policy/browser_dm_token_storage.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"
#include "components/version_info/version_info.h"
#include "components/version_info/version_info_values.h"
#include "content/public/browser/browser_thread.h"

#if defined(OS_WIN)
#include "chrome/installer/util/install_util.h"
#endif

namespace downgrade {

namespace {

// Moves the contents of a User Data directory at |source| to |target|, with the
// exception of files/directories that should be left behind for a full data
// wipe. Returns no value if the target directory could not be created, or the
// number of items that could not be moved.
base::Optional<int> MoveUserData(const base::FilePath& source,
                                 const base::FilePath& target) {
  // Returns true to exclude a file.
  auto exclusion_predicate =
      base::BindRepeating([](const base::FilePath& name) -> bool {
        // TODO(ydago): Share constants instead of hardcoding values here.
        static constexpr base::FilePath::StringPieceType kFilesToKeep[] = {
            FILE_PATH_LITERAL("browsermetrics"),
            FILE_PATH_LITERAL("crashpad"),
            FILE_PATH_LITERAL("first run"),
            FILE_PATH_LITERAL("last version"),
            FILE_PATH_LITERAL("lockfile"),
            FILE_PATH_LITERAL("snapshots"),
            FILE_PATH_LITERAL("stability"),
        };
        // Don't try to move the dir into which everything is being moved.
        if (name.FinalExtension() == kDowngradeDeleteSuffix)
          return true;
        return std::find_if(std::begin(kFilesToKeep), std::end(kFilesToKeep),
                            [&name](const auto& keep) {
                              return base::EqualsCaseInsensitiveASCII(
                                  name.value(), keep);
                            }) != std::end(kFilesToKeep);
      });
  auto result = MoveContents(source, target, std::move(exclusion_predicate));

  // Move the Last Version file last so that any crash before this point results
  // in a retry on the next launch.
  if (!result ||
      !MoveWithoutFallback(source.Append(kDowngradeLastVersionFile),
                           target.Append(kDowngradeLastVersionFile))) {
    if (result)
      *result += 1;
    // Attempt to delete Last Version if all else failed so that Chrome does not
    // continually attempt to perform a migration.
    base::DeleteFile(source.Append(kDowngradeLastVersionFile),
                     false /* recursive */);
    // Inform system administrators that things have gone awry.
    SYSLOG(ERROR) << "Failed to perform User Data migration following a Chrome "
                     "version downgrade. Chrome will run with User Data from a "
                     "higher version and may behave unpredictably.";
    // At this point, Chrome will relaunch with --user-data-migrated. This
    // switch suppresses downgrade processing, so that launch will go through
    // normal startup.
  }
  return result;
}

// Renames |disk_cache_dir| in its containing folder. If that fails, an attempt
// is made to move its contents.
void MoveCache(const base::FilePath& disk_cache_dir) {
  // A cache dir at the root of a volume is not supported.
  const base::FilePath parent = disk_cache_dir.DirName();
  if (parent == disk_cache_dir)
    return;

  // Move the cache within its parent directory from, for example, CacheDir
  // to CacheDir.CHROME_DELETE.
  const base::FilePath target =
      GetTempDirNameForDelete(parent, disk_cache_dir.BaseName());

  // The cache dir should have no files in use, so a simple move should suffice.
  const bool move_result = MoveWithoutFallback(disk_cache_dir, target);
  base::UmaHistogramBoolean("Downgrade.CacheDirMove.Result", move_result);
  if (move_result)
    return;

  // The directory couldn't be moved whole-hog. Attempt a recursive move of its
  // contents.
  auto failure_count =
      MoveContents(disk_cache_dir, target, ExclusionPredicate());
  if (!failure_count || *failure_count) {
    // Report precise values rather than an exponentially bucketed histogram.
    // Bucket 0 means that the target directory could not be created. All other
    // buckets are a count of files/directories left behind.
    base::UmaHistogramExactLinear("Downgrade.CacheDirMove.FailureCount",
                                  failure_count.value_or(0), 50);
  }
}

// Deletes all subdirectories in |dir| named |name|*.CHROME_DELETE.
void DeleteAllRenamedUserDirectories(const base::FilePath& dir,
                                     const base::FilePath& name) {
  base::FilePath::StringType pattern = base::StrCat(
      {name.value(), FILE_PATH_LITERAL("*"), kDowngradeDeleteSuffix});
  base::FileEnumerator enumerator(dir, false, base::FileEnumerator::DIRECTORIES,
                                  pattern);
  for (base::FilePath to_delete = enumerator.Next(); !to_delete.empty();
       to_delete = enumerator.Next()) {
    base::DeleteFileRecursively(to_delete);
  }
}

// Deletes all moved User Data, Snapshots and Cache directories for the given
// dirs.
void DeleteMovedUserData(const base::FilePath& user_data_dir,
                         const base::FilePath& disk_cache_dir) {
  DeleteAllRenamedUserDirectories(user_data_dir, user_data_dir.BaseName());
  DeleteAllRenamedUserDirectories(user_data_dir, base::FilePath(kSnapshotsDir));

  // Prior to Chrome M78, User Data was moved to a new name under its parent. In
  // that case, User Data at a volume's root was unsupported.
  base::FilePath parent = user_data_dir.DirName();
  if (parent != user_data_dir)
    DeleteAllRenamedUserDirectories(parent, user_data_dir.BaseName());

  if (!disk_cache_dir.empty()) {
    // Cache dir at a volume's root is unsupported.
    parent = disk_cache_dir.DirName();
    if (parent != disk_cache_dir)
      DeleteAllRenamedUserDirectories(parent, disk_cache_dir.BaseName());
  }
}

bool UserDataSnapshotEnabled() {
  bool is_enterprise_managed =
      policy::BrowserDMTokenStorage::Get()->RetrieveDMToken().is_valid();
#if defined(OS_WIN) || defined(OS_MACOSX)
  is_enterprise_managed |= base::IsMachineExternallyManaged();
#endif
  return is_enterprise_managed &&
         base::FeatureList::IsEnabled(features::kUserDataSnapshot);
}

#if defined(OS_WIN)
bool IsAdministratorDrivenDowngrade(uint16_t current_milestone) {
  const auto downgrade_version = InstallUtil::GetDowngradeVersion();
  return downgrade_version &&
         downgrade_version->components()[0] > current_milestone;
}
#endif

}  // namespace

bool DowngradeManager::PrepareUserDataDirectoryForCurrentVersion(
    const base::FilePath& user_data_dir) {
  DCHECK_EQ(type_, Type::kNone);
  DCHECK(!user_data_dir.empty());

  // Do not attempt migration if this process is the product of a relaunch from
  // a previous in which migration was attempted/performed.
  auto& command_line = *base::CommandLine::ForCurrentProcess();
  if (command_line.HasSwitch(switches::kUserDataMigrated)) {
    // Strip the switch from the command line so that it does not propagate to
    // any subsequent relaunches.
    command_line.RemoveSwitch(switches::kUserDataMigrated);
    return false;
  }

  base::Optional<base::Version> last_version = GetLastVersion(user_data_dir);
  if (!last_version)
    return false;

  const base::Version& current_version = version_info::GetVersion();

  const bool user_data_snapshot_enabled = UserDataSnapshotEnabled();

  if (!user_data_snapshot_enabled) {
    if (current_version >= *last_version)
      return false;  // Same version or upgrade.

    type_ = GetDowngradeType(user_data_dir, current_version, *last_version);
    DCHECK(type_ == Type::kAdministrativeWipe || type_ == Type::kUnsupported);
    base::UmaHistogramEnumeration("Downgrade.Type", type_);
    return type_ == Type::kAdministrativeWipe;
  }

  auto current_milestone = current_version.components()[0];
  auto last_milestone = last_version->components()[0];

  // Take a snapshot on the first launch after a major version jump.
  if (current_milestone > last_milestone) {
    const int max_number_of_snapshots =
        g_browser_process->local_state()->GetInteger(
            prefs::kUserDataSnapshotRentionLimit);
    SnapshotManager snapshot_manager(user_data_dir);
    if (max_number_of_snapshots > 0)
      snapshot_manager.TakeSnapshot(*last_version);
    snapshot_manager.PurgeInvalidAndOldSnapshots(max_number_of_snapshots);
    return false;
  }

  if (current_version >= *last_version)
    return false;  // Same version or mid-milestone upgrade.

  type_ = GetDowngradeTypeWithSnapshot(user_data_dir, current_version,
                                       *last_version);
  if (type_ != Type::kNone)
    base::UmaHistogramEnumeration("Downgrade.Type", type_);

  return type_ == Type::kAdministrativeWipe || type_ == Type::kSnapshotRestore;
}

void DowngradeManager::UpdateLastVersion(const base::FilePath& user_data_dir) {
  DCHECK(!user_data_dir.empty());
  DCHECK_NE(type_, Type::kAdministrativeWipe);
  const base::StringPiece version(PRODUCT_VERSION);
  base::WriteFile(GetLastVersionFile(user_data_dir), version.data(),
                  version.size());
}

void DowngradeManager::DeleteMovedUserDataSoon(
    const base::FilePath& user_data_dir) {
  DCHECK(!user_data_dir.empty());
  // IWYU note: base/location.h and base/task/task_traits.h are guaranteed to be
  // available via base/task/post_task.h.
  content::BrowserThread::PostBestEffortTask(
      FROM_HERE,
      base::CreateTaskRunner(
          {base::ThreadPool(), base::MayBlock(),
           base::TaskPriority::BEST_EFFORT,
           base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN}),
      base::BindOnce(&DeleteMovedUserData, user_data_dir, GetDiskCacheDir()));
}

void DowngradeManager::ProcessDowngrade(const base::FilePath& user_data_dir) {
  DCHECK(type_ == Type::kAdministrativeWipe || type_ == Type::kSnapshotRestore);
  DCHECK(!user_data_dir.empty());

  const base::FilePath disk_cache_dir(GetDiskCacheDir());
  if (!disk_cache_dir.empty())
    MoveCache(disk_cache_dir);

  // User Data requires special treatment, as certain files/directories should
  // be left behind. Furthermore, User Data is moved to a new directory within
  // itself (for example, to User Data/User Data.CHROME_DELETE) to guarantee
  // that the movement isn't across volumes.
  const auto failure_count = MoveUserData(
      user_data_dir,
      GetTempDirNameForDelete(user_data_dir, user_data_dir.BaseName()));
  enum class UserDataMoveResult {
    kCreateTargetFailure = 0,
    kSuccess = 1,
    kPartialSuccess = 2,
    kMaxValue = kPartialSuccess
  };
  UserDataMoveResult move_result =
      !failure_count ? UserDataMoveResult::kCreateTargetFailure
                     : (*failure_count ? UserDataMoveResult::kPartialSuccess
                                       : UserDataMoveResult::kSuccess);
  base::UmaHistogramEnumeration("Downgrade.UserDataDirMove.Result",
                                move_result);
  if (failure_count && *failure_count) {
    // Report precise values rather than an exponentially bucketed histogram.
    base::UmaHistogramExactLinear("Downgrade.UserDataDirMove.FailureCount",
                                  *failure_count, 50);
  }

  if (type_ == Type::kSnapshotRestore) {
    SnapshotManager snapshot_manager(user_data_dir);
    snapshot_manager.RestoreSnapshot(version_info::GetVersion());
  }

  // Add the migration switch to the command line so that it is propagated to
  // the relaunched process. This is used to prevent a relaunch bomb in case of
  // pathological failure.
  base::CommandLine::ForCurrentProcess()->AppendSwitch(
      switches::kUserDataMigrated);
}

// static
DowngradeManager::Type DowngradeManager::GetDowngradeType(
    const base::FilePath& user_data_dir,
    const base::Version& current_version,
    const base::Version& last_version) {
  DCHECK(!user_data_dir.empty());
  DCHECK_LT(current_version, last_version);

#if defined(OS_WIN)
    // Move User Data aside for a clean launch if it follows an
    // administrator-driven downgrade.
  if (IsAdministratorDrivenDowngrade(current_version.components()[0]))
    return Type::kAdministrativeWipe;
#endif
    return Type::kUnsupported;
}

// static
DowngradeManager::Type DowngradeManager::GetDowngradeTypeWithSnapshot(
    const base::FilePath& user_data_dir,
    const base::Version& current_version,
    const base::Version& last_version) {
  DCHECK(!user_data_dir.empty());
  DCHECK_LT(current_version, last_version);

  const uint16_t milestone = current_version.components()[0];

  // Move User Data and restore from a snapshot if there is a candidate
  // snapshot to restore.
  const auto snapshot_to_restore =
      GetSnapshotToRestore(current_version, user_data_dir);

#if defined(OS_WIN)
  // Move User Data aside for a clean launch if it follows an
  // administrator-driven downgrade when no snapshot is found.
  if (!snapshot_to_restore && IsAdministratorDrivenDowngrade(milestone))
    return Type::kAdministrativeWipe;
#endif

  const uint16_t last_milestone = last_version.components()[0];
  if (last_milestone > milestone)
    return snapshot_to_restore ? Type::kSnapshotRestore : Type::kUnsupported;

  return Type::kMinorDowngrade;
}

}  // namespace downgrade
