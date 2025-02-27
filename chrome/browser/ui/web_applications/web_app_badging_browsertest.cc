// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/strings/string_number_conversions.h"
#include "chrome/browser/badging/badge_manager.h"
#include "chrome/browser/badging/badge_manager_factory.h"
#include "chrome/browser/badging/test_badge_manager_delegate.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/web_applications/web_app_controller_browsertest.h"
#include "chrome/browser/web_applications/components/web_app_provider_base.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_switches.h"

using content::RenderFrameHost;
using content::WebContents;

namespace web_app {

class WebAppBadgingBrowserTest : public WebAppControllerBrowserTest {
 public:
  WebAppBadgingBrowserTest()
      : WebAppControllerBrowserTest(),
        cross_origin_https_server_(net::EmbeddedTestServer::TYPE_HTTPS) {}

  void SetUpCommandLine(base::CommandLine* command_line) override {
    WebAppControllerBrowserTest::SetUpCommandLine(command_line);
    command_line->AppendSwitchASCII(switches::kEnableBlinkFeatures, "Badging");
  }

  void SetUpOnMainThread() override {
    WebAppControllerBrowserTest::SetUpOnMainThread();

    ASSERT_TRUE(cross_origin_https_server_.Start());
    ASSERT_TRUE(https_server()->Start());
    ASSERT_TRUE(embedded_test_server()->Start());

    GURL cross_site_frame_url =
        cross_origin_https_server_.GetURL("/web_app_badging/blank.html");
    cross_site_app_id_ = InstallPWA(cross_site_frame_url);

    // Note: The url for the cross site frame is embedded in the query string.
    GURL app_url = https_server()->GetURL(
        "/web_app_badging/badging_with_frames_and_workers.html?url=" +
        cross_site_frame_url.spec());
    main_app_id_ = InstallPWA(app_url);

    GURL sub_app_url = https_server()->GetURL("/web_app_badging/blank.html");
    auto sub_app_info = std::make_unique<WebApplicationInfo>();
    sub_app_info->app_url = sub_app_url;
    sub_app_info->scope = sub_app_url;
    sub_app_info->open_as_window = true;
    sub_app_id_ = InstallWebApp(std::move(sub_app_info));

    content::WebContents* web_contents = OpenApplication(main_app_id_);
    // There should be exactly 4 frames:
    // 1) The main frame.
    // 2) A frame containing a sub app.
    // 3) A cross site frame, on |cross_site_frame_url|.
    // 4) A sub frame in the app's scope.
    auto frames = web_contents->GetAllFrames();
    ASSERT_EQ(4u, frames.size());

    main_frame_ = web_contents->GetMainFrame();
    for (auto* frame : frames) {
      if (frame->GetLastCommittedURL() == sub_app_url) {
        sub_app_frame_ = frame;
      } else if (url::IsSameOriginWith(frame->GetLastCommittedURL(),
                                       main_frame_->GetLastCommittedURL())) {
        in_scope_frame_ = frame;
      } else if (frame != main_frame_) {
        cross_site_frame_ = frame;
      }
    }

    ASSERT_TRUE(main_frame_);
    ASSERT_TRUE(sub_app_frame_);
    ASSERT_TRUE(in_scope_frame_);
    ASSERT_TRUE(cross_site_frame_);

    // Register two service workers:
    // 1) A service worker with a scope that applies to both the main app and
    // the sub app.
    // 2) A service worker with a scope that applies to the sub app only.
    app_service_worker_scope_ = app_url.GetWithoutFilename();
    const std::string register_app_service_worker_script = content::JsReplace(
        kRegisterServiceWorkerScript, app_service_worker_scope_.spec());
    ASSERT_EQ("OK", EvalJs(main_frame_, register_app_service_worker_script));

    sub_app_service_worker_scope_ = sub_app_url;
    const std::string register_sub_app_service_worker_script =
        content::JsReplace(kRegisterServiceWorkerScript,
                           sub_app_service_worker_scope_.spec());
    ASSERT_EQ("OK",
              EvalJs(main_frame_, register_sub_app_service_worker_script));

    awaiter_ = std::make_unique<base::RunLoop>();

    badging::BadgeManager* badge_manager =
        badging::BadgeManagerFactory::GetInstance()->GetForProfile(profile());

    // The delegate is owned by the badge manager. We hold a pointer to it for
    // the test.
    std::unique_ptr<badging::TestBadgeManagerDelegate> owned_delegate =
        std::make_unique<badging::TestBadgeManagerDelegate>(profile(),
                                                            badge_manager);
    owned_delegate->SetOnBadgeChanged(base::BindRepeating(
        &WebAppBadgingBrowserTest::OnBadgeChanged, base::Unretained(this)));
    delegate_ = owned_delegate.get();

    badge_manager->SetDelegate(std::move(owned_delegate));
  }

