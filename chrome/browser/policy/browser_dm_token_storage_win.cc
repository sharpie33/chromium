// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/policy/browser_dm_token_storage_win.h"

// Must be first.
#include <windows.h>

#include <comutil.h>
#include <objbase.h>
#include <oleauto.h>
#include <unknwn.h>
#include <winerror.h>
#include <wrl/client.h>

#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/callback.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/string16.h"
#include "base/strings/string_piece.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/post_task.h"
#include "base/task_runner_util.h"
#include "base/threading/sequenced_task_runner_handle.h"
#include "base/win/registry.h"
#include "base/win/scoped_bstr.h"
#include "build/branding_buildflags.h"
#include "chrome/install_static/install_util.h"
#include "chrome/installer/util/install_util.h"
#include "chrome/installer/util/util_constants.h"
#include "content/public/browser/browser_thread.h"

#if BUILDFLAG(GOOGLE_CHROME_BRANDING)
#include "google_update/google_update_idl.h"
#endif  // BUILDFLAG(GOOGLE_CHROME_BRANDING)

namespace policy {
namespace {
#if BUILDFLAG(GOOGLE_CHROME_BRANDING)
// Explicitly allow DMTokenStorage impersonate the client since some COM code
// elsewhere in the browser process may have previously used
// CoInitializeSecurity to set the impersonation level to something other than
// the default. Ignore errors since an attempt to use Google Update may succeed
// regardless.
void ConfigureProxyBlanket(IUnknown* interface_pointer) {
  ::CoSetProxyBlanket(
      interface_pointer, RPC_C_AUTHN_DEFAULT, RPC_C_AUTHZ_DEFAULT,
      COLE_DEFAULT_PRINCIPAL, RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
      RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_DYNAMIC_CLOAKING);
}
#endif  // BUILDFLAG(GOOGLE_CHROME_BRANDING)

bool StoreDMTokenInRegistry(const std::string& token) {
#if BUILDFLAG(GOOGLE_CHROME_BRANDING)
  if (token.empty())
    return false;

  Microsoft::WRL::ComPtr<IGoogleUpdate3Web> google_update;
  HRESULT hr = ::CoCreateInstance(CLSID_GoogleUpdate3WebServiceClass, nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&google_update));
  if (FAILED(hr))
    return false;

  ConfigureProxyBlanket(google_update.Get());
  Microsoft::WRL::ComPtr<IDispatch> dispatch;
  hr = google_update->createAppBundleWeb(dispatch.GetAddressOf());
  if (FAILED(hr))
    return false;

  Microsoft::WRL::ComPtr<IAppBundleWeb> app_bundle;
  hr = dispatch.CopyTo(app_bundle.GetAddressOf());
  if (FAILED(hr))
    return false;

  dispatch.Reset();
  ConfigureProxyBlanket(app_bundle.Get());
  app_bundle->initialize();
  const wchar_t* app_guid = install_static::GetAppGuid();
  hr = app_bundle->createInstalledApp(base::win::ScopedBstr(app_guid));
  if (FAILED(hr))
    return false;

  hr = app_bundle->get_appWeb(0, dispatch.GetAddressOf());
  if (FAILED(hr))
    return false;

  Microsoft::WRL::ComPtr<IAppWeb> app;
  hr = dispatch.CopyTo(app.GetAddressOf());
  if (FAILED(hr))
    return false;

  dispatch.Reset();
  ConfigureProxyBlanket(app.Get());
  hr = app->get_command(base::win::ScopedBstr(installer::kCmdStoreDMToken),
                        dispatch.GetAddressOf());
  if (FAILED(hr) || !dispatch)
    return false;

  Microsoft::WRL::ComPtr<IAppCommandWeb> app_command;
  hr = dispatch.CopyTo(app_command.GetAddressOf());
  if (FAILED(hr))
    return false;

  ConfigureProxyBlanket(app_command.Get());
  std::string token_base64;
  base::Base64Encode(token, &token_base64);
  VARIANT var;
  VariantInit(&var);
  _variant_t token_var = token_base64.c_str();
  hr = app_command->execute(token_var, var, var, var, var, var, var, var, var);
  if (FAILED(hr))
    return false;

