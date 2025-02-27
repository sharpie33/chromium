// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/credential_provider/gaiacp/win_http_url_fetcher.h"

#include <Windows.h>
#include <winhttp.h>

#include <atlconv.h>
#include <process.h>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/stl_util.h"
#include "base/strings/strcat.h"
#include "base/strings/string16.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/credential_provider/gaiacp/logging.h"

// Self deleting http service requester. This class will try to make a query
// using the given url fetcher. It will delete itself when the request is
// completed, either because the request completed successfully within the
// timeout or the request has timed out and is allowed to complete in the
// background without having the result read by anyone.
// There are two situations where the request will be deleted:
// 1. If the background thread making the request returns within the given
// timeout, the function is guaranteed to return the result that was fetched.
// 2. If however the background thread times out there are two potential
// race conditions that can occur:
//    1. The main thread making the request can mark that the background thread
//       is orphaned before it can complete. In this case when the background
//       thread completes it will check whether the request is orphaned and self
//       delete.
//    2. The background thread completes before the main thread can mark the
//       request as orphaned. In this case the background thread will have
//       marked that the request is no longer processing and thus the main
//       thread can self delete.
class HttpServiceRequest {
 public:
  explicit HttpServiceRequest(
      std::unique_ptr<credential_provider::WinHttpUrlFetcher> fetcher)
      : fetcher_(std::move(fetcher)) {
    DCHECK(fetcher_);
  }

  // Tries to fetch the request stored in |fetcher_| in a background thread
  // within the given |request_timeout|. If the background thread returns before
  // the timeout expires, it is guaranteed that a result can be returned and the
  // requester will delete itself.
  base::Optional<base::Value> WaitForResponseFromHttpService(
      const base::TimeDelta& request_timeout) {
    base::Optional<base::Value> result;

    // Start the thread and wait on its handle until |request_timeout| expires
    // or the thread finishes.
    unsigned wait_thread_id;
    uintptr_t wait_thread = ::_beginthreadex(
        nullptr, 0, &HttpServiceRequest::FetchResultFromHttpService,
        reinterpret_cast<void*>(this), 0, &wait_thread_id);

    HRESULT hr = S_OK;
    if (wait_thread == 0) {
      return result;
    } else {
      // Hold the handle in the scoped handle so that it can be immediately
      // closed when the wait is complete allowing the thread to finish
      // completely if needed.
      base::win::ScopedHandle thread_handle(
          reinterpret_cast<HANDLE>(wait_thread));
      hr = ::WaitForSingleObject(thread_handle.Get(),
                                 request_timeout.InMilliseconds());
    }

    // The race condition starts here. It is possible that between the expiry of
    // the timeout in the call for WaitForSingleObject and the call to
    // OrphanRequest, the fetching thread could have finished. So there is a two
    // part handshake. Either the background thread has called ProcessingDone
    // in which case it has already passed its own check for |is_orphaned_| and
    // the call to OrphanRequest should delete this object right now. Otherwise
    // the background thread is still running and will be able to query the
    // |is_orphaned_| state and delete the object after thread completion.
    if (hr != WAIT_OBJECT_0) {
      LOGFN(ERROR) << "Wait for response timed out or failed hr="
                   << credential_provider::putHR(hr);
      OrphanRequest();
      return result;
    }

    result = base::JSONReader::Read(
        base::StringPiece(response_.data(), response_.size()),
        base::JSON_ALLOW_TRAILING_COMMAS);
    if (!result || !result->is_dict()) {
      LOGFN(ERROR) << "Failed to read json result from server response";
      result.reset();
    }

    delete this;
    return result;
  }

 private:
  void OrphanRequest() {
    bool delete_self = false;
    {
      base::AutoLock locker(orphan_lock_);
      CHECK(!is_orphaned_);
      if (!is_processing_) {
        delete_self = true;
      } else {
        is_orphaned_ = true;
      }
    }

    if (delete_self)
      delete this;
  }

  void ProcessingDone() {
    bool delete_self = false;
    {
      base::AutoLock locker(orphan_lock_);
      CHECK(is_processing_);
      if (is_orphaned_) {
        delete_self = true;
      } else {
        is_processing_ = false;
      }
    }

    if (delete_self)
      delete this;
  }

  // Background thread function that is used to query the request to the
  // http service. This thread never times out and simply marks the fetcher
  // as finished processing when it is done.
  static unsigned __stdcall FetchResultFromHttpService(void* param) {
    DCHECK(param);
    HttpServiceRequest* requester =
        reinterpret_cast<HttpServiceRequest*>(param);

    HRESULT hr = requester->fetcher_->Fetch(&requester->response_);
    if (FAILED(hr))
      LOGFN(ERROR) << "fetcher.Fetch hr=" << credential_provider::putHR(hr);

    requester->ProcessingDone();
    return 0;
  }

