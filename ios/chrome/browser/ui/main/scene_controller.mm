// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ios/chrome/browser/ui/main/scene_controller.h"

#import "base/logging.h"
#include "base/metrics/histogram_functions.h"
#include "base/metrics/histogram_macros.h"
#include "base/strings/sys_string_conversions.h"
#include "base/task/post_task.h"
#include "components/payments/core/features.h"
#include "components/signin/public/identity_manager/identity_manager.h"
#include "components/url_formatter/url_formatter.h"
#import "ios/chrome/app/application_delegate/app_state.h"
#include "ios/chrome/app/application_delegate/tab_opening.h"
#import "ios/chrome/app/application_delegate/url_opener.h"
#import "ios/chrome/app/chrome_overlay_window.h"
#import "ios/chrome/app/deferred_initialization_runner.h"
#import "ios/chrome/app/main_controller_guts.h"
#include "ios/chrome/browser/browser_state/chrome_browser_state.h"
#include "ios/chrome/browser/browsing_data/browsing_data_remove_mask.h"
#include "ios/chrome/browser/browsing_data/browsing_data_remover.h"
#include "ios/chrome/browser/browsing_data/browsing_data_remover_factory.h"
#include "ios/chrome/browser/chrome_url_constants.h"
#import "ios/chrome/browser/chrome_url_util.h"
#include "ios/chrome/browser/crash_report/breadcrumbs/breadcrumb_manager_browser_agent.h"
#include "ios/chrome/browser/crash_report/breadcrumbs/breadcrumb_manager_keyed_service.h"
#include "ios/chrome/browser/crash_report/breadcrumbs/breadcrumb_manager_keyed_service_factory.h"
#include "ios/chrome/browser/crash_report/breadcrumbs/features.h"
#include "ios/chrome/browser/crash_report/breakpad_helper.h"
#include "ios/chrome/browser/crash_report/crash_report_helper.h"
#include "ios/chrome/browser/main/browser.h"
#include "ios/chrome/browser/ntp/features.h"
#include "ios/chrome/browser/payments/ios_payment_instrument_launcher.h"
#include "ios/chrome/browser/payments/ios_payment_instrument_launcher_factory.h"
#include "ios/chrome/browser/payments/payment_request_constants.h"
#include "ios/chrome/browser/signin/identity_manager_factory.h"
#import "ios/chrome/browser/snapshots/snapshot_tab_helper.h"
#import "ios/chrome/browser/tabs/tab_model.h"
#import "ios/chrome/browser/ui/authentication/signed_in_accounts_view_controller.h"
#import "ios/chrome/browser/ui/browser_view/browser_view_controller.h"
#import "ios/chrome/browser/ui/commands/browser_commands.h"
#import "ios/chrome/browser/ui/commands/command_dispatcher.h"
#import "ios/chrome/browser/ui/commands/open_new_tab_command.h"
#import "ios/chrome/browser/ui/commands/show_signin_command.h"
#include "ios/chrome/browser/ui/history/history_coordinator.h"
#import "ios/chrome/browser/ui/main/browser_interface_provider.h"
#import "ios/chrome/browser/ui/main/browser_view_wrangler.h"
#import "ios/chrome/browser/ui/settings/settings_navigation_controller.h"
#import "ios/chrome/browser/ui/signin_interaction/signin_interaction_coordinator.h"
#include "ios/chrome/browser/ui/tab_grid/tab_grid_coordinator.h"
#import "ios/chrome/browser/ui/toolbar/public/omnibox_focuser.h"
#import "ios/chrome/browser/ui/util/multi_window_support.h"
#import "ios/chrome/browser/ui/util/top_view_controller.h"
#import "ios/chrome/browser/ui/util/uikit_ui_util.h"
#import "ios/chrome/browser/url_loading/app_url_loading_service.h"
#import "ios/chrome/browser/url_loading/url_loading_params.h"
#import "ios/chrome/browser/url_loading/url_loading_service.h"
#import "ios/chrome/browser/url_loading/url_loading_service_factory.h"
#import "ios/chrome/browser/web_state_list/web_state_list.h"
#import "ios/chrome/browser/web_state_list/web_state_list_observer_bridge.h"
#include "ios/public/provider/chrome/browser/chrome_browser_provider.h"
#include "ios/public/provider/chrome/browser/mailto/mailto_handler_provider.h"
#include "ios/public/provider/chrome/browser/signin/chrome_identity_service.h"
#import "ios/public/provider/chrome/browser/user_feedback/user_feedback_provider.h"
#include "ios/web/public/thread/web_task_traits.h"
#import "ios/web/public/web_state.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

namespace {

// A rough estimate of the expected duration of a view controller transition
// animation. It's used to temporarily disable mutally exclusive chrome
// commands that trigger a view controller presentation.
const int64_t kExpectedTransitionDurationInNanoSeconds = 0.2 * NSEC_PER_SEC;

// Possible results of snapshotting at the moment the user enters the tab
// switcher. These values are persisted to logs. Entries should not be
// renumbered and numeric values should never be reused.
enum class EnterTabSwitcherSnapshotResult {
  // Page was loading at the time of the snapshot request, and the snapshot
  // failed.
  kPageLoadingAndSnapshotFailed = 0,
  // Page was loading at the time of the snapshot request, and the snapshot
  // succeeded.
  kPageLoadingAndSnapshotSucceeded = 1,
  // Page was not loading at the time of the snapshot request, and the snapshot
  // failed.
  kPageNotLoadingAndSnapshotFailed = 2,
  // Page was not loading at the time of the snapshot request, and the snapshot
  // succeeded.
  kPageNotLoadingAndSnapshotSucceeded = 3,
  // kMaxValue should share the value of the highest enumerator.
  kMaxValue = kPageNotLoadingAndSnapshotSucceeded,
};

// Used to update the current BVC mode if a new tab is added while the tab
// switcher view is being dismissed.  This is different than ApplicationMode in
// that it can be set to |NONE| when not in use.
enum class TabSwitcherDismissalMode { NONE, NORMAL, INCOGNITO };

}  // namespace

@interface SceneController () <UserFeedbackDataSource,
                               SettingsNavigationControllerDelegate,
                               WebStateListObserving>

// A flag that keeps track of the UI initialization for the controlled scene.
@property(nonatomic, assign) BOOL hasInitializedUI;

// The SigninInteractionCoordinator to present Sign In UI. It is created the
// first time Sign In UI is needed to be presented and should not be destroyed
// while the UI is presented.
@property(nonatomic, strong)
    SigninInteractionCoordinator* signinInteractionCoordinator;

// Returns YES if the settings are presented, either from
// self.settingsNavigationController or from SigninInteractionCoordinator.
@property(nonatomic, assign, readonly, getter=isSettingsViewPresented)
    BOOL settingsViewPresented;

// Coordinator for displaying history.
@property(nonatomic, strong) HistoryCoordinator* historyCoordinator;

