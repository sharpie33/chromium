// Copyright (c) 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/quic/quic_proxy_client_socket.h"

#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/memory/ptr_util.h"
#include "base/run_loop.h"
#include "base/strings/utf_string_conversions.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/time/default_tick_clock.h"
#include "net/dns/mock_host_resolver.h"
#include "net/http/http_auth_cache.h"
#include "net/http/http_auth_handler_factory.h"
#include "net/http/http_response_headers.h"
#include "net/http/transport_security_state.h"
#include "net/log/test_net_log.h"
#include "net/log/test_net_log_util.h"
#include "net/quic/address_utils.h"
#include "net/quic/crypto/proof_verifier_chromium.h"
#include "net/quic/mock_crypto_client_stream_factory.h"
#include "net/quic/mock_quic_data.h"
#include "net/quic/quic_chromium_alarm_factory.h"
#include "net/quic/quic_chromium_client_session.h"
#include "net/quic/quic_chromium_connection_helper.h"
#include "net/quic/quic_chromium_packet_writer.h"
#include "net/quic/quic_crypto_client_config_handle.h"
#include "net/quic/quic_http_utils.h"
#include "net/quic/quic_server_info.h"
#include "net/quic/quic_stream_factory.h"
#include "net/quic/quic_test_packet_maker.h"
#include "net/quic/test_quic_crypto_client_config_handle.h"
#include "net/quic/test_task_runner.h"
#include "net/socket/socket_test_util.h"
#include "net/test/cert_test_util.h"
#include "net/test/gtest_util.h"
#include "net/test/test_data_directory.h"
#include "net/test/test_with_task_environment.h"
#include "net/third_party/quiche/src/quic/core/crypto/null_encrypter.h"
#include "net/third_party/quiche/src/quic/core/quic_utils.h"
#include "net/third_party/quiche/src/quic/test_tools/crypto_test_utils.h"
#include "net/third_party/quiche/src/quic/test_tools/mock_clock.h"
#include "net/third_party/quiche/src/quic/test_tools/mock_random.h"
#include "net/third_party/quiche/src/quic/test_tools/qpack/qpack_test_utils.h"
#include "net/third_party/quiche/src/quic/test_tools/quic_connection_peer.h"
#include "net/third_party/quiche/src/quic/test_tools/quic_test_utils.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

using testing::_;
using testing::AnyNumber;
using testing::Return;

