// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ios/web_view/internal/autofill/cwv_autofill_controller_internal.h"

#import <Foundation/Foundation.h>

#include <memory>

#include "base/run_loop.h"
#include "base/strings/sys_string_conversions.h"
#import "base/test/ios/wait_util.h"
#include "components/autofill/core/browser/autofill_manager.h"
#import "components/autofill/ios/browser/fake_autofill_agent.h"
#import "components/autofill/ios/browser/fake_js_autofill_manager.h"
#import "components/autofill/ios/browser/form_suggestion.h"
#import "components/autofill/ios/browser/js_suggestion_manager.h"
#include "components/autofill/ios/form_util/form_activity_params.h"
#import "components/autofill/ios/form_util/form_activity_tab_helper.h"
#import "components/autofill/ios/form_util/test_form_activity_tab_helper.h"
#import "ios/web/public/deprecated/crw_test_js_injection_receiver.h"
#include "ios/web/public/js_messaging/web_frames_manager.h"
#import "ios/web/public/test/fakes/fake_web_frame.h"
#import "ios/web/public/test/fakes/fake_web_frames_manager.h"
#import "ios/web/public/test/fakes/test_web_state.h"
#include "ios/web/public/test/scoped_testing_web_client.h"
#include "ios/web/public/test/web_task_environment.h"
#include "ios/web/public/web_client.h"
#import "ios/web_view/internal/autofill/cwv_autofill_suggestion_internal.h"
#include "ios/web_view/internal/web_view_browser_state.h"
#import "ios/web_view/public/cwv_autofill_controller_delegate.h"
#include "ios/web_view/test/test_with_locale_and_resources.h"
#include "testing/gtest/include/gtest/gtest.h"
#import "testing/gtest_mac.h"
#include "testing/platform_test.h"
#import "third_party/ocmock/OCMock/OCMock.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

using base::test::ios::kWaitForActionTimeout;
using base::test::ios::WaitUntilConditionOrTimeout;

namespace ios_web_view {

namespace {

NSString* const kTestFormName = @"FormName";
NSString* const kTestFieldIdentifier = @"FieldIdentifier";
NSString* const kTestFrameId = @"FrameID";
NSString* const kTestFieldValue = @"FieldValue";

}  // namespace

class CWVAutofillControllerTest : public TestWithLocaleAndResources {
 protected:
  CWVAutofillControllerTest()
      : web_client_(std::make_unique<web::WebClient>()),
        task_environment_(web::WebTaskEnvironment::IO_MAINLOOP),
        browser_state_(/*off_the_record=*/false) {
    test_web_state_.SetBrowserState(&browser_state_);
    CRWTestJSInjectionReceiver* injectionReceiver =
        [[CRWTestJSInjectionReceiver alloc] init];
    test_web_state_.SetJSInjectionReceiver(injectionReceiver);

    js_autofill_manager_ =
        [[FakeJSAutofillManager alloc] initWithReceiver:injectionReceiver];
    js_suggestion_manager_ = OCMClassMock([JsSuggestionManager class]);

    autofill_agent_ =
        [[FakeAutofillAgent alloc] initWithPrefService:browser_state_.GetPrefs()
                                              webState:&test_web_state_];

    auto frames_manager = std::make_unique<web::FakeWebFramesManager>();
    fake_web_frames_manager_ = frames_manager.get();
    test_web_state_.SetWebFramesManager(std::move(frames_manager));

    autofill_controller_ =
        [[CWVAutofillController alloc] initWithWebState:&test_web_state_
                                          autofillAgent:autofill_agent_
                                      JSAutofillManager:js_autofill_manager_
                                    JSSuggestionManager:js_suggestion_manager_];
    test_form_activity_tab_helper_ =
        std::make_unique<autofill::TestFormActivityTabHelper>(&test_web_state_);
  }