// The tab switcher command and the voice search commands can be sent by views
// that reside in a different UIWindow leading to the fact that the exclusive
// touch property will be ineffective and a command for processing both
// commands may be sent in the same run of the runloop leading to
// inconsistencies. Those two boolean indicate if one of those commands have
// been processed in the last 200ms in order to only allow processing one at
// a time.
// TODO(crbug.com/560296):  Provide a general solution for handling mutually
// exclusive chrome commands sent at nearly the same time.
@property(nonatomic, assign) BOOL isProcessingTabSwitcherCommand;
@property(nonatomic, assign) BOOL isProcessingVoiceSearchCommand;

// If not NONE, the current BVC should be switched to this BVC on completion
// of tab switcher dismissal.
@property(nonatomic, assign)
    TabSwitcherDismissalMode modeToDisplayOnTabSwitcherDismissal;

// A property to track whether the QR Scanner should be started upon tab
// switcher dismissal. It can only be YES if the QR Scanner experiment is
// enabled.
@property(nonatomic, readwrite)
    NTPTabOpeningPostOpeningAction NTPActionAfterTabSwitcherDismissal;

@end

@implementation SceneController
@synthesize settingsNavigationController;  //< From AppNavigation protocol.

- (instancetype)initWithSceneState:(SceneState*)sceneState {
  self = [super init];
  if (self) {
    _sceneState = sceneState;
    [_sceneState addObserver:self];
    // The window is necessary very early in the app/scene lifecycle, so it
    // should be created right away.
    if (!self.sceneState.window) {
      DCHECK(!IsMultiwindowSupported())
          << "The window must be created by the scene delegate";
      self.sceneState.window = [[ChromeOverlayWindow alloc]
          initWithFrame:[[UIScreen mainScreen] bounds]];
    }
  }
  return self;
}

#pragma mark - Setters and getters

- (id<BrowserInterface>)mainInterface {
  return self.mainController.interfaceProvider.mainInterface;
}

- (id<BrowserInterface>)currentInterface {
  return self.mainController.interfaceProvider.currentInterface;
}

- (id<BrowserInterface>)incognitoInterface {
  return self.mainController.interfaceProvider.incognitoInterface;
}

- (id<BrowserInterfaceProvider>)interfaceProvider {
  return self.mainController.interfaceProvider;
}

- (BOOL)isSettingsViewPresented {
  return self.settingsNavigationController ||
         self.signinInteractionCoordinator.isSettingsViewPresented;
}

#pragma mark - SceneStateObserver

- (void)sceneState:(SceneState*)sceneState
    transitionedToActivationLevel:(SceneActivationLevel)level {
  if (level > SceneActivationLevelBackground && !self.hasInitializedUI) {
    [self initializeUI];
  }
}

#pragma mark - private

- (void)initializeUI {
  self.hasInitializedUI = YES;
}

// This method completely destroys all of the UI. It should be called when the
// scene is disconnected.
- (void)teardownUI {
  if (!self.hasInitializedUI) {
    return;  // Nothing to do.
  }

  // The UI should be stopped before the models they observe are stopped.
  [self.signinInteractionCoordinator cancel];
  self.signinInteractionCoordinator = nil;

  [self.historyCoordinator stop];
  self.historyCoordinator = nil;

  self.hasInitializedUI = NO;
}

#pragma mark - ApplicationCommands

- (void)dismissModalDialogs {
  [self dismissModalDialogsWithCompletion:nil dismissOmnibox:YES];
}

- (void)showHistory {
  self.historyCoordinator = [[HistoryCoordinator alloc]
      initWithBaseViewController:self.mainController.currentBVC
                    browserState:self.mainController.mainBrowserState];
  self.historyCoordinator.loadStrategy =
      [self currentPageIsIncognito] ? UrlLoadStrategy::ALWAYS_IN_INCOGNITO
                                    : UrlLoadStrategy::NORMAL;
  self.historyCoordinator.dispatcher = self.mainController.mainBVC.dispatcher;
  [self.historyCoordinator start];
}

// Opens an url from a link in the settings UI.
- (void)closeSettingsUIAndOpenURL:(OpenNewTabCommand*)command {
  [self openUrlFromSettings:command];
}

- (void)closeSettingsUI {
  [self closeSettingsAnimated:YES completion:nullptr];
}

- (void)prepareTabSwitcher {
  web::WebState* currentWebState =
      self.mainController.currentBVC.tabModel.webStateList->GetActiveWebState();
  if (currentWebState) {
    BOOL loading = currentWebState->IsLoading();
    SnapshotTabHelper::FromWebState(currentWebState)
        ->UpdateSnapshotWithCallback(^(UIImage* snapshot) {
          EnterTabSwitcherSnapshotResult snapshotResult;
          if (loading && !snapshot) {
            snapshotResult =
                EnterTabSwitcherSnapshotResult::kPageLoadingAndSnapshotFailed;
          } else if (loading && snapshot) {
            snapshotResult = EnterTabSwitcherSnapshotResult::
                kPageLoadingAndSnapshotSucceeded;
          } else if (!loading && !snapshot) {
            snapshotResult = EnterTabSwitcherSnapshotResult::
                kPageNotLoadingAndSnapshotFailed;
          } else {
            DCHECK(!loading && snapshot);
            snapshotResult = EnterTabSwitcherSnapshotResult::
                kPageNotLoadingAndSnapshotSucceeded;
          }
          UMA_HISTOGRAM_ENUMERATION("IOS.EnterTabSwitcherSnapshotResult",
                                    snapshotResult);
        });
  }
  [self.mainController.mainCoordinator
      prepareToShowTabSwitcher:self.mainController.tabSwitcher];
}

- (void)displayTabSwitcher {
  DCHECK(!self.mainController.isTabSwitcherActive);
  if (!self.isProcessingVoiceSearchCommand) {
    [self.mainController.currentBVC userEnteredTabSwitcher];
    [self showTabSwitcher];
    self.isProcessingTabSwitcherCommand = YES;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                 kExpectedTransitionDurationInNanoSeconds),
                   dispatch_get_main_queue(), ^{
                     self.isProcessingTabSwitcherCommand = NO;
                   });
  }
}

// TODO(crbug.com/779791) : Remove showing settings from MainController.
- (void)showAutofillSettingsFromViewController:
    (UIViewController*)baseViewController {
  DCHECK(!self.signinInteractionCoordinator.isSettingsViewPresented);
  if (self.settingsNavigationController)
    return;

  Browser* browser = self.mainInterface.browser;
  self.settingsNavigationController =
      [SettingsNavigationController autofillProfileControllerForBrowser:browser
                                                               delegate:self];
  [baseViewController presentViewController:self.settingsNavigationController
                                   animated:YES
                                 completion:nil];
}

- (void)showReportAnIssueFromViewController:
    (UIViewController*)baseViewController {
  DCHECK(baseViewController);
  // This dispatch is necessary to give enough time for the tools menu to
  // disappear before taking a screenshot.
  dispatch_async(dispatch_get_main_queue(), ^{
    DCHECK(!self.signinInteractionCoordinator.isSettingsViewPresented);
    if (self.settingsNavigationController)
      return;
    Browser* browser = self.mainInterface.browser;
    self.settingsNavigationController =
        [SettingsNavigationController userFeedbackControllerForBrowser:browser
                                                              delegate:self
                                                    feedbackDataSource:self
                                                            dispatcher:self];
    [baseViewController presentViewController:self.settingsNavigationController
                                     animated:YES
                                   completion:nil];
  });
}