namespace net {
namespace test {

namespace {

static const char kOriginHost[] = "www.google.com";
static const int kOriginPort = 443;
static const char kProxyUrl[] = "https://myproxy:6121/";
static const char kProxyHost[] = "myproxy";
static const int kProxyPort = 6121;
static const char kUserAgent[] = "Mozilla/1.0";
static const char kRedirectUrl[] = "https://example.com/";

static const char kMsg1[] = "\0hello!\xff";
static const int kLen1 = 8;
static const char kMsg2[] = "\0a2345678\0";
static const int kLen2 = 10;
static const char kMsg3[] = "bye!";
static const int kLen3 = 4;
static const char kMsg33[] = "bye!bye!";
static const int kLen33 = kLen3 + kLen3;
static const char kMsg333[] = "bye!bye!bye!";
static const int kLen333 = kLen3 + kLen3 + kLen3;

struct TestParams {
  quic::ParsedQuicVersion version;
  bool client_headers_include_h2_stream_dependency;
};

// Used by ::testing::PrintToStringParamName().
std::string PrintToString(const TestParams& p) {
  return quiche::QuicheStrCat(
      ParsedQuicVersionToString(p.version), "_",
      (p.client_headers_include_h2_stream_dependency ? "" : "No"),
      "Dependency");
}

std::vector<TestParams> GetTestParams() {
  std::vector<TestParams> params;
  quic::ParsedQuicVersionVector all_supported_versions =
      quic::AllSupportedVersions();
  for (const auto& version : all_supported_versions) {
    params.push_back(TestParams{version, false});
    params.push_back(TestParams{version, true});
  }
  return params;
}

}  // anonymous namespace

class QuicProxyClientSocketTest : public ::testing::TestWithParam<TestParams>,
                                  public WithTaskEnvironment {
 protected:
  static const bool kFin = true;
  static const bool kIncludeVersion = true;
  static const bool kIncludeDiversificationNonce = true;
  static const bool kIncludeCongestionFeedback = true;
  static const bool kSendFeedback = true;

  static size_t GetStreamFrameDataLengthFromPacketLength(
      quic::QuicByteCount packet_length,
      quic::ParsedQuicVersion version,
      bool include_version,
      bool include_diversification_nonce,
      quic::QuicConnectionIdLength connection_id_length,
      quic::QuicPacketNumberLength packet_number_length,
      quic::QuicStreamOffset offset) {
    quic::QuicVariableLengthIntegerLength retry_token_length_length =
        quic::VARIABLE_LENGTH_INTEGER_LENGTH_0;
    quic::QuicVariableLengthIntegerLength length_length =
        quic::QuicVersionHasLongHeaderLengths(version.transport_version) &&
                include_version
            ? quic::VARIABLE_LENGTH_INTEGER_LENGTH_2
            : quic::VARIABLE_LENGTH_INTEGER_LENGTH_0;
    size_t min_data_length = 1;
    size_t min_packet_length =
        quic::NullEncrypter(quic::Perspective::IS_CLIENT)
            .GetCiphertextSize(min_data_length) +
        quic::QuicPacketCreator::StreamFramePacketOverhead(
            version.transport_version, quic::PACKET_8BYTE_CONNECTION_ID,
            quic::PACKET_0BYTE_CONNECTION_ID, include_version,
            include_diversification_nonce, packet_number_length,
            retry_token_length_length, length_length, offset);

    DCHECK(packet_length >= min_packet_length);
    return min_data_length + packet_length - min_packet_length;
  }

  QuicProxyClientSocketTest()
      : version_(GetParam().version),
        client_data_stream_id1_(
            quic::VersionUsesHttp3(version_.transport_version)
                ? quic::QuicUtils::GetFirstBidirectionalStreamId(
                      version_.transport_version,
                      quic::Perspective::IS_CLIENT)
                : quic::QuicUtils::GetFirstBidirectionalStreamId(
                      version_.transport_version,
                      quic::Perspective::IS_CLIENT) +
                      quic::QuicUtils::StreamIdDelta(
                          version_.transport_version)),
        client_headers_include_h2_stream_dependency_(
            GetParam().client_headers_include_h2_stream_dependency),
        mock_quic_data_(version_),
        crypto_config_(
            quic::test::crypto_test_utils::ProofVerifierForTesting()),
        connection_id_(quic::test::TestConnectionId(2)),
        client_maker_(version_,
                      connection_id_,
                      &clock_,
                      kProxyHost,
                      quic::Perspective::IS_CLIENT,
                      client_headers_include_h2_stream_dependency_),
        server_maker_(version_,
                      connection_id_,
                      &clock_,
                      kProxyHost,
                      quic::Perspective::IS_SERVER,
                      false),
        random_generator_(0),
        user_agent_(kUserAgent),
        proxy_host_port_(kProxyHost, kProxyPort),
        endpoint_host_port_(kOriginHost, kOriginPort),
        http_auth_cache_(
            false /* key_server_entries_by_network_isolation_key */),
        host_resolver_(new MockCachingHostResolver()),
        http_auth_handler_factory_(HttpAuthHandlerFactory::CreateDefault()) {
    FLAGS_quic_disable_http3_settings_grease_randomness = true;
    IPAddress ip(192, 0, 2, 33);
    peer_addr_ = IPEndPoint(ip, 443);
    clock_.AdvanceTime(quic::QuicTime::Delta::FromMilliseconds(20));
    if (version_.handshake_protocol == quic::PROTOCOL_TLS1_3) {
      quic::QuicEnableVersion(version_);
    }
  }

  void SetUp() override {}

  void TearDown() override {
    sock_.reset();
    EXPECT_TRUE(mock_quic_data_.AllReadDataConsumed());
    EXPECT_TRUE(mock_quic_data_.AllWriteDataConsumed());
  }

  void Initialize() {
    std::unique_ptr<MockUDPClientSocket> socket(new MockUDPClientSocket(
        mock_quic_data_.InitializeAndGetSequencedSocketData(),
        net_log_.bound().net_log()));
    socket->Connect(peer_addr_);
    runner_ = new TestTaskRunner(&clock_);
    send_algorithm_ = new quic::test::MockSendAlgorithm();
    EXPECT_CALL(*send_algorithm_, InRecovery()).WillRepeatedly(Return(false));
    EXPECT_CALL(*send_algorithm_, InSlowStart()).WillRepeatedly(Return(false));
    EXPECT_CALL(*send_algorithm_, OnPacketSent(_, _, _, _, _))
        .Times(testing::AtLeast(1));
    EXPECT_CALL(*send_algorithm_, GetCongestionWindow())
        .WillRepeatedly(Return(quic::kMaxOutgoingPacketSize));
    EXPECT_CALL(*send_algorithm_, PacingRate(_))
        .WillRepeatedly(Return(quic::QuicBandwidth::Zero()));
    EXPECT_CALL(*send_algorithm_, CanSend(_)).WillRepeatedly(Return(true));
    EXPECT_CALL(*send_algorithm_, BandwidthEstimate())
        .WillRepeatedly(Return(quic::QuicBandwidth::Zero()));
    EXPECT_CALL(*send_algorithm_, SetFromConfig(_, _)).Times(AnyNumber());
    EXPECT_CALL(*send_algorithm_, OnApplicationLimited(_)).Times(AnyNumber());
    EXPECT_CALL(*send_algorithm_, GetCongestionControlType())
        .Times(AnyNumber());
    helper_.reset(
        new QuicChromiumConnectionHelper(&clock_, &random_generator_));
    alarm_factory_.reset(new QuicChromiumAlarmFactory(runner_.get(), &clock_));

    QuicChromiumPacketWriter* writer = new QuicChromiumPacketWriter(
        socket.get(), base::ThreadTaskRunnerHandle::Get().get());
    quic::QuicConnection* connection = new quic::QuicConnection(
        connection_id_, net::ToQuicSocketAddress(peer_addr_), helper_.get(),
        alarm_factory_.get(), writer, true /* owns_writer */,
        quic::Perspective::IS_CLIENT, quic::test::SupportedVersions(version_));
    connection->set_visitor(&visitor_);
    quic::test::QuicConnectionPeer::SetSendAlgorithm(connection,
                                                     send_algorithm_);

    // Load a certificate that is valid for *.example.org
    scoped_refptr<X509Certificate> test_cert(
        ImportCertFromFile(GetTestCertsDirectory(), "wildcard.pem"));
    EXPECT_TRUE(test_cert.get());

    verify_details_.cert_verify_result.verified_cert = test_cert;
    verify_details_.cert_verify_result.is_issued_by_known_root = true;
    crypto_client_stream_factory_.AddProofVerifyDetails(&verify_details_);

    base::TimeTicks dns_end = base::TimeTicks::Now();
    base::TimeTicks dns_start = dns_end - base::TimeDelta::FromMilliseconds(1);

    session_.reset(new QuicChromiumClientSession(
        connection, std::move(socket),
        /*stream_factory=*/nullptr, &crypto_client_stream_factory_, &clock_,
        &transport_security_state_, /*ssl_config_service=*/nullptr,
        base::WrapUnique(static_cast<QuicServerInfo*>(nullptr)),
        QuicSessionKey("mail.example.org", 80, PRIVACY_MODE_DISABLED,
                       SocketTag(), NetworkIsolationKey(),
                       false /* disable_secure_dns */),
        /*require_confirmation=*/false,
        /*max_allowed_push_id=*/0,
        /*migrate_session_early_v2=*/false,
        /*migrate_session_on_network_change_v2=*/false,
        /*default_network=*/NetworkChangeNotifier::kInvalidNetworkHandle,
        quic::QuicTime::Delta::FromMilliseconds(
            kDefaultRetransmittableOnWireTimeout.InMilliseconds()),
        /*migrate_idle_session=*/true, /*allow_port_migration=*/false,
        kDefaultIdleSessionMigrationPeriod, kMaxTimeOnNonDefaultNetwork,
        kMaxMigrationsToNonDefaultNetworkOnWriteError,
        kMaxMigrationsToNonDefaultNetworkOnPathDegrading,
        kQuicYieldAfterPacketsRead,
        quic::QuicTime::Delta::FromMilliseconds(
            kQuicYieldAfterDurationMilliseconds),
        /*go_away_on_path_degrading*/ false,
        client_headers_include_h2_stream_dependency_, /*cert_verify_flags=*/0,
        quic::test::DefaultQuicConfig(),
        std::make_unique<TestQuicCryptoClientConfigHandle>(&crypto_config_),
        "CONNECTION_UNKNOWN", dns_start, dns_end, &push_promise_index_, nullptr,
        base::DefaultTickClock::GetInstance(),
        base::ThreadTaskRunnerHandle::Get().get(),
        /*socket_performance_watcher=*/nullptr, net_log_.bound().net_log()));

    writer->set_delegate(session_.get());

    session_handle_ =
        session_->CreateHandle(HostPortPair("mail.example.org", 80));

    session_->Initialize();

    // Blackhole QPACK decoder stream instead of constructing mock writes.
    if (VersionUsesHttp3(version_.transport_version)) {
      session_->qpack_decoder()->set_qpack_stream_sender_delegate(
          &noop_qpack_stream_sender_delegate_);
    }

    TestCompletionCallback callback;
    EXPECT_THAT(session_->CryptoConnect(callback.callback()), IsOk());
    EXPECT_TRUE(session_->OneRttKeysAvailable());

    EXPECT_THAT(session_handle_->RequestStream(true, callback.callback(),
                                               TRAFFIC_ANNOTATION_FOR_TESTS),
                IsOk());
    std::unique_ptr<QuicChromiumClientStream::Handle> stream_handle =
        session_handle_->ReleaseStream();
    EXPECT_TRUE(stream_handle->IsOpen());

    sock_.reset(new QuicProxyClientSocket(
        std::move(stream_handle), std::move(session_handle_), user_agent_,
        endpoint_host_port_, net_log_.bound(),
        new HttpAuthController(HttpAuth::AUTH_PROXY,
                               GURL("https://" + proxy_host_port_.ToString()),
                               NetworkIsolationKey(), &http_auth_cache_,
                               http_auth_handler_factory_.get(),
                               host_resolver_.get())));

    session_->StartReading();
  }

  void PopulateConnectRequestIR(spdy::SpdyHeaderBlock* block) {
    (*block)[":method"] = "CONNECT";
    (*block)[":authority"] = endpoint_host_port_.ToString();
    (*block)["user-agent"] = kUserAgent;
  }

  // Helper functions for constructing packets sent by the client

  std::unique_ptr<quic::QuicReceivedPacket> ConstructSettingsPacket(
      uint64_t packet_number) {
    return client_maker_.MakeInitialSettingsPacket(packet_number);
  }

  std::unique_ptr<quic::QuicReceivedPacket> ConstructAckAndRstOnlyPacket(
      uint64_t packet_number,
      quic::QuicRstStreamErrorCode error_code,
      uint64_t largest_received,
      uint64_t smallest_received,
      uint64_t least_unacked) {
    return client_maker_.MakeAckAndRstPacket(
        packet_number, !kIncludeVersion, client_data_stream_id1_, error_code,
        largest_received, smallest_received, least_unacked, kSendFeedback,
        /*include_stop_sending=*/false);
  }

  std::unique_ptr<quic::QuicReceivedPacket> ConstructAckAndRstPacket(
      uint64_t packet_number,
      quic::QuicRstStreamErrorCode error_code,
      uint64_t largest_received,
      uint64_t smallest_received,
      uint64_t least_unacked) {
    return client_maker_.MakeAckAndRstPacket(
        packet_number, !kIncludeVersion, client_data_stream_id1_, error_code,
        largest_received, smallest_received, least_unacked, kSendFeedback,
        /*include_stop_sending_if_v99=*/true);
  }

  std::unique_ptr<quic::QuicReceivedPacket> ConstructRstPacket(
      uint64_t packet_number,
      quic::QuicRstStreamErrorCode error_code) {
    return client_maker_.MakeRstPacket(packet_number, !kIncludeVersion,
                                       client_data_stream_id1_, error_code,
                                       /*include_stop_sending_if_v99=*/true);
  }

  std::unique_ptr<quic::QuicReceivedPacket> ConstructConnectRequestPacket(
      uint64_t packet_number,
      RequestPriority request_priority = LOWEST) {
    spdy::SpdyHeaderBlock block;
    PopulateConnectRequestIR(&block);
    return client_maker_.MakeRequestHeadersPacket(
        packet_number, client_data_stream_id1_, kIncludeVersion, !kFin,
        ConvertRequestPriorityToQuicPriority(request_priority),
        std::move(block), 0, nullptr);
  }

  std::unique_ptr<quic::QuicReceivedPacket> ConstructConnectAuthRequestPacket(
      uint64_t packet_number) {
    spdy::SpdyHeaderBlock block;
    PopulateConnectRequestIR(&block);
    block["proxy-authorization"] = "Basic Zm9vOmJhcg==";
    return client_maker_.MakeRequestHeadersPacket(
        packet_number, client_data_stream_id1_, kIncludeVersion, !kFin,
        ConvertRequestPriorityToQuicPriority(LOWEST), std::move(block), 0,
        nullptr);
  }

  std::unique_ptr<quic::QuicReceivedPacket> ConstructDataPacket(
      uint64_t packet_number,
      quiche::QuicheStringPiece data) {
    return client_maker_.MakeDataPacket(packet_number, client_data_stream_id1_,
                                        !kIncludeVersion, !kFin, data);
  }

  std::unique_ptr<quic::QuicReceivedPacket> ConstructAckAndDataPacket(
      uint64_t packet_number,
      uint64_t largest_received,
      uint64_t smallest_received,
      uint64_t least_unacked,
      quiche::QuicheStringPiece data) {
    return client_maker_.MakeAckAndDataPacket(
        packet_number, !kIncludeVersion, client_data_stream_id1_,
        largest_received, smallest_received, least_unacked, !kFin, data);
  }

  std::unique_ptr<quic::QuicReceivedPacket> ConstructAckPacket(
      uint64_t packet_number,
      uint64_t largest_received,
      uint64_t smallest_received,
      uint64_t least_unacked) {
    return client_maker_.MakeAckPacket(packet_number, largest_received,
                                       smallest_received, least_unacked,
                                       kSendFeedback);
  }

  // Helper functions for constructing packets sent by the server

  std::unique_ptr<quic::QuicReceivedPacket> ConstructServerRstPacket(
      uint64_t packet_number,
      quic::QuicRstStreamErrorCode error_code) {
    return server_maker_.MakeRstPacket(packet_number, !kIncludeVersion,
                                       client_data_stream_id1_, error_code,
                                       /*include_stop_sending_if_v99=*/true);
  }

  std::unique_ptr<quic::QuicReceivedPacket> ConstructServerDataPacket(
      uint64_t packet_number,
      quiche::QuicheStringPiece data) {
    return server_maker_.MakeDataPacket(packet_number, client_data_stream_id1_,
                                        !kIncludeVersion, !kFin, data);
  }

  std::unique_ptr<quic::QuicReceivedPacket> ConstructServerDataFinPacket(
      uint64_t packet_number,
      quiche::QuicheStringPiece data) {
    return server_maker_.MakeDataPacket(packet_number, client_data_stream_id1_,
                                        !kIncludeVersion, kFin, data);
  }

  std::unique_ptr<quic::QuicReceivedPacket> ConstructServerConnectReplyPacket(
      uint64_t packet_number,
      bool fin,
      size_t* header_length = nullptr) {
    spdy::SpdyHeaderBlock block;
    block[":status"] = "200";

    return server_maker_.MakeResponseHeadersPacket(
        packet_number, client_data_stream_id1_, !kIncludeVersion, fin,
        std::move(block), header_length);
  }

  std::unique_ptr<quic::QuicReceivedPacket>
  ConstructServerConnectAuthReplyPacket(uint64_t packet_number, bool fin) {
    spdy::SpdyHeaderBlock block;
    block[":status"] = "407";
    block["proxy-authenticate"] = "Basic realm=\"MyRealm1\"";
    return server_maker_.MakeResponseHeadersPacket(
        packet_number, client_data_stream_id1_, !kIncludeVersion, fin,
        std::move(block), nullptr);
  }

  std::unique_ptr<quic::QuicReceivedPacket>
  ConstructServerConnectRedirectReplyPacket(uint64_t packet_number, bool fin) {
    spdy::SpdyHeaderBlock block;
    block[":status"] = "302";
    block["location"] = kRedirectUrl;
    block["set-cookie"] = "foo=bar";
    return server_maker_.MakeResponseHeadersPacket(
        packet_number, client_data_stream_id1_, !kIncludeVersion, fin,
        std::move(block), nullptr);
  }

  std::unique_ptr<quic::QuicReceivedPacket>
  ConstructServerConnectErrorReplyPacket(uint64_t packet_number, bool fin) {
    spdy::SpdyHeaderBlock block;
    block[":status"] = "500";

    return server_maker_.MakeResponseHeadersPacket(
        packet_number, client_data_stream_id1_, !kIncludeVersion, fin,
        std::move(block), nullptr);
  }

  void AssertConnectSucceeds() {
    TestCompletionCallback callback;
    ASSERT_THAT(sock_->Connect(callback.callback()), IsError(ERR_IO_PENDING));
    ASSERT_THAT(callback.WaitForResult(), IsOk());
  }

  void AssertConnectFails(int result) {
    TestCompletionCallback callback;
    ASSERT_THAT(sock_->Connect(callback.callback()), IsError(ERR_IO_PENDING));
    ASSERT_EQ(result, callback.WaitForResult());
  }

  void ResumeAndRun() {
    // Run until the pause, if the provider isn't paused yet.
    SequencedSocketData* data = mock_quic_data_.GetSequencedSocketData();
    data->RunUntilPaused();
    data->Resume();
    base::RunLoop().RunUntilIdle();
  }

  void AssertWriteReturns(const char* data, int len, int rv) {
    scoped_refptr<IOBufferWithSize> buf =
        base::MakeRefCounted<IOBufferWithSize>(len);
    memcpy(buf->data(), data, len);
    EXPECT_EQ(rv,
              sock_->Write(buf.get(), buf->size(), write_callback_.callback(),
                           TRAFFIC_ANNOTATION_FOR_TESTS));
  }

  void AssertSyncWriteSucceeds(const char* data, int len) {
    scoped_refptr<IOBufferWithSize> buf =
        base::MakeRefCounted<IOBufferWithSize>(len);
    memcpy(buf->data(), data, len);
    EXPECT_EQ(len,
              sock_->Write(buf.get(), buf->size(), CompletionOnceCallback(),
                           TRAFFIC_ANNOTATION_FOR_TESTS));
  }

  void AssertSyncReadEquals(const char* data, int len) {
    scoped_refptr<IOBuffer> buf = base::MakeRefCounted<IOBuffer>(len);
    ASSERT_EQ(len, sock_->Read(buf.get(), len, CompletionOnceCallback()));
    ASSERT_EQ(std::string(data, len), std::string(buf->data(), len));
    ASSERT_TRUE(sock_->IsConnected());
  }

  void AssertAsyncReadEquals(const char* data, int len) {
    scoped_refptr<IOBuffer> buf = base::MakeRefCounted<IOBuffer>(len);
    ASSERT_EQ(ERR_IO_PENDING,
              sock_->Read(buf.get(), len, read_callback_.callback()));
    EXPECT_TRUE(sock_->IsConnected());

    ResumeAndRun();

    EXPECT_EQ(len, read_callback_.WaitForResult());
    EXPECT_TRUE(sock_->IsConnected());
    ASSERT_EQ(std::string(data, len), std::string(buf->data(), len));
  }

  void AssertReadStarts(const char* data, int len) {
    // Issue the read, which will be completed asynchronously.
    read_buf_ = base::MakeRefCounted<IOBuffer>(len);
    ASSERT_EQ(ERR_IO_PENDING,
              sock_->Read(read_buf_.get(), len, read_callback_.callback()));
    EXPECT_TRUE(sock_->IsConnected());
  }

  void AssertReadReturns(const char* data, int len) {
    EXPECT_TRUE(sock_->IsConnected());

    // Now the read will return.
    EXPECT_EQ(len, read_callback_.WaitForResult());
    ASSERT_EQ(std::string(data, len), std::string(read_buf_->data(), len));
  }

  std::string ConstructDataHeader(size_t body_len) {
    if (version_.transport_version != quic::QUIC_VERSION_99) {
      return "";
    }
    std::unique_ptr<char[]> buffer;
    auto header_length =
        quic::HttpEncoder::SerializeDataFrameHeader(body_len, &buffer);
    return std::string(buffer.get(), header_length);
  }

  RecordingBoundTestNetLog net_log_;
  QuicFlagSaver saver_;
  const quic::ParsedQuicVersion version_;
  const quic::QuicStreamId client_data_stream_id1_;
  const bool client_headers_include_h2_stream_dependency_;

  // order of destruction of these members matter
  quic::MockClock clock_;
  MockQuicData mock_quic_data_;
  std::unique_ptr<QuicChromiumConnectionHelper> helper_;
  std::unique_ptr<QuicChromiumClientSession> session_;
  std::unique_ptr<QuicChromiumClientSession::Handle> session_handle_;
  std::unique_ptr<QuicProxyClientSocket> sock_;

  quic::test::MockSendAlgorithm* send_algorithm_;
  scoped_refptr<TestTaskRunner> runner_;

  std::unique_ptr<QuicChromiumAlarmFactory> alarm_factory_;
  testing::StrictMock<quic::test::MockQuicConnectionVisitor> visitor_;
  TransportSecurityState transport_security_state_;
  quic::QuicCryptoClientConfig crypto_config_;
  quic::QuicClientPushPromiseIndex push_promise_index_;

  const quic::QuicConnectionId connection_id_;
  QuicTestPacketMaker client_maker_;
  QuicTestPacketMaker server_maker_;
  IPEndPoint peer_addr_;
  quic::test::MockRandom random_generator_;
  ProofVerifyDetailsChromium verify_details_;
  MockCryptoClientStreamFactory crypto_client_stream_factory_;

  std::string user_agent_;
  HostPortPair proxy_host_port_;
  HostPortPair endpoint_host_port_;
  HttpAuthCache http_auth_cache_;
  std::unique_ptr<MockHostResolverBase> host_resolver_;
  std::unique_ptr<HttpAuthHandlerRegistryFactory> http_auth_handler_factory_;

  TestCompletionCallback read_callback_;
  scoped_refptr<IOBuffer> read_buf_;

  TestCompletionCallback write_callback_;

  quic::test::NoopQpackStreamSenderDelegate noop_qpack_stream_sender_delegate_;

  DISALLOW_COPY_AND_ASSIGN(QuicProxyClientSocketTest);
};

TEST_P(QuicProxyClientSocketTest, ConnectSendsCorrectRequest) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(packet_number++));
  mock_quic_data_.AddRead(ASYNC, ConstructServerConnectReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(SYNCHRONOUS, ERR_IO_PENDING);
  mock_quic_data_.AddWrite(
      SYNCHRONOUS, ConstructAckAndRstPacket(
                       packet_number++, quic::QUIC_STREAM_CANCELLED, 1, 1, 1));

  Initialize();

  ASSERT_FALSE(sock_->IsConnected());

  AssertConnectSucceeds();

  const HttpResponseInfo* response = sock_->GetConnectResponseInfo();
  ASSERT_TRUE(response != nullptr);
  ASSERT_EQ(200, response->headers->response_code());
}

TEST_P(QuicProxyClientSocketTest, ConnectWithAuthRequested) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(packet_number++));
  mock_quic_data_.AddRead(ASYNC,
                          ConstructServerConnectAuthReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(SYNCHRONOUS, ERR_IO_PENDING);
  mock_quic_data_.AddWrite(
      SYNCHRONOUS, ConstructAckAndRstPacket(
                       packet_number++, quic::QUIC_STREAM_CANCELLED, 1, 1, 1));

  Initialize();

  AssertConnectFails(ERR_PROXY_AUTH_REQUESTED);

  const HttpResponseInfo* response = sock_->GetConnectResponseInfo();
  ASSERT_TRUE(response != nullptr);
  ASSERT_EQ(407, response->headers->response_code());
}