  void OnBadgeChanged() {
    // This is only set up to deal with one badge change at a time per app,
    // in order to make asserting the result of a badge change easier.  A single
    // service worker badge call may affect multiple apps within its scope.
    const size_t total_changes =
        delegate_->cleared_badges().size() + delegate_->set_badges().size();
    ASSERT_LE(total_changes, expected_badge_change_count_);

    if (expected_badge_change_count_ == total_changes) {
      // Update |badge_change_map_| to record each badge clear and badge set
      // that occurred.
      for (const auto& cleared_app_id : delegate_->cleared_badges()) {
        BadgeChange clear_badge_change;
        clear_badge_change.was_cleared_ = true;

        ASSERT_TRUE(badge_change_map_.find(cleared_app_id) ==
                    badge_change_map_.end())
            << "ERROR: Cannot record badge clear.  App with ID: '"
            << cleared_app_id << "' has multiple badge changes.";

        badge_change_map_[cleared_app_id] = clear_badge_change;
      }
      for (const auto& set_app_badge : delegate_->set_badges()) {
        BadgeChange set_badge_change;
        set_badge_change.last_badge_content_ = set_app_badge.second;
        set_badge_change.was_flagged_ =
            set_badge_change.last_badge_content_ == base::nullopt;

        const AppId& set_app_id = set_app_badge.first;
        ASSERT_TRUE(badge_change_map_.find(set_app_id) ==
                    badge_change_map_.end())
            << "ERROR: Cannot record badge set.  App with ID: '" << set_app_id
            << "' has multiple badge changes.";

        badge_change_map_[set_app_id] = set_badge_change;
      }

      awaiter_->Quit();
    }
  }

 protected:
  // Expects a single badge change only.
  void ExecuteScriptAndWaitForBadgeChange(std::string script,
                                          RenderFrameHost* on) {
    ExecuteScriptAndWaitForMultipleBadgeChanges(
        script, on, /*expected_badge_change_count=*/1);
  }

  // Handles badge changes that may affect multiple apps. Useful for testing
  // service workers, which can control many apps.
  void ExecuteScriptAndWaitForMultipleBadgeChanges(
      std::string script,
      RenderFrameHost* on,
      size_t expected_badge_change_count) {
    expected_badge_change_count_ = expected_badge_change_count;
    badge_change_map_.clear();

    awaiter_ = std::make_unique<base::RunLoop>();
    delegate_->ResetBadges();

    ASSERT_TRUE(content::ExecuteScript(on, script));

    if (badge_change_map_.size() >= expected_badge_change_count_)
      return;

    awaiter_->Run();
  }

  // Runs script in |main_frame_| that posts a message to the service worker
  // specified by |service_worker_scope|.  The service worker's message handler
  // then calls setAppBadge() with |badge_value|.
  void SetBadgeInServiceWorkerAndWaitForChanges(
      const GURL& service_worker_scope,
      base::Optional<uint64_t> badge_value,
      size_t expected_badge_change_count) {
    std::string message_data;
    if (badge_value) {
      message_data = "{ command: 'set-app-badge', value: " +
                     base::NumberToString(*badge_value) + "}";
    } else {
      message_data = "{ command: 'set-app-badge' }";
    }

    ExecuteScriptAndWaitForMultipleBadgeChanges(
        "postMessageToServiceWorker('" + service_worker_scope.spec() + "', " +
            message_data + ")",
        main_frame_, expected_badge_change_count);
  }

