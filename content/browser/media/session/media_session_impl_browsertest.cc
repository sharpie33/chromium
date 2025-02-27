// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/media/session/media_session_impl.h"

#include <stddef.h>

#include <list>
#include <vector>

#include "base/macros.h"
#include "base/memory/ptr_util.h"
#include "base/metrics/histogram_samples.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/metrics/histogram_tester.h"
#include "base/test/simple_test_tick_clock.h"
#include "build/build_config.h"
#include "content/browser/frame_host/render_frame_host_impl.h"
#include "content/browser/media/session/audio_focus_delegate.h"
#include "content/browser/media/session/mock_media_session_player_observer.h"
#include "content/browser/media/session/mock_media_session_service_impl.h"
#include "content/browser/web_contents/web_contents_impl.h"
#include "content/public/browser/media_session.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/favicon_url.h"
#include "content/public/test/content_browser_test.h"
#include "content/public/test/content_browser_test_utils.h"
#include "content/shell/browser/shell.h"
#include "media/base/media_content_type.h"
#include "net/base/filename_util.h"
#include "net/dns/mock_host_resolver.h"
#include "services/media_session/public/cpp/test/mock_media_session.h"
#include "services/media_session/public/mojom/audio_focus.mojom.h"
#include "testing/gmock/include/gmock/gmock.h"

using media_session::mojom::AudioFocusType;
using media_session::mojom::MediaPlaybackState;
using media_session::mojom::MediaSessionInfo;

using ::testing::Eq;
using ::testing::Expectation;
using ::testing::NiceMock;
using ::testing::_;

namespace {

const double kDefaultVolumeMultiplier = 1.0;
const double kDuckingVolumeMultiplier = 0.2;
const double kDifferentDuckingVolumeMultiplier = 0.018;

const base::string16 kExpectedSourceTitlePrefix =
    base::ASCIIToUTF16("http://example.com:");

constexpr gfx::Size kDefaultFaviconSize = gfx::Size(16, 16);

class MockAudioFocusDelegate : public content::AudioFocusDelegate {
 public:
  MockAudioFocusDelegate(content::MediaSessionImpl* media_session,
                         bool async_mode)
      : media_session_(media_session), async_mode_(async_mode) {}

  MOCK_METHOD0(AbandonAudioFocus, void());

  AudioFocusDelegate::AudioFocusResult RequestAudioFocus(
      AudioFocusType audio_focus_type) {
    if (async_mode_) {
      requests_.push_back(audio_focus_type);
      return AudioFocusDelegate::AudioFocusResult::kDelayed;
    } else {
      audio_focus_type_ = audio_focus_type;
      return sync_result_;
    }
  }

  base::Optional<AudioFocusType> GetCurrentFocusType() const {
    return audio_focus_type_;
  }

  void MediaSessionInfoChanged(
      media_session::mojom::MediaSessionInfoPtr session_info) override {}

  MOCK_CONST_METHOD0(request_id, const base::UnguessableToken&());

  void ResolveRequest(bool result) {
    if (!async_mode_)
      return;

    audio_focus_type_ = requests_.front();
    requests_.pop_front();

    media_session_->FinishSystemAudioFocusRequest(audio_focus_type_.value(),
                                                  result);
  }

  bool HasRequests() const { return !requests_.empty(); }

  void SetSyncResult(AudioFocusDelegate::AudioFocusResult result) {
    sync_result_ = result;
  }

 private:
  AudioFocusDelegate::AudioFocusResult sync_result_ =
      AudioFocusDelegate::AudioFocusResult::kSuccess;

  content::MediaSessionImpl* media_session_;
  const bool async_mode_ = false;

  std::list<AudioFocusType> requests_;
  base::Optional<AudioFocusType> audio_focus_type_;
};

}  // namespace

namespace content {

class MediaSessionImplBrowserTest : public ContentBrowserTest {
 protected:
  MediaSessionImplBrowserTest() = default;

  void SetUpOnMainThread() override {
    ContentBrowserTest::SetUpOnMainThread();

    // Navigate to a test page with a a real origin.
    ASSERT_TRUE(embedded_test_server()->Start());
    host_resolver()->AddRule("*", "127.0.0.1");
    EXPECT_TRUE(NavigateToURL(shell(), embedded_test_server()->GetURL(
                                           "example.com", "/title1.html")));

    media_session_ = MediaSessionImpl::Get(shell()->web_contents());
    mock_audio_focus_delegate_ = new NiceMock<MockAudioFocusDelegate>(
        media_session_, true /* async_mode */);
    media_session_->SetDelegateForTests(
        base::WrapUnique(mock_audio_focus_delegate_));
    ASSERT_TRUE(media_session_);
  }

  void TearDownOnMainThread() override {
    media_session_->RemoveAllPlayersForTest();
    mock_media_session_service_.reset();

    media_session_ = nullptr;

    ContentBrowserTest::TearDownOnMainThread();
  }

  void StartNewPlayer(MockMediaSessionPlayerObserver* player_observer,
                      media::MediaContentType media_content_type) {
    int player_id = player_observer->StartNewPlayer();

    bool result = AddPlayer(player_observer, player_id, media_content_type);

    EXPECT_TRUE(result);
  }

  bool AddPlayer(MockMediaSessionPlayerObserver* player_observer,
                 int player_id,
                 media::MediaContentType type) {
    return media_session_->AddPlayer(player_observer, player_id, type);
  }

  void RemovePlayer(MockMediaSessionPlayerObserver* player_observer,
                    int player_id) {
    media_session_->RemovePlayer(player_observer, player_id);
  }

  void RemovePlayers(MockMediaSessionPlayerObserver* player_observer) {
    media_session_->RemovePlayers(player_observer);
  }

  void OnPlayerPaused(MockMediaSessionPlayerObserver* player_observer,
                      int player_id) {
    media_session_->OnPlayerPaused(player_observer, player_id);
  }

  void SetPosition(MockMediaSessionPlayerObserver* player_observer,
                   int player_id,
                   media_session::MediaPosition& position) {
    player_observer->SetPosition(player_id, position);
    media_session_->RebuildAndNotifyMediaPositionChanged();
  }

  bool IsActive() { return media_session_->IsActive(); }

  base::Optional<AudioFocusType> GetSessionAudioFocusType() {
    return mock_audio_focus_delegate_->GetCurrentFocusType();
  }

  bool IsControllable() { return media_session_->IsControllable(); }

  void UIResume() { media_session_->Resume(MediaSession::SuspendType::kUI); }

  void SystemResume() {
    media_session_->OnResumeInternal(MediaSession::SuspendType::kSystem);
  }

  void UISuspend() { media_session_->Suspend(MediaSession::SuspendType::kUI); }

  void SystemSuspend(bool temporary) {
    media_session_->OnSuspendInternal(MediaSession::SuspendType::kSystem,
                                      temporary
                                          ? MediaSessionImpl::State::SUSPENDED
                                          : MediaSessionImpl::State::INACTIVE);
  }

  void UISeekForward() {
    media_session_->Seek(base::TimeDelta::FromSeconds(1));
  }

  void UISeekBackward() {
    media_session_->Seek(base::TimeDelta::FromSeconds(-1));
  }

  void SystemStartDucking() { media_session_->StartDucking(); }

  void SystemStopDucking() { media_session_->StopDucking(); }

  void EnsureMediaSessionService() {
    mock_media_session_service_.reset(new NiceMock<MockMediaSessionServiceImpl>(
        shell()->web_contents()->GetMainFrame()));
  }

  void SetPlaybackState(blink::mojom::MediaSessionPlaybackState state) {
    mock_media_session_service_->SetPlaybackState(state);
  }

  void ResolveAudioFocusSuccess() {
    mock_audio_focus_delegate()->ResolveRequest(true /* result */);
  }