TEST_P(QuicProxyClientSocketTest, ConnectWithAuthCredentials) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectAuthRequestPacket(packet_number++));
  mock_quic_data_.AddRead(ASYNC, ConstructServerConnectReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(SYNCHRONOUS, ERR_IO_PENDING);
  mock_quic_data_.AddWrite(
      SYNCHRONOUS, ConstructAckAndRstPacket(
                       packet_number++, quic::QUIC_STREAM_CANCELLED, 1, 1, 1));

  Initialize();

  // Add auth to cache
  const base::string16 kFoo(base::ASCIIToUTF16("foo"));
  const base::string16 kBar(base::ASCIIToUTF16("bar"));
  http_auth_cache_.Add(GURL(kProxyUrl), HttpAuth::AUTH_PROXY, "MyRealm1",
                       HttpAuth::AUTH_SCHEME_BASIC, NetworkIsolationKey(),
                       "Basic realm=MyRealm1", AuthCredentials(kFoo, kBar),
                       "/");

  AssertConnectSucceeds();

  const HttpResponseInfo* response = sock_->GetConnectResponseInfo();
  ASSERT_TRUE(response != nullptr);
  ASSERT_EQ(200, response->headers->response_code());
}

// Tests that a redirect response from a CONNECT fails.
TEST_P(QuicProxyClientSocketTest, ConnectRedirects) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(packet_number++));
  mock_quic_data_.AddRead(ASYNC,
                          ConstructServerConnectRedirectReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(SYNCHRONOUS, ERR_IO_PENDING);
  mock_quic_data_.AddWrite(
      SYNCHRONOUS, ConstructAckAndRstPacket(
                       packet_number++, quic::QUIC_STREAM_CANCELLED, 1, 1, 1));

  Initialize();

  AssertConnectFails(ERR_TUNNEL_CONNECTION_FAILED);

  const HttpResponseInfo* response = sock_->GetConnectResponseInfo();
  ASSERT_TRUE(response != nullptr);

  const HttpResponseHeaders* headers = response->headers.get();
  ASSERT_EQ(302, headers->response_code());
  ASSERT_TRUE(headers->HasHeader("set-cookie"));

  std::string location;
  ASSERT_TRUE(headers->IsRedirect(&location));
  ASSERT_EQ(location, kRedirectUrl);
}