  // Same as SetBadgeInServiceWorkerAndWaitForChanges() above, except runs
  // clearAppBadge() in the service worker.
  void ClearBadgeInServiceWorkerAndWaitForChanges(
      const GURL& service_worker_scope,
      size_t expected_badge_change_count) {
    ExecuteScriptAndWaitForMultipleBadgeChanges(
        "postMessageToServiceWorker('" + service_worker_scope.spec() +
            "', { command: 'clear-app-badge' });",
        main_frame_, expected_badge_change_count);
  }

  const AppId& main_app_id() { return main_app_id_; }
  const AppId& sub_app_id() { return sub_app_id_; }
  const AppId& cross_site_app_id() { return cross_site_app_id_; }

  RenderFrameHost* main_frame_;
  RenderFrameHost* sub_app_frame_;
  RenderFrameHost* in_scope_frame_;
  RenderFrameHost* cross_site_frame_;

  // Use this script text with EvalJs() on |main_frame_| to register a service
  // worker.  Use ReplaceJs() to replace $1 with the service worker scope URL.
  const std::string kRegisterServiceWorkerScript =
      "registerServiceWorker('service_worker.js', $1);";

  // Both the main app and sub app are within this scope.
  GURL app_service_worker_scope_;

  // Only the sub app is within this scope.
  GURL sub_app_service_worker_scope_;

  // Frame badge updates affect the badge for at most 1 app.  However, a single
  // service worker badge update may affect multiple apps.
  size_t expected_badge_change_count_ = 0;

  // Records a badge update for an app.
  struct BadgeChange {
    bool was_cleared_ = false;
    bool was_flagged_ = false;
    base::Optional<uint64_t> last_badge_content_ = base::nullopt;
  };

  // Records a single badge update for multiple apps.
  std::unordered_map<AppId, BadgeChange> badge_change_map_;

  // Gets the recorded badge update for |app_id| from |badge_change_map_|.
  // Asserts when no recorded badge update exists for |app_id|.  Calls should be
  // wrapped in the ASSERT_NO_FATAL_FAILURE() macro.
  void GetBadgeChange(const AppId& app_id, BadgeChange* result) {
    auto it = badge_change_map_.find(app_id);

    ASSERT_NE(it, badge_change_map_.end())
        << "App with ID: '" << app_id << "' did not update a badge.";

    *result = it->second;
  }

