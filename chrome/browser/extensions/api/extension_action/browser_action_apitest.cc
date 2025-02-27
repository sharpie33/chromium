// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdint.h>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/macros.h"
#include "base/run_loop.h"
#include "base/scoped_observer.h"
#include "base/strings/stringprintf.h"
#include "base/test/metrics/histogram_tester.h"
#include "base/threading/thread_restrictions.h"
#include "build/build_config.h"
#include "chrome/browser/download/download_prefs.h"
#include "chrome/browser/extensions/api/extension_action/extension_action_api.h"
#include "chrome/browser/extensions/api/extension_action/test_extension_action_api_observer.h"
#include "chrome/browser/extensions/api/extension_action/test_icon_image_observer.h"
#include "chrome/browser/extensions/extension_action.h"
#include "chrome/browser/extensions/extension_action_icon_factory.h"
#include "chrome/browser/extensions/extension_action_manager.h"
#include "chrome/browser/extensions/extension_action_runner.h"
#include "chrome/browser/extensions/extension_apitest.h"
#include "chrome/browser/extensions/extension_tab_util.h"
#include "chrome/browser/extensions/extension_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_navigator_params.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/extensions/extension_action_test_helper.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/ui_features.h"
#include "chrome/common/pref_names.h"
#include "chrome/common/url_constants.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/notification_service.h"
#include "content/public/browser/overlay_window.h"
#include "content/public/browser/picture_in_picture_window_controller.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/content_browser_test_utils.h"
#include "content/public/test/download_test_observer.h"
#include "content/public/test/test_navigation_observer.h"
#include "content/public/test/test_utils.h"
#include "extensions/browser/extension_host.h"
#include "extensions/browser/extension_host_observer.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extension_system.h"
#include "extensions/browser/notification_types.h"
#include "extensions/browser/process_manager.h"
#include "extensions/browser/test_extension_registry_observer.h"
#include "extensions/common/feature_switch.h"
#include "extensions/common/scoped_worker_based_extensions_channel.h"
#include "extensions/test/extension_test_message_listener.h"
#include "extensions/test/result_catcher.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/image/canvas_image_source.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/gfx/image/image_skia_operations.h"
#include "ui/gfx/image/image_unittest_util.h"
#include "ui/gfx/skia_util.h"

using content::WebContents;

namespace extensions {
namespace {

void ExecuteExtensionAction(Browser* browser, const Extension* extension) {
  ExtensionActionRunner::GetForWebContents(
      browser->tab_strip_model()->GetActiveWebContents())
      ->RunAction(extension, true);
}

const char kEmptyImageDataError[] =
    "The imageData property must contain an ImageData object or dictionary "
    "of ImageData objects.";
const char kEmptyPathError[] = "The path property must not be empty.";

// Makes sure |bar_rendering| has |model_icon| in the middle (there's additional
// padding that correlates to the rest of the button, and this is ignored).
void VerifyIconsMatch(const gfx::Image& bar_rendering,
                      const gfx::Image& model_icon) {
  gfx::Rect icon_portion(gfx::Point(), bar_rendering.Size());
  icon_portion.ClampToCenteredSize(model_icon.Size());

  EXPECT_TRUE(gfx::test::AreBitmapsEqual(
      model_icon.AsImageSkia().GetRepresentation(1.0f).GetBitmap(),
      gfx::ImageSkiaOperations::ExtractSubset(bar_rendering.AsImageSkia(),
                                              icon_portion)
          .GetRepresentation(1.0f)
          .GetBitmap()));
}

class BrowserActionApiTest : public ExtensionApiTest {
 public:
  BrowserActionApiTest() {}
  ~BrowserActionApiTest() override {}

  void SetUpOnMainThread() override {
    ExtensionApiTest::SetUpOnMainThread();
    host_resolver()->AddRule("*", "127.0.0.1");
  }

 protected:
  ExtensionActionTestHelper* GetBrowserActionsBar() {
    if (!browser_action_test_util_)
      browser_action_test_util_ = ExtensionActionTestHelper::Create(browser());
    return browser_action_test_util_.get();
  }

  WebContents* OpenPopup(int index) {
    ResultCatcher catcher;
    content::WindowedNotificationObserver popup_observer(
        content::NOTIFICATION_LOAD_COMPLETED_MAIN_FRAME,
        content::NotificationService::AllSources());
    GetBrowserActionsBar()->Press(index);
    popup_observer.Wait();
    EXPECT_TRUE(catcher.GetNextResult()) << catcher.message();

    if (!GetBrowserActionsBar()->HasPopup())
      return nullptr;

    const auto& source = static_cast<const content::Source<WebContents>&>(
        popup_observer.source());
    return source.ptr();
  }

  ExtensionAction* GetBrowserAction(Browser* browser,
                                    const Extension& extension) {
    ExtensionAction* extension_action =
        ExtensionActionManager::Get(browser->profile())
            ->GetExtensionAction(extension);
    return extension_action->action_type() == ActionInfo::TYPE_BROWSER
               ? extension_action
               : nullptr;
  }

 private:
  std::unique_ptr<ExtensionActionTestHelper> browser_action_test_util_;

  DISALLOW_COPY_AND_ASSIGN(BrowserActionApiTest);
};

// Canvas tests rely on the harness producing pixel output in order to read back
// pixels from a canvas element. So we have to override the setup function.
class BrowserActionApiCanvasTest : public BrowserActionApiTest {
 public:
  void SetUp() override {
    EnablePixelOutput();
    ExtensionApiTest::SetUp();
  }
};

enum TestFlags {
  kNone = 0,
  kUseServiceWorker = 1,
  kUseExtensionsMenuUi = 1 << 1,
};

class BrowserActionApiLazyTest : public BrowserActionApiTest,
                                 public testing::WithParamInterface<int> {
 public:
  void SetUp() override {
    BrowserActionApiTest::SetUp();
    // Service Workers are currently only available on certain channels, so set
    // the channel for those tests.
    if ((GetParam() & kUseServiceWorker) != 0) {
      current_channel_ =
          std::make_unique<extensions::ScopedWorkerBasedExtensionsChannel>();
    }

    if ((GetParam() & kUseExtensionsMenuUi) != 0) {
      feature_list_.InitAndEnableFeature(features::kExtensionsToolbarMenu);
    } else {
      feature_list_.InitAndDisableFeature(features::kExtensionsToolbarMenu);
    }
  }

  const extensions::Extension* LoadExtensionWithParamFlags(
      const base::FilePath& path) {
    int flags = kFlagEnableFileAccess;
    if ((GetParam() & kUseServiceWorker) != 0)
      flags |= ExtensionBrowserTest::kFlagRunAsServiceWorkerBasedExtension;
    return LoadExtensionWithFlags(path, flags);
  }

 private:
  base::test::ScopedFeatureList feature_list_;
  std::unique_ptr<extensions::ScopedWorkerBasedExtensionsChannel>
      current_channel_;
};

// Watches a frame is swapped with a new frame by e.g., navigation.
class RenderFrameChangedWatcher : public content::WebContentsObserver {
 public:
  explicit RenderFrameChangedWatcher(WebContents* web_contents)
      : WebContentsObserver(web_contents) {}

  void RenderFrameHostChanged(content::RenderFrameHost* old_host,
                              content::RenderFrameHost* new_host) override {
    created_frame_ = new_host;
    run_loop_.Quit();
  }

  content::RenderFrameHost* WaitAndReturnNewFrame() {
    run_loop_.Run();
    return created_frame_;
  }

 private:
  base::RunLoop run_loop_;
  content::RenderFrameHost* created_frame_;
};

IN_PROC_BROWSER_TEST_P(BrowserActionApiLazyTest, Basic) {
  ExtensionTestMessageListener ready_listener("ready", false);
  ASSERT_TRUE(embedded_test_server()->Start());
  const Extension* extension = LoadExtensionWithParamFlags(
      test_data_dir_.AppendASCII("browser_action/basics"));
  ASSERT_TRUE(extension) << message_;

  // Test that there is a browser action in the toolbar.
  ASSERT_EQ(1, GetBrowserActionsBar()->NumberOfBrowserActions());

  ASSERT_TRUE(ready_listener.WaitUntilSatisfied());

  // Open a URL in the tab, so the event handler can check the tab's
  // "url" and "title" properties.
  ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("/extensions/test_file.txt"));

