// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_NETWORK_NETWORK_CONTEXT_H_
#define SERVICES_NETWORK_NETWORK_CONTEXT_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "base/callback.h"
#include "base/component_export.h"
#include "base/containers/flat_map.h"
#include "base/containers/unique_ptr_adapters.h"
#include "base/macros.h"
#include "base/optional.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/unique_receiver_set.h"
#include "net/cert/cert_verifier.h"
#include "net/cert/cert_verify_result.h"
#include "net/dns/dns_config_overrides.h"
#include "net/dns/host_resolver.h"
#include "net/http/http_auth_preferences.h"
#include "services/network/cors/preflight_controller.h"
#include "services/network/http_cache_data_counter.h"
#include "services/network/http_cache_data_remover.h"
#include "services/network/network_qualities_pref_delegate.h"
#include "services/network/origin_policy/origin_policy_manager.h"
#include "services/network/public/cpp/cors/origin_access_list.h"
#include "services/network/public/cpp/network_service_buildflags.h"
#include "services/network/public/mojom/host_resolver.mojom.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/origin_policy_manager.mojom.h"
#include "services/network/public/mojom/proxy_lookup_client.mojom.h"
#include "services/network/public/mojom/proxy_resolving_socket.mojom.h"
#include "services/network/public/mojom/restricted_cookie_manager.mojom.h"
#include "services/network/public/mojom/tcp_socket.mojom.h"
#include "services/network/public/mojom/udp_socket.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "services/network/public/mojom/websocket.mojom.h"
#include "services/network/socket_factory.h"
#include "services/network/url_request_context_owner.h"

#if defined(OS_CHROMEOS)
#include "crypto/scoped_nss_types.h"
#endif

namespace base {
class UnguessableToken;
}  // namespace base

namespace net {
class CertNetFetcher;
class CertNetFetcherURLRequest;
class CertVerifier;
class CertVerifyProc;
class HostPortPair;
class NetworkIsolationKey;
class ReportSender;
class StaticHttpUserAgentSettings;
class URLRequestContext;
}  // namespace net

namespace certificate_transparency {
class ChromeRequireCTDelegate;
}  // namespace certificate_transparency

namespace domain_reliability {
class DomainReliabilityMonitor;
}  // namespace domain_reliability

