// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <tuple>

#include "base/bind.h"
#include "base/macros.h"
#include "base/run_loop.h"
#include "base/time/time.h"
#include "chrome/common/chrome_isolated_world_ids.h"
#include "chrome/test/base/chrome_render_view_test.h"
#include "components/translate/content/common/translate.mojom.h"
#include "components/translate/content/renderer/translate_agent.h"
#include "components/translate/core/common/translate_constants.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_view.h"
#include "extensions/common/constants.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/browser_interface_broker_proxy.h"
#include "third_party/blink/public/web/web_local_frame.h"

using testing::_;
using testing::AtLeast;
using testing::Return;

namespace {

class FakeContentTranslateDriver
    : public translate::mojom::ContentTranslateDriver {
 public:
  FakeContentTranslateDriver()
      : called_new_page_(false), page_needs_translation_(false) {}
  ~FakeContentTranslateDriver() override {}

  void BindHandle(mojo::ScopedMessagePipeHandle handle) {
    receivers_.Add(
        this, mojo::PendingReceiver<translate::mojom::ContentTranslateDriver>(
                  std::move(handle)));
  }

  // translate::mojom::ContentTranslateDriver implementation.
  void RegisterPage(
      mojo::PendingRemote<translate::mojom::TranslateAgent> translate_agent,
      const translate::LanguageDetectionDetails& details,
      bool page_needs_translation) override {
    called_new_page_ = true;
    details_ = details;
    page_needs_translation_ = page_needs_translation;
  }

  void ResetNewPageValues() {
    called_new_page_ = false;
    details_ = base::nullopt;
    page_needs_translation_ = false;
  }

  bool called_new_page_;
  base::Optional<translate::LanguageDetectionDetails> details_;
  bool page_needs_translation_;

 private:
  mojo::ReceiverSet<translate::mojom::ContentTranslateDriver> receivers_;
};

}  // namespace

class TestTranslateAgent : public translate::TranslateAgent {
 public:
  explicit TestTranslateAgent(content::RenderFrame* render_frame)
      : translate::TranslateAgent(render_frame,
                                  ISOLATED_WORLD_ID_TRANSLATE,
                                  extensions::kExtensionScheme) {}

  base::TimeDelta AdjustDelay(int delayInMs) override {
    // Just returns base::TimeDelta() which has initial value 0.
    // Tasks doesn't need to be delayed in tests.
    return base::TimeDelta();
  }

  void TranslatePage(const std::string& source_lang,
                     const std::string& target_lang,
                     const std::string& translate_script) {
    // Reset result values firstly.
    page_translated_ = false;
    trans_result_cancelled_ = false;
    trans_result_original_lang_ = base::nullopt;
    trans_result_translated_lang_ = base::nullopt;
    trans_result_error_type_ = translate::TranslateErrors::NONE;

    // Will get new result values via OnPageTranslated.
    TranslateFrame(translate_script, source_lang, target_lang,
                   base::Bind(&TestTranslateAgent::OnPageTranslated,
                              base::Unretained(this)));
  }

  bool GetPageTranslatedResult(std::string* original_lang,
                               std::string* target_lang,
                               translate::TranslateErrors::Type* error) {
    if (!page_translated_)
      return false;
    if (original_lang)
      *original_lang = *trans_result_original_lang_;
    if (target_lang)
      *target_lang = *trans_result_translated_lang_;
    if (error)
      *error = trans_result_error_type_;
    return true;
  }

  MOCK_METHOD0(IsTranslateLibAvailable, bool());
  MOCK_METHOD0(IsTranslateLibReady, bool());
  MOCK_METHOD0(HasTranslationFinished, bool());
  MOCK_METHOD0(HasTranslationFailed, bool());
  MOCK_METHOD0(GetOriginalPageLanguage, std::string());
  MOCK_METHOD0(GetErrorCode, int64_t());
  MOCK_METHOD0(StartTranslation, bool());
  MOCK_METHOD1(ExecuteScript, void(const std::string&));
  MOCK_METHOD2(ExecuteScriptAndGetBoolResult, bool(const std::string&, bool));
  MOCK_METHOD1(ExecuteScriptAndGetStringResult,
               std::string(const std::string&));
  MOCK_METHOD1(ExecuteScriptAndGetDoubleResult, double(const std::string&));
  MOCK_METHOD1(ExecuteScriptAndGetIntegerResult, int64_t(const std::string&));