- (void)openURLInNewTab:(OpenNewTabCommand*)command {
  UrlLoadParams params =
      UrlLoadParams::InNewTab(command.URL, command.virtualURL);
  params.SetInBackground(command.inBackground);
  params.web_params.referrer = command.referrer;
  params.in_incognito = command.inIncognito;
  params.append_to = command.appendTo;
  params.origin_point = command.originPoint;
  params.from_chrome = command.fromChrome;
  params.user_initiated = command.userInitiated;
  params.should_focus_omnibox = command.shouldFocusOmnibox;
  self.mainController.appURLLoadingService->LoadUrlInNewTab(params);
}

// TODO(crbug.com/779791) : Do not pass |baseViewController| through dispatcher.
- (void)showSignin:(ShowSigninCommand*)command
    baseViewController:(UIViewController*)baseViewController {
  if (!self.signinInteractionCoordinator) {
    Browser* mainBrowser = self.mainInterface.browser;
    self.signinInteractionCoordinator =
        [[SigninInteractionCoordinator alloc] initWithBrowser:mainBrowser];
  }

  switch (command.operation) {
    case AUTHENTICATION_OPERATION_REAUTHENTICATE:
      [self.signinInteractionCoordinator
          reAuthenticateWithAccessPoint:command.accessPoint
                            promoAction:command.promoAction
               presentingViewController:baseViewController
                             completion:command.callback];
      break;
    case AUTHENTICATION_OPERATION_SIGNIN:
      [self.signinInteractionCoordinator signInWithIdentity:command.identity
                                                accessPoint:command.accessPoint
                                                promoAction:command.promoAction
                                   presentingViewController:baseViewController
                                                 completion:command.callback];
      break;
  }
}

- (void)showAdvancedSigninSettingsFromViewController:
    (UIViewController*)baseViewController {
  Browser* mainBrowser = self.mainInterface.browser;
  self.signinInteractionCoordinator =
      [[SigninInteractionCoordinator alloc] initWithBrowser:mainBrowser];
  [self.signinInteractionCoordinator
      showAdvancedSigninSettingsWithPresentingViewController:
          baseViewController];
}

// TODO(crbug.com/779791) : Remove settings commands from MainController.
- (void)showAddAccountFromViewController:(UIViewController*)baseViewController {
  Browser* mainBrowser = self.mainInterface.browser;
  if (!self.signinInteractionCoordinator) {
    self.signinInteractionCoordinator =
        [[SigninInteractionCoordinator alloc] initWithBrowser:mainBrowser];
  }

  [self.signinInteractionCoordinator
      addAccountWithAccessPoint:signin_metrics::AccessPoint::
                                    ACCESS_POINT_UNKNOWN
                    promoAction:signin_metrics::PromoAction::
                                    PROMO_ACTION_NO_SIGNIN_PROMO
       presentingViewController:baseViewController
                     completion:nil];
}

- (void)setIncognitoContentVisible:(BOOL)incognitoContentVisible {
  _incognitoContentVisible = incognitoContentVisible;
}

- (void)startVoiceSearch {
  if (!self.isProcessingTabSwitcherCommand) {
    [self startVoiceSearchInCurrentBVC];
    self.isProcessingVoiceSearchCommand = YES;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                 kExpectedTransitionDurationInNanoSeconds),
                   dispatch_get_main_queue(), ^{
                     self.isProcessingVoiceSearchCommand = NO;
                   });
  }
}

- (void)showSettingsFromViewController:(UIViewController*)baseViewController {
  DCHECK(!self.signinInteractionCoordinator.isSettingsViewPresented);
  if (self.settingsNavigationController)
    return;
  [[DeferredInitializationRunner sharedInstance]
      runBlockIfNecessary:kPrefObserverInit];

  Browser* browser = self.mainInterface.browser;

  self.settingsNavigationController =
      [SettingsNavigationController mainSettingsControllerForBrowser:browser
                                                            delegate:self];
  [baseViewController presentViewController:self.settingsNavigationController
                                   animated:YES
                                 completion:nil];
}

#pragma mark - ApplicationSettingsCommands

// TODO(crbug.com/779791) : Remove show settings from MainController.
- (void)showAccountsSettingsFromViewController:
    (UIViewController*)baseViewController {
  DCHECK(!self.signinInteractionCoordinator.isSettingsViewPresented);
  if (!baseViewController) {
    DCHECK_EQ(self.mainController.currentBVC,
              self.mainController.mainCoordinator.activeViewController);
    baseViewController = self.mainController.currentBVC;
  }

  if ([self.mainController currentBrowserState] -> IsOffTheRecord()) {
    NOTREACHED();
    return;
  }
  if (self.settingsNavigationController) {
    [self.settingsNavigationController
        showAccountsSettingsFromViewController:baseViewController];
    return;
  }

  Browser* browser = self.mainInterface.browser;
  self.settingsNavigationController =
      [SettingsNavigationController accountsControllerForBrowser:browser
                                                        delegate:self];
  [baseViewController presentViewController:self.settingsNavigationController
                                   animated:YES
                                 completion:nil];
}

// TODO(crbug.com/779791) : Remove Google services settings from MainController.
- (void)showGoogleServicesSettingsFromViewController:
    (UIViewController*)baseViewController {
  DCHECK(!self.signinInteractionCoordinator.isSettingsViewPresented);
  if (!baseViewController) {
    DCHECK_EQ(self.mainController.currentBVC,
              self.mainController.mainCoordinator.activeViewController);
    baseViewController = self.mainController.currentBVC;
  }

  if (self.settingsNavigationController) {
    // Navigate to the Google services settings if the settings dialog is
    // already opened.
    [self.settingsNavigationController
        showGoogleServicesSettingsFromViewController:baseViewController];
    return;
  }

  Browser* browser = self.mainInterface.browser;
  self.settingsNavigationController =
      [SettingsNavigationController googleServicesControllerForBrowser:browser
                                                              delegate:self];

  [baseViewController presentViewController:self.settingsNavigationController
                                   animated:YES
                                 completion:nil];
}

// TODO(crbug.com/779791) : Remove show settings commands from MainController.
- (void)showSyncPassphraseSettingsFromViewController:
    (UIViewController*)baseViewController {
  DCHECK(!self.signinInteractionCoordinator.isSettingsViewPresented);
  if (self.settingsNavigationController) {
    [self.settingsNavigationController
        showSyncPassphraseSettingsFromViewController:baseViewController];
    return;
  }

  Browser* browser = self.mainInterface.browser;
  self.settingsNavigationController =
      [SettingsNavigationController syncPassphraseControllerForBrowser:browser
                                                              delegate:self];
  [baseViewController presentViewController:self.settingsNavigationController
                                   animated:YES
                                 completion:nil];
}

