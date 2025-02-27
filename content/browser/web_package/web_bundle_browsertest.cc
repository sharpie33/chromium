// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/optional.h"
#include "base/path_service.h"
#include "base/run_loop.h"
#include "base/strings/string_piece.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/system/sys_info.h"
#include "base/test/bind_test_util.h"
#include "base/test/scoped_feature_list.h"
#include "base/threading/thread_restrictions.h"
#include "build/build_config.h"
#include "content/browser/web_package/web_bundle_utils.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/common/content_client.h"
#include "content/public/common/content_features.h"
#include "content/public/common/content_switches.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/content_browser_test.h"
#include "content/shell/browser/shell.h"
#include "net/base/filename_util.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "services/data_decoder/public/cpp/test_support/in_process_data_decoder.h"
#include "services/data_decoder/public/cpp/test_support/web_bundle_builder.h"

#if defined(OS_ANDROID)
#include "base/android/content_uri_utils.h"
#endif  // OS_ANDROID

namespace content {
namespace {

// "%2F" is treated as an invalid character for file URLs.
constexpr char kInvalidFileUrl[] = "file:///tmp/test%2F/a.wbn";

constexpr char kTestPageUrl[] = "https://test.example.org/";
constexpr char kTestPage1Url[] = "https://test.example.org/page1.html";
constexpr char kTestPage2Url[] = "https://test.example.org/page2.html";
constexpr char kTestPageForHashUrl[] =
    "https://test.example.org/hash.html#hello";

constexpr char kDefaultHeaders[] =
    "HTTP/1.1 200 OK\n"
    "Content-Type: application/webbundle\n"
    "X-Content-Type-Options: nosniff\n";

base::FilePath GetTestDataPath(base::StringPiece file) {
  base::FilePath test_data_dir;
  CHECK(base::PathService::Get(base::DIR_SOURCE_ROOT, &test_data_dir));
  return test_data_dir
      .Append(base::FilePath(FILE_PATH_LITERAL("content/test/data/web_bundle")))
      .AppendASCII(file);
}

#if defined(OS_ANDROID)
GURL CopyFileAndGetContentUri(const base::FilePath& file) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  base::FilePath tmp_dir;
  CHECK(base::GetTempDir(&tmp_dir));
  // The directory name "web_bundle" must be kept in sync with
  // content/shell/android/browsertests_apk/res/xml/file_paths.xml
  base::FilePath tmp_wbn_dir = tmp_dir.AppendASCII("web_bundle");
  CHECK(base::CreateDirectoryAndGetError(tmp_wbn_dir, nullptr));
  base::FilePath tmp_dir_in_tmp_wbn_dir;
  CHECK(
      base::CreateTemporaryDirInDir(tmp_wbn_dir, "", &tmp_dir_in_tmp_wbn_dir));
  base::FilePath temp_file = tmp_dir_in_tmp_wbn_dir.Append(file.BaseName());
  CHECK(base::CopyFile(file, temp_file));
  return GURL(base::GetContentUriFromFilePath(temp_file).value());
}
#endif  // OS_ANDROID

class DownloadObserver : public DownloadManager::Observer {
 public:
  explicit DownloadObserver(DownloadManager* manager) : manager_(manager) {
    manager_->AddObserver(this);
  }
  ~DownloadObserver() override { manager_->RemoveObserver(this); }

  void WaitUntilDownloadCreated() { run_loop_.Run(); }
  const GURL& observed_url() const { return url_; }

  // content::DownloadManager::Observer implementation.
  void OnDownloadCreated(content::DownloadManager* manager,
                         download::DownloadItem* item) override {
    url_ = item->GetURL();
    run_loop_.Quit();
  }

 private:
  DownloadManager* manager_;
  base::RunLoop run_loop_;
  GURL url_;

  DISALLOW_COPY_AND_ASSIGN(DownloadObserver);
};

class MockParserFactory;

class MockParser final : public data_decoder::mojom::WebBundleParser {
 public:
  using Index = base::flat_map<GURL, data_decoder::mojom::BundleIndexValuePtr>;

  MockParser(
      MockParserFactory* factory,
      mojo::PendingReceiver<data_decoder::mojom::WebBundleParser> receiver,
      const Index& index,
      const GURL& primary_url,
      bool simulate_parse_metadata_crash,
      bool simulate_parse_response_crash)
      : factory_(factory),
        receiver_(this, std::move(receiver)),
        index_(index),
        primary_url_(primary_url),
        simulate_parse_metadata_crash_(simulate_parse_metadata_crash),
        simulate_parse_response_crash_(simulate_parse_response_crash) {}
  ~MockParser() override = default;

 private:
  // data_decoder::mojom::WebBundleParser implementation.
  void ParseMetadata(ParseMetadataCallback callback) override;
  void ParseResponse(uint64_t response_offset,
                     uint64_t response_length,
                     ParseResponseCallback callback) override;

  MockParserFactory* factory_;
  mojo::Receiver<data_decoder::mojom::WebBundleParser> receiver_;
  const Index& index_;
  const GURL primary_url_;
  const bool simulate_parse_metadata_crash_;
  const bool simulate_parse_response_crash_;

  DISALLOW_COPY_AND_ASSIGN(MockParser);
};

class MockParserFactory final
    : public data_decoder::mojom::WebBundleParserFactory {
 public:
  MockParserFactory(std::vector<GURL> urls,
                    const base::FilePath& response_body_file)
      : primary_url_(urls[0]) {
    base::ScopedAllowBlockingForTesting allow_blocking;
    int64_t response_body_file_size;
    EXPECT_TRUE(
        base::GetFileSize(response_body_file, &response_body_file_size));
    for (const auto& url : urls) {
      data_decoder::mojom::BundleIndexValuePtr item =
          data_decoder::mojom::BundleIndexValue::New();
      item->response_locations.push_back(
          data_decoder::mojom::BundleResponseLocation::New(
              0u, response_body_file_size));
      index_.insert({url, std::move(item)});
    }
    in_process_data_decoder_.service()
        .SetWebBundleParserFactoryBinderForTesting(
            base::BindRepeating(&MockParserFactory::BindWebBundleParserFactory,
                                base::Unretained(this)));
  }
  MockParserFactory(
      const std::vector<std::pair<GURL, const std::string&>> items)
      : primary_url_(items[0].first) {
    uint64_t offset = 0;
    for (const auto& item : items) {
      data_decoder::mojom::BundleIndexValuePtr index_value =
          data_decoder::mojom::BundleIndexValue::New();
      index_value->response_locations.push_back(
          data_decoder::mojom::BundleResponseLocation::New(
              offset, item.second.length()));
      offset += item.second.length();
      index_.insert({item.first, std::move(index_value)});
    }
    in_process_data_decoder_.service()
        .SetWebBundleParserFactoryBinderForTesting(
            base::BindRepeating(&MockParserFactory::BindWebBundleParserFactory,
                                base::Unretained(this)));
  }
  ~MockParserFactory() override = default;

  int GetParserCreationCount() const { return parser_creation_count_; }
  void SimulateParserDisconnect() { parser_ = nullptr; }
  void SimulateParseMetadataCrash() { simulate_parse_metadata_crash_ = true; }
  void SimulateParseResponseCrash() { simulate_parse_response_crash_ = true; }

 private:
  void BindWebBundleParserFactory(
      mojo::PendingReceiver<data_decoder::mojom::WebBundleParserFactory>
          receiver) {
    receivers_.Add(this, std::move(receiver));
  }

  // data_decoder::mojom::WebBundleParserFactory implementation.
  void GetParserForFile(
      mojo::PendingReceiver<data_decoder::mojom::WebBundleParser> receiver,
      base::File file) override {
    {
      base::ScopedAllowBlockingForTesting allow_blocking;
      file.Close();
    }
    DCHECK(!parser_);
    parser_ = std::make_unique<MockParser>(
        this, std::move(receiver), index_, primary_url_,
        simulate_parse_metadata_crash_, simulate_parse_response_crash_);
    parser_creation_count_++;
  }

  void GetParserForDataSource(
      mojo::PendingReceiver<data_decoder::mojom::WebBundleParser> receiver,
      mojo::PendingRemote<data_decoder::mojom::BundleDataSource> data_source)
      override {
    DCHECK(!parser_);
    parser_ = std::make_unique<MockParser>(
        this, std::move(receiver), index_, primary_url_,
        simulate_parse_metadata_crash_, simulate_parse_response_crash_);
    parser_creation_count_++;
  }

  data_decoder::test::InProcessDataDecoder in_process_data_decoder_;
  mojo::ReceiverSet<data_decoder::mojom::WebBundleParserFactory> receivers_;
  bool simulate_parse_metadata_crash_ = false;
  bool simulate_parse_response_crash_ = false;
  std::unique_ptr<MockParser> parser_;
  int parser_creation_count_ = 0;
  base::flat_map<GURL, data_decoder::mojom::BundleIndexValuePtr> index_;
  const GURL primary_url_;

  DISALLOW_COPY_AND_ASSIGN(MockParserFactory);
};

void MockParser::ParseMetadata(ParseMetadataCallback callback) {
  if (simulate_parse_metadata_crash_) {
    factory_->SimulateParserDisconnect();
    return;
  }

  base::flat_map<GURL, data_decoder::mojom::BundleIndexValuePtr> items;
  for (const auto& item : index_) {
    items.insert({item.first, item.second.Clone()});
  }

  data_decoder::mojom::BundleMetadataPtr metadata =
      data_decoder::mojom::BundleMetadata::New();
  metadata->primary_url = primary_url_;
  metadata->requests = std::move(items);

  std::move(callback).Run(std::move(metadata), nullptr);
}

void MockParser::ParseResponse(uint64_t response_offset,
                               uint64_t response_length,
                               ParseResponseCallback callback) {
  if (simulate_parse_response_crash_) {
    factory_->SimulateParserDisconnect();
    return;
  }
  data_decoder::mojom::BundleResponsePtr response =
      data_decoder::mojom::BundleResponse::New();
  response->response_code = 200;
  response->response_headers.insert({"content-type", "text/html"});
  response->payload_offset = response_offset;
  response->payload_length = response_length;
  std::move(callback).Run(std::move(response), nullptr);
}

class TestBrowserClient : public ContentBrowserClient {
 public:
  TestBrowserClient() = default;
  ~TestBrowserClient() override = default;
  bool CanAcceptUntrustedExchangesIfNeeded() override { return true; }
  std::string GetAcceptLangs(BrowserContext* context) override {
    return accept_langs_;
  }
  void SetAcceptLangs(const std::string langs) { accept_langs_ = langs; }

