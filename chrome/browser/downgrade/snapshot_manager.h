// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_DOWNGRADE_SNAPSHOT_MANAGER_H_
#define CHROME_BROWSER_DOWNGRADE_SNAPSHOT_MANAGER_H_

#include <stdint.h>
#include <vector>

#include "base/files/file_path.h"
#include "base/version.h"

namespace downgrade {

struct SnapshotItemDetails;

// Class that handles saving snapshots of some user data after a browser
// upgrade, and loading the appropriate snapshot after a downgrade.
class SnapshotManager {
 public:
  // Instantiates a SnapshotManager that will handle taking a snapshot and
  // restoring snapshots for the data from |user_data_dir| for the current
  // chrome version.
  explicit SnapshotManager(const base::FilePath& user_data_dir);

  SnapshotManager(const SnapshotManager& other) = delete;
  SnapshotManager& operator=(const SnapshotManager&) = delete;

  ~SnapshotManager();

  // Copies specified files from |user_data_dir_| for |version| into the
  // appropriate snapshot directory.
  void TakeSnapshot(const base::Version& version);

  // Restores all the files from the highest available snapshot version that is
  // not greater than |version| to |user_data_dir_|. If the highest available
  // snapshot version is equal to |version|, the snapshot is moved, otherwise
  // it is copied. If the snapshot folder is empty after this operation, it is
  // moved for later deletion.
  void RestoreSnapshot(const base::Version& version);

  // Keeps the number of snapshots on the disk under |max_number_of_snapshots|
  // by moving invalid and older snapshots for later deletion.
  void PurgeInvalidAndOldSnapshots(int max_number_of_snapshots) const;

 private:
  virtual std::vector<SnapshotItemDetails> GetUserSnapshotItemDetails(
      uint16_t milestone) const;
  virtual std::vector<SnapshotItemDetails> GetProfileSnapshotItemDetails(
      uint16_t milestone) const;

  const base::FilePath user_data_dir_;
};

}  // namespace downgrade

#endif  // CHROME_BROWSER_DOWNGRADE_SNAPSHOT_MANAGER_H_