// TODO(crbug.com/779791) : Remove show settings commands from MainController.
- (void)showSavedPasswordsSettingsFromViewController:
    (UIViewController*)baseViewController {
  if (!baseViewController) {
    // TODO(crbug.com/779791): Don't pass base view controller through
    // dispatched command.
    baseViewController = [self.mainController currentBVC];
  }
  DCHECK(!self.signinInteractionCoordinator.isSettingsViewPresented);
  if (self.settingsNavigationController) {
    [self.settingsNavigationController
        showSavedPasswordsSettingsFromViewController:baseViewController];
    return;
  }
  Browser* browser = self.mainInterface.browser;
  self.settingsNavigationController =
      [SettingsNavigationController savePasswordsControllerForBrowser:browser
                                                             delegate:self];
  [baseViewController presentViewController:self.settingsNavigationController
                                   animated:YES
                                 completion:nil];
}

// TODO(crbug.com/779791) : Remove show settings commands from MainController.
- (void)showProfileSettingsFromViewController:
    (UIViewController*)baseViewController {
  DCHECK(!self.signinInteractionCoordinator.isSettingsViewPresented);
  if (self.settingsNavigationController) {
    [self.settingsNavigationController
        showProfileSettingsFromViewController:baseViewController];
    return;
  }
  Browser* browser = self.mainInterface.browser;

  self.settingsNavigationController =
      [SettingsNavigationController autofillProfileControllerForBrowser:browser
                                                               delegate:self];
  [baseViewController presentViewController:self.settingsNavigationController
                                   animated:YES
                                 completion:nil];
}

// TODO(crbug.com/779791) : Remove show settings commands from MainController.
- (void)showCreditCardSettingsFromViewController:
    (UIViewController*)baseViewController {
  DCHECK(!self.signinInteractionCoordinator.isSettingsViewPresented);
  if (self.settingsNavigationController) {
    [self.settingsNavigationController
        showCreditCardSettingsFromViewController:baseViewController];
    return;
  }

  Browser* browser = self.mainInterface.browser;
  self.settingsNavigationController = [SettingsNavigationController
      autofillCreditCardControllerForBrowser:browser
                                    delegate:self];
  [baseViewController presentViewController:self.settingsNavigationController
                                   animated:YES
                                 completion:nil];
}

#pragma mark - ApplicationCommandsHelpers

- (void)openUrlFromSettings:(OpenNewTabCommand*)command {
  DCHECK([command fromChrome]);
  UrlLoadParams params = UrlLoadParams::InNewTab([command URL]);
  params.web_params.transition_type = ui::PAGE_TRANSITION_TYPED;
  ProceduralBlock completion = ^{
    [self dismissModalsAndOpenSelectedTabInMode:ApplicationModeForTabOpening::
                                                    NORMAL
                              withUrlLoadParams:params
                                 dismissOmnibox:YES
                                     completion:nil];
  };
  [self closeSettingsAnimated:YES completion:completion];
}

#pragma mark - UserFeedbackDataSource

- (NSString*)currentPageDisplayURL {
  if (self.mainController.tabSwitcherIsActive)
    return nil;
  web::WebState* webState =
      self.mainController.currentTabModel.webStateList->GetActiveWebState();
  if (!webState)
    return nil;
  // Returns URL of browser tab that is currently showing.
  GURL url = webState->GetVisibleURL();
  base::string16 urlText = url_formatter::FormatUrl(url);
  return base::SysUTF16ToNSString(urlText);
}

- (UIImage*)currentPageScreenshot {
  UIView* lastView =
      self.mainController.mainCoordinator.activeViewController.view;
  DCHECK(lastView);
  CGFloat scale = 0.0;
  // For screenshots of the tab switcher we need to use a scale of 1.0 to avoid
  // spending too much time since the tab switcher can have lots of subviews.
  if (self.mainController.tabSwitcherIsActive)
    scale = 1.0;
  return CaptureView(lastView, scale);
}

- (NSString*)currentPageSyncedUserName {
  ChromeBrowserState* browserState = self.mainController.currentBrowserState;
  if (browserState->IsOffTheRecord())
    return nil;
  signin::IdentityManager* identity_manager =
      IdentityManagerFactory::GetForBrowserState(browserState);
  std::string username = identity_manager->GetPrimaryAccountInfo().email;
  return username.empty() ? nil : base::SysUTF8ToNSString(username);
}

- (BOOL)currentPageIsIncognito {
  return self.mainController.currentBrowserState->IsOffTheRecord();
}

#pragma mark - SettingsNavigationControllerDelegate

- (void)closeSettings {
  [self closeSettingsUI];
}

- (void)settingsWasDismissed {
  [self.settingsNavigationController cleanUpSettings];
  self.settingsNavigationController = nil;
}

- (id<ApplicationCommands, BrowserCommands>)dispatcherForSettings {
  // Assume that settings always wants the dispatcher from the main BVC.
  return self.mainController.mainBVC.dispatcher;
}

#pragma mark - TabSwitcherDelegate

- (void)tabSwitcher:(id<TabSwitcher>)tabSwitcher
    shouldFinishWithBrowser:(Browser*)browser
               focusOmnibox:(BOOL)focusOmnibox {
  [self beginDismissingTabSwitcherWithCurrentModel:browser->GetTabModel()
                                      focusOmnibox:focusOmnibox];
}

- (void)tabSwitcherDismissTransitionDidEnd:(id<TabSwitcher>)tabSwitcher {
  [self finishDismissingTabSwitcher];
}

- (void)beginDismissingTabSwitcherWithCurrentModel:(TabModel*)tabModel
                                      focusOmnibox:(BOOL)focusOmnibox {
  DCHECK(tabModel == self.mainInterface.tabModel ||
         tabModel == self.incognitoInterface.tabModel);

  self.mainController.dismissingTabSwitcher = YES;
  ApplicationMode mode = (tabModel == self.mainInterface.tabModel)
                             ? ApplicationMode::NORMAL
                             : ApplicationMode::INCOGNITO;
  [self setCurrentInterfaceForMode:mode];

  // The call to set currentBVC above does not actually display the BVC, because
  // _dismissingTabSwitcher is YES.  So: Force the BVC transition to start.
  [self displayCurrentBVCAndFocusOmnibox:focusOmnibox];
}

- (void)finishDismissingTabSwitcher {
  // In real world devices, it is possible to have an empty tab model at the
  // finishing block of a BVC presentation animation. This can happen when the
  // following occur: a) There is JS that closes the last incognito tab, b) that
  // JS was paused while the user was in the tab switcher, c) the user enters
  // the tab, activating the JS while the tab is being presented. Effectively,
  // the BVC finishes the presentation animation, but there are no tabs to
  // display. The only appropriate action is to dismiss the BVC and return the
  // user to the tab switcher.
  if (self.currentInterface.browser &&
      self.currentInterface.browser->GetWebStateList() &&
      self.currentInterface.browser->GetWebStateList()->count() == 0U) {
    self.mainController.tabSwitcherIsActive = NO;
    self.mainController.dismissingTabSwitcher = NO;
    self.modeToDisplayOnTabSwitcherDismissal = TabSwitcherDismissalMode::NONE;
    self.NTPActionAfterTabSwitcherDismissal = NO_ACTION;
    [self showTabSwitcher];
    return;
  }

  // The tab switcher dismissal animation runs
  // as part of the BVC presentation process.  The BVC is presented before the
  // animations begin, so it should be the current active VC at this point.
  DCHECK_EQ(self.mainController.mainCoordinator.activeViewController,
            self.mainController.currentBVC);

  if (self.modeToDisplayOnTabSwitcherDismissal ==
      TabSwitcherDismissalMode::NORMAL) {
    [self setCurrentInterfaceForMode:ApplicationMode::NORMAL];
  } else if (self.modeToDisplayOnTabSwitcherDismissal ==
             TabSwitcherDismissalMode::INCOGNITO) {
    [self setCurrentInterfaceForMode:ApplicationMode::INCOGNITO];
  }

  self.modeToDisplayOnTabSwitcherDismissal = TabSwitcherDismissalMode::NONE;

  ProceduralBlock action = [self completionBlockForTriggeringAction:
                                     self.NTPActionAfterTabSwitcherDismissal];
  self.NTPActionAfterTabSwitcherDismissal = NO_ACTION;
  if (action) {
    action();
  }

  self.mainController.tabSwitcherIsActive = NO;
  self.mainController.dismissingTabSwitcher = NO;
}