 private:
  std::string accept_langs_ = "en";
  DISALLOW_COPY_AND_ASSIGN(TestBrowserClient);
};

ContentBrowserClient* MaybeSetBrowserClientForTesting(
    ContentBrowserClient* browser_client) {
#if defined(OS_ANDROID)
  // TODO(crbug.com/864403): It seems that we call unsupported Android APIs on
  // KitKat when we set a ContentBrowserClient. Don't call such APIs and make
  // this test available on KitKat.
  int32_t major_version = 0, minor_version = 0, bugfix_version = 0;
  base::SysInfo::OperatingSystemVersionNumbers(&major_version, &minor_version,
                                               &bugfix_version);
  if (major_version < 5)
    return nullptr;
#endif  // defined(OS_ANDROID)
  return SetBrowserClientForTesting(browser_client);
}

class WebBundleBrowserTestBase : public ContentBrowserTest {
 protected:
  WebBundleBrowserTestBase() = default;
  ~WebBundleBrowserTestBase() override = default;

  void SetUpOnMainThread() override {
    ContentBrowserTest::SetUpOnMainThread();
    original_client_ = MaybeSetBrowserClientForTesting(&browser_client_);
  }

  void TearDownOnMainThread() override {
    ContentBrowserTest::TearDownOnMainThread();
    if (original_client_)
      SetBrowserClientForTesting(original_client_);
  }

  // Returns false if we cannot override accept languages. It happens only on
  // Android Kitkat or older systems.
  bool SetAcceptLangs(const std::string langs) {
    if (!original_client_)
      return false;

    browser_client_.SetAcceptLangs(langs);
    return true;
  }

  void NavigateAndWaitForTitle(const GURL& test_data_url,
                               const GURL& expected_commit_url,
                               base::StringPiece ascii_title) {
    base::string16 expected_title = base::ASCIIToUTF16(ascii_title);
    TitleWatcher title_watcher(shell()->web_contents(), expected_title);
    EXPECT_TRUE(NavigateToURL(shell()->web_contents(), test_data_url,
                              expected_commit_url));
    EXPECT_EQ(expected_title, title_watcher.WaitAndGetTitle());
  }

  void NavigateToBundleAndWaitForReady(const GURL& test_data_url,
                                       const GURL& expected_commit_url) {
    NavigateAndWaitForTitle(test_data_url, expected_commit_url, "Ready");
  }

  void RunTestScript(const std::string& script) {
    EXPECT_TRUE(ExecuteScript(shell()->web_contents(),
                              "loadScript('" + script + "');"));
    base::string16 ok = base::ASCIIToUTF16("OK");
    TitleWatcher title_watcher(shell()->web_contents(), ok);
    title_watcher.AlsoWaitForTitle(base::ASCIIToUTF16("FAIL"));
    EXPECT_EQ(ok, title_watcher.WaitAndGetTitle());
  }

  void ExecuteScriptAndWaitForTitle(const std::string& script,
                                    const std::string& title) {
    base::string16 title16 = base::ASCIIToUTF16(title);
    TitleWatcher title_watcher(shell()->web_contents(), title16);
    EXPECT_TRUE(ExecuteScript(shell()->web_contents(), script));
    EXPECT_EQ(title16, title_watcher.WaitAndGetTitle());
  }

  void NavigateToURLAndWaitForTitle(const GURL& url, const std::string& title) {
    ExecuteScriptAndWaitForTitle(
        base::StringPrintf("location.href = '%s';", url.spec().c_str()), title);
  }

  ContentBrowserClient* original_client_ = nullptr;

 private:
  TestBrowserClient browser_client_;

  DISALLOW_COPY_AND_ASSIGN(WebBundleBrowserTestBase);
};

class FinishNavigationObserver : public WebContentsObserver {
 public:
  explicit FinishNavigationObserver(WebContents* contents,
                                    base::OnceClosure done_closure)
      : WebContentsObserver(contents), done_closure_(std::move(done_closure)) {}

  void DidFinishNavigation(NavigationHandle* navigation_handle) override {
    error_code_ = navigation_handle->GetNetErrorCode();
    std::move(done_closure_).Run();
  }

  const base::Optional<net::Error>& error_code() const { return error_code_; }

 private:
  base::OnceClosure done_closure_;
  base::Optional<net::Error> error_code_;

