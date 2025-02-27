// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/sharing/sharing_message_bridge_impl.h"

#include "base/bind_helpers.h"
#include "base/run_loop.h"
#include "base/test/metrics/histogram_tester.h"
#include "base/test/mock_callback.h"
#include "base/test/task_environment.h"
#include "components/sync/model/metadata_batch.h"
#include "components/sync/model/metadata_change_list.h"
#include "components/sync/model/mock_model_type_change_processor.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

using testing::_;
using testing::InvokeWithoutArgs;
using testing::NotNull;
using testing::Return;
using testing::SaveArg;

// Action SaveArgPointeeMove<k>(pointer) saves the value pointed to by the k-th
// (0-based) argument of the mock function by moving it to *pointer.
ACTION_TEMPLATE(SaveArgPointeeMove,
                HAS_1_TEMPLATE_PARAMS(int, k),
                AND_1_VALUE_PARAMS(pointer)) {
  *pointer = std::move(*testing::get<k>(args));
}

MATCHER_P(HasErrorCode, expected_error_code, "") {
  return arg.error_code() == expected_error_code;
}

class SharingMessageBridgeTest : public testing::Test {
 protected:
  SharingMessageBridgeTest() {
    EXPECT_CALL(*processor(), ModelReadyToSync(NotNull()));
    bridge_ = std::make_unique<SharingMessageBridgeImpl>(
        mock_processor_.CreateForwardingProcessor());
    ON_CALL(*processor(), IsTrackingMetadata()).WillByDefault(Return(true));
  }

  SharingMessageBridgeImpl* bridge() { return bridge_.get(); }
  syncer::MockModelTypeChangeProcessor* processor() { return &mock_processor_; }

  std::unique_ptr<sync_pb::SharingMessageSpecifics> CreateSpecifics(
      const std::string& payload) const {
    auto specifics = std::make_unique<sync_pb::SharingMessageSpecifics>();
    specifics->set_payload(payload);
    return specifics;
  }

 private:
  base::test::TaskEnvironment task_environment_;
  testing::NiceMock<syncer::MockModelTypeChangeProcessor> mock_processor_;
  std::unique_ptr<SharingMessageBridgeImpl> bridge_;
};

TEST_F(SharingMessageBridgeTest, ShouldWriteMessagesToProcessor) {
  syncer::EntityData entity_data;
  EXPECT_CALL(*processor(), Put(_, _, _))
      .WillRepeatedly(SaveArgPointeeMove<1>(&entity_data));
  bridge()->SendSharingMessage(CreateSpecifics("test_payload"),
                               base::DoNothing());

  EXPECT_TRUE(entity_data.specifics.has_sharing_message());
  EXPECT_EQ(entity_data.specifics.sharing_message().payload(), "test_payload");

  entity_data.specifics.Clear();
  bridge()->SendSharingMessage(CreateSpecifics("another_payload"),
                               base::DoNothing());

  EXPECT_TRUE(entity_data.specifics.has_sharing_message());
  EXPECT_EQ(entity_data.specifics.sharing_message().payload(),
            "another_payload");
  EXPECT_FALSE(bridge()->GetStorageKey(entity_data).empty());
}

TEST_F(SharingMessageBridgeTest, ShouldGenerateUniqueStorageKey) {
  std::string first_storage_key;
  std::string second_storage_key;
  EXPECT_CALL(*processor(), Put(_, _, _))
      .WillOnce(SaveArg<0>(&first_storage_key))
      .WillOnce(SaveArg<0>(&second_storage_key));
  bridge()->SendSharingMessage(CreateSpecifics("payload"), base::DoNothing());
  bridge()->SendSharingMessage(CreateSpecifics("another_payload"),
                               base::DoNothing());

  EXPECT_FALSE(first_storage_key.empty());
  EXPECT_FALSE(second_storage_key.empty());
  EXPECT_NE(first_storage_key, second_storage_key);
}

TEST_F(SharingMessageBridgeTest, ShouldInvokeCallbackOnSuccess) {
  base::HistogramTester histogram_tester;
  std::string storage_key;
  EXPECT_CALL(*processor(), Put(_, _, _)).WillOnce(SaveArg<0>(&storage_key));

  base::MockCallback<SharingMessageBridge::CommitFinishedCallback> callback;
  bridge()->SendSharingMessage(CreateSpecifics("payload"), callback.Get());
  EXPECT_CALL(callback,
              Run(HasErrorCode(sync_pb::SharingMessageCommitError::NONE)));

  // Mark data as committed.
  syncer::EntityChangeList change_list;
  change_list.push_back(syncer::EntityChange::CreateDelete(storage_key));
  bridge()->ApplySyncChanges(nullptr, std::move(change_list));

  EXPECT_EQ(bridge()->GetCallbacksCountForTesting(), 0u);
  histogram_tester.ExpectUniqueSample("Sync.SharingMessage.CommitResult",
                                      sync_pb::SharingMessageCommitError::NONE,
                                      1);
}

