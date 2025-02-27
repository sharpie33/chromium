// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/stl_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_piece.h"
#include "base/strings/string_split.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/test/metrics/histogram_tester.h"
#include "base/test/scoped_feature_list.h"
#include "base/threading/platform_thread.h"  // For |Sleep()|.
#include "base/threading/thread_restrictions.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/metrics/subprocess_metrics_provider.h"
#include "chrome/browser/net/profile_network_context_service.h"
#include "chrome/browser/net/profile_network_context_service_factory.h"
#include "chrome/browser/net/profile_network_context_service_test_utils.h"
#include "chrome/browser/policy/policy_test_utils.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/common/chrome_features.h"
#include "chrome/common/chrome_paths_internal.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/pref_names.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/policy/policy_constants.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/common/content_features.h"
#include "content/public/test/simple_url_loader_test_helper.h"
#include "mojo/public/cpp/system/data_pipe_utils.h"
#include "net/base/features.h"
#include "net/base/load_flags.h"
#include "net/http/http_auth_preferences.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "net/test/embedded_test_server/request_handler_util.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "services/network/public/cpp/cors/cors.h"
#include "services/network/public/cpp/features.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"

#if defined(OS_CHROMEOS)
#include "chrome/browser/chromeos/policy/login_policy_test_base.h"
#include "chrome/browser/chromeos/policy/user_policy_test_helper.h"
#include "chrome/browser/chromeos/profiles/profile_helper.h"
#include "components/policy/policy_constants.h"
#include "net/base/features.h"
#endif  // defined(OS_CHROMEOS)

#if BUILDFLAG(BUILTIN_CERT_VERIFIER_FEATURE_SUPPORTED)
#include "chrome/browser/policy/policy_test_utils.h"
#include "components/policy/core/common/policy_map.h"
#include "components/policy/policy_constants.h"
#include "net/base/features.h"
#endif

// Most tests for this class are in NetworkContextConfigurationBrowserTest.
class ProfileNetworkContextServiceBrowsertest : public InProcessBrowserTest {
 public:
  ProfileNetworkContextServiceBrowsertest() = default;

  ~ProfileNetworkContextServiceBrowsertest() override = default;

  void SetUpOnMainThread() override {
    EXPECT_TRUE(embedded_test_server()->Start());
    loader_factory_ = content::BrowserContext::GetDefaultStoragePartition(
                          browser()->profile())
                          ->GetURLLoaderFactoryForBrowserProcess()
                          .get();
  }

  network::mojom::URLLoaderFactory* loader_factory() const {
    return loader_factory_;
  }

 private:
  network::mojom::URLLoaderFactory* loader_factory_ = nullptr;
};

IN_PROC_BROWSER_TEST_F(ProfileNetworkContextServiceBrowsertest,
                       DiskCacheLocation) {
  // Run a request that caches the response, to give the network service time to
  // create a cache directory.
  std::unique_ptr<network::ResourceRequest> request =
      std::make_unique<network::ResourceRequest>();
  request->url = embedded_test_server()->GetURL("/cachetime");
  content::SimpleURLLoaderTestHelper simple_loader_helper;
  std::unique_ptr<network::SimpleURLLoader> simple_loader =
      network::SimpleURLLoader::Create(std::move(request),
                                       TRAFFIC_ANNOTATION_FOR_TESTS);

  simple_loader->DownloadToStringOfUnboundedSizeUntilCrashAndDie(
      loader_factory(), simple_loader_helper.GetCallback());
  simple_loader_helper.WaitForCallback();
  ASSERT_TRUE(simple_loader_helper.response_body());

  base::FilePath expected_cache_path;
  chrome::GetUserCacheDirectory(browser()->profile()->GetPath(),
                                &expected_cache_path);
  expected_cache_path = expected_cache_path.Append(chrome::kCacheDirname);
  base::ScopedAllowBlockingForTesting allow_blocking;
  EXPECT_TRUE(base::PathExists(expected_cache_path));
}

IN_PROC_BROWSER_TEST_F(ProfileNetworkContextServiceBrowsertest,
                       DefaultCacheSize) {
  // We don't have a great way of directly checking that the disk cache has the
  // correct max size, but we can make sure that we set up our network context
  // params correctly.
  ProfileNetworkContextService* profile_network_context_service =
      ProfileNetworkContextServiceFactory::GetForContext(browser()->profile());
  base::FilePath empty_relative_partition_path;
  network::mojom::NetworkContextParamsPtr network_context_params_ptr =
      profile_network_context_service->CreateNetworkContextParams(
          /*in_memory=*/false, empty_relative_partition_path);
  EXPECT_EQ(0, network_context_params_ptr->http_cache_max_size);
}