TEST_P(QuicProxyClientSocketTest, ConnectFails) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(packet_number++));
  mock_quic_data_.AddRead(ASYNC, 0);  // EOF

  Initialize();

  ASSERT_FALSE(sock_->IsConnected());

  AssertConnectFails(ERR_QUIC_PROTOCOL_ERROR);

  ASSERT_FALSE(sock_->IsConnected());
}

TEST_P(QuicProxyClientSocketTest, WasEverUsedReturnsCorrectValue) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(packet_number++));
  mock_quic_data_.AddRead(ASYNC, ConstructServerConnectReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(SYNCHRONOUS, ERR_IO_PENDING);
  mock_quic_data_.AddWrite(
      SYNCHRONOUS, ConstructAckAndRstPacket(
                       packet_number++, quic::QUIC_STREAM_CANCELLED, 1, 1, 1));

  Initialize();

  if (VersionUsesHttp3(version_.transport_version))
    EXPECT_TRUE(sock_->WasEverUsed());  // Used due to crypto handshake
  AssertConnectSucceeds();
  EXPECT_TRUE(sock_->WasEverUsed());
  sock_->Disconnect();
  EXPECT_TRUE(sock_->WasEverUsed());
}

TEST_P(QuicProxyClientSocketTest, GetPeerAddressReturnsCorrectValues) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(packet_number++));
  mock_quic_data_.AddRead(ASYNC, ConstructServerConnectReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(ASYNC, ERR_IO_PENDING);  // Pause
  mock_quic_data_.AddRead(ASYNC, 0);               // EOF

  Initialize();

  IPEndPoint addr;
  EXPECT_THAT(sock_->GetPeerAddress(&addr), IsError(ERR_SOCKET_NOT_CONNECTED));

  AssertConnectSucceeds();
  EXPECT_TRUE(sock_->IsConnected());
  EXPECT_THAT(sock_->GetPeerAddress(&addr), IsOk());

  ResumeAndRun();

  EXPECT_FALSE(sock_->IsConnected());
  EXPECT_THAT(sock_->GetPeerAddress(&addr), IsError(ERR_SOCKET_NOT_CONNECTED));

  sock_->Disconnect();

  EXPECT_THAT(sock_->GetPeerAddress(&addr), IsError(ERR_SOCKET_NOT_CONNECTED));
}

TEST_P(QuicProxyClientSocketTest, IsConnectedAndIdle) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(packet_number++));
  mock_quic_data_.AddRead(ASYNC, ConstructServerConnectReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(ASYNC, ERR_IO_PENDING);  // Pause

  std::string header = ConstructDataHeader(kLen1);
  mock_quic_data_.AddRead(
      ASYNC, ConstructServerDataPacket(2, header + std::string(kMsg1, kLen1)));
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructAckPacket(packet_number++, 2, 1, 1));
  mock_quic_data_.AddRead(SYNCHRONOUS, ERR_IO_PENDING);
  mock_quic_data_.AddWrite(
      SYNCHRONOUS,
      ConstructRstPacket(packet_number++, quic::QUIC_STREAM_CANCELLED));

  Initialize();

  EXPECT_FALSE(sock_->IsConnectedAndIdle());

  AssertConnectSucceeds();

  EXPECT_TRUE(sock_->IsConnectedAndIdle());

  // The next read is consumed and buffered.
  ResumeAndRun();

  EXPECT_FALSE(sock_->IsConnectedAndIdle());

  AssertSyncReadEquals(kMsg1, kLen1);

  EXPECT_TRUE(sock_->IsConnectedAndIdle());
}

TEST_P(QuicProxyClientSocketTest, GetTotalReceivedBytes) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  size_t header_length;
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(packet_number++));
  mock_quic_data_.AddRead(
      ASYNC, ConstructServerConnectReplyPacket(1, !kFin, &header_length));
  mock_quic_data_.AddRead(ASYNC, ERR_IO_PENDING);  // Pause

  std::string data_header = ConstructDataHeader(kLen333);
  mock_quic_data_.AddRead(ASYNC,
                          ConstructServerDataPacket(
                              2, data_header + std::string(kMsg333, kLen333)));
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructAckPacket(packet_number++, 2, 1, 1));
  mock_quic_data_.AddRead(SYNCHRONOUS, ERR_IO_PENDING);
  mock_quic_data_.AddWrite(
      SYNCHRONOUS,
      ConstructRstPacket(packet_number++, quic::QUIC_STREAM_CANCELLED));

  Initialize();

  EXPECT_EQ(0, sock_->GetTotalReceivedBytes());

  AssertConnectSucceeds();

  if (!VersionUsesHttp3(version_.transport_version)) {
    header_length = 0;
    EXPECT_EQ(0, sock_->GetTotalReceivedBytes());
  } else {
    // HTTP/3 sends and receives HTTP headers on the request stream.
    EXPECT_EQ((int64_t)(header_length), sock_->GetTotalReceivedBytes());
  }

  // The next read is consumed and buffered.
  ResumeAndRun();

  if (!VersionUsesHttp3(version_.transport_version)) {
    EXPECT_EQ(0, sock_->GetTotalReceivedBytes());
  } else {
    // HTTP/3 encodes data with DATA frame. The header is consumed.
    EXPECT_EQ((int64_t)(header_length + data_header.length()),
              sock_->GetTotalReceivedBytes());
  }

  // The payload from the single large data frame will be read across
  // two different reads.
  AssertSyncReadEquals(kMsg33, kLen33);

  EXPECT_EQ((int64_t)(header_length + data_header.length() + kLen33),
            sock_->GetTotalReceivedBytes());

  AssertSyncReadEquals(kMsg3, kLen3);

  EXPECT_EQ((int64_t)(header_length + kLen333 + data_header.length()),
            sock_->GetTotalReceivedBytes());
}

TEST_P(QuicProxyClientSocketTest, SetStreamPriority) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  // Despite setting the priority to HIGHEST, the requets initial priority of
  // LOWEST is used.
  mock_quic_data_.AddWrite(
      SYNCHRONOUS, ConstructConnectRequestPacket(packet_number++, LOWEST));
  mock_quic_data_.AddRead(ASYNC, ConstructServerConnectReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(SYNCHRONOUS, ERR_IO_PENDING);
  mock_quic_data_.AddWrite(
      SYNCHRONOUS, ConstructAckAndRstPacket(
                       packet_number++, quic::QUIC_STREAM_CANCELLED, 1, 1, 1));

  Initialize();

  sock_->SetStreamPriority(HIGHEST);
  AssertConnectSucceeds();
}

TEST_P(QuicProxyClientSocketTest, WriteSendsDataInDataFrame) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(packet_number++));
  mock_quic_data_.AddRead(ASYNC, ConstructServerConnectReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(SYNCHRONOUS, ERR_IO_PENDING);
  if (version_.transport_version == quic::QUIC_VERSION_99) {
    std::string header = ConstructDataHeader(kLen1);
    mock_quic_data_.AddWrite(
        SYNCHRONOUS,
        ConstructAckAndDataPacket(packet_number++, 1, 1, 1,
                                  {header + std::string(kMsg1, kLen1)}));
    std::string header2 = ConstructDataHeader(kLen2);
    mock_quic_data_.AddWrite(
        SYNCHRONOUS,
        ConstructDataPacket(packet_number++,
                            {header2 + std::string(kMsg2, kLen2)}));
    mock_quic_data_.AddWrite(
        SYNCHRONOUS,
        ConstructRstPacket(packet_number++, quic::QUIC_STREAM_CANCELLED));
  } else {
    mock_quic_data_.AddWrite(
        SYNCHRONOUS, ConstructAckAndDataPacket(packet_number++, 1, 1, 1,
                                               std::string(kMsg1, kLen1)));
    mock_quic_data_.AddWrite(
        SYNCHRONOUS,
        ConstructDataPacket(packet_number++, std::string(kMsg2, kLen2)));
    mock_quic_data_.AddWrite(
        SYNCHRONOUS,
        ConstructRstPacket(packet_number++, quic::QUIC_STREAM_CANCELLED));
  }

  Initialize();

  AssertConnectSucceeds();

  AssertSyncWriteSucceeds(kMsg1, kLen1);
  AssertSyncWriteSucceeds(kMsg2, kLen2);
}