  base::Lock orphan_lock_;
  std::unique_ptr<credential_provider::WinHttpUrlFetcher> fetcher_;
  std::vector<char> response_;
  bool is_orphaned_ = false;
  bool is_processing_ = true;
};

namespace credential_provider {

// static
WinHttpUrlFetcher::CreatorCallback*
WinHttpUrlFetcher::GetCreatorFunctionStorage() {
  static CreatorCallback creator_for_testing;
  return &creator_for_testing;
}

// static
std::unique_ptr<WinHttpUrlFetcher> WinHttpUrlFetcher::Create(const GURL& url) {
  return !GetCreatorFunctionStorage()->is_null()
             ? GetCreatorFunctionStorage()->Run(url)
             : std::unique_ptr<WinHttpUrlFetcher>(new WinHttpUrlFetcher(url));
}

WinHttpUrlFetcher::WinHttpUrlFetcher(const GURL& url)
    : url_(url), session_(nullptr), request_(nullptr) {
  LOGFN(INFO) << "url=" << url.spec() << " (scheme and port ignored)";

  ScopedWinHttpHandle::Handle session = ::WinHttpOpen(
      L"GaiaCP/1.0 (Windows NT)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    HRESULT hr = HRESULT_FROM_WIN32(::GetLastError());
    LOGFN(ERROR) << "WinHttpOpen hr=" << putHR(hr);
  }
  session_.Set(session);
}

WinHttpUrlFetcher::WinHttpUrlFetcher() {}

WinHttpUrlFetcher::~WinHttpUrlFetcher() {
  // Closing the session handle closes all derived handles too.
}

bool WinHttpUrlFetcher::IsValid() const {
  return session_.IsValid();
}

HRESULT WinHttpUrlFetcher::SetRequestHeader(const char* name,
                                            const char* value) {
  DCHECK(name);
  DCHECK(value);

  // TODO(rogerta): does not support multivalued headers.
  request_headers_[name] = value;
  return S_OK;
}

HRESULT WinHttpUrlFetcher::SetRequestBody(const char* body) {
  DCHECK(body);
  body_ = body;
  return S_OK;
}

HRESULT WinHttpUrlFetcher::SetHttpRequestTimeout(const int timeout_in_millis) {
  DCHECK(timeout_in_millis);
  timeout_in_millis_ = timeout_in_millis;
  return S_OK;
}

HRESULT WinHttpUrlFetcher::Fetch(std::vector<char>* response) {
  USES_CONVERSION;
  DCHECK(response);

  response->clear();

  if (!session_.IsValid()) {
    LOGFN(ERROR) << "Invalid fetcher";
    return E_UNEXPECTED;
  }

  // Open a connection to the server.
  ScopedWinHttpHandle connect;
  {
    ScopedWinHttpHandle::Handle connect_tmp = ::WinHttpConnect(
        session_.Get(), A2CW(url_.host().c_str()), INTERNET_DEFAULT_PORT, 0);
    if (!connect_tmp) {
      HRESULT hr = HRESULT_FROM_WIN32(::GetLastError());
      LOGFN(ERROR) << "WinHttpConnect hr=" << putHR(hr);
      return hr;
    }
    connect.Set(connect_tmp);
  }

  {
    // Set timeout if specified.
    if (timeout_in_millis_ != 0) {
      if (!::WinHttpSetTimeouts(session_.Get(), timeout_in_millis_,
                                timeout_in_millis_, timeout_in_millis_,
                                timeout_in_millis_)) {
        HRESULT hr = HRESULT_FROM_WIN32(::GetLastError());
        LOGFN(ERROR) << "WinHttpSetTimeouts hr=" << putHR(hr);
        return hr;
      }
    }
  }

  {
    bool use_post = !body_.empty();
    ScopedWinHttpHandle::Handle request = ::WinHttpOpenRequest(
        connect.Get(), use_post ? L"POST" : L"GET",
        use_post ? A2CW(url_.path().c_str())
                 : A2CW(url_.PathForRequest().c_str()),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_REFRESH | WINHTTP_FLAG_SECURE);
    if (!request) {
      HRESULT hr = HRESULT_FROM_WIN32(::GetLastError());
      LOGFN(ERROR) << "WinHttpOpenRequest hr=" << putHR(hr);
      return hr;
    }
    request_.Set(request);
  }

  // Add request headers.

  for (const auto& kv : request_headers_) {
    const wchar_t* key = A2CW(kv.first.c_str());
    const wchar_t* value = A2CW(kv.second.c_str());
    base::string16 header = base::StringPrintf(L"%ls: %ls", key, value);
    if (!::WinHttpAddRequestHeaders(
            request_.Get(), header.c_str(), header.length(),
            WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
      HRESULT hr = HRESULT_FROM_WIN32(::GetLastError());
      LOGFN(ERROR) << "WinHttpAddRequestHeaders name=" << kv.first
                   << " hr=" << putHR(hr);
      return hr;
    }
  }

  // Write request body if needed.

  if (!::WinHttpSendRequest(request_.Get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            const_cast<char*>(body_.c_str()), body_.length(),
                            body_.length(),
                            reinterpret_cast<DWORD_PTR>(nullptr))) {
    HRESULT hr = HRESULT_FROM_WIN32(::GetLastError());
    LOGFN(ERROR) << "WinHttpSendRequest hr=" << putHR(hr);
    return hr;
  }

  // Wait for the response.

  if (!::WinHttpReceiveResponse(request_.Get(), nullptr)) {
    HRESULT hr = HRESULT_FROM_WIN32(::GetLastError());
    LOGFN(ERROR) << "WinHttpReceiveResponse hr=" << putHR(hr);
    return hr;
  }

  DWORD length = 0;
  if (!::WinHttpQueryDataAvailable(request_.Get(), &length)) {
    HRESULT hr = HRESULT_FROM_WIN32(::GetLastError());
    LOGFN(ERROR) << "WinHttpQueryDataAvailable hr=" << putHR(hr);
    return hr;
  }

  // 256k max response size to make sure bad data does not crash GCPW.
  // This fetcher is only used to retrieve small information such as token
  // handle status and profile picture images so it should not need a larger
  // buffer than 256k.
  constexpr size_t kMaxResponseSize = 256 * 1024 * 1024;
  // Read the response.
  std::unique_ptr<char> buffer(new char[length]);
  DWORD actual = 0;
  do {
    if (!::WinHttpReadData(request_.Get(), buffer.get(), length, &actual)) {
      HRESULT hr = HRESULT_FROM_WIN32(::GetLastError());
      LOGFN(ERROR) << "WinHttpReadData hr=" << putHR(hr);
      return hr;
    }

    size_t current_size = response->size();
    response->resize(response->size() + actual);
    memcpy(response->data() + current_size, buffer.get(), actual);
    if (response->size() >= kMaxResponseSize) {
      LOGFN(ERROR) << "Response has exceeded max size=" << kMaxResponseSize;
      return E_OUTOFMEMORY;
    }
  } while (actual);

  return S_OK;
}

HRESULT WinHttpUrlFetcher::Close() {
  request_.Close();
  return S_OK;
}

HRESULT WinHttpUrlFetcher::BuildRequestAndFetchResultFromHttpService(
    const GURL& request_url,
    std::string access_token,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const base::Value& request_dict,
    const base::TimeDelta& request_timeout,
    base::Optional<base::Value>* request_result) {
  auto url_fetcher = WinHttpUrlFetcher::Create(request_url);
  if (!url_fetcher) {
    LOGFN(ERROR) << "Could not create valid fetcher for url="
                 << request_url.spec();
    return E_FAIL;
  }

  url_fetcher->SetRequestHeader("Content-Type", "application/json");
  url_fetcher->SetRequestHeader("Authorization",
                                ("Bearer " + access_token).c_str());
  for (auto& header : headers)
    url_fetcher->SetRequestHeader(header.first.c_str(), header.second.c_str());

  HRESULT hr = S_OK;

  if (request_dict.is_dict()) {
    std::string body;
    if (!base::JSONWriter::Write(request_dict, &body)) {
      LOGFN(ERROR) << "base::JSONWriter::Write failed";
      return E_FAIL;
    }

    hr = url_fetcher->SetRequestBody(body.c_str());
    if (FAILED(hr)) {
      LOGFN(ERROR) << "fetcher.SetRequestBody hr=" << putHR(hr);
      return E_FAIL;
    }
  }

  auto extracted_param = (new HttpServiceRequest(std::move(url_fetcher)))
                             ->WaitForResponseFromHttpService(request_timeout);

  if (!extracted_param)
    return E_FAIL;

  *request_result = std::move(extracted_param);

  return hr;
}
}  // namespace credential_provider