 private:
  void OnPageTranslated(bool cancelled,
                        const std::string& original_lang,
                        const std::string& translated_lang,
                        translate::TranslateErrors::Type error_type) {
    page_translated_ = true;
    trans_result_cancelled_ = cancelled;
    trans_result_original_lang_ = original_lang;
    trans_result_translated_lang_ = translated_lang;
    trans_result_error_type_ = error_type;
  }

  bool page_translated_;
  bool trans_result_cancelled_;
  base::Optional<std::string> trans_result_original_lang_;
  base::Optional<std::string> trans_result_translated_lang_;
  translate::TranslateErrors::Type trans_result_error_type_;

  DISALLOW_COPY_AND_ASSIGN(TestTranslateAgent);
};

class TranslateAgentBrowserTest : public ChromeRenderViewTest {
 public:
  TranslateAgentBrowserTest() : translate_agent_(nullptr) {}

 protected:
  void SetUp() override {
    ChromeRenderViewTest::SetUp();
    translate_agent_ = new TestTranslateAgent(view_->GetMainRenderFrame());

    view_->GetMainRenderFrame()
        ->GetBrowserInterfaceBroker()
        ->SetBinderForTesting(
            translate::mojom::ContentTranslateDriver::Name_,
            base::Bind(&FakeContentTranslateDriver::BindHandle,
                       base::Unretained(&fake_translate_driver_)));
  }

  void TearDown() override {
    view_->GetMainRenderFrame()
        ->GetBrowserInterfaceBroker()
        ->SetBinderForTesting(translate::mojom::ContentTranslateDriver::Name_,
                              {});

    delete translate_agent_;
    ChromeRenderViewTest::TearDown();
  }

  TestTranslateAgent* translate_agent_;
  FakeContentTranslateDriver fake_translate_driver_;

 private:
  DISALLOW_COPY_AND_ASSIGN(TranslateAgentBrowserTest);
};

// Tests that the browser gets notified of the translation failure if the
// translate library fails/times-out during initialization.
TEST_F(TranslateAgentBrowserTest, TranslateLibNeverReady) {
  // We make IsTranslateLibAvailable true so we don't attempt to inject the
  // library.
  EXPECT_CALL(*translate_agent_, IsTranslateLibAvailable())
      .Times(AtLeast(1))
      .WillRepeatedly(Return(true));

  EXPECT_CALL(*translate_agent_, IsTranslateLibReady())
      .Times(AtLeast(5))  // See kMaxTranslateInitCheckAttempts in
                          // translate_agent.cc
      .WillRepeatedly(Return(false));

  EXPECT_CALL(*translate_agent_, GetErrorCode())
      .Times(AtLeast(5))
      .WillRepeatedly(Return(translate::TranslateErrors::NONE));

  translate_agent_->TranslatePage("en", "fr", std::string());
  base::RunLoop().RunUntilIdle();

  translate::TranslateErrors::Type error;
  ASSERT_TRUE(
      translate_agent_->GetPageTranslatedResult(nullptr, nullptr, &error));
  EXPECT_EQ(translate::TranslateErrors::TRANSLATION_TIMEOUT, error);
}

// Tests that the browser gets notified of the translation success when the
// translation succeeds.
TEST_F(TranslateAgentBrowserTest, TranslateSuccess) {
  // We make IsTranslateLibAvailable true so we don't attempt to inject the
  // library.
  EXPECT_CALL(*translate_agent_, IsTranslateLibAvailable())
      .Times(AtLeast(1))
      .WillRepeatedly(Return(true));

  EXPECT_CALL(*translate_agent_, IsTranslateLibReady())
      .WillOnce(Return(false))
      .WillOnce(Return(true));

  EXPECT_CALL(*translate_agent_, GetErrorCode())
      .WillOnce(Return(translate::TranslateErrors::NONE));

  EXPECT_CALL(*translate_agent_, StartTranslation()).WillOnce(Return(true));

  // Succeed after few checks.
  EXPECT_CALL(*translate_agent_, HasTranslationFailed())
      .WillRepeatedly(Return(false));
  EXPECT_CALL(*translate_agent_, HasTranslationFinished())
      .WillOnce(Return(false))
      .WillOnce(Return(false))
      .WillOnce(Return(true));

  // V8 call for performance monitoring should be ignored.
  EXPECT_CALL(*translate_agent_, ExecuteScriptAndGetDoubleResult(_)).Times(3);

  std::string original_lang("en");
  std::string target_lang("fr");
  translate_agent_->TranslatePage(original_lang, target_lang, std::string());
  base::RunLoop().RunUntilIdle();

  std::string received_original_lang;
  std::string received_target_lang;
  translate::TranslateErrors::Type error;
  ASSERT_TRUE(translate_agent_->GetPageTranslatedResult(
      &received_original_lang, &received_target_lang, &error));
  EXPECT_EQ(original_lang, received_original_lang);
  EXPECT_EQ(target_lang, received_target_lang);
  EXPECT_EQ(translate::TranslateErrors::NONE, error);
}