  // TODO(crbug.com/823515): Get the status of the app command execution and
  // return a corresponding value for |success|. For now, assume that the call
  // to setup.exe succeeds.
  return true;
#else
  return false;
#endif  // BUILDFLAG(GOOGLE_CHROME_BRANDING)
}
}  // namespace

std::string BrowserDMTokenStorageWin::InitClientId() {
  // For the client id, use the Windows machine GUID.
  // TODO(crbug.com/821977): Need a backup plan if machine GUID doesn't exist.
  base::win::RegKey key;
  LSTATUS status =
      key.Open(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography",
               KEY_QUERY_VALUE | KEY_WOW64_64KEY);
  if (status != ERROR_SUCCESS)
    return std::string();

  base::string16 value;
  status = key.ReadValue(L"MachineGuid", &value);
  if (status != ERROR_SUCCESS)
    return std::string();

  std::string client_id;
  if (!base::WideToUTF8(value.c_str(), value.length(), &client_id))
    return std::string();
  return client_id;
}

std::string BrowserDMTokenStorageWin::InitEnrollmentToken() {
  return base::WideToUTF8(InstallUtil::GetCloudManagementEnrollmentToken());
}

std::string BrowserDMTokenStorageWin::InitDMToken() {
  // At the time of writing (January 2018), the DM token is about 200 bytes
  // long. The initial size of the buffer should be enough to cover most
  // realistic future size-increase scenarios, although we still make an effort
  // to support somewhat larger token sizes just to be safe.
  constexpr size_t kInitialDMTokenSize = 512;

  base::win::RegKey key;
  base::string16 dm_token_value_name;
  std::vector<char> raw_value(kInitialDMTokenSize);

  // Prefer the app-neutral location over the browser's to match Google Update's
  // behavior.
  for (const auto& location : {InstallUtil::BrowserLocation(false),
                               InstallUtil::BrowserLocation(true)}) {
    std::tie(key, dm_token_value_name) =
        InstallUtil::GetCloudManagementDmTokenLocation(
            InstallUtil::ReadOnly(true), location);
    if (!key.Valid())
      continue;

    DWORD dtype = REG_NONE;
    DWORD size = DWORD{raw_value.size()};
    auto result = key.ReadValue(dm_token_value_name.c_str(), raw_value.data(),
                                &size, &dtype);
    if (result == ERROR_MORE_DATA && size <= installer::kMaxDMTokenLength) {
      raw_value.resize(size);
      result = key.ReadValue(dm_token_value_name.c_str(), raw_value.data(),
                             &size, &dtype);
    }
    if (result != ERROR_SUCCESS || dtype != REG_BINARY || size == 0)
      continue;

    DCHECK_LE(size, installer::kMaxDMTokenLength);
    return base::TrimWhitespaceASCII(base::StringPiece(raw_value.data(), size),
                                     base::TRIM_ALL)
        .as_string();
  }

  DVLOG(1) << "Failed to get DMToken from Registry.";
  return std::string();
}

bool BrowserDMTokenStorageWin::InitEnrollmentErrorOption() {
  return InstallUtil::ShouldCloudManagementBlockOnFailure();
}

BrowserDMTokenStorage::StoreTask BrowserDMTokenStorageWin::SaveDMTokenTask(
    const std::string& token,
    const std::string& client_id) {
  return base::BindOnce(&StoreDMTokenInRegistry, token);
}

scoped_refptr<base::TaskRunner>
BrowserDMTokenStorageWin::SaveDMTokenTaskRunner() {
  return com_sta_task_runner_;
}

// static
BrowserDMTokenStorage* BrowserDMTokenStorage::Get() {
  if (storage_for_testing_)
    return storage_for_testing_;

  static base::NoDestructor<BrowserDMTokenStorageWin> storage;
  return storage.get();
}

BrowserDMTokenStorageWin::BrowserDMTokenStorageWin()
    : com_sta_task_runner_(base::CreateCOMSTATaskRunner(
          {base::ThreadPool(), base::MayBlock()})) {}

BrowserDMTokenStorageWin::~BrowserDMTokenStorageWin() {}

}  // namespace policy