namespace network {
class CertVerifierWithTrustAnchors;
class CookieManager;
class ExpectCTReporter;
class HostResolver;
class NetworkService;
class NetworkServiceNetworkDelegate;
class NetworkServiceProxyDelegate;
class MdnsResponderManager;
class NSSTempCertsCacheChromeOS;
class P2PSocketManager;
class ProxyLookupRequest;
class ResourceScheduler;
class ResourceSchedulerClient;
class QuicTransport;
class WebSocketFactory;

namespace cors {
class CorsURLLoaderFactory;
}  // namespace cors

// A NetworkContext creates and manages access to a URLRequestContext.
//
// When the network service is enabled, NetworkContexts are created through
// NetworkService's mojo interface and are owned jointly by the NetworkService
// and the mojo::Remote<NetworkContext> used to talk to them, and the
// NetworkContext is destroyed when either one is torn down.
class COMPONENT_EXPORT(NETWORK_SERVICE) NetworkContext
    : public mojom::NetworkContext {
 public:
  using OnConnectionCloseCallback =
      base::OnceCallback<void(NetworkContext* network_context)>;

  NetworkContext(NetworkService* network_service,
                 mojo::PendingReceiver<mojom::NetworkContext> receiver,
                 mojom::NetworkContextParamsPtr params,
                 OnConnectionCloseCallback on_connection_close_callback =
                     OnConnectionCloseCallback());

  // DEPRECATED: Creates a NetworkContext that simply wraps a consumer-provided
  // URLRequestContext that is not owned by the NetworkContext.
  // TODO(mmenke):  Remove this constructor when the network service ships.
  NetworkContext(NetworkService* network_service,
                 mojo::PendingReceiver<mojom::NetworkContext> receiver,
                 net::URLRequestContext* url_request_context,
                 const std::vector<std::string>& cors_exempt_header_list);

  ~NetworkContext() override;

  // Sets a global CertVerifier to use when initializing all profiles.
  static void SetCertVerifierForTesting(net::CertVerifier* cert_verifier);

  // Whether the NetworkContext should be used for certain URL fetches of
  // global scope (validating certs on some platforms, DNS over HTTPS).
  // May only be set to true the first NetworkContext created using the
  // NetworkService.  Destroying the NetworkContext with this set to true
  // will destroy all other NetworkContexts.
  bool IsPrimaryNetworkContext() const;

  net::URLRequestContext* url_request_context() { return url_request_context_; }

  NetworkService* network_service() { return network_service_; }

  mojom::NetworkContextClient* client() {
    return client_.is_bound() ? client_.get() : nullptr;
  }

  ResourceScheduler* resource_scheduler() { return resource_scheduler_.get(); }

  CookieManager* cookie_manager() { return cookie_manager_.get(); }

  const std::unordered_set<std::string>& cors_exempt_header_list() const {
    return cors_exempt_header_list_;
  }

#if defined(OS_ANDROID)
  base::android::ApplicationStatusListener* app_status_listener() const {
    return app_status_listener_.get();
  }
#endif

  // Creates a URLLoaderFactory with a ResourceSchedulerClient specified. This
  // is used to reuse the existing ResourceSchedulerClient for cloned
  // URLLoaderFactory.
  void CreateURLLoaderFactory(
      mojo::PendingReceiver<mojom::URLLoaderFactory> receiver,
      mojom::URLLoaderFactoryParamsPtr params,
      scoped_refptr<ResourceSchedulerClient> resource_scheduler_client);

  // Enables DoH probes to be sent using this context whenever the DNS
  // configuration contains DoH servers.
  void ActivateDohProbes();

  // mojom::NetworkContext implementation:
  void SetClient(
      mojo::PendingRemote<mojom::NetworkContextClient> client) override;
  void CreateURLLoaderFactory(
      mojo::PendingReceiver<mojom::URLLoaderFactory> receiver,
      mojom::URLLoaderFactoryParamsPtr params) override;
  void ResetURLLoaderFactories() override;
  void GetCookieManager(
      mojo::PendingReceiver<mojom::CookieManager> receiver) override;
  void GetRestrictedCookieManager(
      mojo::PendingReceiver<mojom::RestrictedCookieManager> receiver,
      mojom::RestrictedCookieManagerRole role,
      const url::Origin& origin,
      const net::SiteForCookies& site_for_cookies,
      const url::Origin& top_frame_origin,
      bool is_service_worker,
      int32_t process_id,
      int32_t routing_id) override;
  void ClearNetworkingHistorySince(
      base::Time time,
      base::OnceClosure completion_callback) override;
  void ClearHttpCache(base::Time start_time,
                      base::Time end_time,
                      mojom::ClearDataFilterPtr filter,
                      ClearHttpCacheCallback callback) override;
  void ComputeHttpCacheSize(base::Time start_time,
                            base::Time end_time,
                            ComputeHttpCacheSizeCallback callback) override;
  void NotifyExternalCacheHit(const GURL& url,
                              const std::string& http_method,
                              const net::NetworkIsolationKey& key) override;
  void ClearHostCache(mojom::ClearDataFilterPtr filter,
                      ClearHostCacheCallback callback) override;
  void ClearHttpAuthCache(base::Time start_time,
                          ClearHttpAuthCacheCallback callback) override;
  void ClearReportingCacheReports(
      mojom::ClearDataFilterPtr filter,
      ClearReportingCacheReportsCallback callback) override;
  void ClearReportingCacheClients(
      mojom::ClearDataFilterPtr filter,
      ClearReportingCacheClientsCallback callback) override;
  void ClearNetworkErrorLogging(
      mojom::ClearDataFilterPtr filter,
      ClearNetworkErrorLoggingCallback callback) override;
  void ClearDomainReliability(mojom::ClearDataFilterPtr filter,
                              DomainReliabilityClearMode mode,
                              ClearDomainReliabilityCallback callback) override;
  void GetDomainReliabilityJSON(
      GetDomainReliabilityJSONCallback callback) override;
  void CloseAllConnections(CloseAllConnectionsCallback callback) override;
  void CloseIdleConnections(CloseIdleConnectionsCallback callback) override;
  void SetNetworkConditions(const base::UnguessableToken& throttling_profile_id,
                            mojom::NetworkConditionsPtr conditions) override;
  void SetAcceptLanguage(const std::string& new_accept_language) override;
  void SetEnableReferrers(bool enable_referrers) override;
#if defined(OS_CHROMEOS)
  void UpdateAdditionalCertificates(
      mojom::AdditionalCertificatesPtr additional_certificates) override;
#endif
#if BUILDFLAG(IS_CT_SUPPORTED)
  void SetCTPolicy(
      const std::vector<std::string>& required_hosts,
      const std::vector<std::string>& excluded_hosts,
      const std::vector<std::string>& excluded_spkis,
      const std::vector<std::string>& excluded_legacy_spkis) override;
  void AddExpectCT(const std::string& domain,
                   base::Time expiry,
                   bool enforce,
                   const GURL& report_uri,
                   AddExpectCTCallback callback) override;
  void SetExpectCTTestReport(const GURL& report_uri,
                             SetExpectCTTestReportCallback callback) override;
  void GetExpectCTState(const std::string& domain,
                        GetExpectCTStateCallback callback) override;
#endif  // BUILDFLAG(IS_CT_SUPPORTED)
  void CreateUDPSocket(
      mojo::PendingReceiver<mojom::UDPSocket> receiver,
      mojo::PendingRemote<mojom::UDPSocketListener> listener) override;
  void CreateTCPServerSocket(
      const net::IPEndPoint& local_addr,
      uint32_t backlog,
      const net::MutableNetworkTrafficAnnotationTag& traffic_annotation,
      mojo::PendingReceiver<mojom::TCPServerSocket> receiver,
      CreateTCPServerSocketCallback callback) override;
  void CreateTCPConnectedSocket(
      const base::Optional<net::IPEndPoint>& local_addr,
      const net::AddressList& remote_addr_list,
      mojom::TCPConnectedSocketOptionsPtr tcp_connected_socket_options,
      const net::MutableNetworkTrafficAnnotationTag& traffic_annotation,
      mojo::PendingReceiver<mojom::TCPConnectedSocket> receiver,
      mojo::PendingRemote<mojom::SocketObserver> observer,
      CreateTCPConnectedSocketCallback callback) override;
  void CreateTCPBoundSocket(
      const net::IPEndPoint& local_addr,
      const net::MutableNetworkTrafficAnnotationTag& traffic_annotation,
      mojo::PendingReceiver<mojom::TCPBoundSocket> receiver,
      CreateTCPBoundSocketCallback callback) override;
  void CreateProxyResolvingSocketFactory(
      mojo::PendingReceiver<mojom::ProxyResolvingSocketFactory> receiver)
      override;
  void LookUpProxyForURL(const GURL& url,
                         const net::NetworkIsolationKey& network_isolation_key,
                         mojo::PendingRemote<mojom::ProxyLookupClient>
                             proxy_lookup_client) override;
  void ForceReloadProxyConfig(ForceReloadProxyConfigCallback callback) override;
  void ClearBadProxiesCache(ClearBadProxiesCacheCallback callback) override;
  void CreateWebSocket(
      const GURL& url,
      const std::vector<std::string>& requested_protocols,
      const net::SiteForCookies& site_for_cookies,
      const net::NetworkIsolationKey& network_isolation_key,
      std::vector<mojom::HttpHeaderPtr> additional_headers,
      int32_t process_id,
      int32_t render_frame_id,
      const url::Origin& origin,
      uint32_t options,
      mojo::PendingRemote<mojom::WebSocketHandshakeClient> handshake_client,
      mojo::PendingRemote<mojom::AuthenticationHandler> auth_handler,
      mojo::PendingRemote<mojom::TrustedHeaderClient> header_client) override;
  void CreateQuicTransport(
      const GURL& url,
      const url::Origin& origin,
      const net::NetworkIsolationKey& network_isolation_key,
      mojo::PendingRemote<mojom::QuicTransportHandshakeClient> handshake_client)
      override;
  void CreateNetLogExporter(
      mojo::PendingReceiver<mojom::NetLogExporter> receiver) override;
  void ResolveHost(
      const net::HostPortPair& host,
      const net::NetworkIsolationKey& network_isolation_key,
      mojom::ResolveHostParametersPtr optional_parameters,
      mojo::PendingRemote<mojom::ResolveHostClient> response_client) override;
  void CreateHostResolver(
      const base::Optional<net::DnsConfigOverrides>& config_overrides,
      mojo::PendingReceiver<mojom::HostResolver> receiver) override;
  void VerifyCertForSignedExchange(
      const scoped_refptr<net::X509Certificate>& certificate,
      const GURL& url,
      const std::string& ocsp_result,
      const std::string& sct_list,
      VerifyCertForSignedExchangeCallback callback) override;
  void AddHSTS(const std::string& host,
               base::Time expiry,
               bool include_subdomains,
               AddHSTSCallback callback) override;
  void IsHSTSActiveForHost(const std::string& host,
                           IsHSTSActiveForHostCallback callback) override;
  void GetHSTSState(const std::string& domain,
                    GetHSTSStateCallback callback) override;
  void DeleteDynamicDataForHost(
      const std::string& host,
      DeleteDynamicDataForHostCallback callback) override;
  void SetCorsOriginAccessListsForOrigin(
      const url::Origin& source_origin,
      std::vector<mojom::CorsOriginPatternPtr> allow_patterns,
      std::vector<mojom::CorsOriginPatternPtr> block_patterns,
      SetCorsOriginAccessListsForOriginCallback callback) override;
  void SetCorsExtraSafelistedRequestHeaderNames(
      const std::vector<std::string>&
          cors_extra_safelisted_request_header_names) override;
  void EnableStaticKeyPinningForTesting(
      EnableStaticKeyPinningForTestingCallback callback) override;
  void SetFailingHttpTransactionForTesting(
      int32_t rv,
      SetFailingHttpTransactionForTestingCallback callback) override;
  void VerifyCertificateForTesting(
      const scoped_refptr<net::X509Certificate>& certificate,
      const std::string& hostname,
      const std::string& ocsp_response,
      const std::string& sct_list,
      VerifyCertificateForTestingCallback callback) override;
  void PreconnectSockets(
      uint32_t num_streams,
      const GURL& url,
      bool allow_credentials,
      const net::NetworkIsolationKey& network_isolation_key) override;
  void CreateP2PSocketManager(
      mojo::PendingRemote<mojom::P2PTrustedSocketManagerClient> client,
      mojo::PendingReceiver<mojom::P2PTrustedSocketManager>
          trusted_socket_manager,
      mojo::PendingReceiver<mojom::P2PSocketManager> socket_manager_receiver)
      override;
  void CreateMdnsResponder(
      mojo::PendingReceiver<mojom::MdnsResponder> responder_receiver) override;
  void QueueReport(const std::string& type,
                   const std::string& group,
                   const GURL& url,
                   const base::Optional<std::string>& user_agent,
                   base::Value body) override;
  void QueueSignedExchangeReport(
      mojom::SignedExchangeReportPtr report) override;
  void AddDomainReliabilityContextForTesting(
      const GURL& origin,
      const GURL& upload_url,
      AddDomainReliabilityContextForTestingCallback callback) override;
  void ForceDomainReliabilityUploadsForTesting(
      ForceDomainReliabilityUploadsForTestingCallback callback) override;
  void SetSplitAuthCacheByNetworkIsolationKey(
      bool split_auth_cache_by_network_isolation_key) override;
  void SaveHttpAuthCacheProxyEntries(
      SaveHttpAuthCacheProxyEntriesCallback callback) override;
  void LoadHttpAuthCacheProxyEntries(
      const base::UnguessableToken& cache_key,
      LoadHttpAuthCacheProxyEntriesCallback callback) override;
  void AddAuthCacheEntry(const net::AuthChallengeInfo& challenge,
                         const net::NetworkIsolationKey& network_isolation_key,
                         const net::AuthCredentials& credentials,
                         AddAuthCacheEntryCallback callback) override;
  // TODO(mmenke): Rename this method and update Mojo docs to make it clear this
  // doesn't give proxy auth credentials.
  void LookupServerBasicAuthCredentials(
      const GURL& url,
      const net::NetworkIsolationKey& network_isolation_key,
      LookupServerBasicAuthCredentialsCallback callback) override;
  void GetOriginPolicyManager(
      mojo::PendingReceiver<mojom::OriginPolicyManager> receiver) override;

  // Destroys |request| when a proxy lookup completes.
  void OnProxyLookupComplete(ProxyLookupRequest* proxy_lookup_request);

  // Disables use of QUIC by the NetworkContext.
  void DisableQuic();

  // Destroys the specified factory. Called by the factory itself when it has
  // no open pipes.
  void DestroyURLLoaderFactory(cors::CorsURLLoaderFactory* url_loader_factory);

  // Removes |transport| and destroys it.
  void Remove(QuicTransport* transport);

  // The following methods are used to track the number of requests per process
  // and ensure it doesn't go over a reasonable limit.
  void LoaderCreated(uint32_t process_id);
  void LoaderDestroyed(uint32_t process_id);
  bool CanCreateLoader(uint32_t process_id);

  void set_max_loaders_per_process_for_testing(uint32_t count) {
    max_loaders_per_process_ = count;
  }

  size_t GetNumOutstandingResolveHostRequestsForTesting() const;

  size_t pending_proxy_lookup_requests_for_testing() const {
    return proxy_lookup_requests_.size();
  }

  NetworkServiceProxyDelegate* proxy_delegate() const {
    return proxy_delegate_;
  }

  void set_network_qualities_pref_delegate_for_testing(
      std::unique_ptr<NetworkQualitiesPrefDelegate>
          network_qualities_pref_delegate) {
    network_qualities_pref_delegate_ =
        std::move(network_qualities_pref_delegate);
  }

  cors::PreflightController* cors_preflight_controller() {
    return &cors_preflight_controller_;
  }

  // Returns true if reports should unconditionally be sent without first
  // consulting NetworkContextClient.OnCanSendReportingReports()
  bool SkipReportingPermissionCheck() const;

  // Creates a new url loader factory bound to this network context. For use
  // inside the network service.
  void CreateUrlLoaderFactoryForNetworkService(
      mojo::PendingReceiver<mojom::URLLoaderFactory>
          url_loader_factory_pending_receiver);

  mojom::OriginPolicyManager* origin_policy_manager() const {
    return origin_policy_manager_.get();
  }

  domain_reliability::DomainReliabilityMonitor* domain_reliability_monitor() {
    return domain_reliability_monitor_.get();
  }

  bool IsCorsEnabled() const { return cors_enabled_; }

  // The http_auth_dynamic_params_ would be used to populate
  // the |http_auth_merged_preferences| of the given NetworkContext.
  void OnHttpAuthDynamicParamsChanged(
      const mojom::HttpAuthDynamicParams*
          http_auth_dynamic_network_service_params);

  const net::HttpAuthPreferences* GetHttpAuthPreferences() const;

  size_t NumOpenQuicTransports() const;

  size_t num_url_loader_factories_for_testing() const {
    return url_loader_factories_.size();
  }

 private:
  URLRequestContextOwner MakeURLRequestContext();

  // Invoked when the HTTP cache was cleared. Invokes |callback|.
  void OnHttpCacheCleared(ClearHttpCacheCallback callback,
                          HttpCacheDataRemover* remover);

  void OnHostResolverShutdown(HostResolver* resolver);

  // Invoked when the computation for ComputeHttpCacheSize() has been completed,
  // to report result to user via |callback| and clean things up.
  void OnHttpCacheSizeComputed(ComputeHttpCacheSizeCallback callback,
                               HttpCacheDataCounter* counter,
                               bool is_upper_limit,
                               int64_t result_or_error);

  // On connection errors the NetworkContext destroys itself.
  void OnConnectionError();

  GURL GetHSTSRedirect(const GURL& original_url);

  void DestroySocketManager(P2PSocketManager* socket_manager);

  void CanUploadDomainReliability(const GURL& origin,
                                  base::OnceCallback<void(bool)> callback);

  void OnCertVerifyForSignedExchangeComplete(int cert_verify_id, int result);

#if defined(OS_CHROMEOS)
  void TrustAnchorUsed();

  scoped_refptr<net::CertVerifyProc> CreateCertVerifyProcForUser(
      scoped_refptr<net::CertNetFetcher> net_fetcher,
      crypto::ScopedPK11Slot user_public_slot);

  scoped_refptr<net::CertVerifyProc> CreateCertVerifyProcWithoutUserSlots(
      scoped_refptr<net::CertNetFetcher> net_fetcher);
#endif

#if BUILDFLAG(IS_CT_SUPPORTED)
  void OnSetExpectCTTestReportSuccess();

  void LazyCreateExpectCTReporter(net::URLRequestContext* url_request_context);

  void OnSetExpectCTTestReportFailure();
#endif  // BUILDFLAG(IS_CT_SUPPORTED)

  void InitializeCorsParams();

  NetworkService* const network_service_;

  mojo::Remote<mojom::NetworkContextClient> client_;

  std::unique_ptr<ResourceScheduler> resource_scheduler_;

  // Holds owning pointer to |url_request_context_|. Will contain a nullptr for
  // |url_request_context| when the NetworkContextImpl doesn't own its own
  // URLRequestContext.
  URLRequestContextOwner url_request_context_owner_;

  net::URLRequestContext* url_request_context_;

  // Owned by URLRequestContext.
  NetworkServiceNetworkDelegate* network_delegate_ = nullptr;

  mojom::NetworkContextParamsPtr params_;

  // If non-null, called when the mojo pipe for the NetworkContext is closed.
  OnConnectionCloseCallback on_connection_close_callback_;

#if defined(OS_ANDROID)
  std::unique_ptr<base::android::ApplicationStatusListener>
      app_status_listener_;
#endif

  mojo::Receiver<mojom::NetworkContext> receiver_;

  std::unique_ptr<CookieManager> cookie_manager_;

  std::unique_ptr<SocketFactory> socket_factory_;

  mojo::UniqueReceiverSet<mojom::ProxyResolvingSocketFactory>
      proxy_resolving_socket_factories_;

#if !defined(OS_IOS)
  std::unique_ptr<WebSocketFactory> websocket_factory_;
#endif  // !defined(OS_IOS)

  // These must be below the URLRequestContext, so they're destroyed before it
  // is.
  std::vector<std::unique_ptr<HttpCacheDataRemover>> http_cache_data_removers_;
  std::vector<std::unique_ptr<HttpCacheDataCounter>> http_cache_data_counters_;
  std::set<std::unique_ptr<ProxyLookupRequest>, base::UniquePtrComparator>
      proxy_lookup_requests_;

  // This must be below |url_request_context_| so that the URLRequestContext
  // outlives all the URLLoaderFactories and URLLoaders that depend on it.
  std::set<std::unique_ptr<cors::CorsURLLoaderFactory>,
           base::UniquePtrComparator>
      url_loader_factories_;

  std::set<std::unique_ptr<QuicTransport>, base::UniquePtrComparator>
      quic_transports_;

  // A count of outstanding requests per initiating process.
  std::map<uint32_t, uint32_t> loader_count_per_process_;

  static constexpr uint32_t kMaxOutstandingRequestsPerProcess = 2700;
  uint32_t max_loaders_per_process_ = kMaxOutstandingRequestsPerProcess;

  base::flat_map<P2PSocketManager*, std::unique_ptr<P2PSocketManager>>
      socket_managers_;

#if BUILDFLAG(ENABLE_MDNS)
  std::unique_ptr<MdnsResponderManager> mdns_responder_manager_;
#endif  // BUILDFLAG(ENABLE_MDNS)

  mojo::UniqueReceiverSet<mojom::NetLogExporter> net_log_exporter_receivers_;

  // Ordering: this must be after |cookie_manager_| since it points to its
  // CookieSettings object.
  mojo::UniqueReceiverSet<mojom::RestrictedCookieManager>
      restricted_cookie_manager_receivers_;

  int current_resource_scheduler_client_id_ = 0;

  // Owned by the URLRequestContext
  net::StaticHttpUserAgentSettings* user_agent_settings_ = nullptr;

  // Pointed to by the TransportSecurityState (owned by the
  // URLRequestContext), and must be disconnected from it before it's destroyed.
  std::unique_ptr<net::ReportSender> certificate_report_sender_;

#if BUILDFLAG(IS_CT_SUPPORTED)
  std::unique_ptr<ExpectCTReporter> expect_ct_reporter_;

  std::unique_ptr<certificate_transparency::ChromeRequireCTDelegate>
      require_ct_delegate_;

  std::queue<SetExpectCTTestReportCallback>
      outstanding_set_expect_ct_callbacks_;
#endif  // BUILDFLAG(IS_CT_SUPPORTED)

#if defined(OS_CHROMEOS)
  CertVerifierWithTrustAnchors* cert_verifier_with_trust_anchors_ = nullptr;
  // Additional certificates made available to NSS cert validation as temporary
  // certificates.
  std::unique_ptr<network::NSSTempCertsCacheChromeOS> nss_temp_certs_cache_;
#endif

  // CertNetFetcher used by the context's CertVerifier. May be nullptr if
  // CertNetFetcher is not used by the current platform.
  scoped_refptr<net::CertNetFetcherURLRequest> cert_net_fetcher_;

  // Created on-demand. Null if unused.
  std::unique_ptr<HostResolver> internal_host_resolver_;
  // Map values set to non-null only if that HostResolver has its own private
  // internal net::HostResolver.
  std::map<std::unique_ptr<HostResolver>,
           std::unique_ptr<net::HostResolver>,
           base::UniquePtrComparator>
      host_resolvers_;
  std::unique_ptr<net::HostResolver::ProbeRequest> doh_probes_request_;

  NetworkServiceProxyDelegate* proxy_delegate_ = nullptr;

  // Used for Signed Exchange certificate verification.
  int next_cert_verify_id_ = 0;
  struct PendingCertVerify {
    PendingCertVerify();
    ~PendingCertVerify();
    // CertVerifyResult must be freed after the Request has been destructed.
    // So |result| must be written before |request|.
    std::unique_ptr<net::CertVerifyResult> result;
    std::unique_ptr<net::CertVerifier::Request> request;
    VerifyCertForSignedExchangeCallback callback;
    scoped_refptr<net::X509Certificate> certificate;
    GURL url;
    std::string ocsp_result;
    std::string sct_list;
  };
  std::map<int, std::unique_ptr<PendingCertVerify>> cert_verifier_requests_;

  // Manages allowed origin access lists.
  cors::OriginAccessList cors_origin_access_list_;

  // Manages header keys that are allowed to be used in
  // ResourceRequest::cors_exempt_headers.
  std::unordered_set<std::string> cors_exempt_header_list_;

  // Manages CORS preflight requests and its cache.
  cors::PreflightController cors_preflight_controller_;

  // Manages if OOR-CORS is enabled.
  bool cors_enabled_ = false;

  std::unique_ptr<NetworkQualitiesPrefDelegate>
      network_qualities_pref_delegate_;

  std::unique_ptr<domain_reliability::DomainReliabilityMonitor>
      domain_reliability_monitor_;

  std::unique_ptr<OriginPolicyManager> origin_policy_manager_;

  // Each network context holds its own HttpAuthPreferences.
  // The dynamic preferences of |NetworkService| and the static
  // preferences from |NetworkContext| would be merged to
  // `http_auth_merged_preferences_` which would then be used to create
  // HttpAuthHandle via |NetworkContext::CreateHttpAuthHandlerFactory|.
  net::HttpAuthPreferences http_auth_merged_preferences_;

  DISALLOW_COPY_AND_ASSIGN(NetworkContext);
};

}  // namespace network

#endif  // SERVICES_NETWORK_NETWORK_CONTEXT_H_