// Tests that the browser gets notified of the translation failure when the
// translation fails.
TEST_F(TranslateAgentBrowserTest, TranslateFailure) {
  // We make IsTranslateLibAvailable true so we don't attempt to inject the
  // library.
  EXPECT_CALL(*translate_agent_, IsTranslateLibAvailable())
      .Times(AtLeast(1))
      .WillRepeatedly(Return(true));

  EXPECT_CALL(*translate_agent_, IsTranslateLibReady()).WillOnce(Return(true));

  EXPECT_CALL(*translate_agent_, StartTranslation()).WillOnce(Return(true));

  // Fail after few checks.
  EXPECT_CALL(*translate_agent_, HasTranslationFailed())
      .WillOnce(Return(false))
      .WillOnce(Return(false))
      .WillOnce(Return(false))
      .WillOnce(Return(true));

  EXPECT_CALL(*translate_agent_, HasTranslationFinished())
      .Times(AtLeast(1))
      .WillRepeatedly(Return(false));

  EXPECT_CALL(*translate_agent_, GetErrorCode())
      .WillOnce(Return(translate::TranslateErrors::TRANSLATION_ERROR));

  // V8 call for performance monitoring should be ignored.
  EXPECT_CALL(*translate_agent_, ExecuteScriptAndGetDoubleResult(_)).Times(2);

  translate_agent_->TranslatePage("en", "fr", std::string());
  base::RunLoop().RunUntilIdle();

  translate::TranslateErrors::Type error;
  ASSERT_TRUE(
      translate_agent_->GetPageTranslatedResult(nullptr, nullptr, &error));
  EXPECT_EQ(translate::TranslateErrors::TRANSLATION_ERROR, error);
}

// Tests that when the browser translate a page for which the language is
// undefined we query the translate element to get the language.
TEST_F(TranslateAgentBrowserTest, UndefinedSourceLang) {
  // We make IsTranslateLibAvailable true so we don't attempt to inject the
  // library.
  EXPECT_CALL(*translate_agent_, IsTranslateLibAvailable())
      .Times(AtLeast(1))
      .WillRepeatedly(Return(true));

  EXPECT_CALL(*translate_agent_, IsTranslateLibReady()).WillOnce(Return(true));

  EXPECT_CALL(*translate_agent_, GetOriginalPageLanguage())
      .WillOnce(Return("de"));

  EXPECT_CALL(*translate_agent_, StartTranslation()).WillOnce(Return(true));
  EXPECT_CALL(*translate_agent_, HasTranslationFailed())
      .WillOnce(Return(false));
  EXPECT_CALL(*translate_agent_, HasTranslationFinished())
      .Times(AtLeast(1))
      .WillRepeatedly(Return(true));

  // V8 call for performance monitoring should be ignored.
  EXPECT_CALL(*translate_agent_, ExecuteScriptAndGetDoubleResult(_)).Times(3);

  translate_agent_->TranslatePage(translate::kUnknownLanguageCode, "fr",
                                  std::string());
  base::RunLoop().RunUntilIdle();

  translate::TranslateErrors::Type error;
  std::string original_lang;
  std::string target_lang;
  ASSERT_TRUE(translate_agent_->GetPageTranslatedResult(&original_lang,
                                                        &target_lang, &error));
  EXPECT_EQ("de", original_lang);
  EXPECT_EQ("fr", target_lang);
  EXPECT_EQ(translate::TranslateErrors::NONE, error);
}