  void ResolveAudioFocusFailure() {
    mock_audio_focus_delegate()->ResolveRequest(false /* result */);
  }

  void SetSyncAudioFocusResult(AudioFocusDelegate::AudioFocusResult result) {
    mock_audio_focus_delegate()->SetSyncResult(result);
  }

  bool HasUnresolvedAudioFocusRequest() {
    return mock_audio_focus_delegate()->HasRequests();
  }

  MockAudioFocusDelegate* mock_audio_focus_delegate() {
    return mock_audio_focus_delegate_;
  }

  std::unique_ptr<MediaSessionImpl> CreateDummyMediaSession() {
    return base::WrapUnique<MediaSessionImpl>(
        new MediaSessionImpl(CreateBrowser()->web_contents()));
  }

  MediaSessionUmaHelper* GetMediaSessionUMAHelper() {
    return media_session_->uma_helper_for_test();
  }

  void SetAudioFocusDelegateForTests(MockAudioFocusDelegate* delegate) {
    mock_audio_focus_delegate_ = delegate;
    media_session_->SetDelegateForTests(
        base::WrapUnique(mock_audio_focus_delegate_));
  }

  bool IsDucking() const { return media_session_->is_ducking_; }

  base::string16 GetExpectedSourceTitle() {
    base::string16 expected_title =
        base::StrCat({kExpectedSourceTitlePrefix,
                      base::NumberToString16(embedded_test_server()->port())});

    return expected_title.substr(strlen("http://"));
  }

 protected:
  MediaSessionImpl* media_session_;
  MockAudioFocusDelegate* mock_audio_focus_delegate_;
  std::unique_ptr<MockMediaSessionServiceImpl> mock_media_session_service_;

  DISALLOW_COPY_AND_ASSIGN(MediaSessionImplBrowserTest);
};

class MediaSessionImplParamBrowserTest
    : public MediaSessionImplBrowserTest,
      public testing::WithParamInterface<bool> {
 protected:
  MediaSessionImplParamBrowserTest() = default;

  void SetUpOnMainThread() override {
    MediaSessionImplBrowserTest::SetUpOnMainThread();

    SetAudioFocusDelegateForTests(
        new NiceMock<MockAudioFocusDelegate>(media_session_, GetParam()));
  }
};

class MediaSessionImplSyncBrowserTest : public MediaSessionImplBrowserTest {
 protected:
  MediaSessionImplSyncBrowserTest() = default;