TEST_P(QuicProxyClientSocketTest, WriteSplitsLargeDataIntoMultiplePackets) {
  int write_packet_index = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(write_packet_index++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(write_packet_index++));
  mock_quic_data_.AddRead(ASYNC, ConstructServerConnectReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(SYNCHRONOUS, ERR_IO_PENDING);
  std::string header = ConstructDataHeader(kLen1);
  if (version_.transport_version != quic::QUIC_VERSION_99) {
    mock_quic_data_.AddWrite(
        SYNCHRONOUS, ConstructAckAndDataPacket(write_packet_index++, 1, 1, 1,
                                               std::string(kMsg1, kLen1)));
  } else {
    mock_quic_data_.AddWrite(
        SYNCHRONOUS,
        ConstructAckAndDataPacket(write_packet_index++, 1, 1, 1,
                                  {header + std::string(kMsg1, kLen1)}));
  }

  // Expect |kNumDataPackets| data packets, each containing the max possible
  // amount of data.
  int numDataPackets = 3;
  std::string data(numDataPackets * quic::kDefaultMaxPacketSize, 'x');
  quic::QuicStreamOffset offset = kLen1 + header.length();

  if (version_.transport_version == quic::QUIC_VERSION_99) {
    numDataPackets++;
  }
  size_t total_data_length = 0;
  for (int i = 0; i < numDataPackets; ++i) {
    size_t max_packet_data_length = GetStreamFrameDataLengthFromPacketLength(
        quic::kDefaultMaxPacketSize, version_, !kIncludeVersion,
        !kIncludeDiversificationNonce, quic::PACKET_8BYTE_CONNECTION_ID,
        quic::PACKET_1BYTE_PACKET_NUMBER, offset);
    if (version_.transport_version == quic::QUIC_VERSION_99 && i == 0) {
      // 3973 is the data frame length from packet length.
      std::string header2 = ConstructDataHeader(3973);
      mock_quic_data_.AddWrite(
          SYNCHRONOUS,
          ConstructDataPacket(
              write_packet_index++,
              {header2 +
               std::string(data.c_str(), max_packet_data_length - 7)}));
      offset += max_packet_data_length - header2.length() - 1;
    } else if (version_.transport_version == quic::QUIC_VERSION_99 &&
               i == numDataPackets - 1) {
      mock_quic_data_.AddWrite(
          SYNCHRONOUS, ConstructDataPacket(write_packet_index++,
                                           std::string(data.c_str(), 7)));
      offset += 7;
    } else {
      mock_quic_data_.AddWrite(
          SYNCHRONOUS, ConstructDataPacket(
                           write_packet_index++,
                           std::string(data.c_str(), max_packet_data_length)));
      offset += max_packet_data_length;
    }
    if (i != 3) {
      total_data_length += max_packet_data_length;
    }
  }

  mock_quic_data_.AddWrite(
      SYNCHRONOUS,
      ConstructRstPacket(write_packet_index++, quic::QUIC_STREAM_CANCELLED));

  Initialize();

  AssertConnectSucceeds();

  // Make a small write. An ACK and STOP_WAITING will be bundled. This prevents
  // ACK and STOP_WAITING from being bundled with the subsequent large write.
  // This allows the test code for computing the size of data sent in each
  // packet to not become too complicated.
  AssertSyncWriteSucceeds(kMsg1, kLen1);

  // Make large write that should be split up
  AssertSyncWriteSucceeds(data.c_str(), total_data_length);
}

// ----------- Read

TEST_P(QuicProxyClientSocketTest, ReadReadsDataInDataFrame) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(packet_number++));
  mock_quic_data_.AddRead(ASYNC, ConstructServerConnectReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(ASYNC, ERR_IO_PENDING);  // Pause

  std::string header = ConstructDataHeader(kLen1);
  mock_quic_data_.AddRead(
      ASYNC, ConstructServerDataPacket(2, header + std::string(kMsg1, kLen1)));
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructAckPacket(packet_number++, 2, 1, 1));
  mock_quic_data_.AddRead(SYNCHRONOUS, ERR_IO_PENDING);
  mock_quic_data_.AddWrite(
      SYNCHRONOUS,
      ConstructRstPacket(packet_number++, quic::QUIC_STREAM_CANCELLED));

  Initialize();

  AssertConnectSucceeds();

  ResumeAndRun();
  AssertSyncReadEquals(kMsg1, kLen1);
}

TEST_P(QuicProxyClientSocketTest, ReadDataFromBufferedFrames) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(packet_number++));
  mock_quic_data_.AddRead(ASYNC, ConstructServerConnectReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(ASYNC, ERR_IO_PENDING);  // Pause

  std::string header = ConstructDataHeader(kLen1);
  mock_quic_data_.AddRead(
      ASYNC, ConstructServerDataPacket(2, header + std::string(kMsg1, kLen1)));
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructAckPacket(packet_number++, 2, 1, 1));
  mock_quic_data_.AddRead(ASYNC, ERR_IO_PENDING);  // Pause

  std::string header2 = ConstructDataHeader(kLen2);
  mock_quic_data_.AddRead(
      ASYNC, ConstructServerDataPacket(3, header2 + std::string(kMsg2, kLen2)));
  mock_quic_data_.AddRead(SYNCHRONOUS, ERR_IO_PENDING);

  mock_quic_data_.AddWrite(
      SYNCHRONOUS, ConstructAckAndRstPacket(
                       packet_number++, quic::QUIC_STREAM_CANCELLED, 3, 3, 1));

  Initialize();

  AssertConnectSucceeds();

  ResumeAndRun();
  AssertSyncReadEquals(kMsg1, kLen1);

  ResumeAndRun();
  AssertSyncReadEquals(kMsg2, kLen2);
}

TEST_P(QuicProxyClientSocketTest, ReadDataMultipleBufferedFrames) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(packet_number++));
  mock_quic_data_.AddRead(ASYNC, ConstructServerConnectReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(ASYNC, ERR_IO_PENDING);  // Pause

  std::string header = ConstructDataHeader(kLen1);
  mock_quic_data_.AddRead(
      ASYNC, ConstructServerDataPacket(2, header + std::string(kMsg1, kLen1)));
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructAckPacket(packet_number++, 2, 1, 1));
  std::string header2 = ConstructDataHeader(kLen2);
  mock_quic_data_.AddRead(
      ASYNC, ConstructServerDataPacket(3, header2 + std::string(kMsg2, kLen2)));
  mock_quic_data_.AddRead(SYNCHRONOUS, ERR_IO_PENDING);

  mock_quic_data_.AddWrite(
      SYNCHRONOUS, ConstructAckAndRstPacket(
                       packet_number++, quic::QUIC_STREAM_CANCELLED, 3, 3, 1));

  Initialize();

  AssertConnectSucceeds();

  // The next two reads are consumed and buffered.
  ResumeAndRun();

  AssertSyncReadEquals(kMsg1, kLen1);
  AssertSyncReadEquals(kMsg2, kLen2);
}

TEST_P(QuicProxyClientSocketTest, LargeReadWillMergeDataFromDifferentFrames) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(packet_number++));
  mock_quic_data_.AddRead(ASYNC, ConstructServerConnectReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(ASYNC, ERR_IO_PENDING);  // Pause

  std::string header = ConstructDataHeader(kLen3);
  mock_quic_data_.AddRead(
      ASYNC, ConstructServerDataPacket(2, header + std::string(kMsg3, kLen3)));
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructAckPacket(packet_number++, 2, 1, 1));
  std::string header2 = ConstructDataHeader(kLen3);
  mock_quic_data_.AddRead(
      ASYNC, ConstructServerDataPacket(3, header2 + std::string(kMsg3, kLen3)));
  mock_quic_data_.AddRead(SYNCHRONOUS, ERR_IO_PENDING);

  mock_quic_data_.AddWrite(
      SYNCHRONOUS, ConstructAckAndRstPacket(
                       packet_number++, quic::QUIC_STREAM_CANCELLED, 3, 3, 1));

  Initialize();

  AssertConnectSucceeds();

  // The next two reads are consumed and buffered.
  ResumeAndRun();
  // The payload from two data frames, each with kMsg3 will be combined
  // together into a single read().
  AssertSyncReadEquals(kMsg33, kLen33);
}

