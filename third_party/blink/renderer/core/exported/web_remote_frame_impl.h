// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_EXPORTED_WEB_REMOTE_FRAME_IMPL_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_EXPORTED_WEB_REMOTE_FRAME_IMPL_H_

#include "third_party/blink/public/mojom/frame/user_activation_update_types.mojom-blink-forward.h"
#include "third_party/blink/public/platform/web_insecure_request_policy.h"
#include "third_party/blink/public/web/web_remote_frame.h"
#include "third_party/blink/public/web/web_remote_frame_client.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/frame/remote_frame.h"
#include "third_party/blink/renderer/platform/heap/self_keep_alive.h"
#include "third_party/blink/renderer/platform/wtf/casting.h"

namespace cc {
class Layer;
}

namespace blink {

class FrameOwner;
class RemoteFrame;
class RemoteFrameClientImpl;
enum class WebFrameLoadType;
class WebView;
struct WebRect;
class WindowAgentFactory;

class CORE_EXPORT WebRemoteFrameImpl final
    : public GarbageCollected<WebRemoteFrameImpl>,
      public WebRemoteFrame {
 public:
  static WebRemoteFrameImpl* CreateMainFrame(WebView*,
                                             WebRemoteFrameClient*,
                                             InterfaceRegistry*,
                                             AssociatedInterfaceProvider*,
                                             WebFrame* opener);
  static WebRemoteFrameImpl* CreateForPortal(WebTreeScopeType,
                                             WebRemoteFrameClient*,
                                             InterfaceRegistry*,
                                             AssociatedInterfaceProvider*,
                                             const WebElement& portal_element);

  WebRemoteFrameImpl(WebTreeScopeType,
                     WebRemoteFrameClient*,
                     InterfaceRegistry*,
                     AssociatedInterfaceProvider*);
  ~WebRemoteFrameImpl() override;

  // WebFrame methods:
  void Close() override;
  WebView* View() const override;
  void StopLoading() override;

  // WebRemoteFrame methods:
  WebLocalFrame* CreateLocalChild(WebTreeScopeType,
                                  const WebString& name,
                                  const FramePolicy&,
                                  WebLocalFrameClient*,
                                  blink::InterfaceRegistry*,
                                  WebFrame* previous_sibling,
                                  const WebFrameOwnerProperties&,
                                  FrameOwnerElementType,
                                  WebFrame* opener) override;
  WebRemoteFrame* CreateRemoteChild(WebTreeScopeType,
                                    const WebString& name,
                                    const FramePolicy&,
                                    FrameOwnerElementType,
                                    WebRemoteFrameClient*,
                                    blink::InterfaceRegistry*,
                                    AssociatedInterfaceProvider*,
                                    WebFrame* opener) override;
  void SetCcLayer(cc::Layer*,
                  bool prevent_contents_opaque_changes,
                  bool is_surface_layer) override;
  void SetReplicatedOrigin(
      const WebSecurityOrigin&,
      bool is_potentially_trustworthy_opaque_origin) override;
  void SetReplicatedSandboxFlags(WebSandboxFlags) override;
  void SetReplicatedName(const WebString&) override;
  void SetReplicatedFeaturePolicyHeaderAndOpenerPolicies(
      const ParsedFeaturePolicy& parsed_header,
      const FeaturePolicy::FeatureState&) override;
  void AddReplicatedContentSecurityPolicyHeader(
      const WebString& header_value,
      network::mojom::ContentSecurityPolicyType,
      network::mojom::ContentSecurityPolicySource) override;
  void ResetReplicatedContentSecurityPolicy() override;
  void SetReplicatedInsecureRequestPolicy(WebInsecureRequestPolicy) override;
  void SetReplicatedInsecureNavigationsSet(const WebVector<unsigned>&) override;
  void SetReplicatedAdFrameType(
      mojom::blink::AdFrameType ad_frame_type) override;
  void DidStartLoading() override;
  bool IsIgnoredForHitTest() const override;
  void UpdateUserActivationState(
      mojom::blink::UserActivationUpdateType) override;
  void TransferUserActivationFrom(blink::WebRemoteFrame* source_frame) override;
  void SetHadStickyUserActivationBeforeNavigation(bool value) override;
  v8::Local<v8::Object> GlobalProxy() const override;
  WebRect GetCompositingRect() override;

  void InitializeCoreFrame(Page&,
                           FrameOwner*,
                           const AtomicString& name,
                           WindowAgentFactory*);
  RemoteFrame* GetFrame() const { return frame_.Get(); }

  WebRemoteFrameClient* Client() const { return client_; }

  static WebRemoteFrameImpl* FromFrame(RemoteFrame&);

  void Trace(blink::Visitor*);

 private:
  friend class RemoteFrameClientImpl;

  void SetCoreFrame(RemoteFrame*);

  // Inherited from WebFrame, but intentionally hidden: it never makes sense
  // to call these on a WebRemoteFrameImpl.
  bool IsWebLocalFrame() const override;
  WebLocalFrame* ToWebLocalFrame() override;
  bool IsWebRemoteFrame() const override;
  WebRemoteFrame* ToWebRemoteFrame() override;

  WebRemoteFrameClient* client_;
  // TODO(dcheng): Inline this field directly rather than going through Member.
  Member<RemoteFrameClientImpl> frame_client_;
  Member<RemoteFrame> frame_;

  InterfaceRegistry* const interface_registry_;
  AssociatedInterfaceProvider* const associated_interface_provider_;

  // Oilpan: WebRemoteFrameImpl must remain alive until close() is called.
  // Accomplish that by keeping a self-referential Persistent<>. It is
  // cleared upon close().
  SelfKeepAlive<WebRemoteFrameImpl> self_keep_alive_;
};

template <>
struct DowncastTraits<WebRemoteFrameImpl> {
  static bool AllowFrom(const WebFrame& frame) {
    return frame.IsWebRemoteFrame();
  }
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_EXPORTED_WEB_REMOTE_FRAME_IMPL_H_