  DISALLOW_COPY_AND_ASSIGN(FinishNavigationObserver);
};

std::string ExpectNavigationFailureAndReturnConsoleMessage(
    content::WebContents* web_contents,
    const GURL& url) {
  WebContentsConsoleObserver console_observer(web_contents);
  base::RunLoop run_loop;
  FinishNavigationObserver finish_navigation_observer(web_contents,
                                                      run_loop.QuitClosure());
  EXPECT_FALSE(NavigateToURL(web_contents, url));
  run_loop.Run();
  if (!finish_navigation_observer.error_code()) {
    ADD_FAILURE() << "Unexpected navigation success: " << url;
    return std::string();
  }

  EXPECT_EQ(net::ERR_INVALID_WEB_BUNDLE,
            *finish_navigation_observer.error_code());
  if (console_observer.messages().empty())
    console_observer.Wait();

  if (console_observer.messages().empty()) {
    ADD_FAILURE() << "Could not find a console message.";
    return std::string();
  }
  return base::UTF16ToUTF8(console_observer.messages()[0].message);
}

std::string CreateSimpleWebBundle(const GURL& primary_url) {
  data_decoder::test::WebBundleBuilder builder(primary_url.spec(), "");
  builder.AddExchange(primary_url.spec(),
                      {{":status", "200"}, {"content-type", "text/html"}},
                      "<title>Ready</title>");
  std::vector<uint8_t> bundle = builder.CreateBundle();
  return std::string(bundle.begin(), bundle.end());
}

std::string CreatePathTestWebBundle(
    const net::test_server::EmbeddedTestServer* server) {
  const GURL primary_url = server->GetURL("/web_bundle/path_test/in_scope/");
  data_decoder::test::WebBundleBuilder builder(primary_url.spec(), "");
  builder.AddExchange(primary_url.spec(),
                      {{":status", "200"}, {"content-type", "text/html"}},
                      "<title>Ready</title>");
  builder.AddExchange(
      server->GetURL("/web_bundle/path_test/in_scope/page.html").spec(),
      {{":status", "200"}, {"content-type", "text/html"}},
      "<script>const page_info = 'In scope page in Web Bundle';</script>"
      "<script src=\"page.js\"></script>");
  builder.AddExchange(
      server->GetURL("/web_bundle/path_test/in_scope/page.js").spec(),
      {{":status", "200"}, {"content-type", "application/javascript"}},
      "document.title = page_info + ' / in scope script in Web Bundle';");
  builder.AddExchange(
      server->GetURL("/web_bundle/path_test/out_scope/page.html").spec(),
      {{":status", "200"}, {"content-type", "text/html"}},
      "<script>const page_info = 'Out scope page in Web Bundle';</script>"
      "<script src=\"page.js\"></script>");
  builder.AddExchange(
      server->GetURL("/web_bundle/path_test/out_scope/page.js").spec(),
      {{":status", "200"}, {"content-type", "application/javascript"}},
      "document.title = page_info + ' / out scope script in Web Bundle';");

  std::vector<uint8_t> bundle = builder.CreateBundle();
  return std::string(bundle.begin(), bundle.end());
}

}  // namespace

class InvalidTrustableWebBundleFileUrlBrowserTest : public ContentBrowserTest {
 protected:
  InvalidTrustableWebBundleFileUrlBrowserTest() = default;
  ~InvalidTrustableWebBundleFileUrlBrowserTest() override = default;

  void SetUpOnMainThread() override {
    ContentBrowserTest::SetUpOnMainThread();
    original_client_ = MaybeSetBrowserClientForTesting(&browser_client_);
  }

  void TearDownOnMainThread() override {
    ContentBrowserTest::TearDownOnMainThread();
    if (original_client_)
      SetBrowserClientForTesting(original_client_);
  }

  void SetUpCommandLine(base::CommandLine* command_line) override {
    command_line->AppendSwitchASCII(switches::kTrustableWebBundleFileUrl,
                                    kInvalidFileUrl);
  }

  ContentBrowserClient* original_client_ = nullptr;

 private:
  TestBrowserClient browser_client_;

  DISALLOW_COPY_AND_ASSIGN(InvalidTrustableWebBundleFileUrlBrowserTest);
};

IN_PROC_BROWSER_TEST_F(InvalidTrustableWebBundleFileUrlBrowserTest,
                       NoCrashOnNavigation) {
  // Don't run the test if we couldn't override BrowserClient. It happens only
  // on Android Kitkat or older systems.
  if (!original_client_)
    return;
  base::RunLoop run_loop;
  FinishNavigationObserver finish_navigation_observer(shell()->web_contents(),
                                                      run_loop.QuitClosure());
  EXPECT_FALSE(NavigateToURL(shell()->web_contents(), GURL(kInvalidFileUrl)));
  run_loop.Run();
  ASSERT_TRUE(finish_navigation_observer.error_code());
  EXPECT_EQ(net::ERR_INVALID_URL, *finish_navigation_observer.error_code());
}

class WebBundleTrustableFileBrowserTestBase : public WebBundleBrowserTestBase {
 protected:
  WebBundleTrustableFileBrowserTestBase() = default;
  ~WebBundleTrustableFileBrowserTestBase() override = default;

  void SetUp() override { WebBundleBrowserTestBase::SetUp(); }

  void SetUpCommandLine(base::CommandLine* command_line) override {
    command_line->AppendSwitchASCII(switches::kTrustableWebBundleFileUrl,
                                    test_data_url().spec());
  }

  const GURL& test_data_url() const { return test_data_url_; }
  const GURL& empty_page_url() const { return empty_page_url_; }

  GURL test_data_url_;
  GURL empty_page_url_;

 private:
  DISALLOW_COPY_AND_ASSIGN(WebBundleTrustableFileBrowserTestBase);
};

enum class TestFilePathMode {
  kNormalFilePath,
#if defined(OS_ANDROID)
  kContentURI,
#endif  // OS_ANDROID
};

class WebBundleTrustableFileBrowserTest
    : public testing::WithParamInterface<TestFilePathMode>,
      public WebBundleTrustableFileBrowserTestBase {
 protected:
  WebBundleTrustableFileBrowserTest() {
    if (GetParam() == TestFilePathMode::kNormalFilePath) {
      test_data_url_ =
          net::FilePathToFileURL(GetTestDataPath("web_bundle_browsertest.wbn"));
      empty_page_url_ =
          net::FilePathToFileURL(GetTestDataPath("empty_page.html"));
      return;
    }
#if defined(OS_ANDROID)
    DCHECK_EQ(TestFilePathMode::kContentURI, GetParam());
    test_data_url_ =
        CopyFileAndGetContentUri(GetTestDataPath("web_bundle_browsertest.wbn"));
    empty_page_url_ =
        CopyFileAndGetContentUri(GetTestDataPath("empty_page.html"));
#endif  // OS_ANDROID
  }
  ~WebBundleTrustableFileBrowserTest() override = default;

 private:
  DISALLOW_COPY_AND_ASSIGN(WebBundleTrustableFileBrowserTest);
};

IN_PROC_BROWSER_TEST_P(WebBundleTrustableFileBrowserTest,
                       TrustableWebBundleFile) {
  // Don't run the test if we couldn't override BrowserClient. It happens only
  // on Android Kitkat or older systems.
  if (!original_client_)
    return;
  NavigateToBundleAndWaitForReady(test_data_url(), GURL(kTestPageUrl));
}

IN_PROC_BROWSER_TEST_P(WebBundleTrustableFileBrowserTest, RangeRequest) {
  // Don't run the test if we couldn't override BrowserClient. It happens only
  // on Android Kitkat or older systems.
  if (!original_client_)
    return;

  NavigateToBundleAndWaitForReady(test_data_url(), GURL(kTestPageUrl));
  RunTestScript("test-range-request.js");
}

IN_PROC_BROWSER_TEST_P(WebBundleTrustableFileBrowserTest, Navigations) {
  // Don't run the test if we couldn't override BrowserClient. It happens only
  // on Android Kitkat or older systems.
  if (!original_client_)
    return;

  NavigateToBundleAndWaitForReady(test_data_url(), GURL(kTestPageUrl));
  // Move to page 1.
  NavigateToURLAndWaitForTitle(GURL(kTestPage1Url), "Page 1");
  EXPECT_EQ(shell()->web_contents()->GetLastCommittedURL(),
            GURL(kTestPage1Url));
  // Move to page 2.
  NavigateToURLAndWaitForTitle(GURL(kTestPage2Url), "Page 2");
  EXPECT_EQ(shell()->web_contents()->GetLastCommittedURL(),
            GURL(kTestPage2Url));
  // Back to page 1.
  ExecuteScriptAndWaitForTitle("history.back();", "Page 1");
  EXPECT_EQ(shell()->web_contents()->GetLastCommittedURL(),
            GURL(kTestPage1Url));

  // Back to the initial page.
  ExecuteScriptAndWaitForTitle("history.back();", "Ready");
  EXPECT_EQ(shell()->web_contents()->GetLastCommittedURL(), GURL(kTestPageUrl));

  // Move to page 1.
  ExecuteScriptAndWaitForTitle("history.forward();", "Page 1");
  EXPECT_EQ(shell()->web_contents()->GetLastCommittedURL(),
            GURL(kTestPage1Url));

  // Reload.
  ExecuteScriptAndWaitForTitle("document.title = 'reset';", "reset");
  ExecuteScriptAndWaitForTitle("location.reload();", "Page 1");
  EXPECT_EQ(shell()->web_contents()->GetLastCommittedURL(),
            GURL(kTestPage1Url));

  // Move to page 2.
  ExecuteScriptAndWaitForTitle("history.forward();", "Page 2");
  EXPECT_EQ(shell()->web_contents()->GetLastCommittedURL(),
            GURL(kTestPage2Url));

  // Move out of the Web Bundle.
  NavigateAndWaitForTitle(empty_page_url(), empty_page_url(), "Empty Page");

  // Back to the page 2.
  ExecuteScriptAndWaitForTitle("history.back();", "Page 2");
  EXPECT_EQ(shell()->web_contents()->GetLastCommittedURL(),
            GURL(kTestPage2Url));
}