  ResultCatcher catcher;
  // Simulate the browser action being clicked.
  ExecuteExtensionAction(browser(), extension);

  EXPECT_TRUE(catcher.GetNextResult());
}

IN_PROC_BROWSER_TEST_P(BrowserActionApiLazyTest, Update) {
  ExtensionTestMessageListener ready_listener("ready", true);
  ASSERT_TRUE(embedded_test_server()->Start());
  const Extension* extension = LoadExtensionWithParamFlags(
      test_data_dir_.AppendASCII("browser_action/update"));
  ASSERT_TRUE(extension) << message_;
  // Test that there is a browser action in the toolbar.
  ASSERT_EQ(1, GetBrowserActionsBar()->NumberOfBrowserActions());

  ASSERT_TRUE(ready_listener.WaitUntilSatisfied());
  ExtensionAction* action = GetBrowserAction(browser(), *extension);
  EXPECT_EQ("This is the default title.",
            action->GetTitle(ExtensionAction::kDefaultTabId));
  EXPECT_EQ("",
            action->GetExplicitlySetBadgeText(ExtensionAction::kDefaultTabId));
  EXPECT_EQ(SkColorSetARGB(0, 0, 0, 0),
            action->GetBadgeBackgroundColor(ExtensionAction::kDefaultTabId));

  // Tell the extension to update the browser action state and then
  // catch the result.
  ResultCatcher catcher;
  ready_listener.Reply("update");
  ASSERT_TRUE(catcher.GetNextResult());

  // Test that we received the changes.
  EXPECT_EQ("Modified", action->GetTitle(ExtensionAction::kDefaultTabId));
  EXPECT_EQ("badge",
            action->GetExplicitlySetBadgeText(ExtensionAction::kDefaultTabId));
  EXPECT_EQ(SkColorSetARGB(255, 255, 255, 255),
            action->GetBadgeBackgroundColor(ExtensionAction::kDefaultTabId));
}

INSTANTIATE_TEST_SUITE_P(EventPageAndLegacyToolbar,
                         BrowserActionApiLazyTest,
                         ::testing::Values(kNone));
INSTANTIATE_TEST_SUITE_P(EventPageAndExtensionsMenu,
                         BrowserActionApiLazyTest,
                         ::testing::Values(kUseExtensionsMenuUi));
INSTANTIATE_TEST_SUITE_P(ServiceWorkerAndLegacyToolbar,
                         BrowserActionApiLazyTest,
                         ::testing::Values(kUseServiceWorker));
INSTANTIATE_TEST_SUITE_P(ServiceWorkerAndExtensionsMenu,
                         BrowserActionApiLazyTest,
                         ::testing::Values(kUseServiceWorker |
                                           kUseExtensionsMenuUi));

IN_PROC_BROWSER_TEST_F(BrowserActionApiCanvasTest, DynamicBrowserAction) {
  ASSERT_TRUE(RunExtensionTest("browser_action/no_icon")) << message_;
  const Extension* extension = GetSingleLoadedExtension();
  ASSERT_TRUE(extension) << message_;

#if defined (OS_MACOSX)
  // We need this on mac so we don't loose 2x representations from browser icon
  // in transformations gfx::ImageSkia -> NSImage -> gfx::ImageSkia.
  std::vector<ui::ScaleFactor> supported_scale_factors;
  supported_scale_factors.push_back(ui::SCALE_FACTOR_100P);
  supported_scale_factors.push_back(ui::SCALE_FACTOR_200P);
  ui::SetSupportedScaleFactors(supported_scale_factors);
#endif

  // We should not be creating icons asynchronously, so we don't need an
  // observer.
  ExtensionActionIconFactory icon_factory(
      profile(), extension, GetBrowserAction(browser(), *extension), nullptr);
  // Test that there is a browser action in the toolbar.
  ASSERT_EQ(1, GetBrowserActionsBar()->NumberOfBrowserActions());
  EXPECT_TRUE(GetBrowserActionsBar()->HasIcon(0));

  gfx::Image action_icon = icon_factory.GetIcon(0);
  uint32_t action_icon_last_id = action_icon.ToSkBitmap()->getGenerationID();

  // Let's check that |GetIcon| doesn't always return bitmap with new id.
  ASSERT_EQ(action_icon_last_id,
            icon_factory.GetIcon(0).ToSkBitmap()->getGenerationID());

  gfx::Image last_bar_icon = GetBrowserActionsBar()->GetIcon(0);
  EXPECT_TRUE(gfx::test::AreImagesEqual(last_bar_icon,
                                        GetBrowserActionsBar()->GetIcon(0)));

  // The reason we don't test more standard scales (like 1x, 2x, etc.) is that
  // these may be generated from the provided scales.
  float kSmallIconScale = 21.f / ExtensionAction::ActionIconSize();
  float kLargeIconScale = 42.f / ExtensionAction::ActionIconSize();
  ASSERT_FALSE(ui::IsSupportedScale(kSmallIconScale));
  ASSERT_FALSE(ui::IsSupportedScale(kLargeIconScale));

  // Tell the extension to update the icon using ImageData object.
  ResultCatcher catcher;
  GetBrowserActionsBar()->Press(0);
  ASSERT_TRUE(catcher.GetNextResult());

  EXPECT_FALSE(gfx::test::AreImagesEqual(last_bar_icon,
                                         GetBrowserActionsBar()->GetIcon(0)));
  last_bar_icon = GetBrowserActionsBar()->GetIcon(0);

  action_icon = icon_factory.GetIcon(0);
  uint32_t action_icon_current_id = action_icon.ToSkBitmap()->getGenerationID();
  EXPECT_GT(action_icon_current_id, action_icon_last_id);
  action_icon_last_id = action_icon_current_id;
  VerifyIconsMatch(last_bar_icon, action_icon);

  // Check that only the smaller size was set (only a 21px icon was provided).
  EXPECT_TRUE(action_icon.ToImageSkia()->HasRepresentation(kSmallIconScale));
  EXPECT_FALSE(action_icon.ToImageSkia()->HasRepresentation(kLargeIconScale));

  // Tell the extension to update the icon using path.
  GetBrowserActionsBar()->Press(0);
  ASSERT_TRUE(catcher.GetNextResult());

  // Make sure the browser action bar updated.
  EXPECT_FALSE(gfx::test::AreImagesEqual(last_bar_icon,
                                         GetBrowserActionsBar()->GetIcon(0)));
  last_bar_icon = GetBrowserActionsBar()->GetIcon(0);

  action_icon = icon_factory.GetIcon(0);
  action_icon_current_id = action_icon.ToSkBitmap()->getGenerationID();
  EXPECT_GT(action_icon_current_id, action_icon_last_id);
  action_icon_last_id = action_icon_current_id;
  VerifyIconsMatch(last_bar_icon, action_icon);

  // Check that only the smaller size was set (only a 21px icon was provided).
  EXPECT_TRUE(action_icon.ToImageSkia()->HasRepresentation(kSmallIconScale));
  EXPECT_FALSE(action_icon.ToImageSkia()->HasRepresentation(kLargeIconScale));

  // Tell the extension to update the icon using dictionary of ImageData
  // objects.
  GetBrowserActionsBar()->Press(0);
  ASSERT_TRUE(catcher.GetNextResult());

  EXPECT_FALSE(gfx::test::AreImagesEqual(last_bar_icon,
                                         GetBrowserActionsBar()->GetIcon(0)));
  last_bar_icon = GetBrowserActionsBar()->GetIcon(0);

  action_icon = icon_factory.GetIcon(0);
  action_icon_current_id = action_icon.ToSkBitmap()->getGenerationID();
  EXPECT_GT(action_icon_current_id, action_icon_last_id);
  action_icon_last_id = action_icon_current_id;
  VerifyIconsMatch(last_bar_icon, action_icon);

  // Check both sizes were set (as two icon sizes were provided).
  EXPECT_TRUE(action_icon.ToImageSkia()->HasRepresentation(kSmallIconScale));
  EXPECT_TRUE(action_icon.AsImageSkia().HasRepresentation(kLargeIconScale));

  // Tell the extension to update the icon using dictionary of paths.
  GetBrowserActionsBar()->Press(0);
  ASSERT_TRUE(catcher.GetNextResult());

  EXPECT_FALSE(gfx::test::AreImagesEqual(last_bar_icon,
                                         GetBrowserActionsBar()->GetIcon(0)));
  last_bar_icon = GetBrowserActionsBar()->GetIcon(0);

  action_icon = icon_factory.GetIcon(0);
  action_icon_current_id = action_icon.ToSkBitmap()->getGenerationID();
  EXPECT_GT(action_icon_current_id, action_icon_last_id);
  action_icon_last_id = action_icon_current_id;
  VerifyIconsMatch(last_bar_icon, action_icon);

  // Check both sizes were set (as two icon sizes were provided).
  EXPECT_TRUE(action_icon.ToImageSkia()->HasRepresentation(kSmallIconScale));
  EXPECT_TRUE(action_icon.AsImageSkia().HasRepresentation(kLargeIconScale));

  // Tell the extension to update the icon using dictionary of ImageData
  // objects, but setting only one size.
  GetBrowserActionsBar()->Press(0);
  ASSERT_TRUE(catcher.GetNextResult());

  EXPECT_FALSE(gfx::test::AreImagesEqual(last_bar_icon,
                                         GetBrowserActionsBar()->GetIcon(0)));
  last_bar_icon = GetBrowserActionsBar()->GetIcon(0);

  action_icon = icon_factory.GetIcon(0);
  action_icon_current_id = action_icon.ToSkBitmap()->getGenerationID();
  EXPECT_GT(action_icon_current_id, action_icon_last_id);
  action_icon_last_id = action_icon_current_id;
  VerifyIconsMatch(last_bar_icon, action_icon);

  // Check that only the smaller size was set (only a 21px icon was provided).
  EXPECT_TRUE(action_icon.ToImageSkia()->HasRepresentation(kSmallIconScale));
  EXPECT_FALSE(action_icon.ToImageSkia()->HasRepresentation(kLargeIconScale));

  // Tell the extension to update the icon using dictionary of paths, but
  // setting only one size.
  GetBrowserActionsBar()->Press(0);
  ASSERT_TRUE(catcher.GetNextResult());

  EXPECT_FALSE(gfx::test::AreImagesEqual(last_bar_icon,
                                         GetBrowserActionsBar()->GetIcon(0)));
  last_bar_icon = GetBrowserActionsBar()->GetIcon(0);

  action_icon = icon_factory.GetIcon(0);
  action_icon_current_id = action_icon.ToSkBitmap()->getGenerationID();
  EXPECT_GT(action_icon_current_id, action_icon_last_id);
  action_icon_last_id = action_icon_current_id;
  VerifyIconsMatch(last_bar_icon, action_icon);

  // Check that only the smaller size was set (only a 21px icon was provided).
  EXPECT_TRUE(action_icon.ToImageSkia()->HasRepresentation(kSmallIconScale));
  EXPECT_FALSE(action_icon.ToImageSkia()->HasRepresentation(kLargeIconScale));

  // Tell the extension to update the icon using dictionary of ImageData
  // objects, but setting only size 42.
  GetBrowserActionsBar()->Press(0);
  ASSERT_TRUE(catcher.GetNextResult());

  EXPECT_FALSE(gfx::test::AreImagesEqual(last_bar_icon,
                                         GetBrowserActionsBar()->GetIcon(0)));
  last_bar_icon = GetBrowserActionsBar()->GetIcon(0);

  action_icon = icon_factory.GetIcon(0);
  action_icon_current_id = action_icon.ToSkBitmap()->getGenerationID();
  EXPECT_GT(action_icon_current_id, action_icon_last_id);
  action_icon_last_id = action_icon_current_id;

  // Check that only the larger size was set (only a 42px icon was provided).
  EXPECT_FALSE(action_icon.ToImageSkia()->HasRepresentation(kSmallIconScale));
  EXPECT_TRUE(action_icon.AsImageSkia().HasRepresentation(kLargeIconScale));

  // Try setting icon with empty dictionary of ImageData objects.
  GetBrowserActionsBar()->Press(0);
  ASSERT_FALSE(catcher.GetNextResult());
  EXPECT_EQ(kEmptyImageDataError, catcher.message());

  // Try setting icon with empty dictionary of path objects.
  GetBrowserActionsBar()->Press(0);
  ASSERT_FALSE(catcher.GetNextResult());
  EXPECT_EQ(kEmptyPathError, catcher.message());
}

// https://crbug.com/1019669; flaky on ChromeOS.
#if defined(OS_CHROMEOS)
#define MAYBE_InvisibleIconBrowserAction DISABLED_InvisibleIconBrowserAction
#else
#define MAYBE_InvisibleIconBrowserAction InvisibleIconBrowserAction
#endif
IN_PROC_BROWSER_TEST_F(BrowserActionApiCanvasTest,
                       MAYBE_InvisibleIconBrowserAction) {
  // Turn this on so errors are reported.
  ExtensionActionSetIconFunction::SetReportErrorForInvisibleIconForTesting(
      true);
  ASSERT_TRUE(RunExtensionTest("browser_action/invisible_icon")) << message_;
  const Extension* extension = GetSingleLoadedExtension();
  ASSERT_TRUE(extension) << message_;

  // Test there is a browser action in the toolbar.
  ASSERT_EQ(1, GetBrowserActionsBar()->NumberOfBrowserActions());
  EXPECT_TRUE(GetBrowserActionsBar()->HasIcon(0));
  gfx::Image initial_bar_icon = GetBrowserActionsBar()->GetIcon(0);

  ExtensionHost* background_page =
      ProcessManager::Get(profile())->GetBackgroundHostForExtension(
          extension->id());
  ASSERT_TRUE(background_page);

  static constexpr char kScript[] =
      "setIcon(%s).then(function(arg) {"
      "  domAutomationController.send(arg);"
      "});";

  const std::string histogram_name =
      "Extensions.DynamicExtensionActionIconWasVisible";
  const std::string new_histogram_name =
      "Extensions.DynamicExtensionActionIconWasVisibleRendered";
  {
    base::HistogramTester histogram_tester;
    std::string result;
    EXPECT_TRUE(ExecuteScriptAndExtractString(
        background_page->host_contents(),
        base::StringPrintf(kScript, "invisibleImageData"), &result));
    EXPECT_EQ("Icon not sufficiently visible.", result);
    // The icon should not have changed.
    EXPECT_TRUE(gfx::test::AreImagesEqual(initial_bar_icon,
                                          GetBrowserActionsBar()->GetIcon(0)));
    EXPECT_THAT(histogram_tester.GetAllSamples(histogram_name),
                testing::ElementsAre(base::Bucket(0, 1)));
    EXPECT_THAT(histogram_tester.GetAllSamples(new_histogram_name),
                testing::ElementsAre(base::Bucket(0, 1)));
  }

  {
    base::HistogramTester histogram_tester;
    std::string result;
    EXPECT_TRUE(ExecuteScriptAndExtractString(
        background_page->host_contents(),
        base::StringPrintf(kScript, "visibleImageData"), &result));
    EXPECT_EQ("", result);
    // The icon should have changed.
    EXPECT_FALSE(gfx::test::AreImagesEqual(initial_bar_icon,
                                           GetBrowserActionsBar()->GetIcon(0)));
    EXPECT_THAT(histogram_tester.GetAllSamples(histogram_name),
                testing::ElementsAre(base::Bucket(1, 1)));
    EXPECT_THAT(histogram_tester.GetAllSamples(new_histogram_name),
                testing::ElementsAre(base::Bucket(1, 1)));
  }
}

IN_PROC_BROWSER_TEST_F(BrowserActionApiTest, TabSpecificBrowserActionState) {
  ASSERT_TRUE(RunExtensionTest("browser_action/tab_specific_state")) <<
      message_;
  const Extension* extension = GetSingleLoadedExtension();
  ASSERT_TRUE(extension) << message_;

  // Test that there is a browser action in the toolbar and that it has an icon.
  ASSERT_EQ(1, GetBrowserActionsBar()->NumberOfBrowserActions());
  EXPECT_TRUE(GetBrowserActionsBar()->HasIcon(0));

  // Execute the action, its title should change.
  ResultCatcher catcher;
  GetBrowserActionsBar()->Press(0);
  ASSERT_TRUE(catcher.GetNextResult());
  EXPECT_EQ("Showing icon 2", GetBrowserActionsBar()->GetTooltip(0));

  // Open a new tab, the title should go back.
  chrome::NewTab(browser());
  EXPECT_EQ("hi!", GetBrowserActionsBar()->GetTooltip(0));

  // Go back to first tab, changed title should reappear.
  browser()->tab_strip_model()->ActivateTabAt(
      0, {TabStripModel::GestureType::kOther});
  EXPECT_EQ("Showing icon 2", GetBrowserActionsBar()->GetTooltip(0));

  // Reload that tab, default title should come back.
  ui_test_utils::NavigateToURL(browser(), GURL("about:blank"));
  EXPECT_EQ("hi!", GetBrowserActionsBar()->GetTooltip(0));
}

// http://code.google.com/p/chromium/issues/detail?id=70829
// Mac used to be ok, but then mac 10.5 started failing too. =(
IN_PROC_BROWSER_TEST_F(BrowserActionApiTest, DISABLED_BrowserActionPopup) {
  ASSERT_TRUE(
      LoadExtension(test_data_dir_.AppendASCII("browser_action/popup")));
  ExtensionActionTestHelper* actions_bar = GetBrowserActionsBar();
  const Extension* extension = GetSingleLoadedExtension();
  ASSERT_TRUE(extension) << message_;

  // The extension's popup's size grows by |growFactor| each click.
  const int growFactor = 500;
  gfx::Size minSize = actions_bar->GetMinPopupSize();
  gfx::Size middleSize = gfx::Size(growFactor, growFactor);
  gfx::Size maxSize = actions_bar->GetMaxPopupSize();

  // Ensure that two clicks will exceed the maximum allowed size.
  ASSERT_GT(minSize.height() + growFactor * 2, maxSize.height());
  ASSERT_GT(minSize.width() + growFactor * 2, maxSize.width());

  // Simulate a click on the browser action and verify the size of the resulting
  // popup.  The first one tries to be 0x0, so it should be the min values.
  ASSERT_TRUE(OpenPopup(0));
  EXPECT_EQ(minSize, actions_bar->GetPopupSize());
  EXPECT_TRUE(actions_bar->HidePopup());

  ASSERT_TRUE(OpenPopup(0));
  EXPECT_EQ(middleSize, actions_bar->GetPopupSize());
  EXPECT_TRUE(actions_bar->HidePopup());

  // One more time, but this time it should be constrained by the max values.
  ASSERT_TRUE(OpenPopup(0));
  EXPECT_EQ(maxSize, actions_bar->GetPopupSize());
  EXPECT_TRUE(actions_bar->HidePopup());
}

// Test that calling chrome.browserAction.setPopup() can enable and change
// a popup.
IN_PROC_BROWSER_TEST_F(BrowserActionApiTest, BrowserActionAddPopup) {
  ASSERT_TRUE(RunExtensionTest("browser_action/add_popup")) << message_;
  const Extension* extension = GetSingleLoadedExtension();
  ASSERT_TRUE(extension) << message_;

  int tab_id = ExtensionTabUtil::GetTabId(
      browser()->tab_strip_model()->GetActiveWebContents());

  ExtensionAction* browser_action = GetBrowserAction(browser(), *extension);
  ASSERT_TRUE(browser_action)
      << "Browser action test extension should have a browser action.";

  ASSERT_FALSE(browser_action->HasPopup(tab_id));
  ASSERT_FALSE(browser_action->HasPopup(ExtensionAction::kDefaultTabId));

  // Simulate a click on the browser action icon.  The onClicked handler
  // will add a popup.
  {
    ResultCatcher catcher;
    GetBrowserActionsBar()->Press(0);
    ASSERT_TRUE(catcher.GetNextResult());
  }

  // The call to setPopup in background.html set a tab id, so the
  // current tab's setting should have changed, but the default setting
  // should not have changed.
  ASSERT_TRUE(browser_action->HasPopup(tab_id))
      << "Clicking on the browser action should have caused a popup to "
      << "be added.";
  ASSERT_FALSE(browser_action->HasPopup(ExtensionAction::kDefaultTabId))
      << "Clicking on the browser action should not have set a default "
      << "popup.";

  ASSERT_STREQ("/a_popup.html",
               browser_action->GetPopupUrl(tab_id).path().c_str());

  // Now change the popup from a_popup.html to another_popup.html by loading
  // a page which removes the popup using chrome.browserAction.setPopup().
  {
    ResultCatcher catcher;
    ui_test_utils::NavigateToURL(
        browser(),
        GURL(extension->GetResourceURL("change_popup.html")));
    ASSERT_TRUE(catcher.GetNextResult());
  }

  // The call to setPopup in change_popup.html did not use a tab id,
  // so the default setting should have changed as well as the current tab.
  ASSERT_TRUE(browser_action->HasPopup(tab_id));
  ASSERT_TRUE(browser_action->HasPopup(ExtensionAction::kDefaultTabId));
  ASSERT_STREQ("/another_popup.html",
               browser_action->GetPopupUrl(tab_id).path().c_str());
}

// Test that calling chrome.browserAction.setPopup() can remove a popup.
IN_PROC_BROWSER_TEST_F(BrowserActionApiTest, BrowserActionRemovePopup) {
  // Load the extension, which has a browser action with a default popup.
  ASSERT_TRUE(RunExtensionTest("browser_action/remove_popup")) << message_;
  const Extension* extension = GetSingleLoadedExtension();
  ASSERT_TRUE(extension) << message_;

  int tab_id = ExtensionTabUtil::GetTabId(
      browser()->tab_strip_model()->GetActiveWebContents());

  ExtensionAction* browser_action = GetBrowserAction(browser(), *extension);
  ASSERT_TRUE(browser_action)
      << "Browser action test extension should have a browser action.";

  ASSERT_TRUE(browser_action->HasPopup(tab_id))
      << "Expect a browser action popup before the test removes it.";
  ASSERT_TRUE(browser_action->HasPopup(ExtensionAction::kDefaultTabId))
      << "Expect a browser action popup is the default for all tabs.";

  // Load a page which removes the popup using chrome.browserAction.setPopup().
  {
    ResultCatcher catcher;
    ui_test_utils::NavigateToURL(
        browser(),
        GURL(extension->GetResourceURL("remove_popup.html")));
    ASSERT_TRUE(catcher.GetNextResult());
  }

  ASSERT_FALSE(browser_action->HasPopup(tab_id))
      << "Browser action popup should have been removed.";
  ASSERT_TRUE(browser_action->HasPopup(ExtensionAction::kDefaultTabId))
      << "Browser action popup default should not be changed by setting "
      << "a specific tab id.";
}

IN_PROC_BROWSER_TEST_P(BrowserActionApiLazyTest, IncognitoBasic) {
  ExtensionTestMessageListener ready_listener("ready", false);
  ASSERT_TRUE(embedded_test_server()->Start());
  const Extension* extension = LoadExtensionWithParamFlags(
      test_data_dir_.AppendASCII("browser_action/basics"));
  ASSERT_TRUE(extension) << message_;

  // Test that there is a browser action in the toolbar.
  ASSERT_EQ(1, GetBrowserActionsBar()->NumberOfBrowserActions());

  // Open an incognito window and test that the browser action isn't there by
  // default.
  Browser* incognito_browser = CreateIncognitoBrowser(browser()->profile());

  ASSERT_EQ(0, ExtensionActionTestHelper::Create(incognito_browser)
                   ->NumberOfBrowserActions());

  ASSERT_TRUE(ready_listener.WaitUntilSatisfied());

  // Now enable the extension in incognito mode, and test that the browser
  // action shows up.
  // SetIsIncognitoEnabled() requires a reload of the extension, so we have to
  // wait for it.
  ExtensionTestMessageListener incognito_ready_listener("ready", false);
  TestExtensionRegistryObserver registry_observer(
      ExtensionRegistry::Get(profile()), extension->id());
  extensions::util::SetIsIncognitoEnabled(
      extension->id(), browser()->profile(), true);
  extension = registry_observer.WaitForExtensionLoaded();

  ASSERT_EQ(1, ExtensionActionTestHelper::Create(incognito_browser)
                   ->NumberOfBrowserActions());

  ASSERT_TRUE(incognito_ready_listener.WaitUntilSatisfied());

  // Open a URL in the tab, so the event handler can check the tab's
  // "url" and "title" properties.
  ui_test_utils::NavigateToURL(
      incognito_browser,
      embedded_test_server()->GetURL("/extensions/test_file.txt"));

  ResultCatcher catcher;
  // Simulate the browser action being clicked.
  ExecuteExtensionAction(incognito_browser, extension);

  EXPECT_TRUE(catcher.GetNextResult());
}

IN_PROC_BROWSER_TEST_P(BrowserActionApiLazyTest, IncognitoUpdate) {
  // TODO(crbug.com/1015136): Investigate flakiness WRT Service Workers and
  // incognito mode.
  if ((GetParam() & kUseServiceWorker) != 0)
    return;
  ASSERT_TRUE(embedded_test_server()->Start());
  const Extension* extension = LoadExtensionWithParamFlags(
      test_data_dir_.AppendASCII("browser_action/update"));
  ASSERT_TRUE(extension) << message_;
  // Test that there is a browser action in the toolbar.
  ASSERT_EQ(1, GetBrowserActionsBar()->NumberOfBrowserActions());

  // Open an incognito window and test that the browser action isn't there by
  // default.
  Browser* incognito_browser = CreateIncognitoBrowser(browser()->profile());

  ASSERT_EQ(0, ExtensionActionTestHelper::Create(incognito_browser)
                   ->NumberOfBrowserActions());

  // Set up a listener so we can reply for the extension to do the update.
  ExtensionTestMessageListener incognito_ready_listener("incognito ready",
                                                        true);
  // Now enable the extension in incognito mode, and test that the browser
  // action shows up.
  // SetIsIncognitoEnabled() requires a reload of the extension, so we have to
  // wait for it.
  TestExtensionRegistryObserver registry_observer(
      ExtensionRegistry::Get(profile()), extension->id());
  extensions::util::SetIsIncognitoEnabled(extension->id(), browser()->profile(),
                                          true);
  extension = registry_observer.WaitForExtensionLoaded();
  ASSERT_EQ(1, ExtensionActionTestHelper::Create(incognito_browser)
                   ->NumberOfBrowserActions());

  ASSERT_TRUE(incognito_ready_listener.WaitUntilSatisfied());
  ExtensionAction* action = GetBrowserAction(incognito_browser, *extension);
  EXPECT_EQ("This is the default title.",
            action->GetTitle(ExtensionAction::kDefaultTabId));
  EXPECT_EQ("",
            action->GetExplicitlySetBadgeText(ExtensionAction::kDefaultTabId));
  EXPECT_EQ(SkColorSetARGB(0, 0, 0, 0),
            action->GetBadgeBackgroundColor(ExtensionAction::kDefaultTabId));
  // Tell the extension to update the browser action state and then
  // catch the result.
  ResultCatcher incognito_catcher;
  incognito_ready_listener.Reply("incognito update");
  ASSERT_TRUE(incognito_catcher.GetNextResult());

  // Test that we received the changes.
  EXPECT_EQ("Modified", action->GetTitle(ExtensionAction::kDefaultTabId));
  EXPECT_EQ("badge",
            action->GetExplicitlySetBadgeText(ExtensionAction::kDefaultTabId));
  EXPECT_EQ(SkColorSetARGB(255, 255, 255, 255),
            action->GetBadgeBackgroundColor(ExtensionAction::kDefaultTabId));
}

// Tests that events are dispatched to the correct profile for split mode
// extensions.
IN_PROC_BROWSER_TEST_F(BrowserActionApiTest, IncognitoSplit) {
  ResultCatcher catcher;
  const Extension* extension = LoadExtensionWithFlags(
      test_data_dir_.AppendASCII("browser_action/split_mode"),
      kFlagEnableIncognito);
  ASSERT_TRUE(extension) << message_;

  // Open an incognito browser.
  Browser* incognito_browser = CreateIncognitoBrowser(browser()->profile());
  ASSERT_EQ(1, ExtensionActionTestHelper::Create(incognito_browser)
                   ->NumberOfBrowserActions());

  // A click in the regular profile should open a tab in the regular profile.
  ExecuteExtensionAction(browser(), extension);
  ASSERT_TRUE(catcher.GetNextResult()) << catcher.message();

  // A click in the incognito profile should open a tab in the
  // incognito profile.
  ExecuteExtensionAction(incognito_browser, extension);
  ASSERT_TRUE(catcher.GetNextResult()) << catcher.message();
}

IN_PROC_BROWSER_TEST_F(BrowserActionApiTest, CloseBackgroundPage) {
  ExtensionTestMessageListener listener("ready", /*will_reply=*/false);
  ASSERT_TRUE(LoadExtension(
      test_data_dir_.AppendASCII("browser_action/close_background")));
  const Extension* extension = GetSingleLoadedExtension();
  ASSERT_TRUE(listener.WaitUntilSatisfied());

  // There is a background page and a browser action with no badge text.
  extensions::ProcessManager* manager =
      extensions::ProcessManager::Get(browser()->profile());

  ExtensionHost* extension_host =
      manager->GetBackgroundHostForExtension(extension->id());
  ASSERT_TRUE(extension_host);

  ExtensionAction* action = GetBrowserAction(browser(), *extension);
  ASSERT_EQ("",
            action->GetExplicitlySetBadgeText(ExtensionAction::kDefaultTabId));

  // A helper class to wait for the ExtensionHost to shut down.
  // TODO(devlin): Hoist this somewhere more common and track down other similar
  // usages.
  class ExtensionHostDestructionObserver : public ExtensionHostObserver {
   public:
    explicit ExtensionHostDestructionObserver(ExtensionHost* host) {
      host_observer_.Add(host);
    }
    ExtensionHostDestructionObserver(
        const ExtensionHostDestructionObserver& other) = delete;
    ExtensionHostDestructionObserver& operator=(
        const ExtensionHostDestructionObserver& other) = delete;
    ~ExtensionHostDestructionObserver() override = default;

    void OnExtensionHostDestroyed(const ExtensionHost* host) override {
      // TODO(devlin): It would be nice to
      // ASSERT_TRUE(host_observer_.IsObserving(host));
      // host_observer_.Remove(host);
      // But we can't, because |host| is const. Work around it by just
      // RemoveAll()ing.
      host_observer_.RemoveAll();
      run_loop_.QuitWhenIdle();
    }

    void Wait() { run_loop_.Run(); }

   private:
    base::RunLoop run_loop_;
    ScopedObserver<ExtensionHost, ExtensionHostObserver> host_observer_{this};
  };

  ExtensionHostDestructionObserver host_destroyed_observer(extension_host);

  // Click the browser action.
  ExecuteExtensionAction(browser(), extension);

  host_destroyed_observer.Wait();

  EXPECT_FALSE(manager->GetBackgroundHostForExtension(extension->id()));
  EXPECT_EQ("X",
            action->GetExplicitlySetBadgeText(ExtensionAction::kDefaultTabId));
}

IN_PROC_BROWSER_TEST_F(BrowserActionApiTest, BadgeBackgroundColor) {
  ASSERT_TRUE(embedded_test_server()->Start());
  ASSERT_TRUE(RunExtensionTest("browser_action/color")) << message_;
  const Extension* extension = GetSingleLoadedExtension();
  ASSERT_TRUE(extension) << message_;

  // Test that there is a browser action in the toolbar.
  ASSERT_EQ(1, GetBrowserActionsBar()->NumberOfBrowserActions());

  // Test that CSS values (#FF0000) set color correctly.
  ExtensionAction* action = GetBrowserAction(browser(), *extension);
  ASSERT_EQ(SkColorSetARGB(255, 255, 0, 0),
            action->GetBadgeBackgroundColor(ExtensionAction::kDefaultTabId));

  // Tell the extension to update the browser action state.
  ResultCatcher catcher;
  ui_test_utils::NavigateToURL(browser(),
      GURL(extension->GetResourceURL("update.html")));
  ASSERT_TRUE(catcher.GetNextResult());

  // Test that CSS values (#0F0) set color correctly.
  ASSERT_EQ(SkColorSetARGB(255, 0, 255, 0),
            action->GetBadgeBackgroundColor(ExtensionAction::kDefaultTabId));

  ui_test_utils::NavigateToURL(browser(),
      GURL(extension->GetResourceURL("update2.html")));
  ASSERT_TRUE(catcher.GetNextResult());

  // Test that array values set color correctly.
  ASSERT_EQ(SkColorSetARGB(255, 255, 255, 255),
            action->GetBadgeBackgroundColor(ExtensionAction::kDefaultTabId));

  ui_test_utils::NavigateToURL(browser(),
                               GURL(extension->GetResourceURL("update3.html")));
  ASSERT_TRUE(catcher.GetNextResult());

  // Test that hsl() values 'hsl(120, 100%, 50%)' set color correctly.
  ASSERT_EQ(SkColorSetARGB(255, 0, 255, 0),
            action->GetBadgeBackgroundColor(ExtensionAction::kDefaultTabId));

  // Test basic color keyword set correctly.
  ui_test_utils::NavigateToURL(browser(),
                               GURL(extension->GetResourceURL("update4.html")));
  ASSERT_TRUE(catcher.GetNextResult());

  ASSERT_EQ(SkColorSetARGB(255, 0, 0, 255),
            action->GetBadgeBackgroundColor(ExtensionAction::kDefaultTabId));
}

IN_PROC_BROWSER_TEST_F(BrowserActionApiTest, Getters) {
  ASSERT_TRUE(RunExtensionTest("browser_action/getters")) << message_;
  const Extension* extension = GetSingleLoadedExtension();
  ASSERT_TRUE(extension) << message_;

  // Test that there is a browser action in the toolbar.
  ASSERT_EQ(1, GetBrowserActionsBar()->NumberOfBrowserActions());

  // Test the getters for defaults.
  ResultCatcher catcher;
  ui_test_utils::NavigateToURL(browser(),
      GURL(extension->GetResourceURL("update.html")));
  ASSERT_TRUE(catcher.GetNextResult());

  // Test the getters for a specific tab.
  ui_test_utils::NavigateToURL(browser(),
      GURL(extension->GetResourceURL("update2.html")));
  ASSERT_TRUE(catcher.GetNextResult());
}

// Verify triggering browser action.
IN_PROC_BROWSER_TEST_F(BrowserActionApiTest, TestTriggerBrowserAction) {
  ASSERT_TRUE(embedded_test_server()->Start());

  ASSERT_TRUE(RunExtensionTest("trigger_actions/browser_action")) << message_;
  const Extension* extension = GetSingleLoadedExtension();
  ASSERT_TRUE(extension) << message_;

  // Test that there is a browser action in the toolbar.
  ASSERT_EQ(1, GetBrowserActionsBar()->NumberOfBrowserActions());

  ui_test_utils::NavigateToURL(browser(),
                               embedded_test_server()->GetURL("/simple.html"));

  ExtensionAction* browser_action = GetBrowserAction(browser(), *extension);
  EXPECT_TRUE(browser_action);

  // Simulate a click on the browser action icon.
  {
    ResultCatcher catcher;
    GetBrowserActionsBar()->Press(0);
    EXPECT_TRUE(catcher.GetNextResult());
  }

  WebContents* tab =
      browser()->tab_strip_model()->GetActiveWebContents();
  EXPECT_TRUE(tab);

  // Verify that the browser action turned the background color red.
  const std::string script =
      "window.domAutomationController.send(document.body.style."
      "backgroundColor);";
  std::string result;
  EXPECT_TRUE(content::ExecuteScriptAndExtractString(tab, script, &result));
  EXPECT_EQ(result, "red");
}

// Test that a browser action popup with a web iframe works correctly. The
// iframe is expected to run in a separate process.
// See https://crbug.com/546267.
IN_PROC_BROWSER_TEST_F(BrowserActionApiTest, BrowserActionPopupWithIframe) {
  ASSERT_TRUE(embedded_test_server()->Start());

  ASSERT_TRUE(LoadExtension(
      test_data_dir_.AppendASCII("browser_action/popup_with_iframe")));
  ExtensionActionTestHelper* actions_bar = GetBrowserActionsBar();
  const Extension* extension = GetSingleLoadedExtension();
  ASSERT_TRUE(extension) << message_;

  // Simulate a click on the browser action to open the popup.
  ASSERT_TRUE(OpenPopup(0));

  // Find the RenderFrameHost associated with the iframe in the popup.
  content::RenderFrameHost* frame_host = nullptr;
  extensions::ProcessManager* manager =
      extensions::ProcessManager::Get(browser()->profile());
  std::set<content::RenderFrameHost*> frame_hosts =
      manager->GetRenderFrameHostsForExtension(extension->id());
  for (auto* host : frame_hosts) {
    if (host->GetFrameName() == "child_frame") {
      frame_host = host;
      break;
    }
  }

  ASSERT_TRUE(frame_host);
  EXPECT_EQ(extension->GetResourceURL("frame.html"),
            frame_host->GetLastCommittedURL());
  EXPECT_TRUE(frame_host->GetParent());

  // Navigate the popup's iframe to a (cross-site) web page, and wait for that
  // page to send a message, which will ensure that the page has loaded.
  RenderFrameChangedWatcher watcher(
      WebContents::FromRenderFrameHost(frame_host));
  GURL foo_url(embedded_test_server()->GetURL("foo.com", "/popup_iframe.html"));
  std::string script = "location.href = '" + foo_url.spec() + "'";
  EXPECT_TRUE(ExecuteScript(frame_host, script));

  frame_host = watcher.WaitAndReturnNewFrame();

  // Confirm that the new page (popup_iframe.html) is actually loaded.
  content::DOMMessageQueue dom_message_queue(frame_host);
  std::string json;
  EXPECT_TRUE(dom_message_queue.WaitForMessage(&json));
  EXPECT_EQ("\"DONE\"", json);

  EXPECT_TRUE(actions_bar->HidePopup());
}

IN_PROC_BROWSER_TEST_F(BrowserActionApiTest, BrowserActionWithRectangularIcon) {
  ExtensionTestMessageListener ready_listener("ready", true);

  const Extension* extension = LoadExtension(
      test_data_dir_.AppendASCII("browser_action").AppendASCII("rect_icon"));
  ASSERT_TRUE(extension);
  EXPECT_TRUE(ready_listener.WaitUntilSatisfied());

  // Wait for the default icon to load before accessing the underlying
  // gfx::Image.
  TestIconImageObserver::WaitForExtensionActionIcon(extension, profile());

  gfx::Image first_icon = GetBrowserActionsBar()->GetIcon(0);
  ASSERT_FALSE(first_icon.IsEmpty());

  TestExtensionActionAPIObserver observer(profile(), extension->id());
  ResultCatcher catcher;
  ready_listener.Reply(std::string());
  EXPECT_TRUE(catcher.GetNextResult());
  // Wait for extension action to be updated.
  observer.Wait();

  gfx::Image next_icon = GetBrowserActionsBar()->GetIcon(0);
  ASSERT_FALSE(next_icon.IsEmpty());
  EXPECT_FALSE(gfx::test::AreImagesEqual(first_icon, next_icon));
}

// Test that we don't try and show a browser action popup with
// browserAction.openPopup if there is no toolbar (e.g., for web popup windows).
// Regression test for crbug.com/584747.
IN_PROC_BROWSER_TEST_F(BrowserActionApiTest, BrowserActionOpenPopupOnPopup) {
  // Open a new web popup window.
  NavigateParams params(browser(), GURL("http://www.google.com/"),
                        ui::PAGE_TRANSITION_LINK);
  params.disposition = WindowOpenDisposition::NEW_POPUP;
  params.window_action = NavigateParams::SHOW_WINDOW;
  ui_test_utils::NavigateToURL(&params);
  Browser* popup_browser = params.browser;
  // Verify it is a popup, and it is the active window.
  ASSERT_TRUE(popup_browser);
  // The window isn't considered "active" on MacOSX for odd reasons. The more
  // important test is that it *is* considered the last active browser, since
  // that's what we check when we try to open the popup.
#if !defined(OS_MACOSX)
  EXPECT_TRUE(popup_browser->window()->IsActive());
#endif
  EXPECT_FALSE(browser()->window()->IsActive());
  EXPECT_FALSE(popup_browser->SupportsWindowFeature(Browser::FEATURE_TOOLBAR));
  EXPECT_EQ(popup_browser,
            chrome::FindLastActiveWithProfile(browser()->profile()));

  // Load up the extension, which will call chrome.browserAction.openPopup()
  // when it is loaded and verify that the popup didn't open.
  ExtensionTestMessageListener listener("ready", true);
  EXPECT_TRUE(LoadExtension(
      test_data_dir_.AppendASCII("browser_action/open_popup_on_reply")));
  EXPECT_TRUE(listener.WaitUntilSatisfied());

  ResultCatcher catcher;
  listener.Reply(std::string());
  EXPECT_TRUE(catcher.GetNextResult()) << message_;
}

// Test that a browser action popup can download data URLs. See
// https://crbug.com/821219
// Fails consistently on Win7. https://crbug.com/827160
#if defined(OS_WIN)
#define MAYBE_BrowserActionPopupDownload DISABLED_BrowserActionPopupDownload
#else
#define MAYBE_BrowserActionPopupDownload BrowserActionPopupDownload
#endif
IN_PROC_BROWSER_TEST_F(BrowserActionApiTest, MAYBE_BrowserActionPopupDownload) {
  ASSERT_TRUE(embedded_test_server()->Start());

  ASSERT_TRUE(LoadExtension(
      test_data_dir_.AppendASCII("browser_action/popup_download")));
  const Extension* extension = GetSingleLoadedExtension();
  ASSERT_TRUE(extension) << message_;

  content::DownloadTestObserverTerminal downloads_observer(
      content::BrowserContext::GetDownloadManager(browser()->profile()), 1,
      content::DownloadTestObserver::ON_DANGEROUS_DOWNLOAD_FAIL);

  // Simulate a click on the browser action to open the popup.
  content::WebContents* popup = OpenPopup(0);
  ASSERT_TRUE(popup);
  content::ExecuteScriptAsync(popup, "run_tests()");

  // Wait for the download that this should have triggered to finish.
  downloads_observer.WaitForFinished();

  EXPECT_EQ(1u, downloads_observer.NumDownloadsSeenInState(
                    download::DownloadItem::COMPLETE));
  EXPECT_TRUE(GetBrowserActionsBar()->HidePopup());
}

class NavigatingExtensionPopupBrowserTest : public BrowserActionApiTest {
 public:
  const Extension& popup_extension() { return *popup_extension_; }
  const Extension& other_extension() { return *other_extension_; }

