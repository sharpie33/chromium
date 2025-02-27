// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/files/file_path.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/extensions/extension_browsertest.h"
#include "chrome/browser/extensions/extension_service.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/supervised_user/logged_in_user_mixin.h"
#include "chrome/browser/supervised_user/supervised_user_features.h"
#include "chrome/browser/supervised_user/supervised_user_service.h"
#include "chrome/browser/supervised_user/supervised_user_service_factory.h"
#include "chrome/test/base/mixin_based_in_process_browser_test.h"
#include "content/public/test/test_launcher.h"
#include "extensions/browser/disable_reason.h"
#include "extensions/browser/extension_prefs.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/common/extension.h"

namespace {

constexpr char kGoodCrxId[] = "ldnnhddmnhbkjipkidpdiheffobcpfmf";

}

namespace extensions {

// Tests for the interaction between supervised users and extensions.
class SupervisedUserExtensionTest : public ExtensionBrowserTest {
 public:
  SupervisedUserExtensionTest() {
    // Suppress regular user login to enable child user login.
    set_chromeos_user_ = false;
  }

  // We have to essentially replicate what MixinBasedInProcessBrowserTest does
  // here because ExtensionBrowserTest doesn't inherit from that class.
  void SetUp() override {
    scoped_feature_list_.InitAndEnableFeature(
        supervised_users::kSupervisedUserInitiatedExtensionInstall);
    mixin_host_.SetUp();
    ExtensionBrowserTest::SetUp();
  }

  void SetUpCommandLine(base::CommandLine* command_line) override {
    mixin_host_.SetUpCommandLine(command_line);
    ExtensionBrowserTest::SetUpCommandLine(command_line);
  }

  void SetUpDefaultCommandLine(base::CommandLine* command_line) override {
    mixin_host_.SetUpDefaultCommandLine(command_line);
    ExtensionBrowserTest::SetUpDefaultCommandLine(command_line);
  }

  bool SetUpUserDataDirectory() override {
    return mixin_host_.SetUpUserDataDirectory() &&
           ExtensionBrowserTest::SetUpUserDataDirectory();
  }

  void SetUpInProcessBrowserTestFixture() override {
    mixin_host_.SetUpInProcessBrowserTestFixture();
    ExtensionBrowserTest::SetUpInProcessBrowserTestFixture();
  }

  void CreatedBrowserMainParts(
      content::BrowserMainParts* browser_main_parts) override {
    mixin_host_.CreatedBrowserMainParts(browser_main_parts);
    ExtensionBrowserTest::CreatedBrowserMainParts(browser_main_parts);
  }

  void SetUpOnMainThread() override {
    mixin_host_.SetUpOnMainThread();
    logged_in_user_mixin_.LogInUser();
    ExtensionBrowserTest::SetUpOnMainThread();
  }

  void TearDownOnMainThread() override {
    mixin_host_.TearDownOnMainThread();
    ExtensionBrowserTest::TearDownOnMainThread();
  }

  void TearDownInProcessBrowserTestFixture() override {
    mixin_host_.TearDownInProcessBrowserTestFixture();
    ExtensionBrowserTest::TearDownInProcessBrowserTestFixture();
  }

  void TearDown() override {
    mixin_host_.TearDown();
    ExtensionBrowserTest::TearDown();
  }

 protected:
  SupervisedUserService* GetSupervisedUserService() {
    return SupervisedUserServiceFactory::GetForProfile(profile());
  }

  void SetSupervisedUserExtensionsMayRequestPermissionsPref(bool enabled) {
    GetSupervisedUserService()
        ->SetSupervisedUserExtensionsMayRequestPermissionsPrefForTesting(
            enabled);
  }

  bool IsDisabledForCustodianApproval(const std::string& extension_id) {
    ExtensionPrefs* extension_prefs = ExtensionPrefs::Get(profile());
    return extension_prefs->HasDisableReason(
        extension_id,
        extensions::disable_reason::DISABLE_CUSTODIAN_APPROVAL_REQUIRED);
  }

  bool IsDisabledForBlockedMature(const std::string& extension_id) {
    ExtensionPrefs* extension_prefs = ExtensionPrefs::Get(profile());
    return extension_prefs->HasDisableReason(
        extension_id, extensions::disable_reason::DISABLE_BLOCKED_MATURE);
  }

 private:
  InProcessBrowserTestMixinHost mixin_host_;