IN_PROC_BROWSER_TEST_P(WebBundleTrustableFileBrowserTest, NavigationWithHash) {
  // Don't run the test if we couldn't override BrowserClient. It happens only
  // on Android Kitkat or older systems.
  if (!original_client_)
    return;
  NavigateToBundleAndWaitForReady(test_data_url(), GURL(kTestPageUrl));
  NavigateToURLAndWaitForTitle(GURL(kTestPageForHashUrl), "#hello");
}

INSTANTIATE_TEST_SUITE_P(WebBundleTrustableFileBrowserTests,
                         WebBundleTrustableFileBrowserTest,
                         testing::Values(TestFilePathMode::kNormalFilePath
#if defined(OS_ANDROID)
                                         ,
                                         TestFilePathMode::kContentURI
#endif  // OS_ANDROID
                                         ));

class WebBundleTrustableFileNotFoundBrowserTest
    : public WebBundleTrustableFileBrowserTestBase {
 protected:
  WebBundleTrustableFileNotFoundBrowserTest() {
    base::FilePath test_data_dir;
    CHECK(base::PathService::Get(base::DIR_SOURCE_ROOT, &test_data_dir));
    test_data_url_ =
        net::FilePathToFileURL(test_data_dir.AppendASCII("not_found"));
  }
  ~WebBundleTrustableFileNotFoundBrowserTest() override = default;
};

IN_PROC_BROWSER_TEST_F(WebBundleTrustableFileNotFoundBrowserTest, NotFound) {
  // Don't run the test if we couldn't override BrowserClient. It happens only
  // on Android Kitkat or older systems.
  if (!original_client_)
    return;

  std::string console_message = ExpectNavigationFailureAndReturnConsoleMessage(
      shell()->web_contents(), test_data_url());

  EXPECT_EQ("Failed to read metadata of Web Bundle file: FILE_ERROR_FAILED",
            console_message);
}

class WebBundleFileBrowserTest
    : public testing::WithParamInterface<TestFilePathMode>,
      public WebBundleBrowserTestBase {
 protected:
  WebBundleFileBrowserTest() = default;
  ~WebBundleFileBrowserTest() override = default;

  void SetUp() override {
    feature_list_.InitWithFeatures({features::kWebBundles}, {});
    WebBundleBrowserTestBase::SetUp();
  }

  GURL GetTestUrlForFile(base::FilePath file_path) const {
    switch (GetParam()) {
      case TestFilePathMode::kNormalFilePath:
        return net::FilePathToFileURL(file_path);
#if defined(OS_ANDROID)
      case TestFilePathMode::kContentURI:
        return CopyFileAndGetContentUri(file_path);
#endif  // OS_ANDROID
    }
  }

 private:
  base::test::ScopedFeatureList feature_list_;

  DISALLOW_COPY_AND_ASSIGN(WebBundleFileBrowserTest);
};

IN_PROC_BROWSER_TEST_P(WebBundleFileBrowserTest, BasicNavigation) {
  const GURL test_data_url =
      GetTestUrlForFile(GetTestDataPath("web_bundle_browsertest.wbn"));
  NavigateToBundleAndWaitForReady(
      test_data_url, web_bundle_utils::GetSynthesizedUrlForWebBundle(
                         test_data_url, GURL(kTestPageUrl)));
}

IN_PROC_BROWSER_TEST_P(WebBundleFileBrowserTest, Navigations) {
  const GURL test_data_url =
      GetTestUrlForFile(GetTestDataPath("web_bundle_browsertest.wbn"));
  NavigateToBundleAndWaitForReady(
      test_data_url, web_bundle_utils::GetSynthesizedUrlForWebBundle(
                         test_data_url, GURL(kTestPageUrl)));
  // Move to page 1.
  NavigateToURLAndWaitForTitle(GURL(kTestPage1Url), "Page 1");
  EXPECT_EQ(shell()->web_contents()->GetLastCommittedURL(),
            web_bundle_utils::GetSynthesizedUrlForWebBundle(
                test_data_url, GURL(kTestPage1Url)));
  // Move to page 2.
  NavigateToURLAndWaitForTitle(GURL(kTestPage2Url), "Page 2");
  EXPECT_EQ(shell()->web_contents()->GetLastCommittedURL(),
            web_bundle_utils::GetSynthesizedUrlForWebBundle(
                test_data_url, GURL(kTestPage2Url)));

  // Back to page 1.
  ExecuteScriptAndWaitForTitle("history.back();", "Page 1");
  EXPECT_EQ(shell()->web_contents()->GetLastCommittedURL(),
            web_bundle_utils::GetSynthesizedUrlForWebBundle(
                test_data_url, GURL(kTestPage1Url)));
  // Back to the initial page.
  ExecuteScriptAndWaitForTitle("history.back();", "Ready");
  EXPECT_EQ(shell()->web_contents()->GetLastCommittedURL(),
            web_bundle_utils::GetSynthesizedUrlForWebBundle(
                test_data_url, GURL(kTestPageUrl)));

  // Move to page 1.
  ExecuteScriptAndWaitForTitle("history.forward();", "Page 1");
  EXPECT_EQ(shell()->web_contents()->GetLastCommittedURL(),
            web_bundle_utils::GetSynthesizedUrlForWebBundle(
                test_data_url, GURL(kTestPage1Url)));

  // Reload.
  ExecuteScriptAndWaitForTitle("document.title = 'reset';", "reset");
  ExecuteScriptAndWaitForTitle("location.reload();", "Page 1");
  EXPECT_EQ(shell()->web_contents()->GetLastCommittedURL(),
            web_bundle_utils::GetSynthesizedUrlForWebBundle(
                test_data_url, GURL(kTestPage1Url)));

  // Move to page 2.
  ExecuteScriptAndWaitForTitle("history.forward();", "Page 2");
  EXPECT_EQ(shell()->web_contents()->GetLastCommittedURL(),
            web_bundle_utils::GetSynthesizedUrlForWebBundle(
                test_data_url, GURL(kTestPage2Url)));

  const GURL empty_page_url =
      GetTestUrlForFile(GetTestDataPath("empty_page.html"));

  // Move out of the Web Bundle.
  NavigateAndWaitForTitle(empty_page_url, empty_page_url, "Empty Page");

  // Back to the page 2 in the Web Bundle.
  ExecuteScriptAndWaitForTitle("history.back();", "Page 2");
  EXPECT_EQ(shell()->web_contents()->GetLastCommittedURL(),
            web_bundle_utils::GetSynthesizedUrlForWebBundle(
                test_data_url, GURL(kTestPage2Url)));
}

IN_PROC_BROWSER_TEST_P(WebBundleFileBrowserTest, NavigationWithHash) {
  const GURL test_data_url =
      GetTestUrlForFile(GetTestDataPath("web_bundle_browsertest.wbn"));
  NavigateToBundleAndWaitForReady(
      test_data_url, web_bundle_utils::GetSynthesizedUrlForWebBundle(
                         test_data_url, GURL(kTestPageUrl)));
  NavigateToURLAndWaitForTitle(GURL(kTestPageForHashUrl), "#hello");
  EXPECT_EQ(shell()->web_contents()->GetLastCommittedURL(),
            web_bundle_utils::GetSynthesizedUrlForWebBundle(
                test_data_url, GURL(kTestPageForHashUrl)));
}