  void AddWebFrame(std::unique_ptr<web::WebFrame> frame) {
    web::WebFrame* frame_ptr = frame.get();
    fake_web_frames_manager_->AddWebFrame(std::move(frame));
    test_web_state_.OnWebFrameDidBecomeAvailable(frame_ptr);
  }

  web::ScopedTestingWebClient web_client_;
  web::WebTaskEnvironment task_environment_;
  ios_web_view::WebViewBrowserState browser_state_;
  web::TestWebState test_web_state_;
  web::FakeWebFramesManager* fake_web_frames_manager_;
  CWVAutofillController* autofill_controller_;
  FakeAutofillAgent* autofill_agent_;
  FakeJSAutofillManager* js_autofill_manager_;
  std::unique_ptr<autofill::TestFormActivityTabHelper>
      test_form_activity_tab_helper_;
  id js_suggestion_manager_;
};

// Tests CWVAutofillController fetch suggestions.
// This test is flaky (crbug.com/1048256).
TEST_F(CWVAutofillControllerTest, FLAKY_FetchSuggestions) {
  FormSuggestion* suggestion =
      [FormSuggestion suggestionWithValue:kTestFieldValue
                       displayDescription:nil
                                     icon:nil
                               identifier:0];
  [autofill_agent_ addSuggestion:suggestion
                     forFormName:kTestFormName
                 fieldIdentifier:kTestFieldIdentifier
                         frameID:kTestFrameId];

  __block BOOL fetch_completion_was_called = NO;
  id fetch_completion = ^(NSArray<CWVAutofillSuggestion*>* suggestions) {
    ASSERT_EQ(1U, suggestions.count);
    CWVAutofillSuggestion* suggestion = suggestions.firstObject;
    EXPECT_NSEQ(kTestFieldValue, suggestion.value);
    EXPECT_NSEQ(kTestFormName, suggestion.formName);
    fetch_completion_was_called = YES;
  };
  [autofill_controller_ fetchSuggestionsForFormWithName:kTestFormName
                                        fieldIdentifier:kTestFieldIdentifier
                                              fieldType:@""
                                                frameID:kTestFrameId
                                      completionHandler:fetch_completion];

  EXPECT_TRUE(WaitUntilConditionOrTimeout(kWaitForActionTimeout, ^bool {
    base::RunLoop().RunUntilIdle();
    return fetch_completion_was_called;
  }));
}

// Tests CWVAutofillController accepts suggestion.
TEST_F(CWVAutofillControllerTest, AcceptSuggestion) {
  FormSuggestion* form_suggestion =
      [FormSuggestion suggestionWithValue:kTestFieldValue
                       displayDescription:nil
                                     icon:nil
                               identifier:0];
  CWVAutofillSuggestion* suggestion =
      [[CWVAutofillSuggestion alloc] initWithFormSuggestion:form_suggestion
                                                   formName:kTestFormName
                                            fieldIdentifier:kTestFieldIdentifier
                                                    frameID:kTestFrameId
                                       isPasswordSuggestion:NO];
  __block BOOL accept_completion_was_called = NO;
  [autofill_controller_ acceptSuggestion:suggestion
                       completionHandler:^{
                         accept_completion_was_called = YES;
                       }];

  EXPECT_TRUE(WaitUntilConditionOrTimeout(kWaitForActionTimeout, ^bool {
    base::RunLoop().RunUntilIdle();
    return accept_completion_was_called;
  }));
  EXPECT_NSEQ(
      form_suggestion,
      [autofill_agent_ selectedSuggestionForFormName:kTestFormName
                                     fieldIdentifier:kTestFieldIdentifier
                                             frameID:kTestFrameId]);
}

// Tests CWVAutofillController clears form.
TEST_F(CWVAutofillControllerTest, ClearForm) {
  auto frame = std::make_unique<web::FakeWebFrame>(
      base::SysNSStringToUTF8(kTestFrameId), true, GURL::EmptyGURL());
  AddWebFrame(std::move(frame));
  __block BOOL clear_form_completion_was_called = NO;
  [autofill_controller_ clearFormWithName:kTestFormName
                          fieldIdentifier:kTestFieldIdentifier
                                  frameID:kTestFrameId
                        completionHandler:^{
                          clear_form_completion_was_called = YES;
                        }];

  EXPECT_TRUE(WaitUntilConditionOrTimeout(kWaitForActionTimeout, ^bool {
    base::RunLoop().RunUntilIdle();
    return clear_form_completion_was_called;
  }));
  EXPECT_NSEQ(kTestFormName, js_autofill_manager_.lastClearedFormName);
  EXPECT_NSEQ(kTestFieldIdentifier,
              js_autofill_manager_.lastClearedFieldIdentifier);
  EXPECT_NSEQ(kTestFrameId, js_autofill_manager_.lastClearedFrameIdentifier);
}

// Tests CWVAutofillController focus previous field.
TEST_F(CWVAutofillControllerTest, FocusPrevious) {
  [[js_suggestion_manager_ expect] selectPreviousElementInFrameWithID:nil];
  [autofill_controller_ focusPreviousField];
  [js_suggestion_manager_ verify];
}

// Tests CWVAutofillController focus next field.
TEST_F(CWVAutofillControllerTest, FocusNext) {
  [[js_suggestion_manager_ expect] selectNextElementInFrameWithID:nil];
  [autofill_controller_ focusNextField];
  [js_suggestion_manager_ verify];
}

// Tests CWVAutofillController checks previous and next focusable state.
TEST_F(CWVAutofillControllerTest, CheckFocus) {
  id completionHandler = ^(BOOL previous, BOOL next) {
  };
  [[js_suggestion_manager_ expect]
      fetchPreviousAndNextElementsPresenceInFrameWithID:nil
                                      completionHandler:completionHandler];
  [autofill_controller_
      checkIfPreviousAndNextFieldsAreAvailableForFocusWithCompletionHandler:
          completionHandler];
  [js_suggestion_manager_ verify];
}

// Tests CWVAutofillController delegate focus callback is invoked.
TEST_F(CWVAutofillControllerTest, FocusCallback) {
    id delegate = OCMProtocolMock(@protocol(CWVAutofillControllerDelegate));
    autofill_controller_.delegate = delegate;

    // [delegate expect] returns an autoreleased object, but it must be
    // destroyed before this test exits to avoid holding on to
    // |autofill_controller_|.
    @autoreleasepool {
      [[delegate expect] autofillController:autofill_controller_
              didFocusOnFieldWithIdentifier:kTestFieldIdentifier
                                  fieldType:@""
                                   formName:kTestFormName
                                    frameID:kTestFrameId
                                      value:kTestFieldValue
                              userInitiated:YES];

      autofill::FormActivityParams params;
      params.form_name = base::SysNSStringToUTF8(kTestFormName);
      params.field_identifier = base::SysNSStringToUTF8(kTestFieldIdentifier);
      params.value = base::SysNSStringToUTF8(kTestFieldValue);
      params.frame_id = base::SysNSStringToUTF8(kTestFrameId);
      params.has_user_gesture = true;
      params.type = "focus";
      web::FakeWebFrame frame(base::SysNSStringToUTF8(kTestFrameId), true,
                              GURL::EmptyGURL());
      test_form_activity_tab_helper_->FormActivityRegistered(&frame, params);
      [delegate verify];
  }
}

// Tests CWVAutofillController delegate input callback is invoked.
TEST_F(CWVAutofillControllerTest, InputCallback) {
    id delegate = OCMProtocolMock(@protocol(CWVAutofillControllerDelegate));
    autofill_controller_.delegate = delegate;

    // [delegate expect] returns an autoreleased object, but it must be
    // destroyed before this test exits to avoid holding on to
    // |autofill_controller_|.
    @autoreleasepool {
      [[delegate expect] autofillController:autofill_controller_
              didInputInFieldWithIdentifier:kTestFieldIdentifier
                                  fieldType:@""
                                   formName:kTestFormName
                                    frameID:kTestFrameId
                                      value:kTestFieldValue
                              userInitiated:YES];

      autofill::FormActivityParams params;
      params.form_name = base::SysNSStringToUTF8(kTestFormName);
      params.field_identifier = base::SysNSStringToUTF8(kTestFieldIdentifier);
      params.value = base::SysNSStringToUTF8(kTestFieldValue);
      params.frame_id = base::SysNSStringToUTF8(kTestFrameId);
      params.type = "input";
      params.has_user_gesture = true;
      web::FakeWebFrame frame(base::SysNSStringToUTF8(kTestFrameId), true,
                              GURL::EmptyGURL());
      test_form_activity_tab_helper_->FormActivityRegistered(&frame, params);
      [delegate verify];
  }
}

// Tests CWVAutofillController delegate blur callback is invoked.
TEST_F(CWVAutofillControllerTest, BlurCallback) {
  id delegate = OCMProtocolMock(@protocol(CWVAutofillControllerDelegate));
  autofill_controller_.delegate = delegate;

  // [delegate expect] returns an autoreleased object, but it must be destroyed
  // before this test exits to avoid holding on to |autofill_controller_|.
  @autoreleasepool {
    [[delegate expect] autofillController:autofill_controller_
             didBlurOnFieldWithIdentifier:kTestFieldIdentifier
                                fieldType:@""
                                 formName:kTestFormName
                                  frameID:kTestFrameId
                                    value:kTestFieldValue
                            userInitiated:YES];

    autofill::FormActivityParams params;
    params.form_name = base::SysNSStringToUTF8(kTestFormName);
    params.field_identifier = base::SysNSStringToUTF8(kTestFieldIdentifier);
    params.value = base::SysNSStringToUTF8(kTestFieldValue);
    params.frame_id = base::SysNSStringToUTF8(kTestFrameId);
    params.type = "blur";
    params.has_user_gesture = true;
    web::FakeWebFrame frame(base::SysNSStringToUTF8(kTestFrameId), true,
                            GURL::EmptyGURL());
    test_form_activity_tab_helper_->FormActivityRegistered(&frame, params);

    [delegate verify];
  }
}

// Tests CWVAutofillController delegate submit callback is invoked.
TEST_F(CWVAutofillControllerTest, SubmitCallback) {
  id delegate = OCMProtocolMock(@protocol(CWVAutofillControllerDelegate));
  autofill_controller_.delegate = delegate;

  // [delegate expect] returns an autoreleased object, but it must be destroyed
  // before this test exits to avoid holding on to |autofill_controller_|.
  @autoreleasepool {
    [[delegate expect] autofillController:autofill_controller_
                    didSubmitFormWithName:kTestFormName
                                  frameID:kTestFrameId
                            userInitiated:YES];
    web::FakeWebFrame frame(base::SysNSStringToUTF8(kTestFrameId), true,
                            GURL::EmptyGURL());
    test_form_activity_tab_helper_->DocumentSubmitted(
        /*sender_frame*/ &frame, base::SysNSStringToUTF8(kTestFormName),
        /*form_data=*/"",
        /*user_initiated=*/true,
        /*is_main_frame=*/true);

    [[delegate expect] autofillController:autofill_controller_
                    didSubmitFormWithName:kTestFormName
                                  frameID:kTestFrameId
                            userInitiated:NO];

    test_form_activity_tab_helper_->DocumentSubmitted(
        /*sender_frame*/ &frame, base::SysNSStringToUTF8(kTestFormName),
        /*form_data=*/"",
        /*user_initiated=*/false,
        /*is_main_frame=*/true);

    [delegate verify];
  }
}

}  // namespace ios_web_view
