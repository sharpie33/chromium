// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/cookie_store/cookie_store.h"

#include <utility>

#include "base/optional.h"
#include "services/network/public/mojom/restricted_cookie_manager.mojom-blink.h"
#include "third_party/blink/renderer/bindings/core/v8/script_promise_resolver.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_cookie_list_item.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_cookie_store_delete_options.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_cookie_store_get_options.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_cookie_store_set_extra_options.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_cookie_store_set_options.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/dom_exception.h"
#include "third_party/blink/renderer/modules/cookie_store/cookie_change_event.h"
#include "third_party/blink/renderer/modules/event_modules.h"
#include "third_party/blink/renderer/modules/event_target_modules.h"
#include "third_party/blink/renderer/modules/service_worker/service_worker_global_scope.h"
#include "third_party/blink/renderer/modules/service_worker/service_worker_registration.h"
#include "third_party/blink/renderer/platform/bindings/script_state.h"
#include "third_party/blink/renderer/platform/cookie/canonical_cookie.h"
#include "third_party/blink/renderer/platform/heap/handle.h"
#include "third_party/blink/renderer/platform/heap/heap.h"
#include "third_party/blink/renderer/platform/instrumentation/use_counter.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace blink {

namespace {

// Returns null if and only if an exception is thrown.
network::mojom::blink::CookieManagerGetOptionsPtr ToBackendOptions(
    const CookieStoreGetOptions* options,
    ExceptionState& exception_state) {
  auto backend_options = network::mojom::blink::CookieManagerGetOptions::New();

  if (options->matchType() == "starts-with") {
    backend_options->match_type =
        network::mojom::blink::CookieMatchType::STARTS_WITH;
  } else {
    DCHECK_EQ(options->matchType(), WTF::String("equals"));
    backend_options->match_type =
        network::mojom::blink::CookieMatchType::EQUALS;
  }

  if (options->hasName()) {
    backend_options->name = options->name();
  } else {
    // No name provided. Use a filter that matches all cookies. This overrides
    // a user-provided matchType.
    backend_options->match_type =
        network::mojom::blink::CookieMatchType::STARTS_WITH;
    backend_options->name = g_empty_string;
  }

  return backend_options;
}

// Returns no value if and only if an exception is thrown.
base::Optional<CanonicalCookie> ToCanonicalCookie(
    const KURL& cookie_url,
    const CookieStoreSetExtraOptions* options,
    ExceptionState& exception_state) {
  const String& name = options->name();
  const String& value = options->value();
  if (name.IsEmpty() && value.Contains('=')) {
    exception_state.ThrowTypeError(
        "Cookie value cannot contain '=' if the name is empty");
    return base::nullopt;
  }

  base::Time expires = options->hasExpires()
                           ? base::Time::FromJavaTime(options->expires())
                           : base::Time();

  String cookie_url_host = cookie_url.Host();
  String domain;
  if (options->hasDomain()) {
    // The leading dot (".") from the domain attribute is stripped in the
    // Set-Cookie header, for compatibility. This API doesn't have compatibility
    // constraints, so reject the edge case outright.
    if (options->domain().StartsWith(".")) {
      exception_state.ThrowTypeError("Cookie domain cannot start with \".\"");
      return base::nullopt;
    }

    domain = String(".") + options->domain();
    if (!cookie_url_host.EndsWith(domain) &&
        cookie_url_host != options->domain()) {
      exception_state.ThrowTypeError(
          "Cookie domain must domain-match current host");
      return base::nullopt;
    }
  } else {
    // The absence of "domain" implies a host-only cookie.
    domain = cookie_url_host;
  }

  // Although the Cookie Store API spec always defaults the "secure" cookie
  // attribute to true, we only default to true on cryptographically secure
  // origins, where only secure cookies may be written, and to false otherwise,
  // where only insecure cookies may be written. As a result,
  // cookieStore.set("name", "value") sets a cookie and
  // cookieStore.delete("name") deletes a cookie on both http://localhost and
  // secure origins, without having to specify "secure: false" on
  // http://localhost.
  const bool secure = options->hasSecure()
                          ? options->secure()
                          : SecurityOrigin::IsSecure(cookie_url);
  // If attempting to set/delete a secure cookie on an insecure origin, throw an
  // exception, rather than failing silently as document.cookie does.
  network::mojom::CookieSourceScheme source_scheme_enum =
      SecurityOrigin::IsSecure(cookie_url)
          ? network::mojom::CookieSourceScheme::kSecure
          : network::mojom::CookieSourceScheme::kNonSecure;
  if (secure &&
      source_scheme_enum != network::mojom::CookieSourceScheme::kSecure) {
    exception_state.ThrowTypeError(
        "Cannot modify a secure cookie on insecure origin");
    return base::nullopt;
  }
  if (!secure && (name.StartsWith("__Secure-") || name.StartsWith("__Host-"))) {
    exception_state.ThrowTypeError(
        "__Secure- and __Host- cookies must be secure");
    return base::nullopt;
  }

  network::mojom::CookieSameSite same_site;
  if (options->sameSite() == "strict") {
    same_site = network::mojom::CookieSameSite::STRICT_MODE;
  } else if (options->sameSite() == "lax") {
    same_site = network::mojom::CookieSameSite::LAX_MODE;
  } else {
    DCHECK_EQ(options->sameSite(), "none");
    same_site = network::mojom::CookieSameSite::NO_RESTRICTION;
  }

  return CanonicalCookie::Create(
      name, value, domain, options->path(), base::Time() /*creation*/, expires,
      base::Time() /*last_access*/, secure, false /*http_only*/, same_site,
      CanonicalCookie::kDefaultPriority, source_scheme_enum);
}

const KURL DefaultCookieURL(ExecutionContext* execution_context) {
  DCHECK(execution_context);

  if (auto* document = DynamicTo<Document>(execution_context))
    return document->CookieURL();

  return KURL(To<ServiceWorkerGlobalScope>(execution_context)
                  ->serviceWorker()
                  ->scriptURL());
}

// Return empty KURL if and only if an exception is thrown.
KURL CookieUrlForRead(const CookieStoreGetOptions* options,
                      const KURL& default_cookie_url,
                      ScriptState* script_state,
                      ExceptionState& exception_state) {
  ExecutionContext* context = ExecutionContext::From(script_state);

  if (!options->hasUrl())
    return default_cookie_url;

  KURL cookie_url = KURL(default_cookie_url, options->url());

  if (context->IsDocument()) {
    DCHECK_EQ(default_cookie_url, To<Document>(context)->CookieURL());

    if (cookie_url.GetString() != default_cookie_url.GetString()) {
      exception_state.ThrowTypeError("URL must match the document URL");
      return KURL();
    }
  } else {
    DCHECK(context->IsServiceWorkerGlobalScope());
    DCHECK_EQ(
        default_cookie_url.GetString(),
        To<ServiceWorkerGlobalScope>(context)->serviceWorker()->scriptURL());

    if (!cookie_url.GetString().StartsWith(default_cookie_url.GetString())) {
      exception_state.ThrowTypeError("URL must be within Service Worker scope");
      return KURL();
    }
  }

  return cookie_url;
}

net::SiteForCookies DefaultSiteForCookies(ExecutionContext* execution_context) {
  DCHECK(execution_context);

  if (auto* document = DynamicTo<Document>(execution_context))
    return document->SiteForCookies();

  auto* scope = To<ServiceWorkerGlobalScope>(execution_context);
  return net::SiteForCookies::FromUrl(scope->Url());
}

scoped_refptr<SecurityOrigin> DefaultTopFrameOrigin(
    ExecutionContext* execution_context) {
  DCHECK(execution_context);

  if (auto* document = DynamicTo<Document>(execution_context)) {
    // Can we avoid the copy? TopFrameOrigin is returned as const& but we need
    // a scoped_refptr.
    return document->TopFrameOrigin()->IsolatedCopy();
  }

  auto* scope = To<ServiceWorkerGlobalScope>(execution_context);
  return scope->GetSecurityOrigin()->IsolatedCopy();
}

}  // namespace

