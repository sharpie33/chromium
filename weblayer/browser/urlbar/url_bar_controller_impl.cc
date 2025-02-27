// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "weblayer/browser/urlbar/url_bar_controller_impl.h"

#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "components/omnibox/browser/autocomplete_input.h"
#include "components/omnibox/browser/location_bar_model_impl.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_constants.h"
#include "weblayer/browser/browser_impl.h"
#include "weblayer/browser/tab_impl.h"
#include "weblayer/browser/urlbar/autocomplete_scheme_classifier_impl.h"
#include "weblayer/public/browser.h"
#include "weblayer/public/navigation_controller.h"

#if defined(OS_ANDROID)
#include "base/android/jni_string.h"
#include "weblayer/browser/java/jni/UrlBarControllerImpl_jni.h"
#endif

namespace weblayer {

std::unique_ptr<UrlBarController> UrlBarController::Create(Browser* browser) {
  return std::make_unique<UrlBarControllerImpl>(
      static_cast<BrowserImpl*>(browser));
}

#if defined(OS_ANDROID)
static jlong JNI_UrlBarControllerImpl_CreateUrlBarController(
    JNIEnv* env,
    jlong native_browser) {
  return reinterpret_cast<intptr_t>(
      new UrlBarControllerImpl(reinterpret_cast<BrowserImpl*>(native_browser)));
}

static void JNI_UrlBarControllerImpl_DeleteUrlBarController(
    JNIEnv* env,
    jlong native_urlbarcontroller) {
  delete reinterpret_cast<UrlBarControllerImpl*>(native_urlbarcontroller);
}
#endif

UrlBarControllerImpl::UrlBarControllerImpl(BrowserImpl* browser)
    : browser_(browser),
      location_bar_model_(std::make_unique<LocationBarModelImpl>(
          this,
          content::kMaxURLDisplayChars)) {
  DCHECK(browser_);
}

UrlBarControllerImpl::~UrlBarControllerImpl() = default;

#if defined(OS_ANDROID)
base::android::ScopedJavaLocalRef<jstring>
UrlBarControllerImpl::GetUrlForDisplay(JNIEnv* env) {
  return base::android::ScopedJavaLocalRef<jstring>(
      base::android::ConvertUTF16ToJavaString(env, GetUrlForDisplay()));
}
#endif

base::string16 UrlBarControllerImpl::GetUrlForDisplay() {
  return location_bar_model_->GetURLForDisplay();
}

bool UrlBarControllerImpl::GetURL(GURL* url) const {
  auto* active_tab = static_cast<TabImpl*>(browser_->GetActiveTab());
  if (!active_tab)
    return false;

  auto* active_web_contents = active_tab->web_contents();
  if (!active_web_contents)
    return false;

  DCHECK(url);
  *url = active_web_contents->GetVisibleURL();
  return true;
}

bool UrlBarControllerImpl::ShouldTrimDisplayUrlAfterHostName() const {
  return true;
}

base::string16 UrlBarControllerImpl::FormattedStringWithEquivalentMeaning(
    const GURL& url,
    const base::string16& formatted_url) const {
  return AutocompleteInput::FormattedStringWithEquivalentMeaning(
      url, formatted_url, AutocompleteSchemeClassifierImpl(), nullptr);
}

}  // namespace weblayer