IN_PROC_BROWSER_TEST_F(ProfileNetworkContextServiceBrowsertest, BrotliEnabled) {
  // Brotli is only used over encrypted connections.
  net::EmbeddedTestServer https_server(net::EmbeddedTestServer::TYPE_HTTPS);
  https_server.AddDefaultHandlers(
      base::FilePath(FILE_PATH_LITERAL("content/test/data")));
  ASSERT_TRUE(https_server.Start());

  std::unique_ptr<network::ResourceRequest> request =
      std::make_unique<network::ResourceRequest>();
  request->url = https_server.GetURL("/echoheader?accept-encoding");

  content::SimpleURLLoaderTestHelper simple_loader_helper;
  std::unique_ptr<network::SimpleURLLoader> simple_loader =
      network::SimpleURLLoader::Create(std::move(request),
                                       TRAFFIC_ANNOTATION_FOR_TESTS);
  simple_loader->DownloadToStringOfUnboundedSizeUntilCrashAndDie(
      loader_factory(), simple_loader_helper.GetCallback());
  simple_loader_helper.WaitForCallback();
  ASSERT_TRUE(simple_loader_helper.response_body());
  std::vector<std::string> encodings =
      base::SplitString(*simple_loader_helper.response_body(), ",",
                        base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  EXPECT_TRUE(base::Contains(encodings, "br"));
}

void CheckCacheResetStatus(base::HistogramTester* histograms, bool reset) {
  // TODO(crbug/1041810): The failure case, here, is to time out.  Since Chrome
  // doesn't synchronize cache loading, there's no guarantee that this is
  // complete and it's merely available at earliest convenience.  If shutdown
  // occurs prior to the cache being loaded, then nothing is reported.  This
  // should probably be fixed to avoid the use of the sleep function, but that
  // will require synchronizing in some meaningful way to guarantee the cache
  // has been loaded prior to testing the histograms.
  while (!histograms->GetBucketCount("HttpCache.HardReset", reset)) {
    content::FetchHistogramsFromChildProcesses();
    SubprocessMetricsProvider::MergeHistogramDeltasForTesting();
    base::PlatformThread::Sleep(base::TimeDelta::FromMilliseconds(5));
  }

  if (reset) {
    // Some tests load the cache multiple times, but should only be reset once.
    EXPECT_EQ(histograms->GetBucketCount("HttpCache.HardReset", true), 1);
  } else {
    // Make sure it's never reset.
    EXPECT_EQ(histograms->GetBucketCount("HttpCache.HardReset", true), 0);
  }
}

class ProfileNetworkContextServiceCacheSameBrowsertest
    : public ProfileNetworkContextServiceBrowsertest {
 public:
  ProfileNetworkContextServiceCacheSameBrowsertest() = default;
  ~ProfileNetworkContextServiceCacheSameBrowsertest() override = default;

  void SetUp() override {
    scoped_feature_list_.InitWithFeatures(
        {}, {net::features::kSplitCacheByNetworkIsolationKey,
             net::features::kAppendFrameOriginToNetworkIsolationKey,
             net::features::kUseRegistrableDomainInNetworkIsolationKey});
    ProfileNetworkContextServiceBrowsertest::SetUp();
  }

  base::HistogramTester histograms_;

 private:
  base::test::ScopedFeatureList scoped_feature_list_;
};

IN_PROC_BROWSER_TEST_F(ProfileNetworkContextServiceCacheSameBrowsertest,
                       PRE_TestCacheResetParameter) {
  CheckCacheResetStatus(&histograms_, false);

  // At this point, we have already called the initialization.
  // Verify that we have the correct values in the local_state.
  PrefService* local_state = g_browser_process->local_state();
  DCHECK_EQ(
      local_state->GetString(
          "profile_network_context_service.http_cache_finch_experiment_groups"),
      "None None None");
}

IN_PROC_BROWSER_TEST_F(ProfileNetworkContextServiceCacheSameBrowsertest,
                       TestCacheResetParameter) {
  CheckCacheResetStatus(&histograms_, false);

  // At this point, we have already called the initialization.
  // Verify that we have the correct values in the local_state.
  PrefService* local_state = g_browser_process->local_state();
  DCHECK_EQ(
      local_state->GetString(
          "profile_network_context_service.http_cache_finch_experiment_groups"),
      "None None None");
}

class ProfileNetworkContextServiceCacheChangeBrowsertest
    : public ProfileNetworkContextServiceBrowsertest {
 public:
  ProfileNetworkContextServiceCacheChangeBrowsertest() {
    scoped_feature_list_.InitWithFeaturesAndParameters(
        {{net::features::kAppendFrameOriginToNetworkIsolationKey, {}}},
        {net::features::kSplitCacheByNetworkIsolationKey,
         net::features::kUseRegistrableDomainInNetworkIsolationKey});
  }
  ~ProfileNetworkContextServiceCacheChangeBrowsertest() override = default;

  base::HistogramTester histograms_;

 private:
  base::test::ScopedFeatureList scoped_feature_list_;
};

// Flaky on Linux and Mac: https://crbug.com/1041810
// The first time we load, even if we're in an experiment there's no reset
// from the unknown state.
IN_PROC_BROWSER_TEST_F(ProfileNetworkContextServiceCacheChangeBrowsertest,
                       PRE_TestCacheResetParameter) {
  CheckCacheResetStatus(&histograms_, false);

  // At this point, we have already called the initialization.
  // Verify that we have the correct values in the local_state.
  PrefService* local_state = g_browser_process->local_state();
  DCHECK_EQ(
      local_state->GetString(
          "profile_network_context_service.http_cache_finch_experiment_groups"),
      "None scoped_feature_list_trial_group None");
  // Set the local state for the next test.
  local_state->SetString(
      "profile_network_context_service.http_cache_finch_experiment_groups",
      "None None None");
}

// The second time we load we know the state, which was "None None None" for the
// previous test, so we should see a reset being in an experiment.
IN_PROC_BROWSER_TEST_F(ProfileNetworkContextServiceCacheChangeBrowsertest,
                       TestCacheResetParameter) {
  CheckCacheResetStatus(&histograms_, true);

  // At this point, we have already called the initialization once.
  // Verify that we have the correct values in the local_state.
  PrefService* local_state = g_browser_process->local_state();
  DCHECK_EQ(
      local_state->GetString(
          "profile_network_context_service.http_cache_finch_experiment_groups"),
      "None scoped_feature_list_trial_group None");
}

class AmbientAuthenticationTestWithPolicy
    : public policy::PolicyTest,
      public testing::WithParamInterface<AmbientAuthenticationFeatureState> {
 public:
  AmbientAuthenticationTestWithPolicy() {
    feature_state_ = GetParam();
    AmbientAuthenticationTestHelper::CookTheFeatureList(scoped_feature_list_,
                                                        feature_state_);
    policy::PolicyTest::SetUpInProcessBrowserTestFixture();
  }

  void IsAmbientAuthAllowedForProfilesTest() {
    PrefService* service = g_browser_process->local_state();
    int policy_value =
        service->GetInteger(prefs::kAmbientAuthenticationInPrivateModesEnabled);

    Profile* regular_profile = browser()->profile();
    Profile* incognito_profile = regular_profile->GetOffTheRecordProfile();

    EXPECT_TRUE(AmbientAuthenticationTestHelper::IsAmbientAuthAllowedForProfile(
        regular_profile));
    EXPECT_EQ(AmbientAuthenticationTestHelper::IsAmbientAuthAllowedForProfile(
                  incognito_profile),
              AmbientAuthenticationTestHelper::IsIncognitoAllowedInFeature(
                  feature_state_) ||
                  AmbientAuthenticationTestHelper::IsIncognitoAllowedInPolicy(
                      policy_value));
// ChromeOS guest sessions don't have the capability to
// do ambient authentications.
#if !defined(OS_CHROMEOS)
    EXPECT_EQ(AmbientAuthenticationTestHelper::IsAmbientAuthAllowedForProfile(
                  AmbientAuthenticationTestHelper::GetGuestProfile()),
              AmbientAuthenticationTestHelper::IsGuestAllowedInFeature(
                  feature_state_) ||
                  AmbientAuthenticationTestHelper::IsGuestAllowedInPolicy(
                      policy_value));
#endif
  }

  void EnablePolicyWithValue(net::AmbientAuthAllowedProfileTypes value) {
    SetPolicy(&policies_,
              policy::key::kAmbientAuthenticationInPrivateModesEnabled,
              std::make_unique<base::Value>(static_cast<int>(value)));
    UpdateProviderPolicy(policies_);
  }

 private:
  base::test::ScopedFeatureList scoped_feature_list_;
  AmbientAuthenticationFeatureState feature_state_;
  policy::PolicyMap policies_;
};

INSTANTIATE_TEST_CASE_P(
    AmbientAuthAllFeatureValuesTest,
    AmbientAuthenticationTestWithPolicy,
    testing::Values(AmbientAuthenticationFeatureState::GUEST_OFF_INCOGNITO_OFF,
                    AmbientAuthenticationFeatureState::GUEST_OFF_INCOGNITO_ON,
                    AmbientAuthenticationFeatureState::GUEST_ON_INCOGNITO_OFF,
                    AmbientAuthenticationFeatureState::GUEST_ON_INCOGNITO_ON));

IN_PROC_BROWSER_TEST_P(AmbientAuthenticationTestWithPolicy, RegularOnly) {
  EnablePolicyWithValue(net::AmbientAuthAllowedProfileTypes::REGULAR_ONLY);
  IsAmbientAuthAllowedForProfilesTest();
}

IN_PROC_BROWSER_TEST_P(AmbientAuthenticationTestWithPolicy,
                       IncognitoAndRegular) {
  EnablePolicyWithValue(
      net::AmbientAuthAllowedProfileTypes::INCOGNITO_AND_REGULAR);
  IsAmbientAuthAllowedForProfilesTest();
}

IN_PROC_BROWSER_TEST_P(AmbientAuthenticationTestWithPolicy, GuestAndRegular) {
  EnablePolicyWithValue(net::AmbientAuthAllowedProfileTypes::GUEST_AND_REGULAR);
  IsAmbientAuthAllowedForProfilesTest();
}

IN_PROC_BROWSER_TEST_P(AmbientAuthenticationTestWithPolicy, All) {
  EnablePolicyWithValue(net::AmbientAuthAllowedProfileTypes::ALL);
  IsAmbientAuthAllowedForProfilesTest();
}

// Test subclass that adds switches::kDiskCacheDir and switches::kDiskCacheSize
// to the command line, to make sure they're respected.
class ProfileNetworkContextServiceDiskCacheBrowsertest
    : public ProfileNetworkContextServiceBrowsertest {
 public:
  const int64_t kCacheSize = 7;

  ProfileNetworkContextServiceDiskCacheBrowsertest() {
    EXPECT_TRUE(temp_dir_.CreateUniqueTempDir());
  }

  ~ProfileNetworkContextServiceDiskCacheBrowsertest() override {}

  void SetUpCommandLine(base::CommandLine* command_line) override {
    command_line->AppendSwitchPath(switches::kDiskCacheDir,
                                   temp_dir_.GetPath());
    command_line->AppendSwitchASCII(switches::kDiskCacheSize,
                                    base::NumberToString(kCacheSize));
  }

  const base::FilePath& TempPath() { return temp_dir_.GetPath(); }

 private:
  base::ScopedTempDir temp_dir_;
};

// Makes sure switches::kDiskCacheDir is hooked up correctly.
IN_PROC_BROWSER_TEST_F(ProfileNetworkContextServiceDiskCacheBrowsertest,
                       DiskCacheLocation) {
  // Make sure command line switch is hooked up to the pref.
  ASSERT_EQ(TempPath(), g_browser_process->local_state()->GetFilePath(
                            prefs::kDiskCacheDir));

  // Run a request that caches the response, to give the network service time to
  // create a cache directory.
  std::unique_ptr<network::ResourceRequest> request =
      std::make_unique<network::ResourceRequest>();
  request->url = embedded_test_server()->GetURL("/cachetime");
  content::SimpleURLLoaderTestHelper simple_loader_helper;
  std::unique_ptr<network::SimpleURLLoader> simple_loader =
      network::SimpleURLLoader::Create(std::move(request),
                                       TRAFFIC_ANNOTATION_FOR_TESTS);

  simple_loader->DownloadToStringOfUnboundedSizeUntilCrashAndDie(
      loader_factory(), simple_loader_helper.GetCallback());
  simple_loader_helper.WaitForCallback();
  ASSERT_TRUE(simple_loader_helper.response_body());

  // Cache directory should now exist.
  base::FilePath expected_cache_path =
      TempPath()
          .Append(browser()->profile()->GetPath().BaseName())
          .Append(chrome::kCacheDirname);
  base::ScopedAllowBlockingForTesting allow_blocking;
  EXPECT_TRUE(base::PathExists(expected_cache_path));
}

// Makes sure switches::kDiskCacheSize is hooked up correctly.
IN_PROC_BROWSER_TEST_F(ProfileNetworkContextServiceDiskCacheBrowsertest,
                       DiskCacheSize) {
  // Make sure command line switch is hooked up to the pref.
  ASSERT_EQ(kCacheSize, g_browser_process->local_state()->GetInteger(
                            prefs::kDiskCacheSize));

  // We don't have a great way of directly checking that the disk cache has the
  // correct max size, but we can make sure that we set up our network context
  // params correctly.
  ProfileNetworkContextService* profile_network_context_service =
      ProfileNetworkContextServiceFactory::GetForContext(browser()->profile());
  base::FilePath empty_relative_partition_path;
  network::mojom::NetworkContextParamsPtr network_context_params_ptr =
      profile_network_context_service->CreateNetworkContextParams(
          /*in_memory=*/false, empty_relative_partition_path);
  EXPECT_EQ(kCacheSize, network_context_params_ptr->http_cache_max_size);
}

#if defined(OS_CHROMEOS)
// Base class for verifying which certificate verifier is being used on Chrome
// OS depending on feature state and policies.
class ProfileNetworkContextServiceCertVerifierBrowsertestBase
    : public policy::LoginPolicyTestBase {
 public:
  ProfileNetworkContextServiceCertVerifierBrowsertestBase() = default;
  ~ProfileNetworkContextServiceCertVerifierBrowsertestBase() override = default;

 protected:
  void SetPolicyValue(base::StringPiece policy_key, base::Value value) {
    policy_values_.SetKey(policy_key, std::move(value));
    user_policy_helper()->SetPolicy(policy_values_,
                                    base::Value(base::Value::Type::DICTIONARY));
  }

  bool IsSigninProfileUsingBuiltinCertVerifier() {
    Profile* const profile = chromeos::ProfileHelper::GetSigninProfile();
    ProfileNetworkContextService* const service =
        ProfileNetworkContextServiceFactory::GetForContext(profile);
    return service->using_builtin_cert_verifier();
  }

  bool IsActiveProfileUsingBuiltinCertVerifier() {
    Profile* const profile = GetProfileForActiveUser();
    ProfileNetworkContextService* const service =
        ProfileNetworkContextServiceFactory::GetForContext(profile);
    return service->using_builtin_cert_verifier();
  }

  base::test::ScopedFeatureList scoped_feature_list_;

 private:
  base::Value policy_values_{base::Value::Type::DICTIONARY};

  DISALLOW_COPY_AND_ASSIGN(
      ProfileNetworkContextServiceCertVerifierBrowsertestBase);
};

// When using this class, the built-in certificate verifier has been enabled
// using the UseBuiltinCertVerifier feature.
class ProfileNetworkContextServiceCertVerifierBuiltinEnabledBrowsertest
    : public ProfileNetworkContextServiceCertVerifierBrowsertestBase {
 public:
  ProfileNetworkContextServiceCertVerifierBuiltinEnabledBrowsertest() = default;
  ~ProfileNetworkContextServiceCertVerifierBuiltinEnabledBrowsertest()
      override = default;

  void SetUpInProcessBrowserTestFixture() override {
    scoped_feature_list_.InitAndEnableFeature(
        net::features::kCertVerifierBuiltinFeature);
    ProfileNetworkContextServiceCertVerifierBrowsertestBase::
        SetUpInProcessBrowserTestFixture();
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(
      ProfileNetworkContextServiceCertVerifierBuiltinEnabledBrowsertest);
};

// If the built-in cert verifier is enabled and no policy is present, it should
// be enabled on the sign-in screen and in the user profile.
IN_PROC_BROWSER_TEST_F(
    ProfileNetworkContextServiceCertVerifierBuiltinEnabledBrowsertest,
    TurnedOnByFeature) {
  SkipToLoginScreen();
  EXPECT_TRUE(IsSigninProfileUsingBuiltinCertVerifier());

  LogIn(kAccountId, kAccountPassword, kEmptyServices);

  EXPECT_TRUE(IsActiveProfileUsingBuiltinCertVerifier());
}

// If the built-in cert verifier is enabled, but user policy says to disable it,
// it should be disabled in the user profile.
IN_PROC_BROWSER_TEST_F(
    ProfileNetworkContextServiceCertVerifierBuiltinEnabledBrowsertest,
    TurnedOffByLegacyPolicy) {
  SkipToLoginScreen();

  SetPolicyValue(policy::key::kBuiltinCertificateVerifierEnabled,
                 base::Value(false));
  LogIn(kAccountId, kAccountPassword, kEmptyServices);

  EXPECT_FALSE(IsActiveProfileUsingBuiltinCertVerifier());
}

// When using this class, the built-in certificate verifier has been disabled
// using the UseBuiltinCertVerifier feature.
class ProfileNetworkContextServiceCertVerifierBuiltinDisabledBrowsertest
    : public ProfileNetworkContextServiceCertVerifierBrowsertestBase {
 public:
  ProfileNetworkContextServiceCertVerifierBuiltinDisabledBrowsertest() =
      default;
  ~ProfileNetworkContextServiceCertVerifierBuiltinDisabledBrowsertest()
      override = default;

  void SetUpInProcessBrowserTestFixture() override {
    scoped_feature_list_.InitAndDisableFeature(
        net::features::kCertVerifierBuiltinFeature);
    ProfileNetworkContextServiceCertVerifierBrowsertestBase::
        SetUpInProcessBrowserTestFixture();
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(
      ProfileNetworkContextServiceCertVerifierBuiltinDisabledBrowsertest);
};

// If the built-in cert verifier feature is disabled, it should be disabled in
// user profiles but enabled in the sign-in profile.
IN_PROC_BROWSER_TEST_F(
    ProfileNetworkContextServiceCertVerifierBuiltinDisabledBrowsertest,
    TurnedOffByFeature) {
  SkipToLoginScreen();
  EXPECT_TRUE(IsSigninProfileUsingBuiltinCertVerifier());

  LogIn(kAccountId, kAccountPassword, kEmptyServices);

  EXPECT_FALSE(IsActiveProfileUsingBuiltinCertVerifier());
}

// If the built-in cert verifier feature is disabled, but policy force-enables
// it for a profile, it should be enabled in the profile.
IN_PROC_BROWSER_TEST_F(
    ProfileNetworkContextServiceCertVerifierBuiltinDisabledBrowsertest,
    TurnedOffByFeatureOverrideByPolicy) {
  SkipToLoginScreen();
  EXPECT_TRUE(IsSigninProfileUsingBuiltinCertVerifier());

  SetPolicyValue(policy::key::kBuiltinCertificateVerifierEnabled,
                 base::Value(true));
  LogIn(kAccountId, kAccountPassword, kEmptyServices);

  EXPECT_TRUE(IsActiveProfileUsingBuiltinCertVerifier());
}
#elif BUILDFLAG(BUILTIN_CERT_VERIFIER_FEATURE_SUPPORTED)
class ProfileNetworkContextServiceCertVerifierBuiltinFeaturePolicyTest
    : public policy::PolicyTest,
      public testing::WithParamInterface<bool> {
 public:
  void SetUpInProcessBrowserTestFixture() override {
    scoped_feature_list_.InitWithFeatureState(
        net::features::kCertVerifierBuiltinFeature,
        /*enabled=*/GetParam());
    policy::PolicyTest::SetUpInProcessBrowserTestFixture();
  }

 private:
  base::test::ScopedFeatureList scoped_feature_list_;
};

IN_PROC_BROWSER_TEST_P(
    ProfileNetworkContextServiceCertVerifierBuiltinFeaturePolicyTest,
    Test) {
  ProfileNetworkContextService* profile_network_context_service =
      ProfileNetworkContextServiceFactory::GetForContext(browser()->profile());
  base::FilePath empty_relative_partition_path;
  network::mojom::NetworkContextParamsPtr network_context_params_ptr =
      profile_network_context_service->CreateNetworkContextParams(
          /*in_memory=*/false, empty_relative_partition_path);
  EXPECT_EQ(GetParam(), network_context_params_ptr->use_builtin_cert_verifier);

#if BUILDFLAG(BUILTIN_CERT_VERIFIER_POLICY_SUPPORTED)
  // If the BuiltinCertificateVerifierEnabled policy is set it should override
  // the feature flag.
  policy::PolicyMap policies;
  SetPolicy(&policies, policy::key::kBuiltinCertificateVerifierEnabled,
            std::make_unique<base::Value>(true));
  UpdateProviderPolicy(policies);

  network_context_params_ptr =
      profile_network_context_service->CreateNetworkContextParams(
          /*in_memory=*/false, empty_relative_partition_path);
  EXPECT_TRUE(network_context_params_ptr->use_builtin_cert_verifier);

  SetPolicy(&policies, policy::key::kBuiltinCertificateVerifierEnabled,
            std::make_unique<base::Value>(false));
  UpdateProviderPolicy(policies);

  network_context_params_ptr =
      profile_network_context_service->CreateNetworkContextParams(
          /*in_memory=*/false, empty_relative_partition_path);
  EXPECT_FALSE(network_context_params_ptr->use_builtin_cert_verifier);
#endif
}

INSTANTIATE_TEST_SUITE_P(
    All,
    ProfileNetworkContextServiceCertVerifierBuiltinFeaturePolicyTest,
    ::testing::Bool());
#endif

enum class CorsTestMode {
  kWithCorsMitigationListPolicy,
  kWithoutCorsMitigationListPolicy,
};

class CorsExtraSafelistedHeaderNamesTest
    : public policy::PolicyTest,
      public ::testing::WithParamInterface<CorsTestMode> {
 public:
  CorsExtraSafelistedHeaderNamesTest() {
    switch (GetParam()) {
      case CorsTestMode::kWithCorsMitigationListPolicy: {
        auto list = std::make_unique<base::ListValue>();
        list->AppendString("bar");
        policy::PolicyMap policies;
        policies.Set(policy::key::kCorsMitigationList,
                     policy::POLICY_LEVEL_MANDATORY, policy::POLICY_SCOPE_USER,
                     policy::POLICY_SOURCE_CLOUD, std::move(list), nullptr);
        provider_.UpdateChromePolicy(policies);
        scoped_feature_list_.InitWithFeaturesAndParameters(
            {{network::features::kOutOfBlinkCors, {}},
             {features::kExtraSafelistedRequestHeadersForOutOfBlinkCors,
              {{"extra-safelisted-request-headers-for-enterprise", "foo"}}}},
            {});
        break;
      }
      case CorsTestMode::kWithoutCorsMitigationListPolicy:
        scoped_feature_list_.InitWithFeaturesAndParameters(
            {{network::features::kOutOfBlinkCors, {}},
             {features::kExtraSafelistedRequestHeadersForOutOfBlinkCors,
              {{"extra-safelisted-request-headers", "foo,bar"}}}},
            {});
        break;
    }
  }

  // Override to avoid conflict between the |scoped_feature_list_| and
  // |command_line| that PolicyTest::SetUpCommandLine will introduce.
  // TODO(crbug.com/1002483): Remove this workaround.
  void SetUpCommandLine(base::CommandLine* command_line) override {}

  void SetUpOnMainThread() override {
    ASSERT_TRUE(embedded_test_server()->Start());

    // This base::Unretained is safe because |this| outlives
    // |cross_origin_test_server_|.
    cross_origin_test_server_.RegisterRequestHandler(
        base::BindRepeating(&CorsExtraSafelistedHeaderNamesTest::HandleRequest,
                            base::Unretained(this)));
    ASSERT_TRUE(cross_origin_test_server_.Start());

    PolicyTest::SetUpOnMainThread();
  }

  void LoadAndWait(const GURL& url) {
    content::WebContents* web_contents =
        browser()->tab_strip_model()->GetActiveWebContents();
    base::string16 expected_title(base::ASCIIToUTF16("OK"));
    content::TitleWatcher title_watcher(web_contents, expected_title);
    title_watcher.AlsoWaitForTitle(base::ASCIIToUTF16("FAIL"));
    ui_test_utils::NavigateToURL(browser(), url);
    ASSERT_EQ(expected_title, title_watcher.WaitAndGetTitle());
  }

  uint16_t cross_origin_port() { return cross_origin_test_server_.port(); }
  size_t options_count() {
    base::AutoLock lock(lock_);
    return options_count_;
  }
  size_t get_count() {
    base::AutoLock lock(lock_);
    return get_count_;
  }

  const net::EmbeddedTestServer& cross_origin_test_server() const {
    return cross_origin_test_server_;
  }

  static constexpr char kTestPath[] =
      "/cors-extra-safelisted-header-names.html";

 private:
  std::unique_ptr<net::test_server::HttpResponse> HandleRequest(
      const net::test_server::HttpRequest& request) {
    std::unique_ptr<net::test_server::BasicHttpResponse> response =
        std::make_unique<net::test_server::BasicHttpResponse>();
    response->set_code(net::HTTP_OK);
    response->AddCustomHeader(
        network::cors::header_names::kAccessControlAllowOrigin, "*");
    if (request.method == net::test_server::METHOD_OPTIONS) {
      response->AddCustomHeader(
          network::cors::header_names::kAccessControlAllowMethods,
          "GET, OPTIONS");
      response->AddCustomHeader(
          network::cors::header_names::kAccessControlAllowHeaders, "baz");
      response->AddCustomHeader(
          network::cors::header_names::kAccessControlMaxAge, "60");
      base::AutoLock lock(lock_);
      options_count_++;
    } else if (request.method == net::test_server::METHOD_GET) {
      base::AutoLock lock(lock_);
      get_count_++;
    }
    return response;
  }

  base::test::ScopedFeatureList scoped_feature_list_;
  net::EmbeddedTestServer cross_origin_test_server_;
  base::Lock lock_;

  size_t options_count_ GUARDED_BY(lock_) = 0;
  size_t get_count_ GUARDED_BY(lock_) = 0;
};

constexpr char CorsExtraSafelistedHeaderNamesTest::kTestPath[];

IN_PROC_BROWSER_TEST_P(CorsExtraSafelistedHeaderNamesTest, RequestWithFoo) {
  GURL url(cross_origin_test_server().GetURL("/hello"));
  LoadAndWait(embedded_test_server()->GetURL(base::StringPrintf(
      "%s?url=%s&headers=foo", kTestPath, url.spec().c_str())));
  EXPECT_EQ(0u, options_count());
  EXPECT_EQ(1u, get_count());
}

IN_PROC_BROWSER_TEST_P(CorsExtraSafelistedHeaderNamesTest, RequestWithBar) {
  GURL url(cross_origin_test_server().GetURL("/hello"));
  LoadAndWait(embedded_test_server()->GetURL(base::StringPrintf(
      "%s?url=%s&headers=bar", kTestPath, url.spec().c_str())));
  EXPECT_EQ(0u, options_count());
  EXPECT_EQ(1u, get_count());
}

IN_PROC_BROWSER_TEST_P(CorsExtraSafelistedHeaderNamesTest, RequestWithFooBar) {
  GURL url(cross_origin_test_server().GetURL("/hello"));
  LoadAndWait(embedded_test_server()->GetURL(base::StringPrintf(
      "%s?url=%s&headers=foo,bar", kTestPath, url.spec().c_str())));
  EXPECT_EQ(0u, options_count());
  EXPECT_EQ(1u, get_count());
}

IN_PROC_BROWSER_TEST_P(CorsExtraSafelistedHeaderNamesTest, RequestWithBaz) {
  GURL url(cross_origin_test_server().GetURL("/hello"));
  LoadAndWait(embedded_test_server()->GetURL(base::StringPrintf(
      "%s?url=%s&headers=baz", kTestPath, url.spec().c_str())));
  EXPECT_EQ(1u, options_count());
  EXPECT_EQ(1u, get_count());
}

IN_PROC_BROWSER_TEST_P(CorsExtraSafelistedHeaderNamesTest, RequestWithFooBaz) {
  GURL url(cross_origin_test_server().GetURL("/hello"));
  LoadAndWait(embedded_test_server()->GetURL(base::StringPrintf(
      "%s?url=%s&headers=foo,baz", kTestPath, url.spec().c_str())));
  EXPECT_EQ(1u, options_count());
  EXPECT_EQ(1u, get_count());
}

INSTANTIATE_TEST_SUITE_P(
    WithCorsMitigationListPolicy,
    CorsExtraSafelistedHeaderNamesTest,
    testing::Values(CorsTestMode::kWithCorsMitigationListPolicy));

INSTANTIATE_TEST_SUITE_P(
    WithoutCorsMitigationListPolicy,
    CorsExtraSafelistedHeaderNamesTest,
    testing::Values(CorsTestMode::kWithoutCorsMitigationListPolicy));