CookieStore::CookieStore(
    ExecutionContext* execution_context,
    mojo::Remote<network::mojom::blink::RestrictedCookieManager> backend)
    : ContextLifecycleObserver(execution_context),
      backend_(std::move(backend)),
      default_cookie_url_(DefaultCookieURL(execution_context)),
      default_site_for_cookies_(DefaultSiteForCookies(execution_context)),
      default_top_frame_origin_(DefaultTopFrameOrigin(execution_context)) {
  DCHECK(backend_);
}

CookieStore::~CookieStore() = default;

ScriptPromise CookieStore::getAll(ScriptState* script_state,
                                  const String& name,
                                  ExceptionState& exception_state) {
  CookieStoreGetOptions* options = CookieStoreGetOptions::Create();
  options->setName(name);
  return getAll(script_state, options, exception_state);
}

ScriptPromise CookieStore::getAll(ScriptState* script_state,
                                  const CookieStoreGetOptions* options,
                                  ExceptionState& exception_state) {
  UseCounter::Count(CurrentExecutionContext(script_state->GetIsolate()),
                    WebFeature::kCookieStoreAPI);

  return DoRead(script_state, options, &CookieStore::GetAllForUrlToGetAllResult,
                exception_state);
}

ScriptPromise CookieStore::get(ScriptState* script_state,
                               const String& name,
                               ExceptionState& exception_state) {
  CookieStoreGetOptions* options = CookieStoreGetOptions::Create();
  options->setName(name);
  return get(script_state, options, exception_state);
}

