// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/extensions/api/passwords_private/passwords_private_api.h"

#include <memory>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/location.h"
#include "base/metrics/histogram_macros.h"
#include "base/strings/utf_string_conversions.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/values.h"
#include "chrome/browser/extensions/api/passwords_private/passwords_private_delegate_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sync/profile_sync_service_factory.h"
#include "chrome/common/extensions/api/passwords_private.h"
#include "components/password_manager/core/browser/manage_passwords_referrer.h"
#include "components/password_manager/core/browser/password_manager_util.h"
#include "components/sync/driver/sync_service.h"
#include "content/public/browser/web_contents.h"
#include "extensions/browser/extension_function_registry.h"

namespace extensions {

namespace {

using ResponseAction = ExtensionFunction::ResponseAction;

PasswordsPrivateDelegate* GetDelegate(
    content::BrowserContext* browser_context) {
  return PasswordsPrivateDelegateFactory::GetForBrowserContext(browser_context,
                                                               /*create=*/true);
}

}  // namespace

// PasswordsPrivateRecordPasswordsPageAccessInSettingsFunction
ResponseAction
PasswordsPrivateRecordPasswordsPageAccessInSettingsFunction::Run() {
  UMA_HISTOGRAM_ENUMERATION(
      "PasswordManager.ManagePasswordsReferrer",
      password_manager::ManagePasswordsReferrer::kChromeSettings);
  if (password_manager_util::IsSyncingWithNormalEncryption(
          ProfileSyncServiceFactory::GetForProfile(
              Profile::FromBrowserContext(browser_context())))) {
    // We record this second histogram to better understand the impact of the
    // Google Password Manager experiment for signed in and syncing users.
    UMA_HISTOGRAM_ENUMERATION(
        "PasswordManager.ManagePasswordsReferrerSignedInAndSyncing",
        password_manager::ManagePasswordsReferrer::kChromeSettings);
  }
  return RespondNow(NoArguments());
}

// PasswordsPrivateChangeSavedPasswordFunction
ResponseAction PasswordsPrivateChangeSavedPasswordFunction::Run() {
  auto parameters =
      api::passwords_private::ChangeSavedPassword::Params::Create(*args_);
  EXTENSION_FUNCTION_VALIDATE(parameters);

  GetDelegate(browser_context())
      ->ChangeSavedPassword(
          parameters->id, base::UTF8ToUTF16(parameters->new_username),
          parameters->new_password ? base::make_optional(base::UTF8ToUTF16(
                                         *parameters->new_password))
                                   : base::nullopt);

  return RespondNow(NoArguments());
}

// PasswordsPrivateRemoveSavedPasswordFunction
ResponseAction PasswordsPrivateRemoveSavedPasswordFunction::Run() {
  auto parameters =
      api::passwords_private::RemoveSavedPassword::Params::Create(*args_);
  EXTENSION_FUNCTION_VALIDATE(parameters);
  GetDelegate(browser_context())->RemoveSavedPassword(parameters->id);
  return RespondNow(NoArguments());
}

// PasswordsPrivateRemovePasswordExceptionFunction
ResponseAction PasswordsPrivateRemovePasswordExceptionFunction::Run() {
  auto parameters =
      api::passwords_private::RemovePasswordException::Params::Create(*args_);
  EXTENSION_FUNCTION_VALIDATE(parameters);
  GetDelegate(browser_context())->RemovePasswordException(parameters->id);
  return RespondNow(NoArguments());
}

// PasswordsPrivateUndoRemoveSavedPasswordOrExceptionFunction
ResponseAction
PasswordsPrivateUndoRemoveSavedPasswordOrExceptionFunction::Run() {
  GetDelegate(browser_context())->UndoRemoveSavedPasswordOrException();
  return RespondNow(NoArguments());
}

// PasswordsPrivateRequestPlaintextPasswordFunction
ResponseAction PasswordsPrivateRequestPlaintextPasswordFunction::Run() {
  auto parameters =
      api::passwords_private::RequestPlaintextPassword::Params::Create(*args_);
  EXTENSION_FUNCTION_VALIDATE(parameters);

  GetDelegate(browser_context())
      ->RequestShowPassword(
          parameters->id,
          base::BindOnce(
              &PasswordsPrivateRequestPlaintextPasswordFunction::GotPassword,
              this),
          GetSenderWebContents());

  // GotPassword() might respond before we reach this point.
  return did_respond() ? AlreadyResponded() : RespondLater();
}

void PasswordsPrivateRequestPlaintextPasswordFunction::GotPassword(
    base::Optional<base::string16> password) {
  if (password)
    Respond(OneArgument(std::make_unique<base::Value>(std::move(*password))));
  else
    Respond(NoArguments());
}

// PasswordsPrivateGetSavedPasswordListFunction
ResponseAction PasswordsPrivateGetSavedPasswordListFunction::Run() {
  // GetList() can immediately call GotList() (which would Respond() before
  // RespondLater()). So we post a task to preserve order.
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::BindOnce(&PasswordsPrivateGetSavedPasswordListFunction::GetList,
                     this));
  return RespondLater();
}

