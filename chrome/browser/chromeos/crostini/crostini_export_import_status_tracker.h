// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_CHROMEOS_CROSTINI_CROSTINI_EXPORT_IMPORT_STATUS_TRACKER_H_
#define CHROME_BROWSER_CHROMEOS_CROSTINI_CROSTINI_EXPORT_IMPORT_STATUS_TRACKER_H_

#include <memory>
#include <string>

#include "base/files/file_path.h"
#include "base/strings/string16.h"

namespace crostini {

enum class ExportImportType;

// CrostiniExportImportStatusTracker handles communication between the
// CrostiniExportImport operation and ui elements that reflect progress.
class CrostiniExportImportStatusTracker {
 public:
  enum class Status {
    RUNNING,
    CANCELLING,
    DONE,
    CANCELLED,
    FAILED_UNKNOWN_REASON,
    FAILED_ARCHITECTURE_MISMATCH,
    FAILED_INSUFFICIENT_SPACE,
    FAILED_CONCURRENT_OPERATION,
  };

  CrostiniExportImportStatusTracker(ExportImportType type, base::FilePath path);
  virtual ~CrostiniExportImportStatusTracker();

  Status status() const { return status_; }
  ExportImportType type() const { return type_; }
  const base::FilePath& path() const { return path_; }

  // Can be used to draw attention to the UI without changing its
  // status, even if it has been hidden.
  virtual void ForceRedisplay() {}
  virtual void SetStatusRunningUI(int progress_percent) = 0;
  virtual void SetStatusCancellingUI() = 0;
  virtual void SetStatusDoneUI() = 0;
  virtual void SetStatusCancelledUI() = 0;
  virtual void SetStatusFailedWithMessageUI(Status status,
                                            const base::string16& message) = 0;

  void SetStatusRunning(int progress_percent);
  void SetStatusCancelling();
  void SetStatusDone();
  void SetStatusCancelled();

  void SetStatusFailed();
  void SetStatusFailedArchitectureMismatch(
      const std::string& architecture_container,
      const std::string& architecture_device);
  void SetStatusFailedInsufficientSpace(uint64_t additional_required_space);
  void SetStatusFailedConcurrentOperation(
      ExportImportType in_progress_operation_type);
 private:
  void SetStatusFailedWithMessage(Status status, const base::string16& message);

  ExportImportType type_;
  base::FilePath path_;
  Status status_ = Status::RUNNING;
};

}  // namespace crostini

#endif  // CHROME_BROWSER_CHROMEOS_CROSTINI_CROSTINI_EXPORT_IMPORT_STATUS_TRACKER_H_
