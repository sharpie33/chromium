// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WEBLAYER_BROWSER_DOWNLOAD_IMPL_H_
#define WEBLAYER_BROWSER_DOWNLOAD_IMPL_H_

#include "base/macros.h"
#include "base/memory/weak_ptr.h"
#include "base/supports_user_data.h"
#include "build/build_config.h"
#include "weblayer/public/download.h"

#if defined(OS_ANDROID)
#include "base/android/scoped_java_ref.h"
#endif

namespace download {
class DownloadItem;
}

namespace weblayer {

class DownloadImpl : public Download, public base::SupportsUserData::Data {
 public:
  ~DownloadImpl() override;
  DownloadImpl(const DownloadImpl&) = delete;
  DownloadImpl& operator=(const DownloadImpl&) = delete;

  static void Create(download::DownloadItem* item);
  static DownloadImpl* Get(download::DownloadItem* item);

#if defined(OS_ANDROID)
  void SetJavaDownload(
      JNIEnv* env,
      const base::android::JavaParamRef<jobject>& java_download);
  int GetState(JNIEnv* env, const base::android::JavaParamRef<jobject>& obj) {
    return static_cast<int>(GetState());
  }
  jlong GetTotalBytes(JNIEnv* env,
                      const base::android::JavaParamRef<jobject>& obj) {
    return GetTotalBytes();
  }
  jlong GetReceivedBytes(JNIEnv* env,
                         const base::android::JavaParamRef<jobject>& obj) {
    return GetReceivedBytes();
  }
  void Pause(JNIEnv* env, const base::android::JavaParamRef<jobject>& obj) {
    Pause();
  }
  void Resume(JNIEnv* env, const base::android::JavaParamRef<jobject>& obj) {
    Resume();
  }
  void Cancel(JNIEnv* env, const base::android::JavaParamRef<jobject>& obj) {
    Cancel();
  }
  base::android::ScopedJavaLocalRef<jstring> GetLocation(
      JNIEnv* env,
      const base::android::JavaParamRef<jobject>& obj);
  // Add Impl suffix to avoid compiler clash with the C++ interface method.
  base::android::ScopedJavaLocalRef<jstring> GetMimeTypeImpl(
      JNIEnv* env,
      const base::android::JavaParamRef<jobject>& obj);
  int GetError(JNIEnv* env, const base::android::JavaParamRef<jobject>& obj) {
    return static_cast<int>(GetError());
  }

  base::android::ScopedJavaGlobalRef<jobject> java_download() {
    return java_download_;
  }
#endif

  // Download implementation:
  DownloadState GetState() override;
  int64_t GetTotalBytes() override;
  int64_t GetReceivedBytes() override;
  void Pause() override;
  void Resume() override;
  void Cancel() override;
  base::FilePath GetLocation() override;
  std::string GetMimeType() override;
  DownloadError GetError() override;

 private:
  explicit DownloadImpl(download::DownloadItem* item);

  void PauseInternal();
  void ResumeInternal();
  void CancelInternal();

  download::DownloadItem* item_;
  bool pause_pending_ = false;
  bool resume_pending_ = false;
  bool cancel_pending_ = false;

#if defined(OS_ANDROID)
  base::android::ScopedJavaGlobalRef<jobject> java_download_;
#endif

  base::WeakPtrFactory<DownloadImpl> weak_ptr_factory_{this};
};

}  // namespace weblayer

#endif  // WEBLAYER_BROWSER_DOWNLOAD_IMPL_H_
