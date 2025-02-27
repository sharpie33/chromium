// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/metrics/metrics_service.h"

#include <string>
#include <vector>

#include "base/command_line.h"
#include "base/feature_list.h"
#include "base/memory/weak_ptr.h"
#include "base/optional.h"
#include "base/test/metrics/histogram_tester.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/metrics/chrome_metrics_service_accessor.h"
#include "chrome/browser/metrics/chrome_metrics_services_manager_client.h"
#include "chrome/browser/metrics/testing/demographic_metrics_test_utils.h"
#include "chrome/browser/metrics/testing/sync_metrics_test_utils.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/sync/test/integration/profile_sync_service_harness.h"
#include "chrome/browser/sync/test/integration/sync_test.h"
#include "components/metrics/delegating_provider.h"
#include "components/metrics/demographic_metrics_provider.h"
#include "components/metrics/metrics_switches.h"
#include "components/metrics_services_manager/metrics_services_manager.h"
#include "components/sync/driver/sync_user_settings.h"
#include "testing/gmock/include/gmock/gmock-matchers.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "third_party/metrics_proto/chrome_user_metrics_extension.pb.h"
#include "third_party/metrics_proto/user_demographics.pb.h"
#include "third_party/zlib/google/compression_utils.h"

namespace metrics {
namespace {

class MetricsServiceUserDemographicsBrowserTest
    : public SyncTest,
      public testing::WithParamInterface<test::DemographicsTestParams> {
 public:
  MetricsServiceUserDemographicsBrowserTest() : SyncTest(SINGLE_CLIENT) {
    if (GetParam().enable_feature) {
      // Enable UMA and reporting of the synced user's birth year and gender.
      scoped_feature_list_.InitWithFeatures(
          // enabled_features =
          {internal::kMetricsReportingFeature,
           DemographicMetricsProvider::kDemographicMetricsReporting},
          // disabled_features =
          {});
    } else {
      scoped_feature_list_.InitWithFeatures(
          // enabled_features =
          {internal::kMetricsReportingFeature},
          // disabled_features =
          {DemographicMetricsProvider::kDemographicMetricsReporting});
    }
  }

  void SetUpCommandLine(base::CommandLine* command_line) override {
    // Enable the metrics service for testing (in recording-only mode).
    command_line->AppendSwitch(switches::kMetricsRecordingOnly);
  }

  void SetUp() override {
    // Consent for metrics and crash reporting for testing.
    ChromeMetricsServiceAccessor::SetMetricsAndCrashReportingForTesting(
        &metrics_consent_);
    SyncTest::SetUp();
  }

  // Forces a log record to be generated. Returns a copy of the record on
  // success; otherwise, returns std::nullopt.
  base::Optional<ChromeUserMetricsExtension> GenerateLogRecord() {
    // Make sure that the metrics service is instantiated.
    MetricsService* const metrics_service =
        g_browser_process->GetMetricsServicesManager()->GetMetricsService();
    if (metrics_service == nullptr) {
      LOG(ERROR) << "Metrics service is not available";
      return base::nullopt;
    }

    // Force the creation of a log record (i.e., trigger all metrics providers).
    metrics_service->CloseCurrentLogForTest();

    // Stage/serialize the log record for transmission.
    MetricsLogStore* const log_store = metrics_service->LogStoreForTest();
    log_store->StageNextLog();
    if (!log_store->has_staged_log()) {
      LOG(ERROR) << "No staged log.";
      return base::nullopt;
    }

    // Decompress the staged log.
    std::string uncompressed_log;
    if (!compression::GzipUncompress(log_store->staged_log(),
                                     &uncompressed_log)) {
      LOG(ERROR) << "Decompression failed.";
      return base::nullopt;
    }

    // Deserialize and return the log.
    ChromeUserMetricsExtension uma_proto;
    if (!uma_proto.ParseFromString(uncompressed_log)) {
      LOG(ERROR) << "Deserialization failed.";
      return base::nullopt;
    }

    return uma_proto;
  }

 private:
  bool metrics_consent_ = true;

  base::test::ScopedFeatureList scoped_feature_list_;

  DISALLOW_COPY_AND_ASSIGN(MetricsServiceUserDemographicsBrowserTest);
};

// TODO(crbug/1016118): Add the remaining test cases.
IN_PROC_BROWSER_TEST_P(MetricsServiceUserDemographicsBrowserTest,
                       AddSyncedUserBirthYearAndGenderToProtoData) {
  test::DemographicsTestParams param = GetParam();

  base::HistogramTester histogram;

  const int test_birth_year =
      test::UpdateNetworkTimeAndGetMinimalEligibleBirthYear();
  const UserDemographicsProto::Gender test_gender =
      UserDemographicsProto::GENDER_FEMALE;

  // Add the test synced user birth year and gender priority prefs to the sync
  // server data.
  test::AddUserBirthYearAndGenderToSyncServer(GetFakeServer()->AsWeakPtr(),
                                              test_birth_year, test_gender);

  Profile* test_profile = ProfileManager::GetActiveUserProfile();

  // Enable sync for the test profile.
  std::unique_ptr<ProfileSyncServiceHarness> test_profile_harness =
      test::InitializeProfileForSync(test_profile,
                                     GetFakeServer()->AsWeakPtr());
  test_profile_harness->SetupSync();

  // Make sure that there is only one Profile to allow reporting the user's
  // birth year and gender.
  ASSERT_EQ(1, num_clients());

  // Generate a log record.
  base::Optional<ChromeUserMetricsExtension> uma_proto = GenerateLogRecord();
  ASSERT_TRUE(uma_proto.has_value());

  // Check log content and the histogram.
  if (param.expect_reported_demographics) {
    EXPECT_EQ(test::GetNoisedBirthYear(test_birth_year, *test_profile),
              uma_proto->user_demographics().birth_year());
    EXPECT_EQ(test_gender, uma_proto->user_demographics().gender());
    histogram.ExpectUniqueSample("UMA.UserDemographics.Status",
                                 syncer::UserDemographicsStatus::kSuccess, 1);
  } else {
    EXPECT_FALSE(uma_proto->has_user_demographics());
    histogram.ExpectTotalCount("UMA.UserDemographics.Status", /*count=*/0);
  }

  test_profile_harness->service()->GetUserSettings()->SetSyncRequested(false);
}

#if defined(OS_CHROMEOS)
// Cannot test for the enabled feature on Chrome OS because there are always
// multiple profiles.
static const auto kDemographicsTestParams = testing::Values(
    test::DemographicsTestParams{/*enable_feature=*/false,
                                 /*expect_reported_demographics=*/false});
#else
static const auto kDemographicsTestParams = testing::Values(
    test::DemographicsTestParams{/*enable_feature=*/false,
                                 /*expect_reported_demographics=*/false},
    test::DemographicsTestParams{/*enable_feature=*/true,
                                 /*expect_reported_demographics=*/true});
#endif

INSTANTIATE_TEST_SUITE_P(,
                         MetricsServiceUserDemographicsBrowserTest,
                         kDemographicsTestParams);

}  // namespace
}  // namespace metrics
