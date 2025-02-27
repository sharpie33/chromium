// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_GRAPHICS_ACCELERATED_STATIC_BITMAP_IMAGE_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_GRAPHICS_ACCELERATED_STATIC_BITMAP_IMAGE_H_

#include <memory>

#include "base/memory/weak_ptr.h"
#include "base/single_thread_task_runner.h"
#include "base/threading/thread_checker.h"
#include "third_party/blink/renderer/platform/graphics/mailbox_texture_holder.h"
#include "third_party/blink/renderer/platform/graphics/skia_texture_holder.h"
#include "third_party/blink/renderer/platform/graphics/static_bitmap_image.h"
#include "third_party/blink/renderer/platform/scheduler/public/thread.h"

class GrContext;
struct SkImageInfo;

namespace viz {
class SingleReleaseCallback;
}  // namespace viz

namespace blink {
class WebGraphicsContext3DProviderWrapper;

class PLATFORM_EXPORT AcceleratedStaticBitmapImage final
    : public StaticBitmapImage {
 public:
  ~AcceleratedStaticBitmapImage() override;

  // Creates an image wrapping a shared image mailbox.
  //
  // |sync_token| is the token that must be waited on before reading the
  // contents of this mailbox.
  //
  // |shared_image_texture_id| is an optional texture bound to the shared image
  // mailbox imported into the provided context. If provided the caller must
  // ensure that the texture is bound to the shared image mailbox, stays alive
  // and has a read lock on the shared image until the |release_callback| is
  // invoked.
  //
  // |sk_image_info| provides the metadata associated with the backing.
  //
  // |texture_target| is the target that the texture should be bound to if the
  // backing is used with GL.
  //
  // |is_origin_top_left| indicates whether the origin in texture space
  // corresponds to the top-left content pixel.
  //
  // |context_provider| is the context that the mailbox was created with.
  // |context_thread_ref| and |context_task_runner| refer to the thread the
  // context is bound to. If the image is created on a different thread than
  // |context_thread_ref| then the provided sync_token must be verified and no
  // |shared_image_texture_id| should be provided.
  //
  // |release_callback| is a callback to be invoked when this mailbox can be
  // safely destroyed. It is guaranteed to be invoked on the context thread.
  //
  // Note that it is assumed that the mailbox can only be used for read
  // operations, no writes are allowed.
  static scoped_refptr<AcceleratedStaticBitmapImage> CreateFromCanvasMailbox(
      const gpu::Mailbox&,
      const gpu::SyncToken&,
      GLuint shared_image_texture_id,
      const SkImageInfo& sk_image_info,
      GLenum texture_target,
      bool is_origin_top_left,
      base::WeakPtr<WebGraphicsContext3DProviderWrapper>,
      base::PlatformThreadRef context_thread_ref,
      scoped_refptr<base::SingleThreadTaskRunner> context_task_runner,
      std::unique_ptr<viz::SingleReleaseCallback> release_callback);

  bool CurrentFrameKnownToBeOpaque() override;
  IntSize Size() const override;
  bool IsTextureBacked() const override { return true; }
  scoped_refptr<StaticBitmapImage> ConvertToColorSpace(sk_sp<SkColorSpace>,
                                                       SkColorType) override;

  void Draw(cc::PaintCanvas*,
            const cc::PaintFlags&,
            const FloatRect& dst_rect,
            const FloatRect& src_rect,
            RespectImageOrientationEnum,
            ImageClampingMode,
            ImageDecodingMode) override;

  bool IsValid() const final;
  WebGraphicsContext3DProvider* ContextProvider() const final;
  base::WeakPtr<WebGraphicsContext3DProviderWrapper> ContextProviderWrapper()
      const final;
  scoped_refptr<StaticBitmapImage> MakeUnaccelerated() final;

  bool CopyToTexture(gpu::gles2::GLES2Interface* dest_gl,
                     GLenum dest_target,
                     GLuint dest_texture_id,
                     GLint dest_level,
                     bool unpack_premultiply_alpha,
                     bool unpack_flip_y,
                     const IntPoint& dest_point,
                     const IntRect& source_sub_rectangle) override;
  // To be called on sender thread before performing a transfer to a different
  // thread.
  void Transfer() final;

  void EnsureMailbox(MailboxSyncMode, GLenum filter) final;

  // Updates the sync token that must be waited on before recycling or deleting
  // the mailbox for this image. This must be set by callers using the mailbox
  // externally to this class.
  void UpdateSyncToken(const gpu::SyncToken& sync_token) final {
    mailbox_texture_holder_->UpdateSyncToken(sync_token);
  }

  // Provides the mailbox backing for this image. The caller must wait on the
  // sync token before accessing this mailbox.
  gpu::MailboxHolder GetMailboxHolder() const final;
  bool IsOriginTopLeft() const final {
    return texture_holder()->IsOriginTopLeft();
  }

  PaintImage PaintImageForCurrentFrame() override;

 private:
  AcceleratedStaticBitmapImage(
      const gpu::Mailbox&,
      const gpu::SyncToken&,
      GLuint shared_image_texture_id,
      const SkImageInfo& sk_image_info,
      GLenum texture_target,
      bool is_origin_top_left,
      base::WeakPtr<WebGraphicsContext3DProviderWrapper>,
      base::PlatformThreadRef context_thread_ref,
      scoped_refptr<base::SingleThreadTaskRunner> context_task_runner,
      std::unique_ptr<viz::SingleReleaseCallback> release_callback);

  void CreateImageFromMailboxIfNeeded();
  void WaitSyncTokenIfNeeded();
  void RetainOriginalSkImage();

  // TODO(khushalsagar): Its unclear what to use here for calls checking IsValid
  // or querying the ContextProvider for the image. This can differ in the 2,
  // for instance if the image was transferred between threads.
  const TextureHolder* texture_holder() const {
    if (skia_texture_holder_)
      return skia_texture_holder_.get();
    return mailbox_texture_holder_.get();
  }

  scoped_refptr<TextureHolder::MailboxRef> mailbox_ref_;

  // The image is created with one of the texture holders below while the other
  // one is created lazily if needed and then persisted for the lifetime of the
  // image on a particular thread.
  // When Transfer is called, the image is detached from its current thread to
  // allow it to be used on a different thread. We create(if needed) and cache
  // the mailbox in this case, so the texture can be used with a different
  // context. The skia texture holder is released since the mailbox needs to be
  // imported into the GrContext on the new thread.
  std::unique_ptr<SkiaTextureHolder> skia_texture_holder_;
  std::unique_ptr<MailboxTextureHolder> mailbox_texture_holder_;

  THREAD_CHECKER(thread_checker_);
  PaintImage::ContentId paint_image_content_id_;
};

}  // namespace blink

#endif