IN_PROC_BROWSER_TEST_P(WebBundleFileBrowserTest, InvalidWebBundleFile) {
  const GURL test_data_url =
      GetTestUrlForFile(GetTestDataPath("invalid_web_bundle.wbn"));

  std::string console_message = ExpectNavigationFailureAndReturnConsoleMessage(
      shell()->web_contents(), test_data_url);

  EXPECT_EQ("Failed to read metadata of Web Bundle file: Wrong magic bytes.",
            console_message);
}

IN_PROC_BROWSER_TEST_P(WebBundleFileBrowserTest,
                       ResponseParseErrorInMainResource) {
  const GURL test_data_url = GetTestUrlForFile(
      GetTestDataPath("broken_bundle_broken_first_entry.wbn"));

  std::string console_message = ExpectNavigationFailureAndReturnConsoleMessage(
      shell()->web_contents(), test_data_url);

  EXPECT_EQ(
      "Failed to read response header of Web Bundle file: Response headers map "
      "must have exactly one pseudo-header, :status.",
      console_message);
}

IN_PROC_BROWSER_TEST_P(WebBundleFileBrowserTest,
                       ResponseParseErrorInSubresource) {
  const GURL test_data_url = GetTestUrlForFile(
      GetTestDataPath("broken_bundle_broken_script_entry.wbn"));
  NavigateToBundleAndWaitForReady(
      test_data_url, web_bundle_utils::GetSynthesizedUrlForWebBundle(
                         test_data_url, GURL(kTestPageUrl)));

  WebContents* web_contents = shell()->web_contents();
  WebContentsConsoleObserver console_observer(web_contents);

  ExecuteScriptAndWaitForTitle(R"(
    const script = document.createElement("script");
    script.onerror = () => { document.title = "load failed";};
    script.src = "script.js";
    document.body.appendChild(script);)",
                               "load failed");

  if (console_observer.messages().empty())
    console_observer.Wait();

  ASSERT_FALSE(console_observer.messages().empty());
  EXPECT_EQ(
      "Failed to read response header of Web Bundle file: Response headers map "
      "must have exactly one pseudo-header, :status.",
      base::UTF16ToUTF8(console_observer.messages()[0].message));
}

IN_PROC_BROWSER_TEST_P(WebBundleFileBrowserTest, NoLocalFileScheme) {
  const GURL test_data_url =
      GetTestUrlForFile(GetTestDataPath("web_bundle_browsertest.wbn"));
  NavigateToBundleAndWaitForReady(
      test_data_url, web_bundle_utils::GetSynthesizedUrlForWebBundle(
                         test_data_url, GURL(kTestPageUrl)));

  auto expected_title = base::ASCIIToUTF16("load failed");
  TitleWatcher title_watcher(shell()->web_contents(), expected_title);
  title_watcher.AlsoWaitForTitle(base::ASCIIToUTF16("Local Script"));

  const GURL script_file_url =
      net::FilePathToFileURL(GetTestDataPath("local_script.js"));
  const std::string script = base::StringPrintf(R"(
    const script = document.createElement("script");
    script.onerror = () => { document.title = "load failed";};
    script.src = "%s";
    document.body.appendChild(script);)",
                                                script_file_url.spec().c_str());
  EXPECT_TRUE(ExecuteScript(shell()->web_contents(), script));

  EXPECT_EQ(expected_title, title_watcher.WaitAndGetTitle());
}

IN_PROC_BROWSER_TEST_P(WebBundleFileBrowserTest, DataDecoderRestart) {
  // The content of this file will be read as response body of any exchange.
  base::FilePath test_file_path = GetTestDataPath("mocked.wbn");
  MockParserFactory mock_factory(
      {GURL(kTestPageUrl), GURL(kTestPage1Url), GURL(kTestPage2Url)},
      test_file_path);
  const GURL test_data_url = GetTestUrlForFile(test_file_path);
  NavigateAndWaitForTitle(test_data_url,
                          web_bundle_utils::GetSynthesizedUrlForWebBundle(
                              test_data_url, GURL(kTestPageUrl)),
                          kTestPageUrl);

  EXPECT_EQ(1, mock_factory.GetParserCreationCount());
  mock_factory.SimulateParserDisconnect();

  NavigateToURLAndWaitForTitle(GURL(kTestPage1Url), kTestPage1Url);
  EXPECT_EQ(shell()->web_contents()->GetLastCommittedURL(),
            web_bundle_utils::GetSynthesizedUrlForWebBundle(
                test_data_url, GURL(kTestPage1Url)));

  EXPECT_EQ(2, mock_factory.GetParserCreationCount());
  mock_factory.SimulateParserDisconnect();

  NavigateToURLAndWaitForTitle(GURL(kTestPage2Url), kTestPage2Url);
  EXPECT_EQ(shell()->web_contents()->GetLastCommittedURL(),
            web_bundle_utils::GetSynthesizedUrlForWebBundle(
                test_data_url, GURL(kTestPage2Url)));

  EXPECT_EQ(3, mock_factory.GetParserCreationCount());
}

IN_PROC_BROWSER_TEST_P(WebBundleFileBrowserTest, ParseMetadataCrash) {
  base::FilePath test_file_path = GetTestDataPath("mocked.wbn");
  MockParserFactory mock_factory({GURL(kTestPageUrl)}, test_file_path);
  mock_factory.SimulateParseMetadataCrash();

  std::string console_message = ExpectNavigationFailureAndReturnConsoleMessage(
      shell()->web_contents(), GetTestUrlForFile(test_file_path));

  EXPECT_EQ(
      "Failed to read metadata of Web Bundle file: Cannot connect to the "
      "remote parser service",
      console_message);
}

IN_PROC_BROWSER_TEST_P(WebBundleFileBrowserTest, ParseResponseCrash) {
  base::FilePath test_file_path = GetTestDataPath("mocked.wbn");
  MockParserFactory mock_factory({GURL(kTestPageUrl)}, test_file_path);
  mock_factory.SimulateParseResponseCrash();

  std::string console_message = ExpectNavigationFailureAndReturnConsoleMessage(
      shell()->web_contents(), GetTestUrlForFile(test_file_path));

  EXPECT_EQ(
      "Failed to read response header of Web Bundle file: Cannot connect to "
      "the remote parser service",
      console_message);
}

IN_PROC_BROWSER_TEST_P(WebBundleFileBrowserTest, Variants) {
  if (!SetAcceptLangs("ja,en"))
    return;
  const GURL test_data_url =
      GetTestUrlForFile(GetTestDataPath("variants_test.wbn"));
  NavigateAndWaitForTitle(test_data_url,
                          web_bundle_utils::GetSynthesizedUrlForWebBundle(
                              test_data_url, GURL(kTestPageUrl)),
                          "lang=ja");
  ASSERT_TRUE(SetAcceptLangs("en,ja"));
  NavigateAndWaitForTitle(test_data_url,
                          web_bundle_utils::GetSynthesizedUrlForWebBundle(
                              test_data_url, GURL(kTestPageUrl)),
                          "lang=en");

  ExecuteScriptAndWaitForTitle(R"(
    (async function() {
      const headers = {Accept: 'application/octet-stream'};
      const resp = await fetch('/type', {headers});
      const data = await resp.json();
      document.title = data.text;
    })();)",
                               "octet-stream");
  ExecuteScriptAndWaitForTitle(R"(
    (async function() {
      const headers = {Accept: 'application/json'};
      const resp = await fetch('/type', {headers});
      const data = await resp.json();
      document.title = data.text;
    })();)",
                               "json");
  ExecuteScriptAndWaitForTitle(R"(
    (async function() {
      const headers = {Accept: 'foo/bar'};
      const resp = await fetch('/type', {headers});
      const data = await resp.json();
      document.title = data.text;
    })();)",
                               "octet-stream");

  ExecuteScriptAndWaitForTitle(R"(
    (async function() {
      const resp = await fetch('/lang');
      const data = await resp.json();
      document.title = data.text;
    })();)",
                               "ja");
  // If Accept-Language header is explicitly set, respect it.
  ExecuteScriptAndWaitForTitle(R"(
    (async function() {
      const headers = {'Accept-Language': 'fr'};
      const resp = await fetch('/lang', {headers});
      const data = await resp.json();
      document.title = data.text;
    })();)",
                               "fr");
}