void PasswordsPrivateGetSavedPasswordListFunction::GetList() {
  GetDelegate(browser_context())
      ->GetSavedPasswordsList(base::BindOnce(
          &PasswordsPrivateGetSavedPasswordListFunction::GotList, this));
}

void PasswordsPrivateGetSavedPasswordListFunction::GotList(
    const PasswordsPrivateDelegate::UiEntries& list) {
  Respond(ArgumentList(
      api::passwords_private::GetSavedPasswordList::Results::Create(list)));
}

// PasswordsPrivateGetPasswordExceptionListFunction
ResponseAction PasswordsPrivateGetPasswordExceptionListFunction::Run() {
  // GetList() can immediately call GotList() (which would Respond() before
  // RespondLater()). So we post a task to preserve order.
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::BindOnce(&PasswordsPrivateGetPasswordExceptionListFunction::GetList,
                     this));
  return RespondLater();
}

void PasswordsPrivateGetPasswordExceptionListFunction::GetList() {
  GetDelegate(browser_context())
      ->GetPasswordExceptionsList(base::BindOnce(
          &PasswordsPrivateGetPasswordExceptionListFunction::GotList, this));
}

void PasswordsPrivateGetPasswordExceptionListFunction::GotList(
    const PasswordsPrivateDelegate::ExceptionEntries& entries) {
  Respond(ArgumentList(
      api::passwords_private::GetPasswordExceptionList::Results::Create(
          entries)));
}

// PasswordsPrivateImportPasswordsFunction
ResponseAction PasswordsPrivateImportPasswordsFunction::Run() {
  GetDelegate(browser_context())->ImportPasswords(GetSenderWebContents());
  return RespondNow(NoArguments());
}

// PasswordsPrivateExportPasswordsFunction
ResponseAction PasswordsPrivateExportPasswordsFunction::Run() {
  GetDelegate(browser_context())
      ->ExportPasswords(
          base::BindOnce(
              &PasswordsPrivateExportPasswordsFunction::ExportRequestCompleted,
              this),
          GetSenderWebContents());
  return RespondLater();
}

void PasswordsPrivateExportPasswordsFunction::ExportRequestCompleted(
    const std::string& error) {
  if (error.empty())
    Respond(NoArguments());
  else
    Error(error);
}

// PasswordsPrivateCancelExportPasswordsFunction
ResponseAction PasswordsPrivateCancelExportPasswordsFunction::Run() {
  GetDelegate(browser_context())->CancelExportPasswords();
  return RespondNow(NoArguments());
}

// PasswordsPrivateRequestExportProgressStatusFunction
ResponseAction PasswordsPrivateRequestExportProgressStatusFunction::Run() {
  return RespondNow(ArgumentList(
      api::passwords_private::RequestExportProgressStatus::Results::Create(
          GetDelegate(browser_context())->GetExportProgressStatus())));
}

}  // namespace extensions
