// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/media/history/media_history_origin_table.h"

#include "base/strings/stringprintf.h"
#include "sql/statement.h"

namespace media_history {

const char MediaHistoryOriginTable::kTableName[] = "origin";

MediaHistoryOriginTable::MediaHistoryOriginTable(
    scoped_refptr<base::UpdateableSequencedTaskRunner> db_task_runner)
    : MediaHistoryTableBase(std::move(db_task_runner)) {}

MediaHistoryOriginTable::~MediaHistoryOriginTable() = default;

sql::InitStatus MediaHistoryOriginTable::CreateTableIfNonExistent() {
  if (!CanAccessDatabase())
    return sql::INIT_FAILURE;

  bool success = DB()->Execute(
      base::StringPrintf("CREATE TABLE IF NOT EXISTS %s("
                         "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                         "origin TEXT NOT NULL UNIQUE, "
                         "last_updated_time_s INTEGER,"
                         "has_media_engagement INTEGER, "
                         "media_engagement_visits INTEGER,"
                         "media_engagement_playbacks INTEGER,"
                         "media_engagement_last_playback_time REAL,"
                         "media_engagement_has_high_score INTEGER)",
                         kTableName)
          .c_str());

  if (!success) {
    ResetDB();
    LOG(ERROR) << "Failed to create media history origin table.";
    return sql::INIT_FAILURE;
  }

  return sql::INIT_OK;
}

bool MediaHistoryOriginTable::CreateOriginId(const std::string& origin) {
  DCHECK_LT(0, DB()->transaction_nesting());
  if (!CanAccessDatabase())
    return false;

  // Insert the origin into the table if it does not exist.
  sql::Statement statement(DB()->GetCachedStatement(
      SQL_FROM_HERE,
      base::StringPrintf("INSERT OR IGNORE INTO %s"
                         "(origin, last_updated_time_s) VALUES (?, ?)",
                         kTableName)
          .c_str()));
  statement.BindString(0, origin);
  statement.BindInt64(1,
                      base::Time::Now().ToDeltaSinceWindowsEpoch().InSeconds());
  if (!statement.Run()) {
    LOG(ERROR) << "Failed to create the origin ID.";
    return false;
  }

  return true;
}

}  // namespace media_history