  void SetUpOnMainThread() override {
    MediaSessionImplBrowserTest::SetUpOnMainThread();

    SetAudioFocusDelegateForTests(new NiceMock<MockAudioFocusDelegate>(
        media_session_, false /* async_mode */));
  }
};

INSTANTIATE_TEST_SUITE_P(All,
                         MediaSessionImplParamBrowserTest,
                         testing::Bool());

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       PlayersFromSameObserverDoNotStopEachOtherInSameSession) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  EXPECT_TRUE(player_observer->IsPlaying(0));
  EXPECT_TRUE(player_observer->IsPlaying(1));
  EXPECT_TRUE(player_observer->IsPlaying(2));
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       PlayersFromManyObserverDoNotStopEachOtherInSameSession) {
  auto player_observer_1 = std::make_unique<MockMediaSessionPlayerObserver>();
  auto player_observer_2 = std::make_unique<MockMediaSessionPlayerObserver>();
  auto player_observer_3 = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer_1.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer_2.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer_3.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  EXPECT_TRUE(player_observer_1->IsPlaying(0));
  EXPECT_TRUE(player_observer_2->IsPlaying(0));
  EXPECT_TRUE(player_observer_3->IsPlaying(0));
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       SuspendedMediaSessionStopsPlayers) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  SystemSuspend(true);

  EXPECT_FALSE(player_observer->IsPlaying(0));
  EXPECT_FALSE(player_observer->IsPlaying(1));
  EXPECT_FALSE(player_observer->IsPlaying(2));
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ResumedMediaSessionRestartsPlayers) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  SystemSuspend(true);
  SystemResume();

  EXPECT_TRUE(player_observer->IsPlaying(0));
  EXPECT_TRUE(player_observer->IsPlaying(1));
  EXPECT_TRUE(player_observer->IsPlaying(2));
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       StartedPlayerOnSuspendedSessionPlaysAlone) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  EXPECT_TRUE(player_observer->IsPlaying(0));

  SystemSuspend(true);

  EXPECT_FALSE(player_observer->IsPlaying(0));

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  EXPECT_FALSE(player_observer->IsPlaying(0));
  EXPECT_TRUE(player_observer->IsPlaying(1));

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);

  EXPECT_FALSE(player_observer->IsPlaying(0));
  EXPECT_TRUE(player_observer->IsPlaying(1));
  EXPECT_TRUE(player_observer->IsPlaying(2));
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       InitialVolumeMultiplier) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);

  EXPECT_EQ(kDefaultVolumeMultiplier, player_observer->GetVolumeMultiplier(0));
  EXPECT_EQ(kDefaultVolumeMultiplier, player_observer->GetVolumeMultiplier(1));

  ResolveAudioFocusSuccess();

  EXPECT_EQ(kDefaultVolumeMultiplier, player_observer->GetVolumeMultiplier(0));
  EXPECT_EQ(kDefaultVolumeMultiplier, player_observer->GetVolumeMultiplier(1));
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       StartDuckingReducesVolumeMultiplier) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  SystemStartDucking();

  EXPECT_EQ(kDuckingVolumeMultiplier, player_observer->GetVolumeMultiplier(0));
  EXPECT_EQ(kDuckingVolumeMultiplier, player_observer->GetVolumeMultiplier(1));

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);

  EXPECT_EQ(kDuckingVolumeMultiplier, player_observer->GetVolumeMultiplier(2));
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       StopDuckingRecoversVolumeMultiplier) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  SystemStartDucking();
  SystemStopDucking();

  EXPECT_EQ(kDefaultVolumeMultiplier, player_observer->GetVolumeMultiplier(0));
  EXPECT_EQ(kDefaultVolumeMultiplier, player_observer->GetVolumeMultiplier(1));

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);

  EXPECT_EQ(kDefaultVolumeMultiplier, player_observer->GetVolumeMultiplier(2));
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       DuckingUsesConfiguredMultiplier) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  media_session_->SetDuckingVolumeMultiplier(kDifferentDuckingVolumeMultiplier);
  SystemStartDucking();
  EXPECT_EQ(kDifferentDuckingVolumeMultiplier,
            player_observer->GetVolumeMultiplier(0));
  EXPECT_EQ(kDifferentDuckingVolumeMultiplier,
            player_observer->GetVolumeMultiplier(1));
  SystemStopDucking();
  EXPECT_EQ(kDefaultVolumeMultiplier, player_observer->GetVolumeMultiplier(0));
  EXPECT_EQ(kDefaultVolumeMultiplier, player_observer->GetVolumeMultiplier(1));
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       AudioFocusInitialState) {
  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       AddPlayerOnSuspendedFocusUnducks) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  UISuspend();
  EXPECT_FALSE(IsActive());

  SystemStartDucking();
  EXPECT_EQ(kDuckingVolumeMultiplier, player_observer->GetVolumeMultiplier(0));

  EXPECT_TRUE(
      AddPlayer(player_observer.get(), 0, media::MediaContentType::Persistent));
  ResolveAudioFocusSuccess();
  EXPECT_EQ(kDefaultVolumeMultiplier, player_observer->GetVolumeMultiplier(0));
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       CanRequestFocusBeforePlayerCreation) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  media_session_->RequestSystemAudioFocus(AudioFocusType::kGain);
  EXPECT_TRUE(IsActive());

  ResolveAudioFocusSuccess();
  EXPECT_TRUE(IsActive());

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       StartPlayerGivesFocus) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  EXPECT_TRUE(IsActive());

  ResolveAudioFocusSuccess();
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       SuspendGivesAwayAudioFocus) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  SystemSuspend(true);

  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       StopGivesAwayAudioFocus) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  media_session_->Stop(MediaSession::SuspendType::kUI);

  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       SystemResumeGivesBackAudioFocus) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  SystemSuspend(true);
  SystemResume();

  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       UIResumeGivesBackAudioFocus) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  UISuspend();

  UIResume();
  EXPECT_TRUE(IsActive());

  ResolveAudioFocusSuccess();
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       RemovingLastPlayerDropsAudioFocus) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  RemovePlayer(player_observer.get(), 0);
  EXPECT_TRUE(IsActive());
  RemovePlayer(player_observer.get(), 1);
  EXPECT_TRUE(IsActive());
  RemovePlayer(player_observer.get(), 2);
  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       RemovingLastPlayerFromManyObserversDropsAudioFocus) {
  auto player_observer_1 = std::make_unique<MockMediaSessionPlayerObserver>();
  auto player_observer_2 = std::make_unique<MockMediaSessionPlayerObserver>();
  auto player_observer_3 = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer_1.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer_2.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer_3.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  RemovePlayer(player_observer_1.get(), 0);
  EXPECT_TRUE(IsActive());
  RemovePlayer(player_observer_2.get(), 0);
  EXPECT_TRUE(IsActive());
  RemovePlayer(player_observer_3.get(), 0);
  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       RemovingAllPlayersFromObserversDropsAudioFocus) {
  auto player_observer_1 = std::make_unique<MockMediaSessionPlayerObserver>();
  auto player_observer_2 = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer_1.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer_1.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer_2.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer_2.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  RemovePlayers(player_observer_1.get());
  EXPECT_TRUE(IsActive());
  RemovePlayers(player_observer_2.get());
  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ResumePlayGivesAudioFocus) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  RemovePlayer(player_observer.get(), 0);
  EXPECT_FALSE(IsActive());

  EXPECT_TRUE(
      AddPlayer(player_observer.get(), 0, media::MediaContentType::Persistent));
  ResolveAudioFocusSuccess();
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ResumeSuspendSeekAreSentOnlyOncePerPlayers) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  EXPECT_EQ(0, player_observer->received_suspend_calls());
  EXPECT_EQ(0, player_observer->received_resume_calls());

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);

  EXPECT_EQ(0, player_observer->received_suspend_calls());
  EXPECT_EQ(0, player_observer->received_resume_calls());

  ResolveAudioFocusSuccess();

  EXPECT_EQ(0, player_observer->received_suspend_calls());
  EXPECT_EQ(0, player_observer->received_resume_calls());
  EXPECT_EQ(0, player_observer->received_seek_forward_calls());
  EXPECT_EQ(0, player_observer->received_seek_backward_calls());

  SystemSuspend(true);
  EXPECT_EQ(3, player_observer->received_suspend_calls());

  SystemResume();
  EXPECT_EQ(3, player_observer->received_resume_calls());

  UISeekForward();
  EXPECT_EQ(3, player_observer->received_seek_forward_calls());

  UISeekBackward();
  EXPECT_EQ(3, player_observer->received_seek_backward_calls());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ResumeSuspendSeekAreSentOnlyOncePerPlayersAddedTwice) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  EXPECT_EQ(0, player_observer->received_suspend_calls());
  EXPECT_EQ(0, player_observer->received_resume_calls());

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);

  EXPECT_EQ(0, player_observer->received_suspend_calls());
  EXPECT_EQ(0, player_observer->received_resume_calls());

  ResolveAudioFocusSuccess();

  // Adding the three players above again.
  EXPECT_TRUE(
      AddPlayer(player_observer.get(), 0, media::MediaContentType::Persistent));
  EXPECT_TRUE(
      AddPlayer(player_observer.get(), 1, media::MediaContentType::Persistent));
  EXPECT_TRUE(
      AddPlayer(player_observer.get(), 2, media::MediaContentType::Persistent));

  EXPECT_EQ(0, player_observer->received_suspend_calls());
  EXPECT_EQ(0, player_observer->received_resume_calls());
  EXPECT_EQ(0, player_observer->received_seek_forward_calls());
  EXPECT_EQ(0, player_observer->received_seek_backward_calls());

  SystemSuspend(true);
  EXPECT_EQ(3, player_observer->received_suspend_calls());

  SystemResume();
  EXPECT_EQ(3, player_observer->received_resume_calls());

  UISeekForward();
  EXPECT_EQ(3, player_observer->received_seek_forward_calls());

  UISeekBackward();
  EXPECT_EQ(3, player_observer->received_seek_backward_calls());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       RemovingTheSamePlayerTwiceIsANoop) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  RemovePlayer(player_observer.get(), 0);
  RemovePlayer(player_observer.get(), 0);
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest, AudioFocusType) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  // Starting a player with a given type should set the session to that type.
  StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);
  ResolveAudioFocusSuccess();
  EXPECT_EQ(AudioFocusType::kGainTransientMayDuck, GetSessionAudioFocusType());

  // Adding a player of the same type should have no effect on the type.
  StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);
  EXPECT_FALSE(HasUnresolvedAudioFocusRequest());
  EXPECT_EQ(AudioFocusType::kGainTransientMayDuck, GetSessionAudioFocusType());

  // Adding a player of Content type should override the current type.
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();
  EXPECT_EQ(AudioFocusType::kGain, GetSessionAudioFocusType());

  // Adding a player of the Transient type should have no effect on the type.
  StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);
  EXPECT_FALSE(HasUnresolvedAudioFocusRequest());
  EXPECT_EQ(AudioFocusType::kGain, GetSessionAudioFocusType());

  EXPECT_TRUE(player_observer->IsPlaying(0));
  EXPECT_TRUE(player_observer->IsPlaying(1));
  EXPECT_TRUE(player_observer->IsPlaying(2));
  EXPECT_TRUE(player_observer->IsPlaying(3));

  SystemSuspend(true);

  EXPECT_FALSE(player_observer->IsPlaying(0));
  EXPECT_FALSE(player_observer->IsPlaying(1));
  EXPECT_FALSE(player_observer->IsPlaying(2));
  EXPECT_FALSE(player_observer->IsPlaying(3));

  EXPECT_EQ(AudioFocusType::kGain, GetSessionAudioFocusType());

  SystemResume();

  EXPECT_TRUE(player_observer->IsPlaying(0));
  EXPECT_TRUE(player_observer->IsPlaying(1));
  EXPECT_TRUE(player_observer->IsPlaying(2));
  EXPECT_TRUE(player_observer->IsPlaying(3));

  EXPECT_EQ(AudioFocusType::kGain, GetSessionAudioFocusType());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsShowForContent) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    // Starting a player with a persistent type should show the media controls.
    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsNoShowForTransient) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    // Starting a player with a transient type should not show the media
    // controls.
    StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(false);
  }

  EXPECT_FALSE(IsControllable());
  EXPECT_TRUE(IsActive());
}

