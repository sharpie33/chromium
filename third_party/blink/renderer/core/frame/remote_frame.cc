// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/frame/remote_frame.h"

#include "cc/layers/surface_layer.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "third_party/blink/public/common/features.h"
#include "third_party/blink/public/mojom/frame/intrinsic_sizing_info.mojom-blink.h"
#include "third_party/blink/public/platform/interface_registry.h"
#include "third_party/blink/public/web/web_frame.h"
#include "third_party/blink/public/web/web_view.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_fullscreen_options.h"
#include "third_party/blink/renderer/bindings/core/v8/window_proxy.h"
#include "third_party/blink/renderer/bindings/core/v8/window_proxy_manager.h"
#include "third_party/blink/renderer/core/frame/csp/content_security_policy.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame_client.h"
#include "third_party/blink/renderer/core/frame/remote_dom_window.h"
#include "third_party/blink/renderer/core/frame/remote_frame_client.h"
#include "third_party/blink/renderer/core/frame/remote_frame_view.h"
#include "third_party/blink/renderer/core/fullscreen/fullscreen.h"
#include "third_party/blink/renderer/core/html/html_frame_owner_element.h"
#include "third_party/blink/renderer/core/inspector/console_message.h"
#include "third_party/blink/renderer/core/layout/geometry/physical_rect.h"
#include "third_party/blink/renderer/core/layout/intrinsic_sizing_info.h"
#include "third_party/blink/renderer/core/layout/layout_embedded_content.h"
#include "third_party/blink/renderer/core/layout/layout_view.h"
#include "third_party/blink/renderer/core/loader/frame_load_request.h"
#include "third_party/blink/renderer/core/loader/frame_loader.h"
#include "third_party/blink/renderer/core/loader/mixed_content_checker.h"
#include "third_party/blink/renderer/core/page/page.h"
#include "third_party/blink/renderer/core/page/plugin_script_forbidden_scope.h"
#include "third_party/blink/renderer/core/paint/paint_layer.h"
#include "third_party/blink/renderer/core/probe/core_probes.h"
#include "third_party/blink/renderer/core/scroll/scroll_into_view_params_type_converters.h"
#include "third_party/blink/renderer/core/timing/dom_window_performance.h"
#include "third_party/blink/renderer/platform/graphics/graphics_layer.h"
#include "third_party/blink/renderer/platform/heap/heap.h"
#include "third_party/blink/renderer/platform/loader/fetch/fetch_client_settings_object.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_fetcher.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_fetcher_properties.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_request.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_timing_info.h"
#include "third_party/blink/renderer/platform/weborigin/security_policy.h"