  // We want to log in as child user for all of the PRE tests, and regular user
  // otherwise.
  chromeos::LoggedInUserMixin logged_in_user_mixin_{
      &mixin_host_,
      content::IsPreTest() ? chromeos::LoggedInUserMixin::LogInType::kChild
                           : chromeos::LoggedInUserMixin::LogInType::kRegular,
      embedded_test_server(), this};

  base::test::ScopedFeatureList scoped_feature_list_;
};

// Removing supervision should also remove associated disable reasons, such as
// DISABLE_CUSTODIAN_APPROVAL_REQUIRED. Extensions should become enabled again
// after removing supervision. Prevents a regression to crbug/1045625.
IN_PROC_BROWSER_TEST_F(SupervisedUserExtensionTest,
                       PRE_RemovingSupervisionCustodianApprovalRequired) {
  SetSupervisedUserExtensionsMayRequestPermissionsPref(true);

  EXPECT_TRUE(profile()->IsChild());

  base::FilePath path = test_data_dir_.AppendASCII("good.crx");
  EXPECT_FALSE(LoadExtensionWithFlags(path, kFlagNone));
  const Extension* extension =
      extension_registry()->GetInstalledExtension(kGoodCrxId);
  EXPECT_TRUE(extension);

  // This extension is a supervised user initiated install and should remain
  // disabled.
  EXPECT_TRUE(extension_registry()->disabled_extensions().Contains(kGoodCrxId));
  EXPECT_TRUE(IsDisabledForCustodianApproval(kGoodCrxId));
  EXPECT_FALSE(IsDisabledForBlockedMature(kGoodCrxId));
}

IN_PROC_BROWSER_TEST_F(SupervisedUserExtensionTest,
                       RemovingSupervisionCustodianApprovalRequired) {
  EXPECT_FALSE(profile()->IsChild());
  // The extension should still be installed since we are sharing the same data
  // directory as the PRE test.
  const Extension* extension =
      extension_registry()->GetInstalledExtension(kGoodCrxId);
  EXPECT_TRUE(extension);
  // The extension should be enabled now after removing supervision.
  EXPECT_TRUE(extension_registry()->enabled_extensions().Contains(kGoodCrxId));
  EXPECT_FALSE(IsDisabledForCustodianApproval(kGoodCrxId));
  EXPECT_FALSE(IsDisabledForBlockedMature(kGoodCrxId));
}

// Removing supervision should also remove associated disable reasons, such as
// DISABLE_BLOCKED_MATURE. Extensions should become enabled again after removing
// supervision. Prevents a regression to crbug/1045625.
IN_PROC_BROWSER_TEST_F(SupervisedUserExtensionTest,
                       PRE_RemovingSupervisionBlockedMature) {
  SetSupervisedUserExtensionsMayRequestPermissionsPref(true);

  EXPECT_TRUE(profile()->IsChild());

  base::FilePath path = test_data_dir_.AppendASCII("good.crx");
  EXPECT_FALSE(LoadExtensionWithFlags(path, kFlagNone));
  const Extension* extension =
      extension_registry()->GetInstalledExtension(kGoodCrxId);
  EXPECT_TRUE(extension);

  // Let's pretend this extension is mature.
  extension_service()->DisableExtension(kGoodCrxId,
                                        disable_reason::DISABLE_BLOCKED_MATURE);

  // This extension is a supervised user initiated install and should remain
  // disabled.
  EXPECT_TRUE(extension_registry()->disabled_extensions().Contains(kGoodCrxId));
  EXPECT_TRUE(IsDisabledForCustodianApproval(kGoodCrxId));
  EXPECT_TRUE(IsDisabledForBlockedMature(kGoodCrxId));
}

IN_PROC_BROWSER_TEST_F(SupervisedUserExtensionTest,
                       RemovingSupervisionBlockedMature) {
  EXPECT_FALSE(profile()->IsChild());
  // The extension should still be installed since we are sharing the same data
  // directory as the PRE test.
  const Extension* extension =
      extension_registry()->GetInstalledExtension(kGoodCrxId);
  EXPECT_TRUE(extension);
  // The extension should be enabled now after removing supervision.
  EXPECT_TRUE(extension_registry()->enabled_extensions().Contains(kGoodCrxId));
  EXPECT_FALSE(IsDisabledForCustodianApproval(kGoodCrxId));
  EXPECT_FALSE(IsDisabledForBlockedMature(kGoodCrxId));
}

}  // namespace extensions