// This behaviour is specific to desktop.
#if !defined(OS_ANDROID)

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsNoShowForTransientAndRoutedService) {
  EnsureMediaSessionService();
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>(
      shell()->web_contents()->GetMainFrame());

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    // Starting a player with a transient type should show the media controls.
    StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(false);
  }

  EXPECT_FALSE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsNoShowForTransientAndPlaybackStateNone) {
  EnsureMediaSessionService();
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>(
      shell()->web_contents()->GetMainFrame());

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    // Starting a player with a transient type should not show the media
    // controls.
    StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);
    ResolveAudioFocusSuccess();

    SetPlaybackState(blink::mojom::MediaSessionPlaybackState::NONE);

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(false);
  }

  EXPECT_FALSE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsShowForTransientAndPlaybackStatePaused) {
  EnsureMediaSessionService();
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>(
      shell()->web_contents()->GetMainFrame());

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    // Starting a player with a transient type should show the media controls if
    // we have a playback state from the service.
    StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);
    ResolveAudioFocusSuccess();

    SetPlaybackState(blink::mojom::MediaSessionPlaybackState::PAUSED);

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsShowForTransientAndPlaybackStatePlaying) {
  EnsureMediaSessionService();
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>(
      shell()->web_contents()->GetMainFrame());

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    // Starting a player with a transient type should show the media controls if
    // we have a playback state from the service.
    StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);
    ResolveAudioFocusSuccess();

    SetPlaybackState(blink::mojom::MediaSessionPlaybackState::PLAYING);

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());
}