ScriptPromise CookieStore::get(ScriptState* script_state,
                               const CookieStoreGetOptions* options,
                               ExceptionState& exception_state) {
  UseCounter::Count(CurrentExecutionContext(script_state->GetIsolate()),
                    WebFeature::kCookieStoreAPI);

  return DoRead(script_state, options, &CookieStore::GetAllForUrlToGetResult,
                exception_state);
}

ScriptPromise CookieStore::set(ScriptState* script_state,
                               const String& name,
                               const String& value,
                               const CookieStoreSetOptions* options,
                               ExceptionState& exception_state) {
  CookieStoreSetExtraOptions* set_options =
      CookieStoreSetExtraOptions::Create();
  set_options->setName(name);
  set_options->setValue(value);
  if (options->hasExpires())
    set_options->setExpires(options->expires());
  set_options->setDomain(options->domain());
  set_options->setPath(options->path());
  if (options->hasSecure())
    set_options->setSecure(options->secure());
  set_options->setSameSite(options->sameSite());
  return set(script_state, set_options, exception_state);
}

ScriptPromise CookieStore::set(ScriptState* script_state,
                               const CookieStoreSetExtraOptions* options,
                               ExceptionState& exception_state) {
  UseCounter::Count(CurrentExecutionContext(script_state->GetIsolate()),
                    WebFeature::kCookieStoreAPI);

  return DoWrite(script_state, options, exception_state);
}

ScriptPromise CookieStore::Delete(ScriptState* script_state,
                                  const String& name,
                                  ExceptionState& exception_state) {
  UseCounter::Count(CurrentExecutionContext(script_state->GetIsolate()),
                    WebFeature::kCookieStoreAPI);

  CookieStoreSetExtraOptions* set_options =
      CookieStoreSetExtraOptions::Create();
  set_options->setName(name);
  set_options->setValue(g_empty_string);
  set_options->setExpires(0);
  return DoWrite(script_state, set_options, exception_state);
}

ScriptPromise CookieStore::Delete(ScriptState* script_state,
                                  const CookieStoreDeleteOptions* options,
                                  ExceptionState& exception_state) {
  CookieStoreSetExtraOptions* set_options =
      CookieStoreSetExtraOptions::Create();
  set_options->setName(options->name());
  set_options->setValue(g_empty_string);
  set_options->setExpires(0);
  set_options->setDomain(options->domain());
  set_options->setPath(options->path());
  set_options->setSameSite("strict");
  return DoWrite(script_state, set_options, exception_state);
}

void CookieStore::Trace(blink::Visitor* visitor) {
  EventTargetWithInlineData::Trace(visitor);
  ContextLifecycleObserver::Trace(visitor);
}

void CookieStore::ContextDestroyed(ExecutionContext* execution_context) {
  StopObserving();
  backend_.reset();
}

const AtomicString& CookieStore::InterfaceName() const {
  return event_target_names::kCookieStore;
}

ExecutionContext* CookieStore::GetExecutionContext() const {
  return ContextLifecycleObserver::GetExecutionContext();
}

void CookieStore::RemoveAllEventListeners() {
  EventTargetWithInlineData::RemoveAllEventListeners();
  DCHECK(!HasEventListeners());
  StopObserving();
}

void CookieStore::OnCookieChange(
    network::mojom::blink::CookieChangeInfoPtr change) {
  HeapVector<Member<CookieListItem>> changed, deleted;
  CookieChangeEvent::ToEventInfo(change->cookie, change->cause, changed,
                                 deleted);
  if (changed.IsEmpty() && deleted.IsEmpty()) {
    // The backend only reported OVERWRITE events, which are dropped.
    return;
  }
  DispatchEvent(*CookieChangeEvent::Create(
      event_type_names::kChange, std::move(changed), std::move(deleted)));
}

void CookieStore::AddedEventListener(
    const AtomicString& event_type,
    RegisteredEventListener& registered_listener) {
  EventTargetWithInlineData::AddedEventListener(event_type,
                                                registered_listener);
  StartObserving();
}

void CookieStore::RemovedEventListener(
    const AtomicString& event_type,
    const RegisteredEventListener& registered_listener) {
  EventTargetWithInlineData::RemovedEventListener(event_type,
                                                  registered_listener);
  if (!HasEventListeners())
    StopObserving();
}