  void SetUpOnMainThread() override {
    BrowserActionApiTest::SetUpOnMainThread();

    ASSERT_TRUE(embedded_test_server()->Start());

    // Load an extension with a pop-up.
    ASSERT_TRUE(popup_extension_ = LoadExtension(test_data_dir_.AppendASCII(
                    "browser_action/popup_with_form")));

    // Load another extension (that we can try navigating to).
    ASSERT_TRUE(other_extension_ = LoadExtension(test_data_dir_.AppendASCII(
                    "browser_action/popup_with_iframe")));
  }

  enum ExpectedNavigationStatus {
    EXPECTING_NAVIGATION_SUCCESS,
    EXPECTING_NAVIGATION_FAILURE,
  };

  void TestPopupNavigationViaGet(
      const GURL& target_url,
      ExpectedNavigationStatus expected_navigation_status) {
    std::string navigation_starting_script =
        "window.location = '" + target_url.spec() + "';\n";
    TestPopupNavigation(target_url, expected_navigation_status,
                        navigation_starting_script);
  }

  void TestPopupNavigationViaPost(
      const GURL& target_url,
      ExpectedNavigationStatus expected_navigation_status) {
    std::string navigation_starting_script =
        "var form = document.getElementById('form');\n"
        "form.action = '" + target_url.spec() + "';\n"
        "form.submit();\n";
    TestPopupNavigation(target_url, expected_navigation_status,
                        navigation_starting_script);
  }