#endif  // !defined(OS_ANDROID)

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsHideWhenStopped) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  RemovePlayers(player_observer.get());

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    observer.WaitForState(MediaSessionInfo::SessionState::kInactive);
    observer.WaitForControllable(false);

    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }

  EXPECT_FALSE(IsControllable());
  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsShownAcceptTransient) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  // Transient player join the session without affecting the controls.
  StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsShownAfterContentAdded) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(false);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  // The controls are shown when the content player is added.
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsStayIfOnlyOnePlayerHasBeenPaused) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  // Removing only content player doesn't hide the controls since the session
  // is still active.
  RemovePlayer(player_observer.get(), 0);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsHideWhenTheLastPlayerIsRemoved) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);
  }

  RemovePlayer(player_observer.get(), 0);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());

  RemovePlayer(player_observer.get(), 1);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kInactive);
    observer.WaitForControllable(false);
  }

  EXPECT_FALSE(IsControllable());
  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsHideWhenAllThePlayersAreRemoved) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);
  }

  RemovePlayers(player_observer.get());

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kInactive);
    observer.WaitForControllable(false);
  }

  EXPECT_FALSE(IsControllable());
  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsNotHideWhenTheLastPlayerIsPaused) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  OnPlayerPaused(player_observer.get(), 0);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());

  OnPlayerPaused(player_observer.get(), 1);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kSuspended);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       SuspendTemporaryUpdatesControls) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  SystemSuspend(true);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kSuspended);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsUpdatedWhenResumed) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  SystemSuspend(true);
  SystemResume();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsHideWhenSessionSuspendedPermanently) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  SystemSuspend(false);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kInactive);
    observer.WaitForControllable(false);

    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }

  EXPECT_FALSE(IsControllable());
  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsHideWhenSessionStops) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  media_session_->Stop(MediaSession::SuspendType::kUI);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    observer.WaitForState(MediaSessionInfo::SessionState::kInactive);
    observer.WaitForControllable(false);

    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }

  EXPECT_FALSE(IsControllable());
  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsHideWhenSessionChangesFromContentToTransient) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  SystemSuspend(true);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kSuspended);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    // This should reset the session and change it to a transient, so
    // hide the controls.
    StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(false);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  EXPECT_FALSE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsUpdatedWhenNewPlayerResetsSession) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  SystemSuspend(true);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kSuspended);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    // This should reset the session and update the controls.
    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsResumedWhenPlayerIsResumed) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  SystemSuspend(true);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kSuspended);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    // This should resume the session and update the controls.
    AddPlayer(player_observer.get(), 0, media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsUpdatedDueToResumeSessionAction) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  UISuspend();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kSuspended);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsUpdatedDueToSuspendSessionAction) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  UISuspend();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kSuspended);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }

  UIResume();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());

  ResolveAudioFocusSuccess();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsDontShowWhenOneShotIsPresent) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::OneShot);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(false);

    EXPECT_FALSE(IsControllable());
    EXPECT_TRUE(IsActive());
  }

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(false);

    EXPECT_FALSE(IsControllable());
    EXPECT_TRUE(IsActive());
  }

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(false);

    EXPECT_FALSE(IsControllable());
    EXPECT_TRUE(IsActive());
  }
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsHiddenAfterRemoveOneShotWithoutOtherPlayers) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::OneShot);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(false);
  }

  RemovePlayer(player_observer.get(), 0);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kInactive);
    observer.WaitForControllable(false);
  }

  EXPECT_FALSE(IsControllable());
  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsShowAfterRemoveOneShotWithPersistentPresent) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::OneShot);
    StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);
    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(false);
  }

  RemovePlayer(player_observer.get(), 0);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       DontSuspendWhenOneShotIsPresent) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::OneShot);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  SystemSuspend(false);

  EXPECT_FALSE(IsControllable());
  EXPECT_TRUE(IsActive());

  EXPECT_EQ(0, player_observer->received_suspend_calls());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       DontResumeBySystemUISuspendedSessions) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  UISuspend();
  EXPECT_TRUE(IsControllable());
  EXPECT_FALSE(IsActive());

  SystemResume();
  EXPECT_TRUE(IsControllable());
  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       AllowUIResumeForSystemSuspend) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  SystemSuspend(true);
  EXPECT_TRUE(IsControllable());
  EXPECT_FALSE(IsActive());

  UIResume();
  ResolveAudioFocusSuccess();

  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest, ResumeSuspendFromUI) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  UISuspend();
  EXPECT_TRUE(IsControllable());
  EXPECT_FALSE(IsActive());

  UIResume();
  EXPECT_TRUE(IsActive());

  ResolveAudioFocusSuccess();
  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ResumeSuspendFromSystem) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  SystemSuspend(true);
  EXPECT_TRUE(IsControllable());
  EXPECT_FALSE(IsActive());

  SystemResume();
  EXPECT_FALSE(HasUnresolvedAudioFocusRequest());
  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       OneShotTakesGainFocus) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::OneShot);
  ResolveAudioFocusSuccess();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);
  EXPECT_FALSE(HasUnresolvedAudioFocusRequest());

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  EXPECT_FALSE(HasUnresolvedAudioFocusRequest());

  EXPECT_EQ(AudioFocusType::kGain,
            mock_audio_focus_delegate()->GetCurrentFocusType());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       RemovingOneShotDropsFocus) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  EXPECT_CALL(*mock_audio_focus_delegate(), AbandonAudioFocus());
  StartNewPlayer(player_observer.get(), media::MediaContentType::OneShot);
  ResolveAudioFocusSuccess();

  RemovePlayer(player_observer.get(), 0);
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       RemovingOneShotWhileStillHavingOtherPlayersKeepsFocus) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  EXPECT_CALL(*mock_audio_focus_delegate(), AbandonAudioFocus())
      .Times(1);  // Called in TearDown
  StartNewPlayer(player_observer.get(), media::MediaContentType::OneShot);
  ResolveAudioFocusSuccess();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  EXPECT_FALSE(HasUnresolvedAudioFocusRequest());

  RemovePlayer(player_observer.get(), 0);
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ActualPlaybackStateWhilePlayerPaused) {
  EnsureMediaSessionService();
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>(
      shell()->web_contents()->GetMainFrame());

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  OnPlayerPaused(player_observer.get(), 0);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kSuspended);
    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }

  SetPlaybackState(blink::mojom::MediaSessionPlaybackState::PLAYING);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kSuspended);
    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  SetPlaybackState(blink::mojom::MediaSessionPlaybackState::PAUSED);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kSuspended);
    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }

  SetPlaybackState(blink::mojom::MediaSessionPlaybackState::NONE);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kSuspended);
    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ActualPlaybackStateWhilePlayerPlaying) {
  EnsureMediaSessionService();
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>(
      shell()->web_contents()->GetMainFrame());

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  SetPlaybackState(blink::mojom::MediaSessionPlaybackState::PLAYING);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  SetPlaybackState(blink::mojom::MediaSessionPlaybackState::PAUSED);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  SetPlaybackState(blink::mojom::MediaSessionPlaybackState::NONE);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ActualPlaybackStateWhilePlayerRemoved) {
  EnsureMediaSessionService();
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>(
      shell()->web_contents()->GetMainFrame());

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  RemovePlayer(player_observer.get(), 0);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kInactive);
    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }

  SetPlaybackState(blink::mojom::MediaSessionPlaybackState::PLAYING);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kInactive);
    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }

  SetPlaybackState(blink::mojom::MediaSessionPlaybackState::PAUSED);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kInactive);
    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }

  SetPlaybackState(blink::mojom::MediaSessionPlaybackState::NONE);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kInactive);
    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       UMA_Suspended_SystemTransient) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();
  base::HistogramTester tester;

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();
  SystemSuspend(true);

  std::unique_ptr<base::HistogramSamples> samples(
      tester.GetHistogramSamplesSinceCreation("Media.Session.Suspended"));
  EXPECT_EQ(1, samples->TotalCount());
  EXPECT_EQ(1, samples->GetCount(0));  // System Transient
  EXPECT_EQ(0, samples->GetCount(1));  // System Permanent
  EXPECT_EQ(0, samples->GetCount(2));  // UI
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       UMA_Suspended_SystemPermantent) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();
  base::HistogramTester tester;

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();
  SystemSuspend(false);

  std::unique_ptr<base::HistogramSamples> samples(
      tester.GetHistogramSamplesSinceCreation("Media.Session.Suspended"));
  EXPECT_EQ(1, samples->TotalCount());
  EXPECT_EQ(0, samples->GetCount(0));  // System Transient
  EXPECT_EQ(1, samples->GetCount(1));  // System Permanent
  EXPECT_EQ(0, samples->GetCount(2));  // UI
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest, UMA_Suspended_UI) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  base::HistogramTester tester;

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();
  UISuspend();

  std::unique_ptr<base::HistogramSamples> samples(
      tester.GetHistogramSamplesSinceCreation("Media.Session.Suspended"));
  EXPECT_EQ(1, samples->TotalCount());
  EXPECT_EQ(0, samples->GetCount(0));  // System Transient
  EXPECT_EQ(0, samples->GetCount(1));  // System Permanent
  EXPECT_EQ(1, samples->GetCount(2));  // UI
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       UMA_Suspended_Multiple) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();
  base::HistogramTester tester;

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  UISuspend();
  UIResume();
  ResolveAudioFocusSuccess();

  SystemSuspend(true);
  SystemResume();

  UISuspend();
  UIResume();
  ResolveAudioFocusSuccess();

  SystemSuspend(false);

  std::unique_ptr<base::HistogramSamples> samples(
      tester.GetHistogramSamplesSinceCreation("Media.Session.Suspended"));
  EXPECT_EQ(4, samples->TotalCount());
  EXPECT_EQ(1, samples->GetCount(0));  // System Transient
  EXPECT_EQ(1, samples->GetCount(1));  // System Permanent
  EXPECT_EQ(2, samples->GetCount(2));  // UI
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       UMA_Suspended_Crossing) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();
  base::HistogramTester tester;

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  UISuspend();
  SystemSuspend(true);
  SystemSuspend(false);
  UIResume();
  ResolveAudioFocusSuccess();

  SystemSuspend(true);
  SystemSuspend(true);
  SystemSuspend(false);
  SystemResume();

  std::unique_ptr<base::HistogramSamples> samples(
      tester.GetHistogramSamplesSinceCreation("Media.Session.Suspended"));
  EXPECT_EQ(2, samples->TotalCount());
  EXPECT_EQ(1, samples->GetCount(0));  // System Transient
  EXPECT_EQ(0, samples->GetCount(1));  // System Permanent
  EXPECT_EQ(1, samples->GetCount(2));  // UI
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest, UMA_Suspended_Stop) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();
  base::HistogramTester tester;

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();
  media_session_->Stop(MediaSession::SuspendType::kUI);

  std::unique_ptr<base::HistogramSamples> samples(
      tester.GetHistogramSamplesSinceCreation("Media.Session.Suspended"));
  EXPECT_EQ(1, samples->TotalCount());
  EXPECT_EQ(0, samples->GetCount(0));  // System Transient
  EXPECT_EQ(0, samples->GetCount(1));  // System Permanent
  EXPECT_EQ(1, samples->GetCount(2));  // UI
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       UMA_ActiveTime_NoActivation) {
  base::HistogramTester tester;

  std::unique_ptr<MediaSessionImpl> media_session = CreateDummyMediaSession();
  media_session.reset();

  // A MediaSession that wasn't active doesn't register an active time.
  std::unique_ptr<base::HistogramSamples> samples(
      tester.GetHistogramSamplesSinceCreation("Media.Session.ActiveTime"));
  EXPECT_EQ(0, samples->TotalCount());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       UMA_ActiveTime_SimpleActivation) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();
  base::HistogramTester tester;

  MediaSessionUmaHelper* media_session_uma_helper = GetMediaSessionUMAHelper();
  base::SimpleTestTickClock clock;
  clock.SetNowTicks(base::TimeTicks::Now());
  media_session_uma_helper->SetClockForTest(&clock);

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  clock.Advance(base::TimeDelta::FromMilliseconds(1000));
  media_session_->Stop(MediaSession::SuspendType::kUI);

  std::unique_ptr<base::HistogramSamples> samples(
      tester.GetHistogramSamplesSinceCreation("Media.Session.ActiveTime"));
  EXPECT_EQ(1, samples->TotalCount());
  EXPECT_EQ(1, samples->GetCount(1000));
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       UMA_ActiveTime_ActivationWithUISuspension) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();
  base::HistogramTester tester;

  MediaSessionUmaHelper* media_session_uma_helper = GetMediaSessionUMAHelper();
  base::SimpleTestTickClock clock;
  clock.SetNowTicks(base::TimeTicks::Now());
  media_session_uma_helper->SetClockForTest(&clock);

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  clock.Advance(base::TimeDelta::FromMilliseconds(1000));
  UISuspend();

  clock.Advance(base::TimeDelta::FromMilliseconds(2000));
  UIResume();
  ResolveAudioFocusSuccess();

  clock.Advance(base::TimeDelta::FromMilliseconds(1000));
  media_session_->Stop(MediaSession::SuspendType::kUI);

  std::unique_ptr<base::HistogramSamples> samples(
      tester.GetHistogramSamplesSinceCreation("Media.Session.ActiveTime"));
  EXPECT_EQ(1, samples->TotalCount());
  EXPECT_EQ(1, samples->GetCount(2000));
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       UMA_ActiveTime_ActivationWithSystemSuspension) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();
  base::HistogramTester tester;

  MediaSessionUmaHelper* media_session_uma_helper = GetMediaSessionUMAHelper();
  base::SimpleTestTickClock clock;
  clock.SetNowTicks(base::TimeTicks::Now());
  media_session_uma_helper->SetClockForTest(&clock);

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  clock.Advance(base::TimeDelta::FromMilliseconds(1000));
  SystemSuspend(true);

  clock.Advance(base::TimeDelta::FromMilliseconds(2000));
  SystemResume();

  clock.Advance(base::TimeDelta::FromMilliseconds(1000));
  media_session_->Stop(MediaSession::SuspendType::kUI);

  std::unique_ptr<base::HistogramSamples> samples(
      tester.GetHistogramSamplesSinceCreation("Media.Session.ActiveTime"));
  EXPECT_EQ(1, samples->TotalCount());
  EXPECT_EQ(1, samples->GetCount(2000));
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       UMA_ActiveTime_ActivateSuspendedButNotStopped) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();
  base::HistogramTester tester;

  MediaSessionUmaHelper* media_session_uma_helper = GetMediaSessionUMAHelper();
  base::SimpleTestTickClock clock;
  clock.SetNowTicks(base::TimeTicks::Now());
  media_session_uma_helper->SetClockForTest(&clock);

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();
  clock.Advance(base::TimeDelta::FromMilliseconds(500));
  SystemSuspend(true);

  {
    std::unique_ptr<base::HistogramSamples> samples(
        tester.GetHistogramSamplesSinceCreation("Media.Session.ActiveTime"));
    EXPECT_EQ(0, samples->TotalCount());
  }

  SystemResume();
  clock.Advance(base::TimeDelta::FromMilliseconds(5000));
  UISuspend();

  {
    std::unique_ptr<base::HistogramSamples> samples(
        tester.GetHistogramSamplesSinceCreation("Media.Session.ActiveTime"));
    EXPECT_EQ(0, samples->TotalCount());
  }
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       UMA_ActiveTime_ActivateSuspendStopTwice) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();
  base::HistogramTester tester;

  MediaSessionUmaHelper* media_session_uma_helper = GetMediaSessionUMAHelper();
  base::SimpleTestTickClock clock;
  clock.SetNowTicks(base::TimeTicks::Now());
  media_session_uma_helper->SetClockForTest(&clock);

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();
  clock.Advance(base::TimeDelta::FromMilliseconds(500));
  SystemSuspend(true);
  media_session_->Stop(MediaSession::SuspendType::kUI);

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();
  clock.Advance(base::TimeDelta::FromMilliseconds(5000));
  SystemResume();
  media_session_->Stop(MediaSession::SuspendType::kUI);

  std::unique_ptr<base::HistogramSamples> samples(
      tester.GetHistogramSamplesSinceCreation("Media.Session.ActiveTime"));
  EXPECT_EQ(2, samples->TotalCount());
  EXPECT_EQ(1, samples->GetCount(500));
  EXPECT_EQ(1, samples->GetCount(5000));
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       UMA_ActiveTime_MultipleActivations) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();
  base::HistogramTester tester;

  MediaSessionUmaHelper* media_session_uma_helper = GetMediaSessionUMAHelper();
  base::SimpleTestTickClock clock;
  clock.SetNowTicks(base::TimeTicks::Now());
  media_session_uma_helper->SetClockForTest(&clock);

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();
  clock.Advance(base::TimeDelta::FromMilliseconds(10000));
  RemovePlayer(player_observer.get(), 0);

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();
  clock.Advance(base::TimeDelta::FromMilliseconds(1000));
  media_session_->Stop(MediaSession::SuspendType::kUI);

  std::unique_ptr<base::HistogramSamples> samples(
      tester.GetHistogramSamplesSinceCreation("Media.Session.ActiveTime"));
  EXPECT_EQ(2, samples->TotalCount());
  EXPECT_EQ(1, samples->GetCount(1000));
  EXPECT_EQ(1, samples->GetCount(10000));
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       AddingObserverNotifiesCurrentInformation_EmptyInfo) {
  media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

  media_session::MediaMetadata expected_metadata;
  expected_metadata.title = shell()->web_contents()->GetTitle();
  expected_metadata.source_title = GetExpectedSourceTitle();
  observer.WaitForExpectedMetadata(expected_metadata);
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       AddingMojoObserverNotifiesCurrentInformation_WithInfo) {
  // Set up the service and information.
  EnsureMediaSessionService();

  media_session::MediaMetadata expected_metadata;
  expected_metadata.title = base::ASCIIToUTF16("title");
  expected_metadata.artist = base::ASCIIToUTF16("artist");
  expected_metadata.album = base::ASCIIToUTF16("album");
  expected_metadata.source_title = GetExpectedSourceTitle();

  blink::mojom::SpecMediaMetadataPtr spec_metadata(
      blink::mojom::SpecMediaMetadata::New());
  spec_metadata->title = base::ASCIIToUTF16("title");
  spec_metadata->artist = base::ASCIIToUTF16("artist");
  spec_metadata->album = base::ASCIIToUTF16("album");
  mock_media_session_service_->SetMetadata(std::move(spec_metadata));

  // Make sure the service is routed,
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>(
      shell()->web_contents()->GetMainFrame());

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForExpectedMetadata(expected_metadata);
  }
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplSyncBrowserTest,
                       PepperPlayerNotAddedIfFocusFailed) {
  SetSyncAudioFocusResult(AudioFocusDelegate::AudioFocusResult::kFailed);

  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();
  int player_id = player_observer->StartNewPlayer();

  EXPECT_FALSE(AddPlayer(player_observer.get(), player_id,
                         media::MediaContentType::Pepper));

  EXPECT_FALSE(media_session_->HasPepper());
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest, Async_RequestFailure_Gain) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);

  EXPECT_TRUE(player_observer->IsPlaying(0));
  EXPECT_TRUE(player_observer->IsPlaying(1));
  EXPECT_TRUE(IsActive());

  // The gain request failed so we should suspend the whole session.
  ResolveAudioFocusFailure();
  EXPECT_FALSE(player_observer->IsPlaying(0));
  EXPECT_FALSE(player_observer->IsPlaying(1));
  EXPECT_FALSE(IsActive());

  ResolveAudioFocusSuccess();
  EXPECT_FALSE(player_observer->IsPlaying(0));
  EXPECT_FALSE(player_observer->IsPlaying(1));
  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest,
                       Async_RequestFailure_GainTransient) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);

  EXPECT_TRUE(player_observer->IsPlaying(0));
  EXPECT_TRUE(player_observer->IsPlaying(1));
  EXPECT_TRUE(IsActive());

  ResolveAudioFocusSuccess();
  EXPECT_TRUE(player_observer->IsPlaying(0));
  EXPECT_TRUE(player_observer->IsPlaying(1));
  EXPECT_TRUE(IsActive());

  // A transient audio focus failure should only affect transient players.
  ResolveAudioFocusFailure();
  EXPECT_TRUE(player_observer->IsPlaying(0));
  EXPECT_FALSE(player_observer->IsPlaying(1));
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest, Async_GainThenTransient) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);

  EXPECT_TRUE(player_observer->IsPlaying(0));
  EXPECT_TRUE(player_observer->IsPlaying(1));

  ResolveAudioFocusSuccess();
  EXPECT_TRUE(player_observer->IsPlaying(0));
  EXPECT_TRUE(player_observer->IsPlaying(1));

  ResolveAudioFocusSuccess();
  EXPECT_TRUE(player_observer->IsPlaying(0));
  EXPECT_TRUE(player_observer->IsPlaying(1));
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest, Async_TransientThenGain) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);

  EXPECT_TRUE(player_observer->IsPlaying(0));
  EXPECT_TRUE(player_observer->IsPlaying(1));

  ResolveAudioFocusSuccess();
  EXPECT_TRUE(player_observer->IsPlaying(0));
  EXPECT_TRUE(player_observer->IsPlaying(1));

  ResolveAudioFocusSuccess();
  EXPECT_TRUE(player_observer->IsPlaying(0));
  EXPECT_TRUE(player_observer->IsPlaying(1));
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest,
                       Async_SuspendBeforeResolve) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  EXPECT_TRUE(player_observer->IsPlaying(0));

  SystemSuspend(true);
  EXPECT_FALSE(player_observer->IsPlaying(0));
  EXPECT_FALSE(IsActive());

  ResolveAudioFocusSuccess();
  EXPECT_FALSE(player_observer->IsPlaying(0));
  EXPECT_FALSE(IsActive());

  SystemResume();
  EXPECT_TRUE(IsActive());
  EXPECT_TRUE(player_observer->IsPlaying(0));
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest, Async_ResumeBeforeResolve) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  EXPECT_TRUE(IsActive());
  EXPECT_TRUE(player_observer->IsPlaying(0));

  UISuspend();
  EXPECT_FALSE(IsActive());
  EXPECT_FALSE(player_observer->IsPlaying(0));

  UIResume();
  EXPECT_TRUE(IsActive());
  EXPECT_TRUE(player_observer->IsPlaying(0));

  ResolveAudioFocusSuccess();
  EXPECT_TRUE(IsActive());
  EXPECT_TRUE(player_observer->IsPlaying(0));

  ResolveAudioFocusFailure();
  EXPECT_FALSE(IsActive());
  EXPECT_FALSE(player_observer->IsPlaying(0));
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest, Async_RemoveBeforeResolve) {
  {
    auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

    EXPECT_CALL(*mock_audio_focus_delegate(), AbandonAudioFocus());
    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    EXPECT_TRUE(player_observer->IsPlaying(0));

    RemovePlayer(player_observer.get(), 0);
  }

  ResolveAudioFocusSuccess();
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest, Async_StopBeforeResolve) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);
  ResolveAudioFocusSuccess();
  EXPECT_TRUE(player_observer->IsPlaying(0));

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  EXPECT_TRUE(player_observer->IsPlaying(1));

  media_session_->Stop(MediaSession::SuspendType::kUI);
  ResolveAudioFocusSuccess();

  EXPECT_FALSE(player_observer->IsPlaying(0));
  EXPECT_FALSE(player_observer->IsPlaying(1));
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest, Async_Unducking_Failure) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  EXPECT_TRUE(IsActive());
  EXPECT_TRUE(player_observer->IsPlaying(0));

  SystemStartDucking();
  EXPECT_TRUE(IsDucking());

  ResolveAudioFocusFailure();
  EXPECT_TRUE(IsDucking());
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest, Async_Unducking_Inactive) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  EXPECT_TRUE(IsActive());
  EXPECT_TRUE(player_observer->IsPlaying(0));

  media_session_->Stop(MediaSession::SuspendType::kUI);
  SystemStartDucking();
  EXPECT_TRUE(IsDucking());

  ResolveAudioFocusSuccess();
  EXPECT_TRUE(IsDucking());
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest, Async_Unducking_Success) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  EXPECT_TRUE(IsActive());
  EXPECT_TRUE(player_observer->IsPlaying(0));

  SystemStartDucking();
  EXPECT_TRUE(IsDucking());

  ResolveAudioFocusSuccess();
  EXPECT_FALSE(IsDucking());
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest, Async_Unducking_Suspended) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  EXPECT_TRUE(IsActive());
  EXPECT_TRUE(player_observer->IsPlaying(0));

  UISuspend();
  SystemStartDucking();
  EXPECT_TRUE(IsDucking());

  ResolveAudioFocusSuccess();
  EXPECT_TRUE(IsDucking());
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest, MetadataWhenFileUrlScheme) {
  base::FilePath path = GetTestFilePath(nullptr, "title1.html");
  GURL file_url = net::FilePathToFileURL(path);
  EXPECT_TRUE(NavigateToURL(shell(), file_url));

  media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

  media_session::MediaMetadata expected_metadata;
  expected_metadata.title = shell()->web_contents()->GetTitle();
  expected_metadata.source_title = base::ASCIIToUTF16("Local File");
  observer.WaitForExpectedMetadata(expected_metadata);
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest, UpdateFaviconURL) {
  std::vector<gfx::Size> valid_sizes;
  valid_sizes.push_back(gfx::Size(100, 100));
  valid_sizes.push_back(gfx::Size(200, 200));

  std::vector<FaviconURL> favicons;
  favicons.push_back(FaviconURL(GURL("https://www.example.org/favicon1.png"),
                                FaviconURL::IconType::kInvalid, valid_sizes));
  favicons.push_back(
      FaviconURL(GURL(), FaviconURL::IconType::kFavicon, valid_sizes));
  favicons.push_back(FaviconURL(GURL("https://www.example.org/favicon2.png"),
                                FaviconURL::IconType::kFavicon,
                                std::vector<gfx::Size>()));
  favicons.push_back(FaviconURL(GURL("https://www.example.org/favicon3.png"),
                                FaviconURL::IconType::kFavicon, valid_sizes));
  favicons.push_back(FaviconURL(GURL("https://www.example.org/favicon4.png"),
                                FaviconURL::IconType::kTouchIcon, valid_sizes));
  favicons.push_back(FaviconURL(GURL("https://www.example.org/favicon5.png"),
                                FaviconURL::IconType::kTouchPrecomposedIcon,
                                valid_sizes));
  favicons.push_back(FaviconURL(GURL("https://www.example.org/favicon6.png"),
                                FaviconURL::IconType::kTouchIcon,
                                std::vector<gfx::Size>()));

  media_session_->DidUpdateFaviconURL(favicons);

  {
    std::vector<media_session::MediaImage> expected_images;
    media_session::MediaImage test_image_1;
    test_image_1.src = GURL("https://www.example.org/favicon2.png");
    test_image_1.sizes.push_back(kDefaultFaviconSize);
    expected_images.push_back(test_image_1);

    media_session::MediaImage test_image_2;
    test_image_2.src = GURL("https://www.example.org/favicon3.png");
    test_image_2.sizes = valid_sizes;
    expected_images.push_back(test_image_2);

    media_session::MediaImage test_image_3;
    test_image_3.src = GURL("https://www.example.org/favicon4.png");
    test_image_3.sizes = valid_sizes;
    expected_images.push_back(test_image_3);

    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForExpectedImagesOfType(
        media_session::mojom::MediaSessionImageType::kSourceIcon,
        expected_images);
  }

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    media_session_->DidUpdateFaviconURL(std::vector<FaviconURL>());
    observer.WaitForExpectedImagesOfType(
        media_session::mojom::MediaSessionImageType::kSourceIcon,
        std::vector<media_session::MediaImage>());
  }
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest,
                       UpdateFaviconURL_ClearOnNavigate) {
  std::vector<FaviconURL> favicons;
  favicons.push_back(FaviconURL(GURL("https://www.example.org/favicon1.png"),
                                FaviconURL::IconType::kFavicon,
                                std::vector<gfx::Size>()));

  media_session_->DidUpdateFaviconURL(favicons);

  {
    std::vector<media_session::MediaImage> expected_images;
    media_session::MediaImage test_image_1;
    test_image_1.src = GURL("https://www.example.org/favicon1.png");
    test_image_1.sizes.push_back(kDefaultFaviconSize);
    expected_images.push_back(test_image_1);

    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForExpectedImagesOfType(
        media_session::mojom::MediaSessionImageType::kSourceIcon,
        expected_images);
  }

  {
    std::vector<media_session::MediaImage> expected_images;
    media_session::MediaImage test_image_1;
    test_image_1.src =
        embedded_test_server()->GetURL("example.com", "/favicon.ico");
    test_image_1.sizes.push_back(kDefaultFaviconSize);
    expected_images.push_back(test_image_1);

    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    EXPECT_TRUE(NavigateToURL(shell(), embedded_test_server()->GetURL(
                                           "example.com", "/title1.html")));

    observer.WaitForExpectedImagesOfType(
        media_session::mojom::MediaSessionImageType::kSourceIcon,
        expected_images);
  }
}