INSTANTIATE_TEST_SUITE_P(WebBundleFileBrowserTest,
                         WebBundleFileBrowserTest,
                         testing::Values(TestFilePathMode::kNormalFilePath
#if defined(OS_ANDROID)
                                         ,
                                         TestFilePathMode::kContentURI
#endif  // OS_ANDROID
                                         ));

class WebBundleNetworkBrowserTest : public WebBundleBrowserTestBase {
 protected:
  WebBundleNetworkBrowserTest() = default;
  ~WebBundleNetworkBrowserTest() override = default;

  void SetUpOnMainThread() override {
    WebBundleBrowserTestBase::SetUpOnMainThread();
    host_resolver()->AddRule("*", "127.0.0.1");
  }
  void TearDownOnMainThread() override {
    // Shutdown the server to avoid the data race of |headers_| and |contents_|
    // caused by page reload on error.
    EXPECT_TRUE(embedded_test_server()->ShutdownAndWaitUntilComplete());
    WebBundleBrowserTestBase::TearDownOnMainThread();
  }

  void SetUp() override {
    feature_list_.InitWithFeatures({features::kWebBundlesFromNetwork}, {});
    WebBundleBrowserTestBase::SetUp();
  }

  void RegisterRequestHandler(const std::string& relative_url) {
    embedded_test_server()->RegisterRequestHandler(base::BindLambdaForTesting(
        [this, relative_url](const net::test_server::HttpRequest& request)
            -> std::unique_ptr<net::test_server::HttpResponse> {
          if (request.relative_url != relative_url)
            return nullptr;
          return std::make_unique<net::test_server::RawHttpResponse>(headers_,
                                                                     contents_);
        }));
  }

  void TestNavigationFailure(const GURL& url,
                             const std::string& expected_console_error) {
    std::string console_message =
        ExpectNavigationFailureAndReturnConsoleMessage(shell()->web_contents(),
                                                       url);
    EXPECT_EQ(expected_console_error, console_message);
  }

  void HistoryBackAndWaitUntilConsoleError(
      const std::string& expected_error_message) {
    WebContents* web_contents = shell()->web_contents();
    WebContentsConsoleObserver console_observer(web_contents);

    base::RunLoop run_loop;
    FinishNavigationObserver finish_navigation_observer(web_contents,
                                                        run_loop.QuitClosure());
    EXPECT_TRUE(ExecuteScript(web_contents, "history.back();"));

    run_loop.Run();
    ASSERT_TRUE(finish_navigation_observer.error_code());
    EXPECT_EQ(net::ERR_INVALID_WEB_BUNDLE,
              *finish_navigation_observer.error_code());

    if (console_observer.messages().empty())
      console_observer.Wait();

    ASSERT_FALSE(console_observer.messages().empty());
    EXPECT_EQ(expected_error_message,
              base::UTF16ToUTF8(console_observer.messages()[0].message));
  }

  void SetHeaders(const std::string& headers) { headers_ = headers; }
  void AddHeaders(const std::string& headers) { headers_ += headers; }
  void SetContents(const std::string& contents) { contents_ = contents; }
  const std::string& contents() const { return contents_; }

 private:
  base::test::ScopedFeatureList feature_list_;
  std::string headers_ = kDefaultHeaders;
  std::string contents_;

  DISALLOW_COPY_AND_ASSIGN(WebBundleNetworkBrowserTest);
};

IN_PROC_BROWSER_TEST_F(WebBundleNetworkBrowserTest, Simple) {
  const std::string wbn_path = "/web_bundle/test.wbn";
  const std::string primary_url_path = "/web_bundle/test.html";
  RegisterRequestHandler(wbn_path);
  ASSERT_TRUE(embedded_test_server()->Start());

  const GURL wbn_url = embedded_test_server()->GetURL(wbn_path);
  const GURL primary_url = embedded_test_server()->GetURL(primary_url_path);

  SetContents(CreateSimpleWebBundle(primary_url));
  NavigateToBundleAndWaitForReady(wbn_url, primary_url);
}

IN_PROC_BROWSER_TEST_F(WebBundleNetworkBrowserTest, SimpleWithScript) {
  const std::string wbn_path = "/web_bundle/test.wbn";
  RegisterRequestHandler(wbn_path);
  ASSERT_TRUE(embedded_test_server()->Start());

  const GURL wbn_url = embedded_test_server()->GetURL(wbn_path);
  const GURL primary_url =
      embedded_test_server()->GetURL("/web_bundle/test.html");
  const GURL script_url =
      embedded_test_server()->GetURL("/web_bundle/script.js");

  data_decoder::test::WebBundleBuilder builder(primary_url.spec(), "");
  builder.AddExchange(primary_url.spec(),
                      {{":status", "200"}, {"content-type", "text/html"}},
                      "<script src=\"script.js\"></script>");
  builder.AddExchange(
      script_url.spec(),
      {{":status", "200"}, {"content-type", "application/javascript"}},
      "document.title = 'Ready';");

  std::vector<uint8_t> bundle = builder.CreateBundle();
  SetContents(std::string(bundle.begin(), bundle.end()));
  NavigateToBundleAndWaitForReady(wbn_url, primary_url);
}

IN_PROC_BROWSER_TEST_F(WebBundleNetworkBrowserTest, Download) {
  const std::string wbn_path = "/web_bundle/test.wbn";
  const std::string primary_url_path = "/web_bundle/test.html";
  RegisterRequestHandler(wbn_path);
  AddHeaders("Content-Disposition:attachment; filename=test.wbn\n");
  ASSERT_TRUE(embedded_test_server()->Start());

  const GURL wbn_url = embedded_test_server()->GetURL(wbn_path);
  const GURL primary_url = embedded_test_server()->GetURL(primary_url_path);

  SetContents(CreateSimpleWebBundle(primary_url));
  WebContents* web_contents = shell()->web_contents();
  std::unique_ptr<DownloadObserver> download_observer =
      std::make_unique<DownloadObserver>(BrowserContext::GetDownloadManager(
          web_contents->GetBrowserContext()));

  EXPECT_FALSE(NavigateToURL(web_contents, wbn_url));
  download_observer->WaitUntilDownloadCreated();
  EXPECT_EQ(wbn_url, download_observer->observed_url());
}

IN_PROC_BROWSER_TEST_F(WebBundleNetworkBrowserTest, ContentLength) {
  const std::string wbn_path = "/web_bundle/test.wbn";
  const std::string primary_url_path = "/web_bundle/test.html";
  RegisterRequestHandler(wbn_path);
  ASSERT_TRUE(embedded_test_server()->Start());

  const GURL wbn_url = embedded_test_server()->GetURL(wbn_path);
  const GURL primary_url = embedded_test_server()->GetURL(primary_url_path);

  SetContents(CreateSimpleWebBundle(primary_url));
  AddHeaders(
      base::StringPrintf("Content-Length: %" PRIuS "\n", contents().size()));
  NavigateToBundleAndWaitForReady(wbn_url, primary_url);
}

IN_PROC_BROWSER_TEST_F(WebBundleNetworkBrowserTest, NonSecureUrl) {
  const std::string wbn_path = "/web_bundle/test.wbn";
  const std::string primary_url_path = "/web_bundle/test.html";
  RegisterRequestHandler(wbn_path);
  ASSERT_TRUE(embedded_test_server()->Start());

  const GURL wbn_url = embedded_test_server()->GetURL("example.com", wbn_path);
  const GURL primary_url =
      embedded_test_server()->GetURL("example.com", primary_url_path);
  SetContents(CreateSimpleWebBundle(primary_url));
  TestNavigationFailure(
      wbn_url,
      "Web Bundle response must be served from HTTPS or localhost HTTP.");
}