TEST_P(QuicProxyClientSocketTest, MultipleShortReadsThenMoreRead) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(packet_number++));
  mock_quic_data_.AddRead(ASYNC, ConstructServerConnectReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(ASYNC, ERR_IO_PENDING);  // Pause

  int offset = 0;

  std::string header = ConstructDataHeader(kLen1);
  mock_quic_data_.AddRead(
      ASYNC, ConstructServerDataPacket(2, header + std::string(kMsg1, kLen1)));
  offset += kLen1 + header.length();
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructAckPacket(packet_number++, 2, 1, 1));

  std::string header2 = ConstructDataHeader(kLen3);
  mock_quic_data_.AddRead(
      ASYNC, ConstructServerDataPacket(3, header2 + std::string(kMsg3, kLen3)));
  offset += kLen3 + header2.length();
  mock_quic_data_.AddRead(
      ASYNC, ConstructServerDataPacket(4, header2 + std::string(kMsg3, kLen3)));
  offset += kLen3 + header2.length();
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructAckPacket(packet_number++, 4, 3, 1));

  std::string header3 = ConstructDataHeader(kLen2);
  mock_quic_data_.AddRead(
      ASYNC, ConstructServerDataPacket(5, header3 + std::string(kMsg2, kLen2)));
  offset += kLen2 + header3.length();
  mock_quic_data_.AddRead(SYNCHRONOUS, ERR_IO_PENDING);

  mock_quic_data_.AddWrite(
      SYNCHRONOUS, ConstructAckAndRstPacket(
                       packet_number++, quic::QUIC_STREAM_CANCELLED, 5, 5, 1));

  Initialize();

  AssertConnectSucceeds();

  // The next 4 reads are consumed and buffered.
  ResumeAndRun();

  AssertSyncReadEquals(kMsg1, kLen1);
  // The payload from two data frames, each with kMsg3 will be combined
  // together into a single read().
  AssertSyncReadEquals(kMsg33, kLen33);
  AssertSyncReadEquals(kMsg2, kLen2);
}

TEST_P(QuicProxyClientSocketTest, ReadWillSplitDataFromLargeFrame) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(packet_number++));
  mock_quic_data_.AddRead(ASYNC, ConstructServerConnectReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(ASYNC, ERR_IO_PENDING);  // Pause

  std::string header = ConstructDataHeader(kLen1);
  mock_quic_data_.AddRead(
      ASYNC, ConstructServerDataPacket(2, header + std::string(kMsg1, kLen1)));
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructAckPacket(packet_number++, 2, 1, 1));
  std::string header2 = ConstructDataHeader(kLen33);
  mock_quic_data_.AddRead(ASYNC, ConstructServerDataPacket(
                                     3, header2 + std::string(kMsg33, kLen33)));
  mock_quic_data_.AddRead(SYNCHRONOUS, ERR_IO_PENDING);

  mock_quic_data_.AddWrite(
      SYNCHRONOUS, ConstructAckAndRstPacket(
                       packet_number++, quic::QUIC_STREAM_CANCELLED, 3, 3, 1));

  Initialize();

  AssertConnectSucceeds();

  // The next 2 reads are consumed and buffered.
  ResumeAndRun();

  AssertSyncReadEquals(kMsg1, kLen1);
  // The payload from the single large data frame will be read across
  // two different reads.
  AssertSyncReadEquals(kMsg3, kLen3);
  AssertSyncReadEquals(kMsg3, kLen3);
}

TEST_P(QuicProxyClientSocketTest, MultipleReadsFromSameLargeFrame) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(packet_number++));
  mock_quic_data_.AddRead(ASYNC, ConstructServerConnectReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(ASYNC, ERR_IO_PENDING);  // Pause

  std::string header = ConstructDataHeader(kLen333);
  mock_quic_data_.AddRead(
      ASYNC,
      ConstructServerDataPacket(2, header + std::string(kMsg333, kLen333)));
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructAckPacket(packet_number++, 2, 1, 1));
  mock_quic_data_.AddRead(SYNCHRONOUS, ERR_IO_PENDING);

  mock_quic_data_.AddWrite(
      SYNCHRONOUS,
      ConstructRstPacket(packet_number++, quic::QUIC_STREAM_CANCELLED));

  Initialize();

  AssertConnectSucceeds();

  // The next read is consumed and buffered.
  ResumeAndRun();

  // The payload from the single large data frame will be read across
  // two different reads.
  AssertSyncReadEquals(kMsg33, kLen33);

  // Now attempt to do a read of more data than remains buffered
  scoped_refptr<IOBuffer> buf = base::MakeRefCounted<IOBuffer>(kLen33);
  ASSERT_EQ(kLen3, sock_->Read(buf.get(), kLen33, CompletionOnceCallback()));
  ASSERT_EQ(std::string(kMsg3, kLen3), std::string(buf->data(), kLen3));
  ASSERT_TRUE(sock_->IsConnected());
}

TEST_P(QuicProxyClientSocketTest, ReadAuthResponseBody) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(packet_number++));
  mock_quic_data_.AddRead(ASYNC,
                          ConstructServerConnectAuthReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(ASYNC, ERR_IO_PENDING);  // Pause

  std::string header = ConstructDataHeader(kLen1);
  mock_quic_data_.AddRead(
      ASYNC, ConstructServerDataPacket(2, header + std::string(kMsg1, kLen1)));
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructAckPacket(packet_number++, 2, 1, 1));
  std::string header2 = ConstructDataHeader(kLen2);
  mock_quic_data_.AddRead(
      ASYNC, ConstructServerDataPacket(3, header2 + std::string(kMsg2, kLen2)));
  mock_quic_data_.AddRead(SYNCHRONOUS, ERR_IO_PENDING);

  mock_quic_data_.AddWrite(
      SYNCHRONOUS, ConstructAckAndRstPacket(
                       packet_number++, quic::QUIC_STREAM_CANCELLED, 3, 3, 1));

  Initialize();

  AssertConnectFails(ERR_PROXY_AUTH_REQUESTED);

  // The next two reads are consumed and buffered.
  ResumeAndRun();

  AssertSyncReadEquals(kMsg1, kLen1);
  AssertSyncReadEquals(kMsg2, kLen2);
}

TEST_P(QuicProxyClientSocketTest, ReadErrorResponseBody) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(packet_number++));
  mock_quic_data_.AddRead(ASYNC,
                          ConstructServerConnectErrorReplyPacket(1, !kFin));
  std::string header = ConstructDataHeader(kLen1);
  mock_quic_data_.AddRead(
      SYNCHRONOUS,
      ConstructServerDataPacket(2, header + std::string(kMsg1, kLen1)));
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructAckPacket(packet_number++, 2, 1, 1));
  std::string header2 = ConstructDataHeader(kLen2);
  mock_quic_data_.AddRead(
      SYNCHRONOUS,
      ConstructServerDataPacket(3, header2 + std::string(kMsg2, kLen2)));
  mock_quic_data_.AddRead(SYNCHRONOUS, ERR_IO_PENDING);

  mock_quic_data_.AddWrite(
      SYNCHRONOUS, ConstructAckAndRstPacket(
                       packet_number++, quic::QUIC_STREAM_CANCELLED, 3, 3, 1));
  Initialize();

  AssertConnectFails(ERR_TUNNEL_CONNECTION_FAILED);
}

// ----------- Reads and Writes

TEST_P(QuicProxyClientSocketTest, AsyncReadAroundWrite) {
  int write_packet_index = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(write_packet_index++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(write_packet_index++));
  mock_quic_data_.AddRead(ASYNC, ConstructServerConnectReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(ASYNC, ERR_IO_PENDING);  // Pause

  std::string header = ConstructDataHeader(kLen1);
  mock_quic_data_.AddRead(
      ASYNC, ConstructServerDataPacket(2, header + std::string(kMsg1, kLen1)));
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructAckPacket(write_packet_index++, 2, 1, 1));

  std::string header2 = ConstructDataHeader(kLen2);
  if (version_.transport_version == quic::QUIC_VERSION_99) {
    mock_quic_data_.AddWrite(
        SYNCHRONOUS,
        ConstructDataPacket(write_packet_index++,
                            {header2 + std::string(kMsg2, kLen2)}));
  } else {
    mock_quic_data_.AddWrite(
        SYNCHRONOUS,
        ConstructDataPacket(write_packet_index++, std::string(kMsg2, kLen2)));
  }

  mock_quic_data_.AddRead(ASYNC, ERR_IO_PENDING);  // Pause

  std::string header3 = ConstructDataHeader(kLen3);
  mock_quic_data_.AddRead(
      ASYNC, ConstructServerDataPacket(3, header3 + std::string(kMsg3, kLen3)));
  mock_quic_data_.AddRead(SYNCHRONOUS, ERR_IO_PENDING);

  mock_quic_data_.AddWrite(
      SYNCHRONOUS,
      ConstructAckAndRstPacket(write_packet_index++,
                               quic::QUIC_STREAM_CANCELLED, 3, 3, 1));

  Initialize();

  AssertConnectSucceeds();

  ResumeAndRun();

  AssertSyncReadEquals(kMsg1, kLen1);

  AssertReadStarts(kMsg3, kLen3);
  // Read should block until after the write succeeds.

  AssertSyncWriteSucceeds(kMsg2, kLen2);

  ASSERT_FALSE(read_callback_.have_result());
  ResumeAndRun();

  // Now the read will return.
  AssertReadReturns(kMsg3, kLen3);
}