class MediaSessionFaviconBrowserTest : public ContentBrowserTest {
 protected:
  MediaSessionFaviconBrowserTest() = default;

  void SetUpOnMainThread() override {
    ContentBrowserTest::SetUpOnMainThread();

    host_resolver()->AddRule("*", "127.0.0.1");
  }
};

// Helper class that waits to receive a favicon from the renderer process.
class FaviconWaiter : public WebContentsObserver {
 public:
  explicit FaviconWaiter(WebContents* web_contents)
      : WebContentsObserver(web_contents) {}

  void DidUpdateFaviconURL(const std::vector<FaviconURL>& candidates) override {
    received_favicon_ = true;
    run_loop_.Quit();
  }

  void Wait() {
    if (received_favicon_)
      return;
    run_loop_.Run();
  }

 private:
  bool received_favicon_ = false;
  base::RunLoop run_loop_;
};

IN_PROC_BROWSER_TEST_F(MediaSessionFaviconBrowserTest, StartupInitalization) {
  ASSERT_TRUE(embedded_test_server()->Start());
  EXPECT_TRUE(NavigateToURL(
      shell(), embedded_test_server()->GetURL("example.com", "/title1.html")));

  std::unique_ptr<FaviconWaiter> favicon_waiter(
      new FaviconWaiter(shell()->web_contents()));

  // Insert the favicon dynamically.
  ASSERT_TRUE(content::ExecuteScript(
      shell()->web_contents(),
      "let l = document.createElement('link'); "
      "l.rel='icon'; l.type='image/png'; l.href='single_face.jpg'; "
      "document.head.appendChild(l)"));

  // Wait until it's received by the browser process.
  favicon_waiter->Wait();

  // The MediaSession should be created with the favicon already available.
  MediaSession* media_session = MediaSessionImpl::Get(shell()->web_contents());

  media_session::MediaImage icon;
  icon.src = embedded_test_server()->GetURL("example.com", "/single_face.jpg");
  icon.sizes.push_back({16, 16});

  media_session::test::MockMediaSessionMojoObserver observer(*media_session);
  observer.WaitForExpectedImagesOfType(
      media_session::mojom::MediaSessionImageType::kSourceIcon, {icon});
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest,
                       PositionStateRouteWithTwoPlayers) {
  media_session::MediaPosition expected_position(
      0.0, base::TimeDelta::FromSeconds(10), base::TimeDelta());

  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();
  int player_id = player_observer->StartNewPlayer();
  SetPosition(player_observer.get(), player_id, expected_position);

  {
    // With one normal player we should use the position that one provides.
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    AddPlayer(player_observer.get(), player_id,
              media::MediaContentType::Persistent);
    observer.WaitForExpectedPosition(expected_position);
  }

  int player_id_2 = player_observer->StartNewPlayer();

  {
    // If we add another player then we should become empty again.
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    AddPlayer(player_observer.get(), player_id_2,
              media::MediaContentType::Persistent);
    observer.WaitForEmptyPosition();
  }

  {
    // If we remove the player then we should use the first player position.
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    RemovePlayer(player_observer.get(), player_id_2);
    observer.WaitForExpectedPosition(expected_position);
  }
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest,
                       PositionStateWithOneShotPlayer) {
  media_session::MediaPosition expected_position(
      0.0, base::TimeDelta::FromSeconds(10), base::TimeDelta());

  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();
  int player_id = player_observer->StartNewPlayer();
  SetPosition(player_observer.get(), player_id, expected_position);
  AddPlayer(player_observer.get(), player_id, media::MediaContentType::OneShot);

  // OneShot players should be ignored for position data.
  media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
  observer.WaitForEmptyPosition();
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest,
                       PositionStateWithPepperPlayer) {
  media_session::MediaPosition expected_position(
      0.0, base::TimeDelta::FromSeconds(10), base::TimeDelta());

  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();
  int player_id = player_observer->StartNewPlayer();
  SetPosition(player_observer.get(), player_id, expected_position);
  AddPlayer(player_observer.get(), player_id, media::MediaContentType::Pepper);

  // Pepper players should be ignored for position data.
  media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
  observer.WaitForEmptyPosition();
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest,
                       PositionStateRouteWithTwoPlayers_OneShot) {
  media_session::MediaPosition expected_position(
      0.0, base::TimeDelta::FromSeconds(10), base::TimeDelta());

  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();
  int player_id = player_observer->StartNewPlayer();
  SetPosition(player_observer.get(), player_id, expected_position);

  {
    // With one normal player we should use the position that one provides.
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    AddPlayer(player_observer.get(), player_id,
              media::MediaContentType::Persistent);
    observer.WaitForExpectedPosition(expected_position);
  }

  {
    // If we add an OneShot player then we should become empty again.
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    StartNewPlayer(player_observer.get(), media::MediaContentType::OneShot);
    observer.WaitForEmptyPosition();
  }
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest,
                       PositionStateRouteWithTwoPlayers_Pepper) {
  media_session::MediaPosition expected_position(
      0.0, base::TimeDelta::FromSeconds(10), base::TimeDelta());

  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();
  int player_id = player_observer->StartNewPlayer();
  SetPosition(player_observer.get(), player_id, expected_position);

  {
    // With one normal player we should use the position that one provides.
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    AddPlayer(player_observer.get(), player_id,
              media::MediaContentType::Persistent);
    observer.WaitForExpectedPosition(expected_position);
  }

  {
    // If we add a Papper player then we should become empty again.
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    StartNewPlayer(player_observer.get(), media::MediaContentType::Pepper);
    observer.WaitForEmptyPosition();
  }
}