IN_PROC_BROWSER_TEST_F(WebBundleNetworkBrowserTest, MissingNosniff) {
  const std::string wbn_path = "/web_bundle/test.wbn";
  const std::string primary_url_path = "/web_bundle/test.html";
  RegisterRequestHandler(wbn_path);
  ASSERT_TRUE(embedded_test_server()->Start());

  const GURL wbn_url = embedded_test_server()->GetURL(wbn_path);
  const GURL primary_url = embedded_test_server()->GetURL(primary_url_path);

  SetContents(CreateSimpleWebBundle(primary_url));
  SetHeaders(
      "HTTP/1.1 200 OK\n"
      "Content-Type: application/webbundle\n");
  TestNavigationFailure(wbn_url,
                        "Web Bundle response must have "
                        "\"X-Content-Type-Options: nosniff\" header.");
}

IN_PROC_BROWSER_TEST_F(WebBundleNetworkBrowserTest, PrimaryURLNotFound) {
  const std::string wbn_path = "/web_bundle/test.wbn";
  const std::string primary_url_path = "/web_bundle/test.html";
  RegisterRequestHandler(wbn_path);
  ASSERT_TRUE(embedded_test_server()->Start());

  const GURL wbn_url = embedded_test_server()->GetURL(wbn_path);
  const GURL primary_url = embedded_test_server()->GetURL(primary_url_path);
  const GURL inner_url =
      embedded_test_server()->GetURL("/web_bundle/inner.html");
  data_decoder::test::WebBundleBuilder builder(primary_url.spec(), "");
  builder.AddExchange(inner_url.spec(),
                      {{":status", "200"}, {"content-type", "text/html"}},
                      "<title>Ready</title>");
  std::vector<uint8_t> bundle = builder.CreateBundle();
  SetContents(std::string(bundle.begin(), bundle.end()));
  TestNavigationFailure(
      wbn_url, "The primary URL resource is not found in the web bundle.");
}

IN_PROC_BROWSER_TEST_F(WebBundleNetworkBrowserTest, OriginMismatch) {
  const std::string wbn_path = "/web_bundle/test.wbn";
  const std::string primary_url_path = "/web_bundle/test.html";
  RegisterRequestHandler(wbn_path);
  ASSERT_TRUE(embedded_test_server()->Start());

  const GURL primary_url =
      embedded_test_server()->GetURL("localhost", primary_url_path);

  SetContents(CreateSimpleWebBundle(primary_url));
  TestNavigationFailure(
      embedded_test_server()->GetURL("127.0.0.1", wbn_path),
      "The origin of primary URL doesn't match with the origin of the web "
      "bundle.");
}

IN_PROC_BROWSER_TEST_F(WebBundleNetworkBrowserTest, InvalidFile) {
  const std::string wbn_path = "/web_bundle/test.wbn";
  const std::string primary_url_path = "/web_bundle/test.html";
  RegisterRequestHandler(wbn_path);
  SetContents("This is an invalid Web Bundle file.");
  ASSERT_TRUE(embedded_test_server()->Start());

  TestNavigationFailure(
      embedded_test_server()->GetURL(wbn_path),
      "Failed to read metadata of Web Bundle file: Wrong magic bytes.");
}

IN_PROC_BROWSER_TEST_F(WebBundleNetworkBrowserTest, DataDecoderRestart) {
  const std::string wbn_path = "/web_bundle/test.wbn";
  RegisterRequestHandler(wbn_path);
  ASSERT_TRUE(embedded_test_server()->Start());

  const GURL primary_url =
      embedded_test_server()->GetURL("/web_bundle/test.html");
  const GURL script_url =
      embedded_test_server()->GetURL("/web_bundle/script.js");
  const std::string primary_url_content = "<title>Ready</title>";
  const std::string script_url_content = "document.title = 'OK'";
  SetContents(primary_url_content + script_url_content);

  std::vector<std::pair<GURL, const std::string&>> items = {
      {primary_url, primary_url_content}, {script_url, script_url_content}};
  MockParserFactory mock_factory(items);

  NavigateToBundleAndWaitForReady(embedded_test_server()->GetURL(wbn_path),
                                  primary_url);

  EXPECT_EQ(1, mock_factory.GetParserCreationCount());
  mock_factory.SimulateParserDisconnect();

  ExecuteScriptAndWaitForTitle(R"(
    const script = document.createElement("script");
    script.src = "script.js";
    document.body.appendChild(script);)",
                               "OK");

  EXPECT_EQ(2, mock_factory.GetParserCreationCount());
}

IN_PROC_BROWSER_TEST_F(WebBundleNetworkBrowserTest, ParseMetadataCrash) {
  const std::string wbn_path = "/web_bundle/test.wbn";
  RegisterRequestHandler(wbn_path);
  SetContents("<title>Ready</title>");
  ASSERT_TRUE(embedded_test_server()->Start());

  const GURL primary_url =
      embedded_test_server()->GetURL("/web_bundle/test.html");
  std::vector<std::pair<GURL, const std::string&>> items = {
      {primary_url, contents()}};
  MockParserFactory mock_factory(items);
  mock_factory.SimulateParseMetadataCrash();

  TestNavigationFailure(embedded_test_server()->GetURL(wbn_path),
                        "Failed to read metadata of Web Bundle file: Cannot "
                        "connect to the remote parser service");
}

IN_PROC_BROWSER_TEST_F(WebBundleNetworkBrowserTest, ParseResponseCrash) {
  const std::string wbn_path = "/web_bundle/test.wbn";
  RegisterRequestHandler(wbn_path);
  SetContents("<title>Ready</title>");
  ASSERT_TRUE(embedded_test_server()->Start());

  const GURL primary_url =
      embedded_test_server()->GetURL("/web_bundle/test.html");
  std::vector<std::pair<GURL, const std::string&>> items = {
      {primary_url, contents()}};
  MockParserFactory mock_factory(items);
  mock_factory.SimulateParseResponseCrash();

  TestNavigationFailure(embedded_test_server()->GetURL(wbn_path),
                        "Failed to read response header of Web Bundle file: "
                        "Cannot connect to the remote parser service");
}

IN_PROC_BROWSER_TEST_F(WebBundleNetworkBrowserTest, PathMismatch) {
  const std::string wbn_path = "/web_bundle/test.wbn";
  const std::string primary_url_path = "/other_dir/test.html";
  RegisterRequestHandler(wbn_path);
  ASSERT_TRUE(embedded_test_server()->Start());

  const GURL wbn_url = embedded_test_server()->GetURL(wbn_path);
  const GURL primary_url = embedded_test_server()->GetURL(primary_url_path);

  SetContents(CreateSimpleWebBundle(primary_url));
  TestNavigationFailure(
      wbn_url,
      base::StringPrintf("Path restriction mismatch: Can't navigate to %s in "
                         "the web bundle served from %s.",
                         primary_url.spec().c_str(), wbn_url.spec().c_str()));
}

IN_PROC_BROWSER_TEST_F(WebBundleNetworkBrowserTest, Navigations) {
  const std::string wbn_path = "/web_bundle/path_test/in_scope/path_test.wbn";
  RegisterRequestHandler(wbn_path);
  ASSERT_TRUE(embedded_test_server()->Start());
  SetContents(CreatePathTestWebBundle(embedded_test_server()));

  const GURL wbn_url = embedded_test_server()->GetURL(wbn_path);
  const GURL primary_url =
      embedded_test_server()->GetURL("/web_bundle/path_test/in_scope/");
  NavigateToBundleAndWaitForReady(wbn_url, primary_url);

  NavigateToURLAndWaitForTitle(
      embedded_test_server()->GetURL(
          "/web_bundle/path_test/in_scope/page.html"),
      "In scope page in Web Bundle / in scope script in Web Bundle");
  NavigateToURLAndWaitForTitle(
      embedded_test_server()->GetURL(
          "/web_bundle/path_test/out_scope/page.html"),
      "Out scope page from server / out scope script from server");
  NavigateToURLAndWaitForTitle(
      embedded_test_server()->GetURL(
          "/web_bundle/path_test/in_scope/page.html"),
      "In scope page from server / in scope script from server");
}