 private:
  void TestPopupNavigation(const GURL& target_url,
                           ExpectedNavigationStatus expected_navigation_status,
                           std::string navigation_starting_script) {
    // Were there any failures so far (e.g. in SetUpOnMainThread)?
    ASSERT_FALSE(HasFailure());

    // Simulate a click on the browser action to open the popup.
    WebContents* popup = OpenPopup(0);
    ASSERT_TRUE(popup);
    GURL popup_url = popup_extension().GetResourceURL("popup.html");
    EXPECT_EQ(popup_url, popup->GetLastCommittedURL());

    // Note that the |setTimeout| call below is needed to make sure
    // ExecuteScriptAndExtractBool returns *after* a scheduled navigation has
    // already started.
    std::string script_to_execute =
        navigation_starting_script +
        "setTimeout(\n"
        "    function() { window.domAutomationController.send(true); },\n"
        "    0);\n";

    // Try to navigate the pop-up.
    bool ignored_script_result = false;
    content::WebContentsDestroyedWatcher popup_destruction_watcher(popup);
    content::TestNavigationObserver popup_navigation_observer(popup);
    EXPECT_TRUE(ExecuteScriptAndExtractBool(popup, script_to_execute,
                                            &ignored_script_result));
    popup = popup_destruction_watcher.web_contents();

    // Verify if the popup navigation succeeded or failed as expected.
    if (!popup) {
      // If navigation ends up in a tab, then the tab will be focused and
      // therefore the popup will be closed, destroying associated WebContents -
      // don't do any verification in this case.
      ADD_FAILURE() << "Navigation should not close extension pop-up";
    } else {
      // If the extension popup is still opened, then wait until there is no
      // load in progress, and verify whether the navigation succeeded or not.
      popup_navigation_observer.Wait();

      // The popup should still be alive.
      ASSERT_TRUE(popup_destruction_watcher.web_contents());

      if (expected_navigation_status == EXPECTING_NAVIGATION_SUCCESS) {
        EXPECT_EQ(target_url, popup->GetLastCommittedURL())
            << "Navigation to " << target_url
            << " should succeed in an extension pop-up";
      } else {
        EXPECT_NE(target_url, popup->GetLastCommittedURL())
            << "Navigation to " << target_url
            << " should fail in an extension pop-up";
        EXPECT_THAT(
            popup->GetLastCommittedURL(),
            ::testing::AnyOf(::testing::Eq(popup_url),
                             ::testing::Eq(GURL("chrome-extension://invalid")),
                             ::testing::Eq(GURL("about:blank"))));
      }

      // Close the pop-up.
      EXPECT_TRUE(GetBrowserActionsBar()->HidePopup());
      popup_destruction_watcher.Wait();
    }

    // Make sure that the web navigation did not succeed somewhere outside of
    // the extension popup (as it might if ExtensionViewHost::OpenURLFromTab
    // forwards the navigation to Browser::OpenURL [which doesn't specify a
    // source WebContents]).
    TabStripModel* tabs = browser()->tab_strip_model();
    for (int i = 0; i < tabs->count(); i++) {
      WebContents* tab_contents = tabs->GetWebContentsAt(i);
      WaitForLoadStop(tab_contents);
      EXPECT_NE(target_url, tab_contents->GetLastCommittedURL())
          << "Navigating an extension pop-up should not affect tabs.";
    }
  }