ScriptPromise CookieStore::DoRead(
    ScriptState* script_state,
    const CookieStoreGetOptions* options,
    DoReadBackendResultConverter backend_result_converter,
    ExceptionState& exception_state) {
  network::mojom::blink::CookieManagerGetOptionsPtr backend_options =
      ToBackendOptions(options, exception_state);
  KURL cookie_url = CookieUrlForRead(options, default_cookie_url_, script_state,
                                     exception_state);
  if (backend_options.is_null() || cookie_url.IsNull()) {
    DCHECK(exception_state.HadException());
    return ScriptPromise();
  }

  if (!backend_) {
    exception_state.ThrowDOMException(DOMExceptionCode::kInvalidStateError,
                                      "CookieStore backend went away");
    return ScriptPromise();
  }

  auto* resolver = MakeGarbageCollected<ScriptPromiseResolver>(script_state);
  backend_->GetAllForUrl(
      cookie_url, default_site_for_cookies_, default_top_frame_origin_,
      std::move(backend_options),
      WTF::Bind(backend_result_converter, WrapPersistent(resolver)));
  return resolver->Promise();
}

// static
void CookieStore::GetAllForUrlToGetAllResult(
    ScriptPromiseResolver* resolver,
    const Vector<CanonicalCookie>& backend_cookies) {
  ScriptState* script_state = resolver->GetScriptState();
  if (!script_state->ContextIsValid())
    return;

  HeapVector<Member<CookieListItem>> cookies;
  cookies.ReserveInitialCapacity(backend_cookies.size());
  for (const auto& backend_cookie : backend_cookies) {
    cookies.push_back(CookieChangeEvent::ToCookieListItem(
        backend_cookie, false /* is_deleted */));
  }

  resolver->Resolve(std::move(cookies));
}

// static
void CookieStore::GetAllForUrlToGetResult(
    ScriptPromiseResolver* resolver,
    const Vector<CanonicalCookie>& backend_cookies) {
  ScriptState* script_state = resolver->GetScriptState();
  if (!script_state->ContextIsValid())
    return;

  if (backend_cookies.IsEmpty()) {
    resolver->Resolve(v8::Null(script_state->GetIsolate()));
    return;
  }

  const auto& backend_cookie = backend_cookies.front();
  CookieListItem* cookie = CookieChangeEvent::ToCookieListItem(
      backend_cookie, false /* is_deleted */);
  resolver->Resolve(cookie);
}

ScriptPromise CookieStore::DoWrite(ScriptState* script_state,
                                   const CookieStoreSetExtraOptions* options,
                                   ExceptionState& exception_state) {
  base::Optional<CanonicalCookie> canonical_cookie =
      ToCanonicalCookie(default_cookie_url_, options, exception_state);
  if (!canonical_cookie) {
    DCHECK(exception_state.HadException());
    return ScriptPromise();
  }

  if (!backend_) {
    exception_state.ThrowDOMException(DOMExceptionCode::kInvalidStateError,
                                      "CookieStore backend went away");
    return ScriptPromise();
  }

  auto* resolver = MakeGarbageCollected<ScriptPromiseResolver>(script_state);
  backend_->SetCanonicalCookie(
      std::move(canonical_cookie.value()), default_cookie_url_,
      default_site_for_cookies_, default_top_frame_origin_,
      WTF::Bind(&CookieStore::OnSetCanonicalCookieResult,
                WrapPersistent(resolver)));
  return resolver->Promise();
}

// static
void CookieStore::OnSetCanonicalCookieResult(ScriptPromiseResolver* resolver,
                                             bool backend_success) {
  ScriptState* script_state = resolver->GetScriptState();
  if (!script_state->ContextIsValid())
    return;

  if (!backend_success) {
    resolver->Reject(MakeGarbageCollected<DOMException>(
        DOMExceptionCode::kUnknownError,
        "An unknown error occured while writing the cookie."));
    return;
  }
  resolver->Resolve();
}

void CookieStore::StartObserving() {
  if (change_listener_receiver_.is_bound() || !backend_)
    return;

  // See https://bit.ly/2S0zRAS for task types.
  auto task_runner =
      GetExecutionContext()->GetTaskRunner(TaskType::kMiscPlatformAPI);
  backend_->AddChangeListener(
      default_cookie_url_, default_site_for_cookies_, default_top_frame_origin_,
      change_listener_receiver_.BindNewPipeAndPassRemote(task_runner), {});
}

void CookieStore::StopObserving() {
  if (!change_listener_receiver_.is_bound())
    return;
  change_listener_receiver_.reset();
}

}  // namespace blink