// Tests that starting a translation while a similar one is pending does not
// break anything.
TEST_F(TranslateAgentBrowserTest, MultipleSimilarTranslations) {
  // We make IsTranslateLibAvailable true so we don't attempt to inject the
  // library.
  EXPECT_CALL(*translate_agent_, IsTranslateLibAvailable())
      .Times(AtLeast(1))
      .WillRepeatedly(Return(true));

  EXPECT_CALL(*translate_agent_, IsTranslateLibReady())
      .WillRepeatedly(Return(true));
  EXPECT_CALL(*translate_agent_, StartTranslation())
      .WillRepeatedly(Return(true));
  EXPECT_CALL(*translate_agent_, HasTranslationFailed())
      .WillRepeatedly(Return(false));
  EXPECT_CALL(*translate_agent_, HasTranslationFinished())
      .WillOnce(Return(true));

  // V8 call for performance monitoring should be ignored.
  EXPECT_CALL(*translate_agent_, ExecuteScriptAndGetDoubleResult(_)).Times(3);

  std::string original_lang("en");
  std::string target_lang("fr");
  translate_agent_->TranslatePage(original_lang, target_lang, std::string());
  // While this is running call again TranslatePage to make sure noting bad
  // happens.
  translate_agent_->TranslatePage(original_lang, target_lang, std::string());
  base::RunLoop().RunUntilIdle();

  std::string received_original_lang;
  std::string received_target_lang;
  translate::TranslateErrors::Type error;
  ASSERT_TRUE(translate_agent_->GetPageTranslatedResult(
      &received_original_lang, &received_target_lang, &error));
  EXPECT_EQ(original_lang, received_original_lang);
  EXPECT_EQ(target_lang, received_target_lang);
  EXPECT_EQ(translate::TranslateErrors::NONE, error);
}

// Tests that starting a translation while a different one is pending works.
TEST_F(TranslateAgentBrowserTest, MultipleDifferentTranslations) {
  EXPECT_CALL(*translate_agent_, IsTranslateLibAvailable())
      .Times(AtLeast(1))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(*translate_agent_, IsTranslateLibReady())
      .WillRepeatedly(Return(true));
  EXPECT_CALL(*translate_agent_, StartTranslation())
      .WillRepeatedly(Return(true));
  EXPECT_CALL(*translate_agent_, HasTranslationFailed())
      .WillRepeatedly(Return(false));
  EXPECT_CALL(*translate_agent_, HasTranslationFinished())
      .WillOnce(Return(true));

  // V8 call for performance monitoring should be ignored.
  EXPECT_CALL(*translate_agent_, ExecuteScriptAndGetDoubleResult(_)).Times(5);

  std::string original_lang("en");
  std::string target_lang("fr");
  translate_agent_->TranslatePage(original_lang, target_lang, std::string());
  // While this is running call again TranslatePage with a new target lang.
  std::string new_target_lang("de");
  translate_agent_->TranslatePage(original_lang, new_target_lang,
                                  std::string());
  base::RunLoop().RunUntilIdle();

  std::string received_original_lang;
  std::string received_target_lang;
  translate::TranslateErrors::Type error;
  ASSERT_TRUE(translate_agent_->GetPageTranslatedResult(
      &received_original_lang, &received_target_lang, &error));
  EXPECT_EQ(original_lang, received_original_lang);
  EXPECT_EQ(new_target_lang, received_target_lang);
  EXPECT_EQ(translate::TranslateErrors::NONE, error);
}

// Tests that we send the right translate language message for a page and that
// we respect the "no translate" meta-tag.
TEST_F(TranslateAgentBrowserTest, TranslatablePage) {
  LoadHTML("<html><body>A random page with random content.</body></html>");

  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(fake_translate_driver_.called_new_page_);
  EXPECT_TRUE(fake_translate_driver_.page_needs_translation_)
      << "Page should be translatable.";
  fake_translate_driver_.ResetNewPageValues();

  // Now the page specifies the META tag to prevent translation.
  LoadHTML(
      "<html><head><meta name=\"google\" value=\"notranslate\"></head>"
      "<body>A random page with random content.</body></html>");

  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(fake_translate_driver_.called_new_page_);
  EXPECT_FALSE(fake_translate_driver_.page_needs_translation_)
      << "Page should not be translatable.";
  fake_translate_driver_.ResetNewPageValues();

  // Try the alternate version of the META tag (content instead of value).
  LoadHTML(
      "<html><head><meta name=\"google\" content=\"notranslate\"></head>"
      "<body>A random page with random content.</body></html>");

  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(fake_translate_driver_.called_new_page_);
  EXPECT_FALSE(fake_translate_driver_.page_needs_translation_)
      << "Page should not be translatable.";
}