  const Extension* popup_extension_;
  const Extension* other_extension_;
};

// Flaky - crbug.com/1021172
#if defined(OS_LINUX)
#define MAYBE_Webpage DISABLED_Webpage
#else
#define MAYBE_Webpage Webpage
#endif
// Tests that an extension pop-up cannot be navigated to a web page.
IN_PROC_BROWSER_TEST_F(NavigatingExtensionPopupBrowserTest, MAYBE_Webpage) {
  GURL web_url(embedded_test_server()->GetURL("foo.com", "/title1.html"));
  TestPopupNavigationViaGet(web_url, EXPECTING_NAVIGATION_FAILURE);
  TestPopupNavigationViaPost(web_url, EXPECTING_NAVIGATION_FAILURE);
}

// Tests that an extension pop-up can be navigated to another page
// in the same extension.
// Times out on all platforms: https://crbug.com/882200
IN_PROC_BROWSER_TEST_F(NavigatingExtensionPopupBrowserTest,
                       DISABLED_PageInSameExtension) {
  GURL other_page_in_same_extension =
      popup_extension().GetResourceURL("other_page.html");
  TestPopupNavigationViaGet(other_page_in_same_extension,
                            EXPECTING_NAVIGATION_SUCCESS);
  TestPopupNavigationViaPost(other_page_in_same_extension,
                             EXPECTING_NAVIGATION_SUCCESS);
}

// Tests that an extension pop-up cannot be navigated to a page
// in another extension.
IN_PROC_BROWSER_TEST_F(NavigatingExtensionPopupBrowserTest,
                       PageInOtherExtension) {
  GURL other_extension_url = other_extension().GetResourceURL("other.html");
  TestPopupNavigationViaGet(other_extension_url, EXPECTING_NAVIGATION_FAILURE);
  TestPopupNavigationViaPost(other_extension_url, EXPECTING_NAVIGATION_FAILURE);
}

// Tests that navigating an extension pop-up to a http URI that returns
// Content-Disposition: attachment; filename=...
// works: No navigation, but download shelf visible + download goes through.
IN_PROC_BROWSER_TEST_F(NavigatingExtensionPopupBrowserTest, DownloadViaPost) {
  // Setup monitoring of the downloads.
  content::DownloadTestObserverTerminal downloads_observer(
      content::BrowserContext::GetDownloadManager(browser()->profile()),
      1,  // == wait_count (only waiting for "download-test3.gif").
      content::DownloadTestObserver::ON_DANGEROUS_DOWNLOAD_FAIL);

  // Navigate to a URL that replies with
  // Content-Disposition: attachment; filename=...
  // header.
  GURL download_url(
      embedded_test_server()->GetURL("foo.com", "/download-test3.gif"));
  TestPopupNavigationViaPost(download_url, EXPECTING_NAVIGATION_FAILURE);

  // Verify that "download-test3.gif got downloaded.
  downloads_observer.WaitForFinished();
  EXPECT_EQ(0u, downloads_observer.NumDangerousDownloadsSeen());
  EXPECT_EQ(1u, downloads_observer.NumDownloadsSeenInState(
                    download::DownloadItem::COMPLETE));

  base::ScopedAllowBlockingForTesting allow_blocking;
  base::FilePath downloads_directory =
      DownloadPrefs(browser()->profile()).DownloadPath();
  EXPECT_TRUE(base::PathExists(
      downloads_directory.AppendASCII("download-test3-attachment.gif")));

  // The test verification below is applicable only to scenarios where the
  // download shelf is supported - on ChromeOS, instead of the download shelf,
  // there is a download notification in the right-bottom corner of the screen.
#if !defined(OS_CHROMEOS)
  EXPECT_TRUE(browser()->window()->IsDownloadShelfVisible());
#endif
}

IN_PROC_BROWSER_TEST_F(NavigatingExtensionPopupBrowserTest, DownloadViaGet) {
  // Setup monitoring of the downloads.
  content::DownloadTestObserverTerminal downloads_observer(
      content::BrowserContext::GetDownloadManager(browser()->profile()),
      1,  // == wait_count (only waiting for "download-test3.gif").
      content::DownloadTestObserver::ON_DANGEROUS_DOWNLOAD_FAIL);

  // Navigate to a URL that replies with
  // Content-Disposition: attachment; filename=...
  // header.
  GURL download_url(
      embedded_test_server()->GetURL("foo.com", "/download-test3.gif"));
  TestPopupNavigationViaGet(download_url, EXPECTING_NAVIGATION_FAILURE);

  // Verify that "download-test3.gif got downloaded.
  downloads_observer.WaitForFinished();
  EXPECT_EQ(0u, downloads_observer.NumDangerousDownloadsSeen());
  EXPECT_EQ(1u, downloads_observer.NumDownloadsSeenInState(
                    download::DownloadItem::COMPLETE));

  base::ScopedAllowBlockingForTesting allow_blocking;
  base::FilePath downloads_directory =
      DownloadPrefs(browser()->profile()).DownloadPath();
  EXPECT_TRUE(base::PathExists(
      downloads_directory.AppendASCII("download-test3-attachment.gif")));

  // The test verification below is applicable only to scenarios where the
  // download shelf is supported - on ChromeOS, instead of the download shelf,
  // there is a download notification in the right-bottom corner of the screen.
#if !defined(OS_CHROMEOS)
  EXPECT_TRUE(browser()->window()->IsDownloadShelfVisible());
#endif
}

// Verify video can enter and exit Picture-in_Picture when browser action icon
// is clicked.
IN_PROC_BROWSER_TEST_F(BrowserActionApiTest,
                       TestPictureInPictureOnBrowserActionIconClick) {
  ASSERT_TRUE(StartEmbeddedTestServer());

  ASSERT_TRUE(
      RunExtensionTest("trigger_actions/browser_action_picture_in_picture"))
      << message_;
  const Extension* extension = GetSingleLoadedExtension();
  ASSERT_TRUE(extension) << message_;

  // Test that there is a browser action in the toolbar.
  ASSERT_EQ(1, GetBrowserActionsBar()->NumberOfBrowserActions());

  ExtensionAction* browser_action = GetBrowserAction(browser(), *extension);
  EXPECT_TRUE(browser_action);

  // Find the background page.
  ProcessManager* process_manager =
      extensions::ProcessManager::Get(browser()->profile());
  content::WebContents* web_contents =
      process_manager->GetBackgroundHostForExtension(extension->id())
          ->web_contents();
  ASSERT_TRUE(web_contents);
  content::PictureInPictureWindowController* window_controller =
      content::PictureInPictureWindowController::GetOrCreateForWebContents(
          web_contents);
  ASSERT_TRUE(window_controller->GetWindowForTesting());
  EXPECT_FALSE(window_controller->GetWindowForTesting()->IsVisible());

  // Click on the browser action icon to enter Picture-in-Picture.
  ResultCatcher catcher;
  GetBrowserActionsBar()->Press(0);
  EXPECT_TRUE(catcher.GetNextResult());
  EXPECT_TRUE(window_controller->GetWindowForTesting()->IsVisible());

  // Click on the browser action icon to exit Picture-in-Picture.
  GetBrowserActionsBar()->Press(0);
  EXPECT_TRUE(catcher.GetNextResult());
  EXPECT_FALSE(window_controller->GetWindowForTesting()->IsVisible());
}

}  // namespace
}  // namespace extensions