TEST_P(QuicProxyClientSocketTest, AsyncWriteAroundReads) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(packet_number++));
  mock_quic_data_.AddRead(ASYNC, ConstructServerConnectReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(ASYNC, ERR_IO_PENDING);  // Pause

  std::string header = ConstructDataHeader(kLen1);
  mock_quic_data_.AddRead(
      ASYNC, ConstructServerDataPacket(2, header + std::string(kMsg1, kLen1)));
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructAckPacket(packet_number++, 2, 1, 1));
  mock_quic_data_.AddRead(ASYNC, ERR_IO_PENDING);  // Pause

  std::string header2 = ConstructDataHeader(kLen3);
  mock_quic_data_.AddRead(
      ASYNC, ConstructServerDataPacket(3, header2 + std::string(kMsg3, kLen3)));
  mock_quic_data_.AddRead(SYNCHRONOUS, ERR_IO_PENDING);

  mock_quic_data_.AddWrite(ASYNC, ERR_IO_PENDING);  // Pause

  std::string header3 = ConstructDataHeader(kLen2);
  if (version_.transport_version != quic::QUIC_VERSION_99) {
    mock_quic_data_.AddWrite(
        ASYNC, ConstructDataPacket(packet_number++, std::string(kMsg2, kLen2)));
    mock_quic_data_.AddWrite(
        SYNCHRONOUS, ConstructAckAndDataPacket(packet_number++, 3, 3, 1,
                                               std::string(kMsg2, kLen2)));
  } else {
    mock_quic_data_.AddWrite(
        ASYNC, ConstructDataPacket(packet_number++,
                                   {header3 + std::string(kMsg2, kLen2)}));
    mock_quic_data_.AddWrite(
        ASYNC, ConstructAckAndDataPacket(packet_number++, 3, 3, 1,
                                         header3 + std::string(kMsg2, kLen2)));
  }

  mock_quic_data_.AddWrite(
      SYNCHRONOUS,
      ConstructRstPacket(packet_number++, quic::QUIC_STREAM_CANCELLED));

  Initialize();

  AssertConnectSucceeds();

  ResumeAndRun();
  AssertSyncReadEquals(kMsg1, kLen1);

  // Write should block until the next read completes.
  // QuicChromiumClientStream::Handle::WriteStreamData() will only be
  // asynchronous starting with the second time it's called while the UDP socket
  // is write-blocked. Therefore, at least two writes need to be called on
  // |sock_| to get an asynchronous one.
  AssertWriteReturns(kMsg2, kLen2, kLen2);
  AssertWriteReturns(kMsg2, kLen2, ERR_IO_PENDING);

  AssertAsyncReadEquals(kMsg3, kLen3);

  ASSERT_FALSE(write_callback_.have_result());

  // Now the write will complete
  ResumeAndRun();
  EXPECT_EQ(kLen2, write_callback_.WaitForResult());
}

// ----------- Reading/Writing on Closed socket

// Reading from an already closed socket should return 0
TEST_P(QuicProxyClientSocketTest, ReadOnClosedSocketReturnsZero) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(packet_number++));
  mock_quic_data_.AddRead(ASYNC, ConstructServerConnectReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(ASYNC, ERR_IO_PENDING);  // Pause

  mock_quic_data_.AddRead(ASYNC, 0);  // EOF

  Initialize();

  AssertConnectSucceeds();

  ResumeAndRun();

  ASSERT_FALSE(sock_->IsConnected());
  ASSERT_EQ(0, sock_->Read(nullptr, 1, CompletionOnceCallback()));
  ASSERT_EQ(0, sock_->Read(nullptr, 1, CompletionOnceCallback()));
  ASSERT_EQ(0, sock_->Read(nullptr, 1, CompletionOnceCallback()));
  ASSERT_FALSE(sock_->IsConnectedAndIdle());
}

// Read pending when socket is closed should return 0
TEST_P(QuicProxyClientSocketTest, PendingReadOnCloseReturnsZero) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(packet_number++));
  mock_quic_data_.AddRead(ASYNC, ConstructServerConnectReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(ASYNC, ERR_IO_PENDING);  // Pause

  mock_quic_data_.AddRead(ASYNC, 0);  // EOF

  Initialize();

  AssertConnectSucceeds();

  AssertReadStarts(kMsg1, kLen1);

  ResumeAndRun();

  ASSERT_EQ(0, read_callback_.WaitForResult());
}

// Reading from a disconnected socket is an error
TEST_P(QuicProxyClientSocketTest, ReadOnDisconnectSocketReturnsNotConnected) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(packet_number++));
  mock_quic_data_.AddRead(ASYNC, ConstructServerConnectReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(SYNCHRONOUS, ERR_IO_PENDING);
  mock_quic_data_.AddWrite(
      SYNCHRONOUS, ConstructAckAndRstPacket(
                       packet_number++, quic::QUIC_STREAM_CANCELLED, 1, 1, 1));

  Initialize();

  AssertConnectSucceeds();

  sock_->Disconnect();

  ASSERT_EQ(ERR_SOCKET_NOT_CONNECTED,
            sock_->Read(nullptr, 1, CompletionOnceCallback()));
}

// Reading data after receiving FIN should return buffered data received before
// FIN, then 0.
TEST_P(QuicProxyClientSocketTest, ReadAfterFinReceivedReturnsBufferedData) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(packet_number++));
  mock_quic_data_.AddRead(ASYNC, ConstructServerConnectReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(ASYNC, ERR_IO_PENDING);  // Pause

  std::string header = ConstructDataHeader(kLen1);
  mock_quic_data_.AddRead(ASYNC, ConstructServerDataFinPacket(
                                     2, header + std::string(kMsg1, kLen1)));
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructAckPacket(packet_number++, 2, 1, 1));
  mock_quic_data_.AddRead(SYNCHRONOUS, ERR_IO_PENDING);
  mock_quic_data_.AddWrite(
      SYNCHRONOUS,
      ConstructRstPacket(packet_number++, quic::QUIC_STREAM_CANCELLED));

  Initialize();

  AssertConnectSucceeds();

  ResumeAndRun();

  AssertSyncReadEquals(kMsg1, kLen1);
  ASSERT_EQ(0, sock_->Read(nullptr, 1, CompletionOnceCallback()));
  ASSERT_EQ(0, sock_->Read(nullptr, 1, CompletionOnceCallback()));

  sock_->Disconnect();
  ASSERT_EQ(ERR_SOCKET_NOT_CONNECTED,
            sock_->Read(nullptr, 1, CompletionOnceCallback()));
}

// Calling Write() on a closed socket is an error.
TEST_P(QuicProxyClientSocketTest, WriteOnClosedStream) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(packet_number++));
  mock_quic_data_.AddRead(ASYNC, ConstructServerConnectReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(ASYNC, ERR_IO_PENDING);  // Pause

  mock_quic_data_.AddRead(ASYNC, 0);  // EOF

  Initialize();

  AssertConnectSucceeds();

  ResumeAndRun();

  AssertWriteReturns(kMsg1, kLen1, ERR_QUIC_PROTOCOL_ERROR);
}

// Calling Write() on a disconnected socket is an error.
TEST_P(QuicProxyClientSocketTest, WriteOnDisconnectedSocket) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(packet_number++));
  mock_quic_data_.AddRead(ASYNC, ConstructServerConnectReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(SYNCHRONOUS, ERR_IO_PENDING);
  mock_quic_data_.AddWrite(
      SYNCHRONOUS, ConstructAckAndRstPacket(
                       packet_number++, quic::QUIC_STREAM_CANCELLED, 1, 1, 1));

  Initialize();

  AssertConnectSucceeds();

  sock_->Disconnect();

  AssertWriteReturns(kMsg1, kLen1, ERR_SOCKET_NOT_CONNECTED);
}

// If the socket is closed with a pending Write(), the callback should be called
// with the same error the session was closed with.
TEST_P(QuicProxyClientSocketTest, WritePendingOnClose) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(packet_number++));
  mock_quic_data_.AddRead(ASYNC, ConstructServerConnectReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(SYNCHRONOUS, ERR_IO_PENDING);
  mock_quic_data_.AddWrite(SYNCHRONOUS, ERR_IO_PENDING);

  Initialize();

  AssertConnectSucceeds();

  // QuicChromiumClientStream::Handle::WriteStreamData() will only be
  // asynchronous starting with the second time it's called while the UDP socket
  // is write-blocked. Therefore, at least two writes need to be called on
  // |sock_| to get an asynchronous one.
  AssertWriteReturns(kMsg1, kLen1, kLen1);

  // This second write will be async. This is the pending write that's being
  // tested.
  AssertWriteReturns(kMsg1, kLen1, ERR_IO_PENDING);

  // Make sure the write actually starts.
  base::RunLoop().RunUntilIdle();

  session_->CloseSessionOnError(ERR_CONNECTION_CLOSED,
                                quic::QUIC_INTERNAL_ERROR,
                                quic::ConnectionCloseBehavior::SILENT_CLOSE);

  EXPECT_THAT(write_callback_.WaitForResult(), IsError(ERR_CONNECTION_CLOSED));
}

TEST_P(QuicProxyClientSocketTest, DisconnectWithWritePending) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(packet_number++));
  mock_quic_data_.AddRead(ASYNC, ConstructServerConnectReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(SYNCHRONOUS, ERR_IO_PENDING);
  mock_quic_data_.AddWrite(SYNCHRONOUS, ERR_IO_PENDING);

  Initialize();

  AssertConnectSucceeds();

  // QuicChromiumClientStream::Handle::WriteStreamData() will only be
  // asynchronous starting with the second time it's called while the UDP socket
  // is write-blocked. Therefore, at least two writes need to be called on
  // |sock_| to get an asynchronous one.
  AssertWriteReturns(kMsg1, kLen1, kLen1);

  // This second write will be async. This is the pending write that's being
  // tested.
  AssertWriteReturns(kMsg1, kLen1, ERR_IO_PENDING);

  // Make sure the write actually starts.
  base::RunLoop().RunUntilIdle();

  sock_->Disconnect();
  EXPECT_FALSE(sock_->IsConnected());

  base::RunLoop().RunUntilIdle();

  EXPECT_FALSE(sock_->IsConnected());
  EXPECT_FALSE(write_callback_.have_result());
}

// If the socket is Disconnected with a pending Read(), the callback
// should not be called.
TEST_P(QuicProxyClientSocketTest, DisconnectWithReadPending) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(packet_number++));
  mock_quic_data_.AddRead(ASYNC, ConstructServerConnectReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(SYNCHRONOUS, ERR_IO_PENDING);
  mock_quic_data_.AddWrite(
      SYNCHRONOUS, ConstructAckAndRstPacket(
                       packet_number++, quic::QUIC_STREAM_CANCELLED, 1, 1, 1));

  Initialize();

  AssertConnectSucceeds();

  EXPECT_TRUE(sock_->IsConnected());

  AssertReadStarts(kMsg1, kLen1);

  sock_->Disconnect();
  EXPECT_FALSE(sock_->IsConnected());

  base::RunLoop().RunUntilIdle();

  EXPECT_FALSE(sock_->IsConnected());
  EXPECT_FALSE(read_callback_.have_result());
}