// Tests that the language meta tag takes precedence over the CLD when reporting
// the page's language.
TEST_F(TranslateAgentBrowserTest, LanguageMetaTag) {
  LoadHTML(
      "<html><head><meta http-equiv=\"content-language\" content=\"es\">"
      "</head><body>A random page with random content.</body></html>");

  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(fake_translate_driver_.called_new_page_);
  EXPECT_EQ("es", fake_translate_driver_.details_->adopted_language);
  fake_translate_driver_.ResetNewPageValues();

  // Makes sure we support multiple languages specified.
  LoadHTML(
      "<html><head><meta http-equiv=\"content-language\" "
      "content=\" fr , es,en \">"
      "</head><body>A random page with random content.</body></html>");

  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(fake_translate_driver_.called_new_page_);
  EXPECT_EQ("fr", fake_translate_driver_.details_->adopted_language);
}

// Tests that the language meta tag works even with non-all-lower-case.
// http://code.google.com/p/chromium/issues/detail?id=145689
TEST_F(TranslateAgentBrowserTest, LanguageMetaTagCase) {
  LoadHTML(
      "<html><head><meta http-equiv=\"Content-Language\" content=\"es\">"
      "</head><body>A random page with random content.</body></html>");
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(fake_translate_driver_.called_new_page_);
  EXPECT_EQ("es", fake_translate_driver_.details_->adopted_language);
  fake_translate_driver_.ResetNewPageValues();

  // Makes sure we support multiple languages specified.
  LoadHTML(
      "<html><head><meta http-equiv=\"Content-Language\" "
      "content=\" fr , es,en \">"
      "</head><body>A random page with random content.</body></html>");
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(fake_translate_driver_.called_new_page_);
  EXPECT_EQ("fr", fake_translate_driver_.details_->adopted_language);
}

// Tests that the language meta tag is converted to Chrome standard of dashes
// instead of underscores and proper capitalization.
// http://code.google.com/p/chromium/issues/detail?id=159487
TEST_F(TranslateAgentBrowserTest, LanguageCommonMistakesAreCorrected) {
  LoadHTML(
      "<html><head><meta http-equiv='Content-Language' content='EN_us'>"
      "</head><body>A random page with random content.</body></html>");
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(fake_translate_driver_.called_new_page_);
  EXPECT_EQ("en", fake_translate_driver_.details_->adopted_language);
  fake_translate_driver_.ResetNewPageValues();

  LoadHTML(
      "<html><head><meta http-equiv='Content-Language' content='ZH_tw'>"
      "</head><body>A random page with random content.</body></html>");
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(fake_translate_driver_.called_new_page_);
  EXPECT_EQ("zh-TW", fake_translate_driver_.details_->adopted_language);
}

// Tests that a back navigation gets a translate language message.
TEST_F(TranslateAgentBrowserTest, BackToTranslatablePage) {
  LoadHTML(
      "<html><head><meta http-equiv=\"content-language\" content=\"es\">"
      "</head><body>This page is in Spanish.</body></html>");
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(fake_translate_driver_.called_new_page_);
  EXPECT_EQ("es", fake_translate_driver_.details_->adopted_language);
  fake_translate_driver_.ResetNewPageValues();

  content::PageState back_state = GetCurrentPageState();

  LoadHTML(
      "<html><head><meta http-equiv=\"content-language\" content=\"fr\">"
      "</head><body>This page is in French.</body></html>");
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(fake_translate_driver_.called_new_page_);
  EXPECT_EQ("fr", fake_translate_driver_.details_->adopted_language);
  fake_translate_driver_.ResetNewPageValues();

  GoBack(GURL("data:text/html;charset=utf-8,<html><head>"
              "<meta http-equiv=\"content-language\" content=\"es\">"
              "</head><body>This page is in Spanish.</body></html>"),
         back_state);

  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(fake_translate_driver_.called_new_page_);
  EXPECT_EQ("es", fake_translate_driver_.details_->adopted_language);
}