namespace blink {

namespace {
FloatRect DeNormalizeRect(const WebFloatRect& normalized, const IntRect& base) {
  FloatRect result = normalized;
  result.Scale(base.Width(), base.Height());
  result.MoveBy(FloatPoint(base.Location()));
  return result;
}

}  // namespace

RemoteFrame::RemoteFrame(
    RemoteFrameClient* client,
    Page& page,
    FrameOwner* owner,
    WindowAgentFactory* inheriting_agent_factory,
    InterfaceRegistry* interface_registry,
    AssociatedInterfaceProvider* associated_interface_provider)
    : Frame(client,
            page,
            owner,
            MakeGarbageCollected<RemoteWindowProxyManager>(*this),
            inheriting_agent_factory) {
  dom_window_ = MakeGarbageCollected<RemoteDOMWindow>(*this);

  interface_registry->AddAssociatedInterface(WTF::BindRepeating(
      &RemoteFrame::BindToReceiver, WrapWeakPersistent(this)));

  associated_interface_provider->GetInterface(
      remote_frame_host_remote_.BindNewEndpointAndPassReceiver());

  UpdateInertIfPossible();
  UpdateInheritedEffectiveTouchActionIfPossible();
  UpdateVisibleToHitTesting();
  Initialize();
}

RemoteFrame::~RemoteFrame() {
  DCHECK(!view_);
}

void RemoteFrame::Trace(blink::Visitor* visitor) {
  visitor->Trace(view_);
  visitor->Trace(security_context_);
  Frame::Trace(visitor);
}

void RemoteFrame::Navigate(FrameLoadRequest& frame_request,
                           WebFrameLoadType frame_load_type) {
  if (!navigation_rate_limiter().CanProceed())
    return;

  frame_request.SetFrameType(IsMainFrame()
                                 ? mojom::RequestContextFrameType::kTopLevel
                                 : mojom::RequestContextFrameType::kNested);

  const KURL& url = frame_request.GetResourceRequest().Url();
  if (!frame_request.CanDisplay(url)) {
    if (frame_request.OriginDocument()) {
      frame_request.OriginDocument()->AddConsoleMessage(ConsoleMessage::Create(
          mojom::ConsoleMessageSource::kSecurity,
          mojom::ConsoleMessageLevel::kError,
          "Not allowed to load local resource: " + url.ElidedString()));
    }
    return;
  }

  // The process where this frame actually lives won't have sufficient
  // information to upgrade the url, since it won't have access to the
  // originDocument. Do it now.
  const FetchClientSettingsObject* fetch_client_settings_object = nullptr;
  if (frame_request.OriginDocument()) {
    fetch_client_settings_object = &frame_request.OriginDocument()
                                        ->Fetcher()
                                        ->GetProperties()
                                        .GetFetchClientSettingsObject();
  }
  LocalFrame* frame = frame_request.OriginDocument()
                          ? frame_request.OriginDocument()->GetFrame()
                          : nullptr;
  MixedContentChecker::UpgradeInsecureRequest(
      frame_request.GetResourceRequest(), fetch_client_settings_object,
      frame_request.OriginDocument(), frame_request.GetFrameType(),
      frame ? frame->GetContentSettingsClient() : nullptr);

  // Navigations in portal contexts do not create back/forward entries.
  if (GetPage()->InsidePortal() &&
      frame_load_type == WebFrameLoadType::kStandard) {
    frame_load_type = WebFrameLoadType::kReplaceCurrentItem;
  }

  bool is_opener_navigation = false;
  bool initiator_frame_has_download_sandbox_flag = false;
  bool initiator_frame_is_ad = false;

  if (frame) {
    is_opener_navigation = frame->Client()->Opener() == this;
    initiator_frame_has_download_sandbox_flag =
        frame->GetSecurityContext() &&
        frame->GetSecurityContext()->IsSandboxed(WebSandboxFlags::kDownloads);
    initiator_frame_is_ad = frame->IsAdSubframe();
    if (frame_request.ClientRedirectReason() != ClientNavigationReason::kNone) {
      probe::FrameRequestedNavigation(frame, this, url,
                                      frame_request.ClientRedirectReason());
    }
  }

  Client()->Navigate(frame_request.GetResourceRequest(),
                     frame_load_type == WebFrameLoadType::kReplaceCurrentItem,
                     is_opener_navigation,
                     initiator_frame_has_download_sandbox_flag,
                     initiator_frame_is_ad, frame_request.GetBlobURLToken());
}

void RemoteFrame::DetachImpl(FrameDetachType type) {
  PluginScriptForbiddenScope forbid_plugin_destructor_scripting;
  DetachChildren();
  if (!Client())
    return;

  // Clean up the frame's view if needed. A remote frame only has a view if
  // the parent is a local frame.
  if (view_)
    view_->Dispose();
  GetWindowProxyManager()->ClearForClose();
  SetView(nullptr);
  // ... the RemoteDOMWindow will need to be informed of detachment,
  // as otherwise it will keep a strong reference back to this RemoteFrame.
  // That combined with wrappers (owned and kept alive by RemoteFrame) keeping
  // persistent strong references to RemoteDOMWindow will prevent the GCing
  // of all these objects. Break the cycle by notifying of detachment.
  To<RemoteDOMWindow>(dom_window_.Get())->FrameDetached();
  if (cc_layer_)
    SetCcLayer(nullptr, false, false);
  receiver_.reset();
}

bool RemoteFrame::DetachDocument() {
  DetachChildren();
  return !!GetPage();
}

void RemoteFrame::CheckCompleted() {
  // Notify the client so that the corresponding LocalFrame can do the check.
  GetRemoteFrameHostRemote().CheckCompleted();
}

const RemoteSecurityContext* RemoteFrame::GetSecurityContext() const {
  return &security_context_;
}

bool RemoteFrame::ShouldClose() {
  // TODO(nasko): Implement running the beforeunload handler in the actual
  // LocalFrame running in a different process and getting back a real result.
  return true;
}

void RemoteFrame::SetIsInert(bool inert) {
  if (inert != is_inert_)
    Client()->SetIsInert(inert);
  is_inert_ = inert;
}

void RemoteFrame::SetInheritedEffectiveTouchAction(TouchAction touch_action) {
  if (inherited_effective_touch_action_ != touch_action)
    GetRemoteFrameHostRemote().SetInheritedEffectiveTouchAction(touch_action);
  inherited_effective_touch_action_ = touch_action;
}

bool RemoteFrame::BubbleLogicalScrollFromChildFrame(
    mojom::blink::ScrollDirection direction,
    ScrollGranularity granularity,
    Frame* child) {
  DCHECK(child->Client());
  To<LocalFrame>(child)
      ->GetLocalFrameHostRemote()
      .BubbleLogicalScrollInParentFrame(direction, granularity);
  return false;
}

void RemoteFrame::RenderFallbackContent() {
  // TODO(ekaramad): If the owner renders its own content, then the current
  // ContentFrame() should detach and free-up the OOPIF process (see
  // https://crbug.com/850223).
  auto* owner = DeprecatedLocalOwner();
  DCHECK(IsA<HTMLObjectElement>(owner));
  owner->RenderFallbackContent(this);
}

void RemoteFrame::AddResourceTimingFromChild(
    mojom::blink::ResourceTimingInfoPtr timing) {
  HTMLFrameOwnerElement* owner_element = To<HTMLFrameOwnerElement>(Owner());
  DCHECK(owner_element);

  // TODO(https://crbug.com/900700): Take a Mojo pending receiver for
  // WorkerTimingContainer for navigation from the calling function.
  DOMWindowPerformance::performance(*owner_element->GetDocument().domWindow())
      ->AddResourceTiming(std::move(timing), owner_element->localName(),
                          /*worker_timing_receiver=*/mojo::NullReceiver());
}

void RemoteFrame::DidStartLoading() {
  SetIsLoading(true);
}

void RemoteFrame::DidStopLoading() {
  SetIsLoading(false);

  // When a subframe finishes loading, the parent should check if *all*
  // subframes have finished loading (which may mean that the parent can declare
  // that the parent itself has finished loading). This remote-subframe-focused
  // code has a local-subframe equivalent in FrameLoader::DidFinishNavigation.
  Frame* parent = Tree().Parent();
  if (parent)
    parent->CheckCompleted();
}

void RemoteFrame::DidFocus() {
  GetRemoteFrameHostRemote().DidFocusFrame();
}

void RemoteFrame::SetView(RemoteFrameView* view) {
  // Oilpan: as RemoteFrameView performs no finalization actions,
  // no explicit Dispose() of it needed here. (cf. LocalFrameView::Dispose().)
  view_ = view;
}

void RemoteFrame::CreateView() {
  // If the RemoteFrame does not have a LocalFrame parent, there's no need to
  // create a EmbeddedContentView for it.
  if (!DeprecatedLocalOwner())
    return;

  DCHECK(!DeprecatedLocalOwner()->OwnedEmbeddedContentView());

  SetView(MakeGarbageCollected<RemoteFrameView>(this));

  if (OwnerLayoutObject())
    DeprecatedLocalOwner()->SetEmbeddedContentView(view_);
}

mojom::blink::RemoteFrameHost& RemoteFrame::GetRemoteFrameHostRemote() {
  return *remote_frame_host_remote_.get();
}

RemoteFrameClient* RemoteFrame::Client() const {
  return static_cast<RemoteFrameClient*>(Frame::Client());
}

void RemoteFrame::DidChangeVisibleToHitTesting() {
  if (!cc_layer_ || !is_surface_layer_)
    return;

  static_cast<cc::SurfaceLayer*>(cc_layer_)->SetHasPointerEventsNone(
      IsIgnoredForHitTest());
}

void RemoteFrame::SetReplicatedFeaturePolicyHeaderAndOpenerPolicies(
    const ParsedFeaturePolicy& parsed_header,
    const FeaturePolicy::FeatureState& opener_feature_state) {
  feature_policy_header_ = parsed_header;
  if (RuntimeEnabledFeatures::FeaturePolicyForSandboxEnabled()) {
    DCHECK(opener_feature_state.empty() || IsMainFrame());
    if (OpenerFeatureState().empty()) {
      SetOpenerFeatureState(opener_feature_state);
    }
  }
  ApplyReplicatedFeaturePolicyHeader();
}

void RemoteFrame::SetReplicatedSandboxFlags(WebSandboxFlags flags) {
  security_context_.ResetAndEnforceSandboxFlags(flags);
}

void RemoteFrame::SetInsecureRequestPolicy(WebInsecureRequestPolicy policy) {
  security_context_.SetInsecureRequestPolicy(policy);
}

void RemoteFrame::SetInsecureNavigationsSet(const WebVector<unsigned>& set) {
  security_context_.SetInsecureNavigationsSet(set);
}

void RemoteFrame::WillEnterFullscreen() {
  // This should only ever be called when the FrameOwner is local.
  HTMLFrameOwnerElement* owner_element = To<HTMLFrameOwnerElement>(Owner());

  // Call |requestFullscreen()| on |ownerElement| to make it the pending
  // fullscreen element in anticipation of the coming |didEnterFullscreen()|
  // call.
  //
  // PrefixedForCrossProcessDescendant is necessary because:
  //  - The fullscreen element ready check and other checks should be bypassed.
  //  - |ownerElement| will need :-webkit-full-screen-ancestor style in addition
  //    to :fullscreen.
  //
  // TODO(alexmos): currently, this assumes prefixed requests, but in the
  // future, this should plumb in information about which request type
  // (prefixed or unprefixed) to use for firing fullscreen events.
  Fullscreen::RequestFullscreen(
      *owner_element, FullscreenOptions::Create(),
      Fullscreen::RequestType::kPrefixedForCrossProcessDescendant);
}

void RemoteFrame::AddReplicatedContentSecurityPolicies(
    WTF::Vector<network::mojom::blink::ContentSecurityPolicyHeaderPtr>
        headers) {
  for (auto& header : headers) {
    GetSecurityContext()->GetContentSecurityPolicy()->AddPolicyFromHeaderValue(
        header->header_value, header->type, header->source);
  }
}

void RemoteFrame::ResetReplicatedContentSecurityPolicy() {
  security_context_.ResetReplicatedContentSecurityPolicy();
}

void RemoteFrame::EnforceInsecureNavigationsSet(
    const WTF::Vector<uint32_t>& set) {
  security_context_.SetInsecureNavigationsSet(set);
}

void RemoteFrame::SetReplicatedOrigin(
    const scoped_refptr<const SecurityOrigin>& origin,
    bool is_potentially_trustworthy_unique_origin) {
  scoped_refptr<SecurityOrigin> security_origin = origin->IsolatedCopy();
  security_origin->SetOpaqueOriginIsPotentiallyTrustworthy(
      is_potentially_trustworthy_unique_origin);
  security_context_.SetReplicatedOrigin(security_origin);
  ApplyReplicatedFeaturePolicyHeader();

  // If the origin of a remote frame changed, the accessibility object for the
  // owner element now points to a different child.
  //
  // TODO(dmazzoni, dcheng): there's probably a better way to solve this.
  // Run SitePerProcessAccessibilityBrowserTest.TwoCrossSiteNavigations to
  // ensure an alternate fix works.  http://crbug.com/566222
  FrameOwner* owner = Owner();
  HTMLElement* owner_element = DynamicTo<HTMLFrameOwnerElement>(owner);
  if (owner_element) {
    AXObjectCache* cache = owner_element->GetDocument().ExistingAXObjectCache();
    if (cache)
      cache->ChildrenChanged(owner_element);
  }
}

void RemoteFrame::SetReplicatedAdFrameType(
    mojom::blink::AdFrameType ad_frame_type) {
  if (ad_frame_type_ == mojom::blink::AdFrameType::kNonAd) {
    ad_frame_type_ = ad_frame_type;
  } else {
    DCHECK_EQ(ad_frame_type_, ad_frame_type);
  }
}

void RemoteFrame::DispatchLoadEventForFrameOwner() {
  DCHECK(Owner()->IsLocal());
  Owner()->DispatchLoad();
}

void RemoteFrame::Collapse(bool collapsed) {
  FrameOwner* owner = Owner();
  To<HTMLFrameOwnerElement>(owner)->SetCollapsed(collapsed);
}

void RemoteFrame::Focus() {
  FocusImpl();
}

void RemoteFrame::SetHadStickyUserActivationBeforeNavigation(bool value) {
  Frame::SetHadStickyUserActivationBeforeNavigation(value);
}

void RemoteFrame::SetNeedsOcclusionTracking(bool needs_tracking) {
  View()->SetNeedsOcclusionTracking(needs_tracking);
}

void RemoteFrame::BubbleLogicalScroll(
    mojom::blink::ScrollDirection direction,
    ui::input_types::ScrollGranularity granularity) {
  Frame* parent_frame = Client()->Parent();
  DCHECK(parent_frame);
  DCHECK(parent_frame->IsLocalFrame());

  parent_frame->BubbleLogicalScrollFromChildFrame(direction, granularity, this);
}

void RemoteFrame::UpdateUserActivationState(
    mojom::blink::UserActivationUpdateType update_type) {
  switch (update_type) {
    case mojom::blink::UserActivationUpdateType::kNotifyActivation:
      NotifyUserActivationInLocalTree();
      break;
    case mojom::blink::UserActivationUpdateType::kConsumeTransientActivation:
      ConsumeTransientUserActivationInLocalTree();
      break;
    case mojom::blink::UserActivationUpdateType::kClearActivation:
      ClearUserActivationInLocalTree();
      break;
    case mojom::blink::UserActivationUpdateType::
        kNotifyActivationPendingBrowserVerification:
      NOTREACHED() << "Unexpected UserActivationUpdateType from browser";
      break;
  }
}

void RemoteFrame::SetEmbeddingToken(
    const base::UnguessableToken& embedding_token) {
  FrameOwner* owner = Owner();
  To<HTMLFrameOwnerElement>(owner)->SetEmbeddingToken(embedding_token);
}

void RemoteFrame::SetPageFocus(bool is_focused) {
  WebFrame::FromFrame(this)->View()->SetFocus(is_focused);
}

void RemoteFrame::ScrollRectToVisible(
    const WebRect& rect_to_scroll,
    mojom::blink::ScrollIntoViewParamsPtr params) {
  Element* owner_element = DeprecatedLocalOwner();
  LayoutObject* owner_object = owner_element->GetLayoutObject();
  if (!owner_object) {
    // The LayoutObject could be nullptr by the time we get here. For instance
    // <iframe>'s style might have been set to 'display: none' right after
    // scrolling starts in the OOPIF's process (see https://crbug.com/777811).
    return;
  }

  // Schedule the scroll.
  PhysicalRect absolute_rect = owner_object->LocalToAncestorRect(
      PhysicalRect(LayoutUnit(rect_to_scroll.x), LayoutUnit(rect_to_scroll.y),
                   LayoutUnit(rect_to_scroll.width),
                   LayoutUnit(rect_to_scroll.height)),
      owner_object->View());

  if (!params->zoom_into_rect ||
      !owner_object->GetDocument().GetFrame()->LocalFrameRoot().IsMainFrame()) {
    owner_object->ScrollRectToVisible(absolute_rect, std::move(params));
    return;
  }

  // ZoomAndScrollToFocusedEditableElementRect will scroll only the layout and
  // visual viewports. Ensure the element is actually visible in the viewport
  // scrolling layer. (i.e. isn't clipped by some other content).
  auto relative_element_bounds = params->relative_element_bounds;
  auto relative_caret_bounds = params->relative_caret_bounds;

  params->stop_at_main_frame_layout_viewport = true;
  absolute_rect =
      owner_object->ScrollRectToVisible(absolute_rect, std::move(params));

  IntRect rect_in_document =
      owner_object->GetDocument()
          .GetFrame()
          ->LocalFrameRoot()
          .View()
          ->RootFrameToDocument(EnclosingIntRect(
              owner_element->GetDocument().View()->ConvertToRootFrame(
                  absolute_rect)));
  IntRect element_bounds_in_document = EnclosingIntRect(
      DeNormalizeRect(relative_element_bounds, rect_in_document));
  IntRect caret_bounds_in_document = EnclosingIntRect(
      DeNormalizeRect(relative_caret_bounds, rect_in_document));

  // This is due to something such as scroll focused editable element into
  // view on Android which also requires an automatic zoom into legible scale.
  // This is handled by main frame's WebView.
  WebFrame::FromFrame(this)->View()->ZoomAndScrollToFocusedEditableElementRect(
      element_bounds_in_document, caret_bounds_in_document, true);
}

void RemoteFrame::IntrinsicSizingInfoOfChildChanged(
    mojom::blink::IntrinsicSizingInfoPtr info) {
  FrameOwner* owner = Owner();
  // Only communication from HTMLPluginElement-owned subframes is allowed
  // at present. This includes <embed> and <object> tags.
  if (!owner || !owner->IsPlugin())
    return;

  // TODO(https://crbug.com/1044304): Should either remove the native
  // C++ Blink type and use the Mojo type everywhere or typemap the
  // Mojo type to the pre-existing native C++ Blink type.
  IntrinsicSizingInfo sizing_info;
  sizing_info.size = FloatSize(info->size->width, info->size->height);
  sizing_info.aspect_ratio =
      FloatSize(info->aspect_ratio->width, info->aspect_ratio->height);
  sizing_info.has_width = info->has_width;
  sizing_info.has_height = info->has_height;
  View()->SetIntrinsicSizeInfo(sizing_info);

  owner->IntrinsicSizingInfoChanged();
}

void RemoteFrame::SetMainFrameViewportSize(
    const IntSize& main_frame_viewport_size) {
  DCHECK(IsMainFrame());
  main_frame_viewport_size_ = main_frame_viewport_size;
}

IntSize RemoteFrame::GetMainFrameViewportSize() const {
  if (!IsMainFrame())
    return Tree().Top().GetMainFrameViewportSize();
  return main_frame_viewport_size_;
}

void RemoteFrame::SetMainFrameScrollOffset(
    const IntPoint& main_frame_scroll_offset) {
  DCHECK(IsMainFrame());
  main_frame_scroll_offset_ = main_frame_scroll_offset;
}

IntPoint RemoteFrame::GetMainFrameScrollOffset() const {
  if (!IsMainFrame())
    return Tree().Top().GetMainFrameScrollOffset();
  return main_frame_scroll_offset_;
}

bool RemoteFrame::IsIgnoredForHitTest() const {
  HTMLFrameOwnerElement* owner = DeprecatedLocalOwner();
  if (!owner || !owner->GetLayoutObject())
    return false;

  return owner->OwnerType() == FrameOwnerElementType::kPortal ||
         !visible_to_hit_testing_;
}

void RemoteFrame::UpdateHitTestOcclusionData() {
  if (!cc_layer_ || !is_surface_layer_)
    return;
  bool unoccluded = false;
  if (base::FeatureList::IsEnabled(
          blink::features::kVizHitTestOcclusionCheck)) {
    if (LayoutEmbeddedContent* owner = OwnerLayoutObject()) {
      if (owner->GetFrame()->IsAttached() &&
          !owner->GetFrameView()->CanThrottleRendering()) {
        // TODO(szager): remove this CHECK after diagnosing crash.
        CHECK(owner->GetFrame()->GetPage());
        HitTestResult hit_test_result(owner->HitTestForOcclusion());
        const Node* hit_node = hit_test_result.InnerNode();
        unoccluded = (!hit_node || hit_node == owner->GetNode());
      }
    }
  }
  static_cast<cc::SurfaceLayer*>(cc_layer_)->SetUnoccludedForHitTesting(
      unoccluded);
}

void RemoteFrame::SetCcLayer(cc::Layer* cc_layer,
                             bool prevent_contents_opaque_changes,
                             bool is_surface_layer) {
  DCHECK(Owner());

  cc_layer_ = cc_layer;
  prevent_contents_opaque_changes_ = prevent_contents_opaque_changes;
  is_surface_layer_ = is_surface_layer;
  if (cc_layer_) {
    if (is_surface_layer) {
      static_cast<cc::SurfaceLayer*>(cc_layer_)->SetHasPointerEventsNone(
          IsIgnoredForHitTest());
    }
  }

  To<HTMLFrameOwnerElement>(Owner())->SetNeedsCompositingUpdate();
}

void RemoteFrame::AdvanceFocus(mojom::blink::FocusType type,
                               LocalFrame* source) {
  Client()->AdvanceFocus(type, source);
}

void RemoteFrame::DetachChildren() {
  using FrameVector = HeapVector<Member<Frame>>;
  FrameVector children_to_detach;
  children_to_detach.ReserveCapacity(Tree().ChildCount());
  for (Frame* child = Tree().FirstChild(); child;
       child = child->Tree().NextSibling())
    children_to_detach.push_back(child);
  for (const auto& child : children_to_detach)
    child->Detach(FrameDetachType::kRemove);
}

void RemoteFrame::ApplyReplicatedFeaturePolicyHeader() {
  const FeaturePolicy* parent_feature_policy = nullptr;
  if (Frame* parent_frame = Client()->Parent()) {
    parent_feature_policy =
        parent_frame->GetSecurityContext()->GetFeaturePolicy();
  }
  ParsedFeaturePolicy container_policy;
  if (Owner())
    container_policy = Owner()->GetFramePolicy().container_policy;
  const FeaturePolicy::FeatureState& opener_feature_state =
      OpenerFeatureState();
  security_context_.InitializeFeaturePolicy(
      feature_policy_header_, container_policy, parent_feature_policy,
      opener_feature_state.empty() ? nullptr : &opener_feature_state);
}

void RemoteFrame::BindToReceiver(
    blink::RemoteFrame* frame,
    mojo::PendingAssociatedReceiver<mojom::blink::RemoteFrame> receiver) {
  DCHECK(frame);
  frame->receiver_.Bind(std::move(receiver));
}

}  // namespace blink