#pragma mark Tab opening utility methods.

- (ProceduralBlock)completionBlockForTriggeringAction:
    (NTPTabOpeningPostOpeningAction)action {
  switch (action) {
    case START_VOICE_SEARCH:
      return ^{
        [self startVoiceSearchInCurrentBVC];
      };
    case START_QR_CODE_SCANNER:
      return ^{
        [self.mainController.currentBVC.dispatcher showQRScanner];
      };
    case FOCUS_OMNIBOX:
      return ^{
        [self.mainController.currentBVC.dispatcher focusOmnibox];
      };
    default:
      return nil;
  }
}

// Starts a voice search on the current BVC.
- (void)startVoiceSearchInCurrentBVC {
  // If the background (non-current) BVC is playing TTS audio, call
  // -startVoiceSearch on it to stop the TTS.
  BrowserViewController* backgroundBVC =
      self.mainInterface == self.currentInterface ? self.incognitoInterface.bvc
                                                  : self.mainInterface.bvc;
  if (backgroundBVC.playingTTS)
    [backgroundBVC startVoiceSearch];
  else
    [self.mainController.currentBVC startVoiceSearch];
}

#pragma mark - TabSwitching

- (BOOL)openNewTabFromTabSwitcher {
  if (!self.mainController.tabSwitcher)
    return NO;

  UrlLoadParams urlLoadParams =
      UrlLoadParams::InNewTab(GURL(kChromeUINewTabURL));
  urlLoadParams.web_params.transition_type = ui::PAGE_TRANSITION_TYPED;

  Browser* mainBrowser = self.mainInterface.browser;
  WebStateList* webStateList = mainBrowser->GetWebStateList();
  [self.mainController.tabSwitcher
      dismissWithNewTabAnimationToBrowser:mainBrowser
                        withUrlLoadParams:urlLoadParams
                                  atIndex:webStateList->count()];
  return YES;
}

#pragma mark - TabOpening implementation.

- (void)dismissModalsAndOpenSelectedTabInMode:
            (ApplicationModeForTabOpening)targetMode
                            withUrlLoadParams:
                                (const UrlLoadParams&)urlLoadParams
                               dismissOmnibox:(BOOL)dismissOmnibox
                                   completion:(ProceduralBlock)completion {
  UrlLoadParams copyOfUrlLoadParams = urlLoadParams;
  [self
      dismissModalDialogsWithCompletion:^{
        [self openSelectedTabInMode:targetMode
                  withUrlLoadParams:copyOfUrlLoadParams
                         completion:completion];
      }
                         dismissOmnibox:dismissOmnibox];
}

- (void)openTabFromLaunchOptions:(NSDictionary*)launchOptions
              startupInformation:(id<StartupInformation>)startupInformation
                        appState:(AppState*)appState {
  if (launchOptions) {
    BOOL applicationIsActive =
        [[UIApplication sharedApplication] applicationState] ==
        UIApplicationStateActive;

    [URLOpener handleLaunchOptions:launchOptions
                 applicationActive:applicationIsActive
                         tabOpener:self
                startupInformation:startupInformation
                          appState:appState];
  }
}

- (BOOL)shouldCompletePaymentRequestOnCurrentTab:
    (id<StartupInformation>)startupInformation {
  if (!startupInformation.startupParameters)
    return NO;

  if (!startupInformation.startupParameters.completePaymentRequest)
    return NO;

  if (!base::FeatureList::IsEnabled(payments::features::kWebPaymentsNativeApps))
    return NO;

  payments::IOSPaymentInstrumentLauncher* paymentAppLauncher =
      payments::IOSPaymentInstrumentLauncherFactory::GetInstance()
          ->GetForBrowserState(self.mainController.mainBrowserState);

  if (!paymentAppLauncher->delegate())
    return NO;

  std::string payment_id =
      startupInformation.startupParameters.externalURLParams
          .find(payments::kPaymentRequestIDExternal)
          ->second;
  if (paymentAppLauncher->payment_request_id() != payment_id)
    return NO;

  std::string payment_response =
      startupInformation.startupParameters.externalURLParams
          .find(payments::kPaymentRequestDataExternal)
          ->second;
  paymentAppLauncher->ReceiveResponseFromIOSPaymentInstrument(payment_response);
  [startupInformation setStartupParameters:nil];
  return YES;
}

- (BOOL)URLIsOpenedInRegularMode:(const GURL&)URL {
  WebStateList* webStateList =
      self.mainController.interfaceProvider.mainInterface.tabModel.webStateList;
  return webStateList && webStateList->GetIndexOfWebStateWithURL(URL) !=
                             WebStateList::kInvalidIndex;
}

- (BOOL)shouldOpenNTPTabOnActivationOfTabModel:(TabModel*)tabModel {
  if (self.mainController.tabSwitcherIsActive) {
    TabModel* mainTabModel =
        self.mainController.interfaceProvider.mainInterface.tabModel;
    TabModel* otrTabModel =
        self.mainController.interfaceProvider.incognitoInterface.tabModel;
    // Only attempt to dismiss the tab switcher and open a new tab if:
    // - there are no tabs open in either tab model, and
    // - the tab switcher controller is not directly or indirectly presenting
    // another view controller.
    if (![mainTabModel isEmpty] || ![otrTabModel isEmpty])
      return NO;

    // If the tabSwitcher is contained, check if the parent container is
    // presenting another view controller.
    if ([[self.mainController.tabSwitcher viewController]
                .parentViewController presentedViewController]) {
      return NO;
    }

    // Check if the tabSwitcher is directly presenting another view controller.
    if ([self.mainController.tabSwitcher viewController]
            .presentedViewController) {
      return NO;
    }

    return YES;
  }
  return ![tabModel count] && [tabModel browserState] &&
             ![tabModel browserState] -> IsOffTheRecord();
}

#pragma mark - AppURLLoadingServiceDelegate