TEST_F(SharingMessageBridgeTest, ShouldInvokeCallbackOnFailure) {
  base::HistogramTester histogram_tester;
  syncer::EntityData entity_data;
  EXPECT_CALL(*processor(), Put(_, _, _))
      .WillRepeatedly(SaveArgPointeeMove<1>(&entity_data));

  base::MockCallback<SharingMessageBridge::CommitFinishedCallback> callback;
  sync_pb::SharingMessageCommitError commit_error;
  EXPECT_CALL(callback, Run).WillOnce(SaveArg<0>(&commit_error));

  bridge()->SendSharingMessage(CreateSpecifics("payload"), callback.Get());

  EXPECT_FALSE(entity_data.client_tag_hash.value().empty());

  // The callback should be called only after committing data.
  EXPECT_FALSE(commit_error.has_error_code());

  syncer::FailedCommitResponseDataList response_list;
  {
    syncer::FailedCommitResponseData response;
    response.client_tag_hash = entity_data.client_tag_hash;
    response.datatype_specific_error.mutable_sharing_message_error()
        ->set_error_code(sync_pb::SharingMessageCommitError::PERMISSION_DENIED);
    response_list.push_back(std::move(response));
  }
  EXPECT_CALL(*processor(),
              UntrackEntityForClientTagHash(entity_data.client_tag_hash));

  bridge()->OnCommitAttemptErrors(response_list);

  EXPECT_TRUE(commit_error.has_error_code());
  EXPECT_EQ(commit_error.error_code(),
            sync_pb::SharingMessageCommitError::PERMISSION_DENIED);
  EXPECT_EQ(bridge()->GetCallbacksCountForTesting(), 0u);
  histogram_tester.ExpectUniqueSample(
      "Sync.SharingMessage.CommitResult",
      sync_pb::SharingMessageCommitError::PERMISSION_DENIED, 1);
}

TEST_F(SharingMessageBridgeTest, ShouldInvokeCallbackIfSyncIsDisabled) {
  base::HistogramTester histogram_tester;
  ON_CALL(*processor(), IsTrackingMetadata()).WillByDefault(Return(false));
  EXPECT_CALL(*processor(), Put).Times(0);

  base::MockCallback<SharingMessageBridge::CommitFinishedCallback> callback;
  EXPECT_CALL(
      callback,
      Run(HasErrorCode(sync_pb::SharingMessageCommitError::SYNC_TURNED_OFF)));

  bridge()->SendSharingMessage(CreateSpecifics("test_payload"), callback.Get());

  EXPECT_EQ(bridge()->GetCallbacksCountForTesting(), 0u);
  histogram_tester.ExpectUniqueSample(
      "Sync.SharingMessage.CommitResult",
      sync_pb::SharingMessageCommitError::SYNC_TURNED_OFF, 1);
}

TEST_F(SharingMessageBridgeTest, ShouldInvokeCallbackOnSyncStoppedEvent) {
  base::HistogramTester histogram_tester;
  base::MockCallback<SharingMessageBridge::CommitFinishedCallback> callback;
  bridge()->SendSharingMessage(CreateSpecifics("test_payload"), callback.Get());
  ASSERT_EQ(bridge()->GetCallbacksCountForTesting(), 1u);

  EXPECT_CALL(
      callback,
      Run(HasErrorCode(sync_pb::SharingMessageCommitError::SYNC_TURNED_OFF)));
  bridge()->ApplyStopSyncChanges(nullptr);

  EXPECT_EQ(bridge()->GetCallbacksCountForTesting(), 0u);
  histogram_tester.ExpectUniqueSample(
      "Sync.SharingMessage.CommitResult",
      sync_pb::SharingMessageCommitError::SYNC_TURNED_OFF, 1);
}

TEST_F(SharingMessageBridgeTest, ShouldInvokeCallbackOnSyncCommitFailure) {
  base::HistogramTester histogram_tester;
  base::MockCallback<SharingMessageBridge::CommitFinishedCallback> callback;
  bridge()->SendSharingMessage(CreateSpecifics("test_payload"), callback.Get());
  ASSERT_EQ(bridge()->GetCallbacksCountForTesting(), 1u);

  EXPECT_CALL(
      callback,
      Run(HasErrorCode(sync_pb::SharingMessageCommitError::SYNC_ERROR)));
  bridge()->OnCommitAttemptFailed();

  EXPECT_EQ(bridge()->GetCallbacksCountForTesting(), 0u);
  histogram_tester.ExpectUniqueSample(
      "Sync.SharingMessage.CommitResult",
      sync_pb::SharingMessageCommitError::SYNC_ERROR, 1);
}

}  // namespace
