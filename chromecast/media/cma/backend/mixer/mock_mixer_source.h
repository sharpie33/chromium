// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMECAST_MEDIA_CMA_BACKEND_MIXER_MOCK_MIXER_SOURCE_H_
#define CHROMECAST_MEDIA_CMA_BACKEND_MIXER_MOCK_MIXER_SOURCE_H_

#include <memory>
#include <string>

#include "base/macros.h"
#include "chromecast/media/cma/backend/mixer/mixer_input.h"
#include "chromecast/public/volume_control.h"
#include "media/audio/audio_device_description.h"
#include "media/base/channel_layout.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace media {
class AudioBus;
}  // namespace media

namespace chromecast {
namespace media {

class MockMixerSource : public MixerInput::Source {
 public:
  using RenderingDelay = MixerInput::RenderingDelay;

  MockMixerSource(int samples_per_second,
                  const std::string& device_id =
                      ::media::AudioDeviceDescription::kDefaultDeviceId);
  ~MockMixerSource() override;

  // MixerInput::Source implementation:
  size_t num_channels() const override { return num_channels_; }
  ::media::ChannelLayout channel_layout() const override {
    return channel_layout_;
  }
  int sample_rate() const override { return samples_per_second_; }
  bool primary() override { return primary_; }
  const std::string& device_id() override { return device_id_; }
  AudioContentType content_type() override { return content_type_; }
  int desired_read_size() override { return 1; }
  int playout_channel() override { return playout_channel_; }
  bool active() override { return true; }

  MOCK_METHOD2(InitializeAudioPlayback, void(int, RenderingDelay));
  MOCK_METHOD3(FillAudioPlaybackFrames,
               int(int, RenderingDelay, ::media::AudioBus*));
  MOCK_METHOD1(OnAudioPlaybackError, void(MixerError));
  MOCK_METHOD0(FinalizeAudioPlayback, void());

  // Setters and getters for test control.
  void SetData(std::unique_ptr<::media::AudioBus> data);

  void set_num_channels(int num_channels) { num_channels_ = num_channels; }
  void set_channel_layout(::media::ChannelLayout channel_layout) {
    channel_layout_ = channel_layout;
  }
  void set_primary(bool primary) { primary_ = primary; }
  void set_content_type(AudioContentType content_type) {
    content_type_ = content_type;
  }
  void set_playout_channel(int channel) { playout_channel_ = channel; }
  void set_multiplier(float multiplier) { multiplier_ = multiplier; }
  float multiplier() const { return multiplier_; }

  const ::media::AudioBus& data() { return *data_; }

 private:
  int GetData(int num_frames,
              RenderingDelay rendering_delay,
              ::media::AudioBus* buffer);

  const int samples_per_second_;
  bool primary_ = true;
  int num_channels_ = 2;
  ::media::ChannelLayout channel_layout_ = ::media::CHANNEL_LAYOUT_STEREO;
  const std::string device_id_;
  AudioContentType content_type_ = AudioContentType::kMedia;
  int playout_channel_ = kChannelAll;
  float multiplier_ = 1.0f;

  std::unique_ptr<::media::AudioBus> data_;
  int data_offset_ = 0;

  DISALLOW_COPY_AND_ASSIGN(MockMixerSource);
};

}  // namespace media
}  // namespace chromecast

#endif  // CHROMECAST_MEDIA_CMA_BACKEND_MIXER_MOCK_MIXER_SOURCE_H_