// Note that the current tab of |browserCoordinator|'s BVC will normally be
// reloaded by this method. If a new tab is about to be added, call
// expectNewForegroundTab on the BVC first to avoid extra work and possible page
// load side-effects for the tab being replaced.
- (void)setCurrentInterfaceForMode:(ApplicationMode)mode {
  DCHECK(self.interfaceProvider);
  BOOL incognitio = mode == ApplicationMode::INCOGNITO;
  id<BrowserInterface> currentInterface =
      self.interfaceProvider.currentInterface;
  id<BrowserInterface> newInterface =
      incognitio ? self.interfaceProvider.incognitoInterface
                 : self.interfaceProvider.mainInterface;
  if (currentInterface && currentInterface == newInterface)
    return;

  // Update the snapshot before switching another application mode.  This
  // ensures that the snapshot is correct when links are opened in a different
  // application mode.
  [self updateActiveWebStateSnapshot];

  self.interfaceProvider.currentInterface = newInterface;

  if (!self.mainController.dismissingTabSwitcher)
    [self displayCurrentBVCAndFocusOmnibox:NO];

  // Tell the BVC that was made current that it can use the web.
  [self activateBVCAndMakeCurrentBVCPrimary];
}

- (void)dismissModalDialogsWithCompletion:(ProceduralBlock)completion
                           dismissOmnibox:(BOOL)dismissOmnibox {
  // Immediately hide modals from the provider (alert views, action sheets,
  // popovers). They will be ultimately dismissed by their owners, but at least,
  // they are not visible.
  ios::GetChromeBrowserProvider()->HideModalViewStack();

  // ChromeIdentityService is responsible for the dialogs displayed by the
  // services it wraps.
  ios::GetChromeBrowserProvider()->GetChromeIdentityService()->DismissDialogs();

  // MailtoHandlerProvider is responsible for the dialogs displayed by the
  // services it wraps.
  ios::GetChromeBrowserProvider()
      ->GetMailtoHandlerProvider()
      ->DismissAllMailtoHandlerInterfaces();

  // Then, depending on what the SSO view controller is presented on, dismiss
  // it.
  ProceduralBlock completionWithBVC = ^{
    DCHECK(self.mainController.currentBVC);
    DCHECK(!self.mainController.tabSwitcherIsActive);
    DCHECK(!self.signinInteractionCoordinator.isActive);
    // This will dismiss the SSO view controller.
    [self.interfaceProvider.currentInterface
        clearPresentedStateWithCompletion:completion
                           dismissOmnibox:dismissOmnibox];
  };
  ProceduralBlock completionWithoutBVC = ^{
    // |self.mainController.currentBVC| may exist but tab switcher should be
    // active.
    DCHECK(self.mainController.tabSwitcherIsActive);
    // This will dismiss the SSO view controller.
    [self.signinInteractionCoordinator cancelAndDismiss];
    // History coordinator can be started on top of the tab grid. This is not
    // true of the other tab switchers.
    DCHECK(self.mainController.mainCoordinator);
    [self.mainController.mainCoordinator
        stopChildCoordinatorsWithCompletion:completion];
  };

  // As a top level rule, if the settings are showing, they need to be
  // dismissed. Then, based on whether the BVC is present or not, a different
  // completion callback is called.
  if (!self.mainController.tabSwitcherIsActive &&
      self.isSettingsViewPresented) {
    // In this case, the settings are up and the BVC is showing. Close the
    // settings then call the BVC completion.
    [self closeSettingsAnimated:NO completion:completionWithBVC];
  } else if (self.isSettingsViewPresented) {
    // In this case, the settings are up but the BVC is not showing. Close the
    // settings then call the no-BVC completion.
    [self closeSettingsAnimated:NO completion:completionWithoutBVC];
  } else if (![self.mainController isTabSwitcherActive]) {
    // In this case, the settings are not shown but the BVC is showing. Call the
    // BVC completion.
    [self.signinInteractionCoordinator cancel];
    completionWithBVC();
  } else {
    // In this case, neither the settings nor the BVC are shown. Call the no-BVC
    // completion.
    completionWithoutBVC();
  }

  // Verify that no modal views are left presented.
  ios::GetChromeBrowserProvider()->LogIfModalViewsArePresented();
}

// Opens a tab in the target BVC, and switches to it in a way that's appropriate
// to the current UI, based on the |dismissModals| flag:
// - If a modal dialog is showing and |dismissModals| is NO, the selected tab of
// the main tab model will change in the background, but the view won't change.
// - Otherwise, any modal view will be dismissed, the tab switcher will animate
// out if it is showing, the target BVC will become active, and the new tab will
// be shown.
// If the current tab in |targetMode| is a NTP, it can be reused to open URL.
// |completion| is executed after the tab is opened. After Tab is open the
// virtual URL is set to the pending navigation item.
- (void)openSelectedTabInMode:(ApplicationModeForTabOpening)tabOpeningTargetMode
            withUrlLoadParams:(const UrlLoadParams&)urlLoadParams
                   completion:(ProceduralBlock)completion {
  // Update the snapshot before opening a new tab. This ensures that the
  // snapshot is correct when tabs are openned via the dispatcher.
  [self updateActiveWebStateSnapshot];

  ApplicationMode targetMode;

  if (tabOpeningTargetMode == ApplicationModeForTabOpening::CURRENT) {
    targetMode = self.interfaceProvider.currentInterface.incognito
                     ? ApplicationMode::INCOGNITO
                     : ApplicationMode::NORMAL;
  } else if (tabOpeningTargetMode == ApplicationModeForTabOpening::NORMAL) {
    targetMode = ApplicationMode::NORMAL;
  } else {
    targetMode = ApplicationMode::INCOGNITO;
  }

  id<BrowserInterface> targetInterface =
      targetMode == ApplicationMode::NORMAL
          ? self.interfaceProvider.mainInterface
          : self.interfaceProvider.incognitoInterface;
  NSUInteger tabIndex = NSNotFound;
  ProceduralBlock startupCompletion = [self
      completionBlockForTriggeringAction:[self.mainController.startupParameters
                                                 postOpeningAction]];

  // Commands are only allowed on NTP.
  DCHECK(IsURLNtp(urlLoadParams.web_params.url) || !startupCompletion);

  ProceduralBlock tabOpenedCompletion = nil;
  if (startupCompletion && completion) {
    tabOpenedCompletion = ^{
      // Order is important here. |completion| may do cleaning tasks that will
      // invalidate |startupCompletion|.
      startupCompletion();
      completion();
    };
  } else if (startupCompletion) {
    tabOpenedCompletion = startupCompletion;
  } else {
    tabOpenedCompletion = completion;
  }

  if (self.mainController.tabSwitcherIsActive) {
    // If the tab switcher is already being dismissed, simply add the tab and
    // note that when the tab switcher finishes dismissing, the current BVC
    // should be switched to be the main BVC if necessary.
    if (self.mainController.dismissingTabSwitcher) {
      self.modeToDisplayOnTabSwitcherDismissal =
          targetMode == ApplicationMode::NORMAL
              ? TabSwitcherDismissalMode::NORMAL
              : TabSwitcherDismissalMode::INCOGNITO;
      [targetInterface.bvc appendTabAddedCompletion:tabOpenedCompletion];
      UrlLoadParams savedParams = urlLoadParams;
      savedParams.in_incognito = targetMode == ApplicationMode::INCOGNITO;
      UrlLoadingServiceFactory::GetForBrowserState(
          [targetInterface.bvc browserState])
          ->Load(savedParams);
    } else {
      // Voice search, QRScanner and the omnibox are presented by the BVC.
      // They must be started after the BVC view is added in the hierarchy.
      self.NTPActionAfterTabSwitcherDismissal =
          [self.mainController.startupParameters postOpeningAction];
      [self.mainController setStartupParameters:nil];
      [self.mainController.tabSwitcher
          dismissWithNewTabAnimationToBrowser:targetInterface.browser
                            withUrlLoadParams:urlLoadParams
                                      atIndex:tabIndex];
    }
  } else {
    if (!self.mainController.currentBVC.presentedViewController) {
      [targetInterface.bvc expectNewForegroundTab];
    }
    [self setCurrentInterfaceForMode:targetMode];
    [self openOrReuseTabInMode:targetMode
             withUrlLoadParams:urlLoadParams
           tabOpenedCompletion:tabOpenedCompletion];
  }

  if (self.mainController.restoreHelper) {
    // Now that all the operations on the tabs have been done, display the
    // restore infobar if needed.
    dispatch_async(dispatch_get_main_queue(), ^{
      TabModel* currentTabModel = [self currentTabModel];
      [self.mainController.restoreHelper
          showRestoreIfNeededUsingWebState:currentTabModel.webStateList
                                               ->GetActiveWebState()
                           sessionRestorer:currentTabModel];
      self.mainController.restoreHelper = nil;
    });
  }
}