// If the socket is Reset when both a read and write are pending,
// both should be called back.
TEST_P(QuicProxyClientSocketTest, RstWithReadAndWritePending) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(packet_number++));
  mock_quic_data_.AddRead(ASYNC, ConstructServerConnectReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(ASYNC, ERR_IO_PENDING);  // Pause

  mock_quic_data_.AddRead(
      ASYNC, ConstructServerRstPacket(2, quic::QUIC_STREAM_CANCELLED));
  mock_quic_data_.AddRead(SYNCHRONOUS, ERR_IO_PENDING);
  std::string header = ConstructDataHeader(kLen2);
  if (version_.transport_version != quic::QUIC_VERSION_99) {
    mock_quic_data_.AddWrite(
        ASYNC, ConstructAckAndDataPacket(packet_number++, 1, 1, 1,
                                         std::string(kMsg2, kLen2)));
    mock_quic_data_.AddWrite(
        SYNCHRONOUS,
        ConstructAckAndRstPacket(packet_number++,
                                 quic::QUIC_RST_ACKNOWLEDGEMENT, 2, 2, 1));
  } else {
    mock_quic_data_.AddWrite(
        ASYNC, ConstructAckAndDataPacket(packet_number++, 1, 1, 1,
                                         {header + std::string(kMsg2, kLen2)}));
    mock_quic_data_.AddWrite(
        SYNCHRONOUS,
        ConstructAckAndRstOnlyPacket(packet_number++,
                                     quic::QUIC_STREAM_CANCELLED, 2, 2, 1));
  }

  Initialize();

  AssertConnectSucceeds();

  EXPECT_TRUE(sock_->IsConnected());

  AssertReadStarts(kMsg1, kLen1);

  // Write should block until the next read completes.
  // QuicChromiumClientStream::Handle::WriteStreamData() will only be
  // asynchronous starting with the second time it's called while the UDP socket
  // is write-blocked. Therefore, at least two writes need to be called on
  // |sock_| to get an asynchronous one.
  AssertWriteReturns(kMsg2, kLen2, kLen2);

  AssertWriteReturns(kMsg2, kLen2, ERR_IO_PENDING);

  ResumeAndRun();

  EXPECT_TRUE(read_callback_.have_result());
  EXPECT_TRUE(write_callback_.have_result());
}

// Makes sure the proxy client socket's source gets the expected NetLog events
// and only the expected NetLog events (No SpdySession events).
TEST_P(QuicProxyClientSocketTest, NetLog) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(packet_number++));
  mock_quic_data_.AddRead(ASYNC, ConstructServerConnectReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(ASYNC, ERR_IO_PENDING);  // Pause

  std::string header = ConstructDataHeader(kLen1);
  mock_quic_data_.AddRead(
      ASYNC, ConstructServerDataPacket(2, header + std::string(kMsg1, kLen1)));
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructAckPacket(packet_number++, 2, 1, 1));
  mock_quic_data_.AddRead(SYNCHRONOUS, ERR_IO_PENDING);
  mock_quic_data_.AddWrite(
      SYNCHRONOUS,
      ConstructRstPacket(packet_number++, quic::QUIC_STREAM_CANCELLED));

  Initialize();

  AssertConnectSucceeds();

  ResumeAndRun();
  AssertSyncReadEquals(kMsg1, kLen1);

  NetLogSource sock_source = sock_->NetLog().source();
  sock_.reset();

  auto entry_list = net_log_.GetEntriesForSource(sock_source);

  ASSERT_EQ(entry_list.size(), 10u);
  EXPECT_TRUE(
      LogContainsBeginEvent(entry_list, 0, NetLogEventType::SOCKET_ALIVE));
  EXPECT_TRUE(LogContainsEvent(entry_list, 1,
                               NetLogEventType::HTTP2_PROXY_CLIENT_SESSION,
                               NetLogEventPhase::NONE));
  EXPECT_TRUE(LogContainsBeginEvent(
      entry_list, 2, NetLogEventType::HTTP_TRANSACTION_TUNNEL_SEND_REQUEST));
  EXPECT_TRUE(LogContainsEvent(
      entry_list, 3, NetLogEventType::HTTP_TRANSACTION_SEND_TUNNEL_HEADERS,
      NetLogEventPhase::NONE));
  EXPECT_TRUE(LogContainsEndEvent(
      entry_list, 4, NetLogEventType::HTTP_TRANSACTION_TUNNEL_SEND_REQUEST));
  EXPECT_TRUE(LogContainsBeginEvent(
      entry_list, 5, NetLogEventType::HTTP_TRANSACTION_TUNNEL_READ_HEADERS));
  EXPECT_TRUE(LogContainsEvent(
      entry_list, 6,
      NetLogEventType::HTTP_TRANSACTION_READ_TUNNEL_RESPONSE_HEADERS,
      NetLogEventPhase::NONE));
  EXPECT_TRUE(LogContainsEndEvent(
      entry_list, 7, NetLogEventType::HTTP_TRANSACTION_TUNNEL_READ_HEADERS));
  EXPECT_TRUE(LogContainsEvent(entry_list, 8,
                               NetLogEventType::SOCKET_BYTES_RECEIVED,
                               NetLogEventPhase::NONE));
  EXPECT_TRUE(
      LogContainsEndEvent(entry_list, 9, NetLogEventType::SOCKET_ALIVE));
}

// A helper class that will delete |sock| when the callback is invoked.
class DeleteSockCallback : public TestCompletionCallbackBase {
 public:
  explicit DeleteSockCallback(std::unique_ptr<QuicProxyClientSocket>* sock)
      : sock_(sock) {}

  ~DeleteSockCallback() override {}

  CompletionOnceCallback callback() {
    return base::BindOnce(&DeleteSockCallback::OnComplete,
                          base::Unretained(this));
  }

 private:
  void OnComplete(int result) {
    sock_->reset(nullptr);
    SetResult(result);
  }

  std::unique_ptr<QuicProxyClientSocket>* sock_;

  DISALLOW_COPY_AND_ASSIGN(DeleteSockCallback);
};

// If the socket is reset when both a read and write are pending, and the
// read callback causes the socket to be deleted, the write callback should
// not be called.
TEST_P(QuicProxyClientSocketTest, RstWithReadAndWritePendingDelete) {
  int packet_number = 1;
  if (VersionUsesHttp3(version_.transport_version)) {
    mock_quic_data_.AddWrite(SYNCHRONOUS,
                             ConstructSettingsPacket(packet_number++));
  }
  mock_quic_data_.AddWrite(SYNCHRONOUS,
                           ConstructConnectRequestPacket(packet_number++));
  mock_quic_data_.AddRead(ASYNC, ConstructServerConnectReplyPacket(1, !kFin));
  mock_quic_data_.AddRead(ASYNC, ERR_IO_PENDING);  // Pause

  mock_quic_data_.AddRead(
      ASYNC, ConstructServerRstPacket(2, quic::QUIC_STREAM_CANCELLED));
  mock_quic_data_.AddRead(SYNCHRONOUS, ERR_IO_PENDING);
  if (version_.transport_version != quic::QUIC_VERSION_99) {
    mock_quic_data_.AddWrite(
        ASYNC, ConstructAckAndDataPacket(packet_number++, 1, 1, 1,
                                         std::string(kMsg1, kLen1)));
    mock_quic_data_.AddWrite(
        SYNCHRONOUS,
        ConstructAckAndRstPacket(packet_number++,
                                 quic::QUIC_RST_ACKNOWLEDGEMENT, 2, 2, 1));
  } else {
    std::string header = ConstructDataHeader(kLen1);
    mock_quic_data_.AddWrite(
        ASYNC, ConstructAckAndDataPacket(packet_number++, 1, 1, 1,
                                         {header + std::string(kMsg1, kLen1)}));
    mock_quic_data_.AddWrite(
        SYNCHRONOUS,
        ConstructAckAndRstOnlyPacket(packet_number++,
                                     quic::QUIC_STREAM_CANCELLED, 2, 2, 1));
  }

  Initialize();

  AssertConnectSucceeds();

  EXPECT_TRUE(sock_->IsConnected());

  DeleteSockCallback read_callback(&sock_);
  scoped_refptr<IOBuffer> read_buf = base::MakeRefCounted<IOBuffer>(kLen1);
  ASSERT_EQ(ERR_IO_PENDING,
            sock_->Read(read_buf.get(), kLen1, read_callback.callback()));

  // QuicChromiumClientStream::Handle::WriteStreamData() will only be
  // asynchronous starting with the second time it's called while the UDP socket
  // is write-blocked. Therefore, at least two writes need to be called on
  // |sock_| to get an asynchronous one.
  AssertWriteReturns(kMsg1, kLen1, kLen1);

  AssertWriteReturns(kMsg1, kLen1, ERR_IO_PENDING);

  ResumeAndRun();

  EXPECT_FALSE(sock_.get());

  EXPECT_EQ(0, read_callback.WaitForResult());
  EXPECT_FALSE(write_callback_.have_result());
}

INSTANTIATE_TEST_SUITE_P(VersionIncludeStreamDependencySequence,
                         QuicProxyClientSocketTest,
                         ::testing::ValuesIn(GetTestParams()),
                         ::testing::PrintToStringParamName());

}  // namespace test
}  // namespace net