 private:
  AppId main_app_id_;
  AppId sub_app_id_;
  AppId cross_site_app_id_;
  std::unique_ptr<base::RunLoop> awaiter_;
  badging::TestBadgeManagerDelegate* delegate_;
  net::EmbeddedTestServer cross_origin_https_server_;
};

// Tests that the badge for the main frame is not affected by changing the badge
// of a cross site subframe.
IN_PROC_BROWSER_TEST_P(WebAppBadgingBrowserTest,
                       CrossSiteFrameCannotChangeMainFrameBadge) {
  // Clearing from cross site frame should affect only the cross site app.
  ExecuteScriptAndWaitForBadgeChange("navigator.clearExperimentalAppBadge()",
                                     cross_site_frame_);
  BadgeChange badge_change;
  ASSERT_NO_FATAL_FAILURE(GetBadgeChange(cross_site_app_id(), &badge_change));
  ASSERT_TRUE(badge_change.was_cleared_);
  ASSERT_FALSE(badge_change.was_flagged_);

  // Setting from cross site frame should affect only the cross site app.
  ExecuteScriptAndWaitForBadgeChange("navigator.setExperimentalAppBadge(77)",
                                     cross_site_frame_);

  ASSERT_NO_FATAL_FAILURE(GetBadgeChange(cross_site_app_id(), &badge_change));
  ASSERT_FALSE(badge_change.was_cleared_);
  ASSERT_FALSE(badge_change.was_flagged_);
  ASSERT_EQ(77u, badge_change.last_badge_content_);
}

// Tests that setting the badge to an integer will be propagated across
// processes.
IN_PROC_BROWSER_TEST_P(WebAppBadgingBrowserTest, BadgeCanBeSetToAnInteger) {
  ExecuteScriptAndWaitForBadgeChange("navigator.setExperimentalAppBadge(99)",
                                     main_frame_);
  BadgeChange badge_change;
  ASSERT_NO_FATAL_FAILURE(GetBadgeChange(main_app_id(), &badge_change));
  ASSERT_FALSE(badge_change.was_cleared_);
  ASSERT_FALSE(badge_change.was_flagged_);
  ASSERT_EQ(base::Optional<uint64_t>(99u), badge_change.last_badge_content_);
}

// Tests that calls to |Badge.clear| are propagated across processes.
IN_PROC_BROWSER_TEST_P(WebAppBadgingBrowserTest,
                       BadgeCanBeClearedWithClearMethod) {
  ExecuteScriptAndWaitForBadgeChange("navigator.setExperimentalAppBadge(55)",
                                     main_frame_);
  BadgeChange badge_change;
  ASSERT_NO_FATAL_FAILURE(GetBadgeChange(main_app_id(), &badge_change));
  ASSERT_FALSE(badge_change.was_cleared_);
  ASSERT_FALSE(badge_change.was_flagged_);
  ASSERT_EQ(base::Optional<uint64_t>(55u), badge_change.last_badge_content_);

  ExecuteScriptAndWaitForBadgeChange("navigator.clearExperimentalAppBadge()",
                                     main_frame_);
  ASSERT_NO_FATAL_FAILURE(GetBadgeChange(main_app_id(), &badge_change));
  ASSERT_TRUE(badge_change.was_cleared_);
  ASSERT_FALSE(badge_change.was_flagged_);
  ASSERT_EQ(base::nullopt, badge_change.last_badge_content_);
}

// Tests that calling Badge.set(0) is equivalent to calling |Badge.clear| and
// that it propagates across processes.
IN_PROC_BROWSER_TEST_P(WebAppBadgingBrowserTest, BadgeCanBeClearedWithZero) {
  ExecuteScriptAndWaitForBadgeChange("navigator.setExperimentalAppBadge(0)",
                                     main_frame_);
  BadgeChange badge_change;
  ASSERT_NO_FATAL_FAILURE(GetBadgeChange(main_app_id(), &badge_change));
  ASSERT_TRUE(badge_change.was_cleared_);
  ASSERT_FALSE(badge_change.was_flagged_);
  ASSERT_EQ(base::nullopt, badge_change.last_badge_content_);
}

// Tests that setting the badge without content is propagated across processes.
IN_PROC_BROWSER_TEST_P(WebAppBadgingBrowserTest, BadgeCanBeSetWithoutAValue) {
  ExecuteScriptAndWaitForBadgeChange("navigator.setExperimentalAppBadge()",
                                     main_frame_);
  BadgeChange badge_change;
  ASSERT_NO_FATAL_FAILURE(GetBadgeChange(main_app_id(), &badge_change));
  ASSERT_FALSE(badge_change.was_cleared_);
  ASSERT_TRUE(badge_change.was_flagged_);
  ASSERT_EQ(base::nullopt, badge_change.last_badge_content_);
}

// Tests that the badge can be set and cleared from an in scope frame.
IN_PROC_BROWSER_TEST_P(WebAppBadgingBrowserTest,
                       BadgeCanBeSetAndClearedFromInScopeFrame) {
  ExecuteScriptAndWaitForBadgeChange("navigator.setExperimentalAppBadge()",
                                     in_scope_frame_);
  BadgeChange badge_change;
  ASSERT_NO_FATAL_FAILURE(GetBadgeChange(main_app_id(), &badge_change));
  ASSERT_FALSE(badge_change.was_cleared_);
  ASSERT_TRUE(badge_change.was_flagged_);
  ASSERT_EQ(base::nullopt, badge_change.last_badge_content_);

  ExecuteScriptAndWaitForBadgeChange("navigator.clearExperimentalAppBadge()",
                                     in_scope_frame_);
  ASSERT_NO_FATAL_FAILURE(GetBadgeChange(main_app_id(), &badge_change));
  ASSERT_TRUE(badge_change.was_cleared_);
  ASSERT_FALSE(badge_change.was_flagged_);
  ASSERT_EQ(base::nullopt, badge_change.last_badge_content_);
}

// Tests that changing the badge of a subframe with an app affects the
// subframe's app.
IN_PROC_BROWSER_TEST_P(WebAppBadgingBrowserTest, SubFrameBadgeAffectsSubApp) {
  ExecuteScriptAndWaitForBadgeChange("navigator.setExperimentalAppBadge()",
                                     sub_app_frame_);
  BadgeChange badge_change;
  ASSERT_NO_FATAL_FAILURE(GetBadgeChange(sub_app_id(), &badge_change));
  ASSERT_FALSE(badge_change.was_cleared_);
  ASSERT_TRUE(badge_change.was_flagged_);
  ASSERT_EQ(base::nullopt, badge_change.last_badge_content_);

  ExecuteScriptAndWaitForBadgeChange("navigator.clearExperimentalAppBadge()",
                                     sub_app_frame_);
  ASSERT_NO_FATAL_FAILURE(GetBadgeChange(sub_app_id(), &badge_change));
  ASSERT_TRUE(badge_change.was_cleared_);
  ASSERT_FALSE(badge_change.was_flagged_);
  ASSERT_EQ(base::nullopt, badge_change.last_badge_content_);
}

// Tests that setting a badge on a subframe with an app only effects the sub
// app.
IN_PROC_BROWSER_TEST_P(WebAppBadgingBrowserTest, BadgeSubFrameAppViaNavigator) {
  ExecuteScriptAndWaitForBadgeChange(
      "window['sub-app'].navigator.setExperimentalAppBadge()", main_frame_);
  BadgeChange badge_change;
  ASSERT_NO_FATAL_FAILURE(GetBadgeChange(sub_app_id(), &badge_change));
  ASSERT_FALSE(badge_change.was_cleared_);
  ASSERT_TRUE(badge_change.was_flagged_);
  ASSERT_EQ(base::nullopt, badge_change.last_badge_content_);
}

// Tests that setting a badge on a subframe via call() craziness sets the
// subframe app's badge.
IN_PROC_BROWSER_TEST_P(WebAppBadgingBrowserTest, BadgeSubFrameAppViaCall) {
  ExecuteScriptAndWaitForBadgeChange(
      "const promise = "
      "  window.navigator.setExperimentalAppBadge"
      "    .call(window['sub-app'].navigator);"
      "if (promise instanceof window.Promise)"
      "  throw new Error('Should be an instance of the subframes Promise!')",
      main_frame_);
  BadgeChange badge_change;
  ASSERT_NO_FATAL_FAILURE(GetBadgeChange(sub_app_id(), &badge_change));
  ASSERT_FALSE(badge_change.was_cleared_);
  ASSERT_TRUE(badge_change.was_flagged_);
  ASSERT_EQ(base::nullopt, badge_change.last_badge_content_);
}

// Test that badging through a service worker scoped to the sub app updates
// badges for the sub app only.  These badge updates must not affect the main
// app.
IN_PROC_BROWSER_TEST_P(WebAppBadgingBrowserTest,
                       SubAppServiceWorkerBadgeAffectsSubApp) {
  const uint64_t badge_value = 1u;
  SetBadgeInServiceWorkerAndWaitForChanges(sub_app_service_worker_scope_,
                                           badge_value,
                                           /*expected_badge_change_count=*/1);
  BadgeChange badge_change;
  ASSERT_NO_FATAL_FAILURE(GetBadgeChange(sub_app_id(), &badge_change));
  ASSERT_FALSE(badge_change.was_cleared_);
  ASSERT_FALSE(badge_change.was_flagged_);
  ASSERT_EQ(badge_value, badge_change.last_badge_content_);

  ClearBadgeInServiceWorkerAndWaitForChanges(sub_app_service_worker_scope_,
                                             /*expected_badge_change_count=*/1);
  ASSERT_NO_FATAL_FAILURE(GetBadgeChange(sub_app_id(), &badge_change));
  ASSERT_TRUE(badge_change.was_cleared_);
  ASSERT_FALSE(badge_change.was_flagged_);
  ASSERT_EQ(base::nullopt, badge_change.last_badge_content_);
}

// Test that badging through a service worker scoped to the main app updates
// badges for both the main app and the sub app.  Each service worker badge
// function call must generate 2 badge changes.
IN_PROC_BROWSER_TEST_P(WebAppBadgingBrowserTest,
                       AppServiceWorkerBadgeAffectsMultipleApps) {
  SetBadgeInServiceWorkerAndWaitForChanges(app_service_worker_scope_,
                                           base::nullopt,
                                           /*expected_badge_change_count=*/2);
  BadgeChange badge_change;
  ASSERT_NO_FATAL_FAILURE(GetBadgeChange(main_app_id(), &badge_change));
  ASSERT_FALSE(badge_change.was_cleared_);
  ASSERT_TRUE(badge_change.was_flagged_);
  ASSERT_EQ(base::nullopt, badge_change.last_badge_content_);

  ASSERT_NO_FATAL_FAILURE(GetBadgeChange(sub_app_id(), &badge_change));
  ASSERT_FALSE(badge_change.was_cleared_);
  ASSERT_TRUE(badge_change.was_flagged_);
  ASSERT_EQ(base::nullopt, badge_change.last_badge_content_);

  ClearBadgeInServiceWorkerAndWaitForChanges(app_service_worker_scope_,
                                             /*expected_badge_change_count=*/2);
  ASSERT_NO_FATAL_FAILURE(GetBadgeChange(main_app_id(), &badge_change));
  ASSERT_TRUE(badge_change.was_cleared_);
  ASSERT_FALSE(badge_change.was_flagged_);
  ASSERT_EQ(base::nullopt, badge_change.last_badge_content_);

  ASSERT_NO_FATAL_FAILURE(GetBadgeChange(sub_app_id(), &badge_change));
  ASSERT_TRUE(badge_change.was_cleared_);
  ASSERT_FALSE(badge_change.was_flagged_);
  ASSERT_EQ(base::nullopt, badge_change.last_badge_content_);
}

// Tests that badging incognito windows does not cause a crash.
IN_PROC_BROWSER_TEST_P(WebAppBadgingBrowserTest,
                       BadgingIncognitoWindowsDoesNotCrash) {
  Browser* incognito_browser =
      OpenURLOffTheRecord(profile(), main_frame_->GetLastCommittedURL());
  RenderFrameHost* incognito_frame = incognito_browser->tab_strip_model()
                                         ->GetActiveWebContents()
                                         ->GetMainFrame();

  ASSERT_TRUE(content::ExecuteScript(incognito_frame,
                                     "navigator.setExperimentalAppBadge()"));
  ASSERT_TRUE(content::ExecuteScript(incognito_frame,
                                     "navigator.clearExperimentalAppBadge()"));

  // Updating badges through a ServiceWorkerGlobalScope must not crash.
  const std::string register_app_service_worker_script = content::JsReplace(
      kRegisterServiceWorkerScript, app_service_worker_scope_.spec());
  ASSERT_EQ("OK", EvalJs(incognito_frame, register_app_service_worker_script));

  const std::string set_badge_script = content::JsReplace(
      "postMessageToServiceWorker('$1', { command: 'set-app-badge', value: 29 "
      "});",
      app_service_worker_scope_.spec());
  ASSERT_EQ("OK", EvalJs(incognito_frame, set_badge_script));

  const std::string clear_badge_script = content::JsReplace(
      "postMessageToServiceWorker('$1', { command: 'clear-app-badge' });",
      app_service_worker_scope_.spec());
  ASSERT_EQ("OK", EvalJs(incognito_frame, clear_badge_script));
}

INSTANTIATE_TEST_SUITE_P(
    All,
    WebAppBadgingBrowserTest,
    ::testing::Values(ControllerType::kHostedAppController,
                      ControllerType::kUnifiedControllerWithBookmarkApp,
                      ControllerType::kUnifiedControllerWithWebApp),
    ControllerTypeParamToString);

}  // namespace web_app