- (void)openURLInNewTabWithCommand:(OpenNewTabCommand*)command {
  [self openURLInNewTab:command];
}

- (void)expectNewForegroundTabForMode:(ApplicationMode)targetMode {
  id<BrowserInterface> interface =
      targetMode == ApplicationMode::INCOGNITO
          ? self.interfaceProvider.incognitoInterface
          : self.interfaceProvider.mainInterface;
  DCHECK(interface);
  [interface.bvc expectNewForegroundTab];
}

- (void)openNewTabFromOriginPoint:(CGPoint)originPoint
                     focusOmnibox:(BOOL)focusOmnibox {
  [self.mainController.currentBVC openNewTabFromOriginPoint:originPoint
                                               focusOmnibox:focusOmnibox];
}

- (ChromeBrowserState*)currentBrowserState {
  return self.mainController.interfaceProvider.currentInterface.browserState;
}

- (TabModel*)currentTabModel {
  return self.mainController.interfaceProvider.currentInterface.bvc.tabModel;
}

// Asks the respective Snapshot helper to update the snapshot for the active
// WebState.
- (void)updateActiveWebStateSnapshot {
  WebStateList* webStateList =
      self.mainController.currentBVC.tabModel.webStateList;
  if (webStateList) {
    web::WebState* webState = webStateList->GetActiveWebState();
    if (webState) {
      SnapshotTabHelper::FromWebState(webState)->UpdateSnapshotWithCallback(
          nil);
    }
  }
}


// Checks the target BVC's current tab's URL. If this URL is chrome://newtab,
// loads |urlLoadParams| in this tab. Otherwise, open |urlLoadParams| in a new
// tab in the target BVC. |tabDisplayedCompletion| will be called on the new tab
// (if not nil).
- (void)openOrReuseTabInMode:(ApplicationMode)targetMode
           withUrlLoadParams:(const UrlLoadParams&)urlLoadParams
         tabOpenedCompletion:(ProceduralBlock)tabOpenedCompletion {
  BrowserViewController* targetBVC =
      targetMode == ApplicationMode::NORMAL
          ? self.mainController.interfaceProvider.mainInterface.bvc
          : self.mainController.interfaceProvider.incognitoInterface.bvc;
  web::WebState* currentWebState =
      targetBVC.tabModel.webStateList->GetActiveWebState();

  // Don't call loadWithParams for chrome://newtab when it's already loaded.
  // Note that it's safe to use -GetVisibleURL here, as it doesn't matter if the
  // NTP hasn't finished loading.
  if (currentWebState && IsURLNtp(currentWebState->GetVisibleURL()) &&
      IsURLNtp(urlLoadParams.web_params.url)) {
    if (tabOpenedCompletion) {
      tabOpenedCompletion();
    }
    return;
  }

  // With kBrowserContainerContainsNTP enabled paired with a restored NTP
  // session, the NTP may appear committed when it is still loading.  For the
  // time being, always load within a new tab when this feature is enabled.
  // TODO(crbug.com/931284): Revert this change when fixed.
  BOOL alwaysInsertNewTab =
      base::FeatureList::IsEnabled(kBlockNewTabPagePendingLoad);
  // If the current tab isn't an NTP, open a new tab.  Be sure to use
  // -GetLastCommittedURL incase the NTP is still loading.
  if (alwaysInsertNewTab ||
      !(currentWebState && IsURLNtp(currentWebState->GetVisibleURL()))) {
    [targetBVC appendTabAddedCompletion:tabOpenedCompletion];
    UrlLoadParams newTabParams = urlLoadParams;
    newTabParams.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
    newTabParams.in_incognito = targetMode == ApplicationMode::INCOGNITO;
    UrlLoadingServiceFactory::GetForBrowserState([targetBVC browserState])
        ->Load(newTabParams);
    return;
  }

  // Otherwise, load |urlLoadParams| in the current tab.
  UrlLoadParams sameTabParams = urlLoadParams;
  sameTabParams.disposition = WindowOpenDisposition::CURRENT_TAB;
  UrlLoadingServiceFactory::GetForBrowserState([targetBVC browserState])
      ->Load(sameTabParams);
  if (tabOpenedCompletion) {
    tabOpenedCompletion();
  }
}

// Displays current (incognito/normal) BVC and optionally focuses the omnibox.
- (void)displayCurrentBVCAndFocusOmnibox:(BOOL)focusOmnibox {
  ProceduralBlock completion = nil;
  if (focusOmnibox) {
    __weak BrowserViewController* weakCurrentBVC =
        self.mainController.currentBVC;
    completion = ^{
      [weakCurrentBVC.dispatcher focusOmnibox];
    };
  }
  [self.mainController.mainCoordinator
      showTabViewController:self.currentInterface.bvc
                 completion:completion];
  [self.currentInterface.bvc.dispatcher
      setIncognitoContentVisible:(self.currentInterface ==
                                  self.incognitoInterface)];
}

#pragma mark - AppNavigation

- (void)presentSignedInAccountsViewControllerForBrowserState:
    (ChromeBrowserState*)browserState {
  UIViewController* accountsViewController =
      [[SignedInAccountsViewController alloc]
          initWithBrowserState:browserState
                    dispatcher:self.mainController.mainBVC.dispatcher];
  [[self topPresentedViewController]
      presentViewController:accountsViewController
                   animated:YES
                 completion:nil];
}