#if defined(OS_CHROMEOS) || defined(OS_ANDROID)
// TODO(https://crbug.com/1000400): Re-enable this test.
#define MAYBE_PositionStateRouteWithOnePlayer \
  DISABLED_PositionStateRouteWithOnePlayer
#else
#define MAYBE_PositionStateRouteWithOnePlayer PositionStateRouteWithOnePlayer
#endif
IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest,
                       MAYBE_PositionStateRouteWithOnePlayer) {
  EXPECT_TRUE(NavigateToURL(
      shell(), embedded_test_server()->GetURL("example.com",
                                              "/media/session/position.html")));

  auto* main_frame = shell()->web_contents()->GetMainFrame();
  const base::TimeDelta duration = base::TimeDelta::FromMilliseconds(6060);

  {
    // By default we should have an empty position.
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForEmptyPosition();
  }

  {
    // With one normal player we should use the position that one provides.
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    ASSERT_TRUE(
        ExecuteScript(main_frame, "document.getElementById('video').play()"));

    observer.WaitForExpectedPosition(
        media_session::MediaPosition(1.0, duration, base::TimeDelta()));
  }

  {
    // If we seek the player then the position should be updated.
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    ASSERT_TRUE(ExecuteScript(
        main_frame, "document.getElementById('video').currentTime = 1"));

    observer.WaitForExpectedPosition(media_session::MediaPosition(
        1.0, duration, base::TimeDelta::FromSeconds(1)));
  }

  {
    // If we pause the player then the position should be updated.
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    ASSERT_TRUE(
        ExecuteScript(main_frame, "document.getElementById('video').pause()"));

    observer.WaitForExpectedPosition(media_session::MediaPosition(
        0.0, duration, base::TimeDelta::FromSeconds(1)));
  }

  {
    // If we resume the player then the position should be updated.
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    ASSERT_TRUE(
        ExecuteScript(main_frame, "document.getElementById('video').play()"));

    observer.WaitForExpectedPosition(media_session::MediaPosition(
        1.0, duration, base::TimeDelta::FromSeconds(1)));
  }

  {
    // If we change the playback rate then the position should be updated.
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    ASSERT_TRUE(ExecuteScript(
        main_frame, "document.getElementById('video').playbackRate = 2"));

    observer.WaitForExpectedPosition(media_session::MediaPosition(
        2.0, duration, base::TimeDelta::FromSeconds(1)));
  }

  {
    // If we remove the player then we should become empty again.
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    ASSERT_TRUE(
        ExecuteScript(main_frame, "document.getElementById('video').src = ''"));

    observer.WaitForEmptyPosition();
  }
}

}  // namespace content