IN_PROC_BROWSER_TEST_F(WebBundleNetworkBrowserTest, HistoryNavigations) {
  const std::string wbn_path = "/web_bundle/path_test/in_scope/path_test.wbn";
  RegisterRequestHandler(wbn_path);
  ASSERT_TRUE(embedded_test_server()->Start());
  SetContents(CreatePathTestWebBundle(embedded_test_server()));

  const GURL wbn_url = embedded_test_server()->GetURL(wbn_path);
  const GURL primary_url =
      embedded_test_server()->GetURL("/web_bundle/path_test/in_scope/");
  NavigateToBundleAndWaitForReady(wbn_url, primary_url);

  NavigateToURLAndWaitForTitle(
      embedded_test_server()->GetURL(
          "/web_bundle/path_test/in_scope/page.html"),
      "In scope page in Web Bundle / in scope script in Web Bundle");

  NavigateToURLAndWaitForTitle(
      embedded_test_server()->GetURL("/web_bundle/path_test/in_scope/"),
      "Ready");

  ExecuteScriptAndWaitForTitle(
      "history.back();",
      "In scope page in Web Bundle / in scope script in Web Bundle");
  EXPECT_EQ(shell()->web_contents()->GetLastCommittedURL(),
            embedded_test_server()->GetURL(
                "/web_bundle/path_test/in_scope/page.html"));

  NavigateToURLAndWaitForTitle(
      embedded_test_server()->GetURL(
          "/web_bundle/path_test/out_scope/page.html"),
      "Out scope page from server / out scope script from server");

  ExecuteScriptAndWaitForTitle(
      "history.back();",
      "In scope page in Web Bundle / in scope script in Web Bundle");
  EXPECT_EQ(shell()->web_contents()->GetLastCommittedURL(),
            embedded_test_server()->GetURL(
                "/web_bundle/path_test/in_scope/page.html"));
}

IN_PROC_BROWSER_TEST_F(WebBundleNetworkBrowserTest,
                       HistoryNavigationError_UnexpectedContentType) {
  const std::string wbn_path = "/web_bundle/test.wbn";
  const std::string primary_url_path = "/web_bundle/test.html";
  RegisterRequestHandler(wbn_path);
  ASSERT_TRUE(embedded_test_server()->Start());

  const GURL wbn_url = embedded_test_server()->GetURL(wbn_path);
  const GURL primary_url = embedded_test_server()->GetURL(primary_url_path);

  SetHeaders(
      "HTTP/1.1 200 OK\n"
      "Cache-Control:no-store\n"
      "Content-Type:application/webbundle\n"
      "X-Content-Type-Options: nosniff\n");
  SetContents(CreateSimpleWebBundle(primary_url));
  NavigateToBundleAndWaitForReady(wbn_url, primary_url);
  NavigateToURLAndWaitForTitle(
      embedded_test_server()->GetURL("/web_bundle/empty_page.html"),
      "Empty Page");

  SetHeaders(
      "HTTP/1.1 200 OK\n"
      "Cache-Control:no-store\n"
      "Content-Type:application/foo_bar\n"
      "X-Content-Type-Options: nosniff\n");
  HistoryBackAndWaitUntilConsoleError("Unexpected content type.");
}

IN_PROC_BROWSER_TEST_F(WebBundleNetworkBrowserTest,
                       HistoryNavigationError_MissingNosniff) {
  const std::string wbn_path = "/web_bundle/test.wbn";
  const std::string primary_url_path = "/web_bundle/test.html";
  RegisterRequestHandler(wbn_path);
  ASSERT_TRUE(embedded_test_server()->Start());

  const GURL wbn_url = embedded_test_server()->GetURL(wbn_path);
  const GURL primary_url = embedded_test_server()->GetURL(primary_url_path);

  SetHeaders(
      "HTTP/1.1 200 OK\n"
      "Cache-Control:no-store\n"
      "Content-Type:application/webbundle\n"
      "X-Content-Type-Options: nosniff\n");
  SetContents(CreateSimpleWebBundle(primary_url));
  NavigateToBundleAndWaitForReady(wbn_url, primary_url);
  NavigateToURLAndWaitForTitle(
      embedded_test_server()->GetURL("/web_bundle/empty_page.html"),
      "Empty Page");

  SetHeaders(
      "HTTP/1.1 200 OK\n"
      "Cache-Control:no-store\n"
      "Content-Type:application/webbundle\n");
  HistoryBackAndWaitUntilConsoleError(
      "Web Bundle response must have \"X-Content-Type-Options: nosniff\" "
      "header.");
}

IN_PROC_BROWSER_TEST_F(WebBundleNetworkBrowserTest,
                       HistoryNavigationError_UnexpectedRedirect) {
  const std::string wbn_path = "/web_bundle/test.wbn";
  const std::string primary_url_path = "/web_bundle/test.html";
  RegisterRequestHandler(wbn_path);
  ASSERT_TRUE(embedded_test_server()->Start());

  const GURL wbn_url = embedded_test_server()->GetURL(wbn_path);
  const GURL primary_url = embedded_test_server()->GetURL(primary_url_path);

  SetHeaders(
      "HTTP/1.1 200 OK\n"
      "Cache-Control:no-store\n"
      "Content-Type:application/webbundle\n"
      "X-Content-Type-Options: nosniff\n");
  SetContents(CreateSimpleWebBundle(primary_url));
  NavigateToBundleAndWaitForReady(wbn_url, primary_url);
  NavigateToURLAndWaitForTitle(
      embedded_test_server()->GetURL("/web_bundle/empty_page.html"),
      "Empty Page");

  SetHeaders(
      "HTTP/1.1 302 OK\n"
      "Location:/web_bundle/empty_page.html\n"
      "X-Content-Type-Options: nosniff\n");
  SetContents("");
  HistoryBackAndWaitUntilConsoleError("Unexpected redirect.");
}

IN_PROC_BROWSER_TEST_F(WebBundleNetworkBrowserTest,
                       HistoryNavigationError_ReadMetadataFailure) {
  const std::string wbn_path = "/web_bundle/test.wbn";
  const std::string primary_url_path = "/web_bundle/test.html";
  RegisterRequestHandler(wbn_path);
  ASSERT_TRUE(embedded_test_server()->Start());

  const GURL wbn_url = embedded_test_server()->GetURL(wbn_path);
  const GURL primary_url = embedded_test_server()->GetURL(primary_url_path);

  AddHeaders("Cache-Control:no-store\n");
  SetContents(CreateSimpleWebBundle(primary_url));
  NavigateToBundleAndWaitForReady(wbn_url, primary_url);
  NavigateToURLAndWaitForTitle(
      embedded_test_server()->GetURL("/web_bundle/empty_page.html"),
      "Empty Page");

  SetContents("This is an invalid Web Bundle file.");
  HistoryBackAndWaitUntilConsoleError(
      "Failed to read metadata of Web Bundle file: Wrong magic bytes.");
}

IN_PROC_BROWSER_TEST_F(WebBundleNetworkBrowserTest,
                       HistoryNavigationError_ExpectedUrlNotFound) {
  const std::string wbn_path = "/web_bundle/test.wbn";
  const std::string primary_url_path = "/web_bundle/test.html";
  const std::string alt_primary_url_path = "/web_bundle/alt.html";
  RegisterRequestHandler(wbn_path);
  ASSERT_TRUE(embedded_test_server()->Start());

  const GURL wbn_url = embedded_test_server()->GetURL(wbn_path);
  const GURL primary_url = embedded_test_server()->GetURL(primary_url_path);

  AddHeaders("Cache-Control:no-store\n");
  SetContents(CreateSimpleWebBundle(primary_url));
  NavigateToBundleAndWaitForReady(wbn_url, primary_url);
  NavigateToURLAndWaitForTitle(
      embedded_test_server()->GetURL("/web_bundle/empty_page.html"),
      "Empty Page");

  SetContents(CreateSimpleWebBundle(
      embedded_test_server()->GetURL(alt_primary_url_path)));
  HistoryBackAndWaitUntilConsoleError(
      "The expected URL resource is not found in the web bundle.");
}

}  // namespace content