- (void)closeSettingsAnimated:(BOOL)animated
                   completion:(ProceduralBlock)completion {
  if (self.settingsNavigationController) {
    [self.settingsNavigationController cleanUpSettings];
    UIViewController* presentingViewController =
        [self.settingsNavigationController presentingViewController];
    // If presentingViewController is nil it means the VC was already dismissed
    // by some other action like swiping down.
    DCHECK(presentingViewController);
    [presentingViewController dismissViewControllerAnimated:animated
                                                 completion:completion];
    self.settingsNavigationController = nil;
    return;
  }
  // |self.signinInteractionCoordinator| can also present settings, like
  // the advanced sign-in settings navigation controller. If the settings has
  // to be cloase, it is thus the responsibility of the main controller to
  // dismiss the the advanced sign-in settings by dismssing the settings
  // presented by |self.signinInteractionCoordinator|.
  // To reproduce this case:
  //  - open Bookmark view
  //  - start sign-in
  //  - tap on "Settings" to open the advanced sign-in settings
  //  - tap on "Manage Your Google Account"
  DCHECK(self.signinInteractionCoordinator.isSettingsViewPresented);
  [self.signinInteractionCoordinator
      abortAndDismissSettingsViewAnimated:animated
                               completion:completion];
}

- (UIViewController*)topPresentedViewController {
  // TODO(crbug.com/754642): Implement TopPresentedViewControllerFrom()
  // privately.
  return top_view_controller::TopPresentedViewControllerFrom(
      self.mainController.mainCoordinator.viewController);
}

#pragma mark - WebStateListObserving

// Called when a WebState is removed. Triggers the switcher view when the last
// WebState is closed on a device that uses the switcher.
- (void)webStateList:(WebStateList*)notifiedWebStateList
    didDetachWebState:(web::WebState*)webState
              atIndex:(int)atIndex {
  // Do nothing on initialization.
  if (![self currentTabModel].webStateList)
    return;

  if (notifiedWebStateList->empty()) {
    if (webState->GetBrowserState()->IsOffTheRecord()) {
      [self lastIncognitoTabClosed];
    } else {
      [self lastRegularTabClosed];
    }
  }
}

// Called when the last incognito tab was closed.
- (void)lastIncognitoTabClosed {
  // This seems the best place to mark the start of destroying the incognito
  // browser state.
  breakpad_helper::SetDestroyingAndRebuildingIncognitoBrowserState(
      /*in_progress=*/true);
  DCHECK(self.mainController.mainBrowserState
             ->HasOffTheRecordChromeBrowserState());
  [self clearIOSSpecificIncognitoData];

  // OffTheRecordProfileIOData cannot be deleted before all the requests are
  // deleted. Queue browser state recreation on IO thread.
  base::PostTaskAndReply(FROM_HERE, {web::WebThread::IO}, base::DoNothing(),
                         base::BindRepeating(^{
                           [self destroyAndRebuildIncognitoBrowserState];
                         }));

  // a) The first condition can happen when the last incognito tab is closed
  // from the tab switcher.
  // b) The second condition can happen if some other code (like JS) triggers
  // closure of tabs from the otr tab model when it's not current.
  // Nothing to do here. The next user action (like clicking on an existing
  // regular tab or creating a new incognito tab from the settings menu) will
  // take care of the logic to mode switch.
  if (self.mainController.tabSwitcherIsActive ||
      ![self.mainController.currentTabModel isOffTheRecord]) {
    return;
  }

  if ([self.mainController.currentTabModel count] == 0U) {
    [self showTabSwitcher];
  } else {
    [self setCurrentInterfaceForMode:ApplicationMode::NORMAL];
  }
}

// Called when the last regular tab was closed.
- (void)lastRegularTabClosed {
  // a) The first condition can happen when the last regular tab is closed from
  // the tab switcher.
  // b) The second condition can happen if some other code (like JS) triggers
  // closure of tabs from the main tab model when the main tab model is not
  // current.
  // Nothing to do here.
  if (self.mainController.tabSwitcherIsActive ||
      [self.mainController.currentTabModel isOffTheRecord]) {
    return;
  }

  [self showTabSwitcher];
}

- (void)clearIOSSpecificIncognitoData {
  DCHECK(self.mainController.mainBrowserState
             ->HasOffTheRecordChromeBrowserState());
  ChromeBrowserState* otrBrowserState =
      self.mainController.mainBrowserState->GetOffTheRecordChromeBrowserState();
  [self.mainController
      removeBrowsingDataForBrowserState:otrBrowserState
                             timePeriod:browsing_data::TimePeriod::ALL_TIME
                             removeMask:BrowsingDataRemoveMask::REMOVE_ALL
                        completionBlock:^{
                          [self activateBVCAndMakeCurrentBVCPrimary];
                        }];
}

- (void)activateBVCAndMakeCurrentBVCPrimary {
  // If there are pending removal operations, the activation will be deferred
  // until the callback is received.
  BrowsingDataRemover* browsingDataRemover =
      BrowsingDataRemoverFactory::GetForBrowserStateIfExists(
          self.currentBrowserState);
  if (browsingDataRemover && browsingDataRemover->IsRemoving())
    return;

  self.interfaceProvider.mainInterface.userInteractionEnabled = YES;
  self.interfaceProvider.incognitoInterface.userInteractionEnabled = YES;
  [self.mainController.currentBVC setPrimary:YES];
}

- (void)showTabSwitcher {
  DCHECK(self.mainController.tabSwitcher);
  // Tab switcher implementations may need to rebuild state before being
  // displayed.
  [self.mainController.tabSwitcher
      restoreInternalStateWithMainBrowser:self.mainInterface.browser
                               otrBrowser:self.incognitoInterface.browser
                            activeBrowser:self.currentInterface.browser];
  self.mainController.tabSwitcherIsActive = YES;
  [self.mainController.tabSwitcher setDelegate:self];

  [self.mainController.mainCoordinator
      showTabSwitcher:self.mainController.tabSwitcher];
}

// Destroys and rebuilds the incognito browser state.
- (void)destroyAndRebuildIncognitoBrowserState {
  BOOL otrBVCIsCurrent = (self.interfaceProvider.mainInterface.bvc ==
                          self.interfaceProvider.incognitoInterface.bvc);

  // Clear the Incognito Browser and notify the _tabSwitcher that its otrBrowser
  // will be destroyed.
  [self.mainController.tabSwitcher setOtrBrowser:nil];

  if (base::FeatureList::IsEnabled(kLogBreadcrumbs)) {
    BreadcrumbManagerBrowserAgent::FromBrowser(self.incognitoInterface.browser)
        ->SetLoggingEnabled(false);

    breakpad::StopMonitoringBreadcrumbManagerService(
        BreadcrumbManagerKeyedServiceFactory::GetForBrowserState(
            self.incognitoInterface.browserState));
  }

  [self.mainController.browserViewWrangler destroyAndRebuildIncognitoBrowser];

  if (base::FeatureList::IsEnabled(kLogBreadcrumbs)) {
    breakpad::MonitorBreadcrumbManagerService(
        BreadcrumbManagerKeyedServiceFactory::GetForBrowserState(
            self.incognitoInterface.browserState));
  }

  if (otrBVCIsCurrent) {
    [self activateBVCAndMakeCurrentBVCPrimary];
  }

  // Always set the new otr Browser for the tablet or grid switcher.
  // Notify the _tabSwitcher with the new Incognito Browser.
  [self.mainController.tabSwitcher
      setOtrBrowser:self.incognitoInterface.browser];

  // This seems the best place to deem the destroying and rebuilding the
  // incognito browser state to be completed.
  breakpad_helper::SetDestroyingAndRebuildingIncognitoBrowserState(
      /*in_progress=*/false);
}

@end
