// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/webgl/webgl2_rendering_context_base.h"

#include <memory>

#include "base/numerics/checked_math.h"
#include "base/numerics/safe_conversions.h"
#include "base/stl_util.h"
#include "gpu/command_buffer/client/gles2_interface.h"
#include "third_party/blink/public/platform/web_graphics_context_3d_provider.h"
#include "third_party/blink/renderer/bindings/modules/v8/webgl_any.h"
#include "third_party/blink/renderer/core/html/canvas/html_canvas_element.h"
#include "third_party/blink/renderer/core/html/canvas/image_data.h"
#include "third_party/blink/renderer/core/html/html_image_element.h"
#include "third_party/blink/renderer/core/html/media/html_video_element.h"
#include "third_party/blink/renderer/core/imagebitmap/image_bitmap.h"
#include "third_party/blink/renderer/modules/webgl/webgl_active_info.h"
#include "third_party/blink/renderer/modules/webgl/webgl_buffer.h"
#include "third_party/blink/renderer/modules/webgl/webgl_fence_sync.h"
#include "third_party/blink/renderer/modules/webgl/webgl_framebuffer.h"
#include "third_party/blink/renderer/modules/webgl/webgl_program.h"
#include "third_party/blink/renderer/modules/webgl/webgl_query.h"
#include "third_party/blink/renderer/modules/webgl/webgl_renderbuffer.h"
#include "third_party/blink/renderer/modules/webgl/webgl_sampler.h"
#include "third_party/blink/renderer/modules/webgl/webgl_sync.h"
#include "third_party/blink/renderer/modules/webgl/webgl_texture.h"
#include "third_party/blink/renderer/modules/webgl/webgl_transform_feedback.h"
#include "third_party/blink/renderer/modules/webgl/webgl_uniform_location.h"
#include "third_party/blink/renderer/modules/webgl/webgl_vertex_array_object.h"
#include "third_party/blink/renderer/platform/heap/heap.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

using WTF::String;

namespace blink {

namespace {

const GLuint64 kMaxClientWaitTimeout = 0u;

// TODO(kainino): Change outByteLength to GLuint and change the associated
// range checking (and all uses) - overflow becomes possible in cases below
bool ValidateSubSourceAndGetData(DOMArrayBufferView* view,
                                 int64_t sub_offset,
                                 int64_t sub_length,
                                 void** out_base_address,
                                 int64_t* out_byte_length) {
  // This is guaranteed to be non-null by DOM.
  DCHECK(view);

  size_t type_size = view->TypeSize();
  DCHECK_GE(8u, type_size);
  int64_t byte_length = 0;
  if (sub_length) {
    // type size is at most 8, so no overflow.
    byte_length = sub_length * type_size;
  }
  int64_t byte_offset = 0;
  if (sub_offset) {
    // type size is at most 8, so no overflow.
    byte_offset = sub_offset * type_size;
  }
  base::CheckedNumeric<int64_t> total = byte_offset;
  total += byte_length;
  if (!total.IsValid() || total.ValueOrDie() > view->byteLengthAsSizeT()) {
    return false;
  }
  if (!byte_length) {
    byte_length = view->byteLengthAsSizeT() - byte_offset;
  }
  uint8_t* data = static_cast<uint8_t*>(view->BaseAddressMaybeShared());
  data += byte_offset;
  *out_base_address = data;
  *out_byte_length = byte_length;
  return true;
}

class PointableStringArray {
 public:
  PointableStringArray(const Vector<String>& strings)
      : data_(std::make_unique<std::string[]>(strings.size())),
        pointers_(strings.size()) {
    DCHECK(strings.size() < std::numeric_limits<GLsizei>::max());
    for (wtf_size_t i = 0; i < strings.size(); ++i) {
      // Strings must never move once they are stored in data_...
      data_[i] = strings[i].Ascii();
      // ... so that the c_str() remains valid.
      pointers_[i] = data_[i].c_str();
    }
  }

  GLsizei size() const { return pointers_.size(); }
  char const* const* data() const { return pointers_.data(); }

 private:
  std::unique_ptr<std::string[]> data_;
  Vector<const char*> pointers_;
};

}  // namespace

// These enums are from manual pages for glTexStorage2D/glTexStorage3D.
const GLenum kSupportedInternalFormatsStorage[] = {
    GL_R8,
    GL_R8_SNORM,
    GL_R16F,
    GL_R32F,
    GL_R8UI,
    GL_R8I,
    GL_R16UI,
    GL_R16I,
    GL_R32UI,
    GL_R32I,
    GL_RG8,
    GL_RG8_SNORM,
    GL_RG16F,
    GL_RG32F,
    GL_RG8UI,
    GL_RG8I,
    GL_RG16UI,
    GL_RG16I,
    GL_RG32UI,
    GL_RG32I,
    GL_RGB8,
    GL_SRGB8,
    GL_RGB565,
    GL_RGB8_SNORM,
    GL_R11F_G11F_B10F,
    GL_RGB9_E5,
    GL_RGB16F,
    GL_RGB32F,
    GL_RGB8UI,
    GL_RGB8I,
    GL_RGB16UI,
    GL_RGB16I,
    GL_RGB32UI,
    GL_RGB32I,
    GL_RGBA8,
    GL_SRGB8_ALPHA8,
    GL_RGBA8_SNORM,
    GL_RGB5_A1,
    GL_RGBA4,
    GL_RGB10_A2,
    GL_RGBA16F,
    GL_RGBA32F,
    GL_RGBA8UI,
    GL_RGBA8I,
    GL_RGB10_A2UI,
    GL_RGBA16UI,
    GL_RGBA16I,
    GL_RGBA32UI,
    GL_RGBA32I,
    GL_DEPTH_COMPONENT16,
    GL_DEPTH_COMPONENT24,
    GL_DEPTH_COMPONENT32F,
    GL_DEPTH24_STENCIL8,
    GL_DEPTH32F_STENCIL8,
};

WebGL2RenderingContextBase::WebGL2RenderingContextBase(
    CanvasRenderingContextHost* host,
    std::unique_ptr<WebGraphicsContext3DProvider> context_provider,
    bool using_gpu_compositing,
    const CanvasContextCreationAttributesCore& requested_attributes,
    Platform::ContextType context_type)
    : WebGLRenderingContextBase(host,
                                std::move(context_provider),
                                using_gpu_compositing,
                                requested_attributes,
                                context_type) {
  for (size_t i = 0; i < base::size(kSupportedInternalFormatsStorage); ++i) {
    supported_internal_formats_storage_.insert(
        kSupportedInternalFormatsStorage[i]);
  }
}

void WebGL2RenderingContextBase::DestroyContext() {
  WebGLRenderingContextBase::DestroyContext();
}

void WebGL2RenderingContextBase::InitializeNewContext() {
  DCHECK(!isContextLost());
  DCHECK(GetDrawingBuffer());

  read_framebuffer_binding_ = nullptr;

  bound_copy_read_buffer_ = nullptr;
  bound_copy_write_buffer_ = nullptr;
  bound_pixel_pack_buffer_ = nullptr;
  bound_pixel_unpack_buffer_ = nullptr;
  bound_transform_feedback_buffer_ = nullptr;
  bound_uniform_buffer_ = nullptr;

  current_boolean_occlusion_query_ = nullptr;
  current_transform_feedback_primitives_written_query_ = nullptr;
  current_elapsed_query_ = nullptr;

  GLint num_combined_texture_image_units = 0;
  ContextGL()->GetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS,
                           &num_combined_texture_image_units);
  sampler_units_.clear();
  sampler_units_.resize(num_combined_texture_image_units);

  max_transform_feedback_separate_attribs_ = 0;
  // This must be queried before instantiating any transform feedback
  // objects.
  ContextGL()->GetIntegerv(GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS,
                           &max_transform_feedback_separate_attribs_);
  // Create a default transform feedback object so there is a place to
  // hold any bound buffers.
  default_transform_feedback_ = MakeGarbageCollected<WebGLTransformFeedback>(
      this, WebGLTransformFeedback::TFTypeDefault);
  transform_feedback_binding_ = default_transform_feedback_;

  GLint max_uniform_buffer_bindings = 0;
  ContextGL()->GetIntegerv(GL_MAX_UNIFORM_BUFFER_BINDINGS,
                           &max_uniform_buffer_bindings);
  bound_indexed_uniform_buffers_.clear();
  bound_indexed_uniform_buffers_.resize(max_uniform_buffer_bindings);
  max_bound_uniform_buffer_index_ = 0;

  pack_row_length_ = 0;
  pack_skip_pixels_ = 0;
  pack_skip_rows_ = 0;
  unpack_row_length_ = 0;
  unpack_image_height_ = 0;
  unpack_skip_pixels_ = 0;
  unpack_skip_rows_ = 0;
  unpack_skip_images_ = 0;

  WebGLRenderingContextBase::InitializeNewContext();
}

void WebGL2RenderingContextBase::bufferData(
    GLenum target,
    MaybeShared<DOMArrayBufferView> src_data,
    GLenum usage,
    GLuint src_offset,
    GLuint length) {
  if (isContextLost())
    return;
  void* sub_base_address = nullptr;
  int64_t sub_byte_length = 0;
  if (!ValidateSubSourceAndGetData(src_data.View(), src_offset, length,
                                   &sub_base_address, &sub_byte_length)) {
    SynthesizeGLError(GL_INVALID_VALUE, "bufferData",
                      "srcOffset + length too large");
    return;
  }
  BufferDataImpl(target, static_cast<GLsizeiptr>(sub_byte_length),
                 sub_base_address, usage);
}

void WebGL2RenderingContextBase::bufferData(GLenum target,
                                            int64_t size,
                                            GLenum usage) {
  WebGLRenderingContextBase::bufferData(target, size, usage);
}

void WebGL2RenderingContextBase::bufferData(GLenum target,
                                            DOMArrayBuffer* data,
                                            GLenum usage) {
  WebGLRenderingContextBase::bufferData(target, data, usage);
}

void WebGL2RenderingContextBase::bufferData(
    GLenum target,
    MaybeShared<DOMArrayBufferView> data,
    GLenum usage) {
  WebGLRenderingContextBase::bufferData(target, data, usage);
}

void WebGL2RenderingContextBase::bufferSubData(
    GLenum target,
    int64_t dst_byte_offset,
    MaybeShared<DOMArrayBufferView> src_data,
    GLuint src_offset,
    GLuint length) {
  if (isContextLost())
    return;
  void* sub_base_address = nullptr;
  int64_t sub_byte_length = 0;
  if (!ValidateSubSourceAndGetData(src_data.View(), src_offset, length,
                                   &sub_base_address, &sub_byte_length)) {
    SynthesizeGLError(GL_INVALID_VALUE, "bufferSubData",
                      "srcOffset + length too large");
    return;
  }
  BufferSubDataImpl(target, dst_byte_offset,
                    static_cast<GLsizeiptr>(sub_byte_length), sub_base_address);
}

void WebGL2RenderingContextBase::bufferSubData(GLenum target,
                                               int64_t offset,
                                               DOMArrayBuffer* data) {
  WebGLRenderingContextBase::bufferSubData(target, offset, data);
}

void WebGL2RenderingContextBase::bufferSubData(
    GLenum target,
    int64_t offset,
    const FlexibleArrayBufferView& data) {
  WebGLRenderingContextBase::bufferSubData(target, offset, data);
}

void WebGL2RenderingContextBase::copyBufferSubData(GLenum read_target,
                                                   GLenum write_target,
                                                   int64_t read_offset,
                                                   int64_t write_offset,
                                                   int64_t size) {
  if (isContextLost())
    return;

  if (!ValidateValueFitNonNegInt32("copyBufferSubData", "readOffset",
                                   read_offset) ||
      !ValidateValueFitNonNegInt32("copyBufferSubData", "writeOffset",
                                   write_offset) ||
      !ValidateValueFitNonNegInt32("copyBufferSubData", "size", size)) {
    return;
  }

  WebGLBuffer* read_buffer =
      ValidateBufferDataTarget("copyBufferSubData", read_target);
  if (!read_buffer)
    return;

  WebGLBuffer* write_buffer =
      ValidateBufferDataTarget("copyBufferSubData", write_target);
  if (!write_buffer)
    return;

  if (read_offset + size > read_buffer->GetSize() ||
      write_offset + size > write_buffer->GetSize()) {
    SynthesizeGLError(GL_INVALID_VALUE, "copyBufferSubData", "buffer overflow");
    return;
  }

  if ((write_buffer->GetInitialTarget() == GL_ELEMENT_ARRAY_BUFFER &&
       read_buffer->GetInitialTarget() != GL_ELEMENT_ARRAY_BUFFER) ||
      (write_buffer->GetInitialTarget() != GL_ELEMENT_ARRAY_BUFFER &&
       read_buffer->GetInitialTarget() == GL_ELEMENT_ARRAY_BUFFER)) {
    SynthesizeGLError(GL_INVALID_OPERATION, "copyBufferSubData",
                      "Cannot copy into an element buffer destination from a "
                      "non-element buffer source");
    return;
  }

  if (write_buffer->GetInitialTarget() == 0)
    write_buffer->SetInitialTarget(read_buffer->GetInitialTarget());

  ContextGL()->CopyBufferSubData(
      read_target, write_target, static_cast<GLintptr>(read_offset),
      static_cast<GLintptr>(write_offset), static_cast<GLsizeiptr>(size));
}

void WebGL2RenderingContextBase::getBufferSubData(
    GLenum target,
    int64_t src_byte_offset,
    MaybeShared<DOMArrayBufferView> dst_data,
    GLuint dst_offset,
    GLuint length) {
  WebGLBuffer* source_buffer = nullptr;
  void* destination_data_ptr = nullptr;
  int64_t destination_byte_length = 0;
  const char* message = ValidateGetBufferSubData(
      __FUNCTION__, target, src_byte_offset, dst_data.View(), dst_offset,
      length, &source_buffer, &destination_data_ptr, &destination_byte_length);
  if (message) {
    // If there was a GL error, it was already synthesized in
    // validateGetBufferSubData, so it's not done here.
    return;
  }

  // If the length of the copy is zero, this is a no-op.
  if (!destination_byte_length) {
    return;
  }

  void* mapped_data = ContextGL()->MapBufferRange(
      target, static_cast<GLintptr>(src_byte_offset),
      static_cast<GLsizeiptr>(destination_byte_length), GL_MAP_READ_BIT);

  if (!mapped_data)
    return;

  memcpy(destination_data_ptr, mapped_data,
         static_cast<size_t>(destination_byte_length));

  ContextGL()->UnmapBuffer(target);
}

void WebGL2RenderingContextBase::blitFramebuffer(GLint src_x0,
                                                 GLint src_y0,
                                                 GLint src_x1,
                                                 GLint src_y1,
                                                 GLint dst_x0,
                                                 GLint dst_y0,
                                                 GLint dst_x1,
                                                 GLint dst_y1,
                                                 GLbitfield mask,
                                                 GLenum filter) {
  if (isContextLost())
    return;

  bool user_framebuffer_bound = GetFramebufferBinding(GL_DRAW_FRAMEBUFFER);
  DrawingBuffer::ScopedRGBEmulationForBlitFramebuffer emulation(
      GetDrawingBuffer(), user_framebuffer_bound);
  ContextGL()->BlitFramebufferCHROMIUM(src_x0, src_y0, src_x1, src_y1, dst_x0,
                                       dst_y0, dst_x1, dst_y1, mask, filter);
  MarkContextChanged(kCanvasChanged);
}

bool WebGL2RenderingContextBase::ValidateTexFuncLayer(const char* function_name,
                                                      GLenum tex_target,
                                                      GLint layer) {
  if (layer < 0) {
    SynthesizeGLError(GL_INVALID_VALUE, function_name, "layer out of range");
    return false;
  }
  switch (tex_target) {
    case GL_TEXTURE_3D:
      if (layer > max3d_texture_size_ - 1) {
        SynthesizeGLError(GL_INVALID_VALUE, function_name,
                          "layer out of range");
        return false;
      }
      break;
    case GL_TEXTURE_2D_ARRAY:
      if (layer > max_array_texture_layers_ - 1) {
        SynthesizeGLError(GL_INVALID_VALUE, function_name,
                          "layer out of range");
        return false;
      }
      break;
    default:
      NOTREACHED();
      return false;
  }
  return true;
}

void WebGL2RenderingContextBase::framebufferTextureLayer(GLenum target,
                                                         GLenum attachment,
                                                         WebGLTexture* texture,
                                                         GLint level,
                                                         GLint layer) {
  if (isContextLost() ||
      !ValidateFramebufferFuncParameters("framebufferTextureLayer", target,
                                         attachment) ||
      !ValidateNullableWebGLObject("framebufferTextureLayer", texture))
    return;
  GLenum textarget = texture ? texture->GetTarget() : 0;
  if (texture) {
    if (textarget != GL_TEXTURE_3D && textarget != GL_TEXTURE_2D_ARRAY) {
      SynthesizeGLError(GL_INVALID_OPERATION, "framebufferTextureLayer",
                        "invalid texture type");
      return;
    }
    if (!ValidateTexFuncLayer("framebufferTextureLayer", textarget, layer))
      return;
    if (!ValidateTexFuncLevel("framebufferTextureLayer", textarget, level))
      return;
  }

  WebGLFramebuffer* framebuffer_binding = GetFramebufferBinding(target);
  if (!framebuffer_binding || !framebuffer_binding->Object()) {
    SynthesizeGLError(GL_INVALID_OPERATION, "framebufferTextureLayer",
                      "no framebuffer bound");
    return;
  }
  // Don't allow modifications to opaque framebuffer attachements.
  if (framebuffer_binding && framebuffer_binding->Opaque()) {
    SynthesizeGLError(GL_INVALID_OPERATION, "framebufferTextureLayer",
                      "opaque framebuffer bound");
    return;
  }
  framebuffer_binding->SetAttachmentForBoundFramebuffer(
      target, attachment, textarget, texture, level, layer, 0);
  ApplyStencilTest();
}

ScriptValue WebGL2RenderingContextBase::getInternalformatParameter(
    ScriptState* script_state,
    GLenum target,
    GLenum internalformat,
    GLenum pname) {
  if (isContextLost())
    return ScriptValue::CreateNull(script_state->GetIsolate());

  if (target != GL_RENDERBUFFER) {
    SynthesizeGLError(GL_INVALID_ENUM, "getInternalformatParameter",
                      "invalid target");
    return ScriptValue::CreateNull(script_state->GetIsolate());
  }

  switch (internalformat) {
    // Renderbuffer doesn't support unsized internal formats,
    // though GL_RGB and GL_RGBA are color-renderable.
    case GL_RGB:
    case GL_RGBA:
    // Multisampling is not supported for signed and unsigned integer internal
    // formats.
    case GL_R8UI:
    case GL_R8I:
    case GL_R16UI:
    case GL_R16I:
    case GL_R32UI:
    case GL_R32I:
    case GL_RG8UI:
    case GL_RG8I:
    case GL_RG16UI:
    case GL_RG16I:
    case GL_RG32UI:
    case GL_RG32I:
    case GL_RGBA8UI:
    case GL_RGBA8I:
    case GL_RGB10_A2UI:
    case GL_RGBA16UI:
    case GL_RGBA16I:
    case GL_RGBA32UI:
    case GL_RGBA32I:
      return WebGLAny(script_state, DOMInt32Array::Create(0));
    case GL_R8:
    case GL_RG8:
    case GL_RGB8:
    case GL_RGB565:
    case GL_RGBA8:
    case GL_SRGB8_ALPHA8:
    case GL_RGB5_A1:
    case GL_RGBA4:
    case GL_RGB10_A2:
    case GL_DEPTH_COMPONENT16:
    case GL_DEPTH_COMPONENT24:
    case GL_DEPTH_COMPONENT32F:
    case GL_DEPTH24_STENCIL8:
    case GL_DEPTH32F_STENCIL8:
    case GL_STENCIL_INDEX8:
      break;
    case GL_R16F:
    case GL_RG16F:
    case GL_RGBA16F:
    case GL_R32F:
    case GL_RG32F:
    case GL_RGBA32F:
    case GL_R11F_G11F_B10F:
      if (!ExtensionEnabled(kEXTColorBufferFloatName)) {
        SynthesizeGLError(GL_INVALID_ENUM, "getInternalformatParameter",
                          "invalid internalformat when EXT_color_buffer_float "
                          "is not enabled");
        return ScriptValue::CreateNull(script_state->GetIsolate());
      }
      break;
    default:
      SynthesizeGLError(GL_INVALID_ENUM, "getInternalformatParameter",
                        "invalid internalformat");
      return ScriptValue::CreateNull(script_state->GetIsolate());
  }

  switch (pname) {
    case GL_SAMPLES: {
      GLint length = -1;
      ContextGL()->GetInternalformativ(target, internalformat,
                                       GL_NUM_SAMPLE_COUNTS, 1, &length);
      if (length <= 0)
        return WebGLAny(script_state, DOMInt32Array::Create(0));

      auto values = std::make_unique<GLint[]>(length);
      for (GLint ii = 0; ii < length; ++ii)
        values[ii] = 0;
      ContextGL()->GetInternalformativ(target, internalformat, GL_SAMPLES,
                                       length, values.get());
      return WebGLAny(script_state,
                      DOMInt32Array::Create(values.get(), length));
    }
    default:
      SynthesizeGLError(GL_INVALID_ENUM, "getInternalformatParameter",
                        "invalid parameter name");
      return ScriptValue::CreateNull(script_state->GetIsolate());
  }
}

bool WebGL2RenderingContextBase::CheckAndTranslateAttachments(
    const char* function_name,
    GLenum target,
    Vector<GLenum>& attachments) {
  if (!ValidateFramebufferTarget(target)) {
    SynthesizeGLError(GL_INVALID_ENUM, function_name, "invalid target");
    return false;
  }

  WebGLFramebuffer* framebuffer_binding = GetFramebufferBinding(target);
  DCHECK(framebuffer_binding || GetDrawingBuffer());
  if (!framebuffer_binding) {
    // For the default framebuffer, translate GL_COLOR/GL_DEPTH/GL_STENCIL.
    // The default framebuffer of WebGL is not fb 0, it is an internal fbo.
    for (wtf_size_t i = 0; i < attachments.size(); ++i) {
      switch (attachments[i]) {
        case GL_COLOR:
          attachments[i] = GL_COLOR_ATTACHMENT0;
          break;
        case GL_DEPTH:
          attachments[i] = GL_DEPTH_ATTACHMENT;
          break;
        case GL_STENCIL:
          attachments[i] = GL_STENCIL_ATTACHMENT;
          break;
        default:
          SynthesizeGLError(GL_INVALID_ENUM, function_name,
                            "invalid attachment");
          return false;
      }
    }
  }
  return true;
}

IntRect WebGL2RenderingContextBase::GetTextureSourceSubRectangle(
    GLsizei width,
    GLsizei height) {
  return IntRect(unpack_skip_pixels_, unpack_skip_rows_, width, height);
}

void WebGL2RenderingContextBase::invalidateFramebuffer(
    GLenum target,
    const Vector<GLenum>& attachments) {
  if (isContextLost())
    return;

  Vector<GLenum> translated_attachments = attachments;
  if (!CheckAndTranslateAttachments("invalidateFramebuffer", target,
                                    translated_attachments))
    return;
  ContextGL()->InvalidateFramebuffer(target, translated_attachments.size(),
                                     translated_attachments.data());
}

void WebGL2RenderingContextBase::invalidateSubFramebuffer(
    GLenum target,
    const Vector<GLenum>& attachments,
    GLint x,
    GLint y,
    GLsizei width,
    GLsizei height) {
  if (isContextLost())
    return;

  Vector<GLenum> translated_attachments = attachments;
  if (!CheckAndTranslateAttachments("invalidateSubFramebuffer", target,
                                    translated_attachments))
    return;
  ContextGL()->InvalidateSubFramebuffer(target, translated_attachments.size(),
                                        translated_attachments.data(), x, y,
                                        width, height);
}

void WebGL2RenderingContextBase::readBuffer(GLenum mode) {
  if (isContextLost())
    return;

  switch (mode) {
    case GL_BACK:
    case GL_NONE:
    case GL_COLOR_ATTACHMENT0:
      break;
    default:
      if (mode > GL_COLOR_ATTACHMENT0 &&
          mode <
              static_cast<GLenum>(GL_COLOR_ATTACHMENT0 + MaxColorAttachments()))
        break;
      SynthesizeGLError(GL_INVALID_ENUM, "readBuffer", "invalid read buffer");
      return;
  }

  WebGLFramebuffer* read_framebuffer_binding =
      GetFramebufferBinding(GL_READ_FRAMEBUFFER);
  if (!read_framebuffer_binding) {
    DCHECK(GetDrawingBuffer());
    if (mode != GL_BACK && mode != GL_NONE) {
      SynthesizeGLError(GL_INVALID_OPERATION, "readBuffer",
                        "invalid read buffer");
      return;
    }
    read_buffer_of_default_framebuffer_ = mode;
    // translate GL_BACK to GL_COLOR_ATTACHMENT0, because the default
    // framebuffer for WebGL is not fb 0, it is an internal fbo.
    if (mode == GL_BACK)
      mode = GL_COLOR_ATTACHMENT0;
  } else {
    if (mode == GL_BACK) {
      SynthesizeGLError(GL_INVALID_OPERATION, "readBuffer",
                        "invalid read buffer");
      return;
    }
    read_framebuffer_binding->ReadBuffer(mode);
  }
  ContextGL()->ReadBuffer(mode);
}

void WebGL2RenderingContextBase::pixelStorei(GLenum pname, GLint param) {
  if (isContextLost())
    return;
  if (param < 0) {
    SynthesizeGLError(GL_INVALID_VALUE, "pixelStorei", "negative value");
    return;
  }
  switch (pname) {
    case GL_PACK_ROW_LENGTH:
      pack_row_length_ = param;
      break;
    case GL_PACK_SKIP_PIXELS:
      pack_skip_pixels_ = param;
      break;
    case GL_PACK_SKIP_ROWS:
      pack_skip_rows_ = param;
      break;
    case GL_UNPACK_ROW_LENGTH:
      unpack_row_length_ = param;
      break;
    case GL_UNPACK_IMAGE_HEIGHT:
      unpack_image_height_ = param;
      break;
    case GL_UNPACK_SKIP_PIXELS:
      unpack_skip_pixels_ = param;
      break;
    case GL_UNPACK_SKIP_ROWS:
      unpack_skip_rows_ = param;
      break;
    case GL_UNPACK_SKIP_IMAGES:
      unpack_skip_images_ = param;
      break;
    default:
      WebGLRenderingContextBase::pixelStorei(pname, param);
      return;
  }
  ContextGL()->PixelStorei(pname, param);
}

void WebGL2RenderingContextBase::readPixels(
    GLint x,
    GLint y,
    GLsizei width,
    GLsizei height,
    GLenum format,
    GLenum type,
    MaybeShared<DOMArrayBufferView> pixels) {
  if (isContextLost())
    return;
  if (bound_pixel_pack_buffer_.Get()) {
    SynthesizeGLError(GL_INVALID_OPERATION, "readPixels",
                      "PIXEL_PACK buffer should not be bound");
    return;
  }

  ReadPixelsHelper(x, y, width, height, format, type, pixels.View(), 0);
}

void WebGL2RenderingContextBase::readPixels(
    GLint x,
    GLint y,
    GLsizei width,
    GLsizei height,
    GLenum format,
    GLenum type,
    MaybeShared<DOMArrayBufferView> pixels,
    int64_t offset) {
  if (isContextLost())
    return;
  if (bound_pixel_pack_buffer_.Get()) {
    SynthesizeGLError(GL_INVALID_OPERATION, "readPixels",
                      "PIXEL_PACK buffer should not be bound");
    return;
  }

  ReadPixelsHelper(x, y, width, height, format, type, pixels.View(), offset);
}

void WebGL2RenderingContextBase::readPixels(GLint x,
                                            GLint y,
                                            GLsizei width,
                                            GLsizei height,
                                            GLenum format,
                                            GLenum type,
                                            int64_t offset) {
  if (isContextLost())
    return;

  // Due to WebGL's same-origin restrictions, it is not possible to
  // taint the origin using the WebGL API.
  DCHECK(canvas()->OriginClean());

  if (!ValidateValueFitNonNegInt32("readPixels", "offset", offset))
    return;

  WebGLBuffer* buffer = bound_pixel_pack_buffer_.Get();
  if (!buffer) {
    SynthesizeGLError(GL_INVALID_OPERATION, "readPixels",
                      "no PIXEL_PACK buffer bound");
    return;
  }

  const char* reason = "framebuffer incomplete";
  WebGLFramebuffer* framebuffer = GetReadFramebufferBinding();
  if (framebuffer && framebuffer->CheckDepthStencilStatus(&reason) !=
                         GL_FRAMEBUFFER_COMPLETE) {
    SynthesizeGLError(GL_INVALID_FRAMEBUFFER_OPERATION, "readPixels", reason);
    return;
  }

  int64_t size = buffer->GetSize() - offset;
  // If size is negative, or size is not large enough to store pixels, those
  // cases are handled by validateReadPixelsFuncParameters to generate
  // INVALID_OPERATION.
  if (!ValidateReadPixelsFuncParameters(width, height, format, type, nullptr,
                                        size))
    return;

  ClearIfComposited();

  {
    ScopedDrawingBufferBinder binder(GetDrawingBuffer(), framebuffer);
    ContextGL()->ReadPixels(x, y, width, height, format, type,
                            reinterpret_cast<void*>(offset));
  }
}

void WebGL2RenderingContextBase::RenderbufferStorageHelper(
    GLenum target,
    GLsizei samples,
    GLenum internalformat,
    GLsizei width,
    GLsizei height,
    const char* function_name) {
  if (!samples) {
    ContextGL()->RenderbufferStorage(target, internalformat, width, height);
  } else {
    GLint max_number_of_samples = 0;
    ContextGL()->GetInternalformativ(target, internalformat, GL_SAMPLES, 1,
                                     &max_number_of_samples);
    if (samples > max_number_of_samples) {
      SynthesizeGLError(GL_INVALID_OPERATION, function_name,
                        "samples out of range");
      return;
    }
    ContextGL()->RenderbufferStorageMultisampleCHROMIUM(
        target, samples, internalformat, width, height);
  }
}

void WebGL2RenderingContextBase::RenderbufferStorageImpl(
    GLenum target,
    GLsizei samples,
    GLenum internalformat,
    GLsizei width,
    GLsizei height,
    const char* function_name) {
  switch (internalformat) {
    case GL_R8UI:
    case GL_R8I:
    case GL_R16UI:
    case GL_R16I:
    case GL_R32UI:
    case GL_R32I:
    case GL_RG8UI:
    case GL_RG8I:
    case GL_RG16UI:
    case GL_RG16I:
    case GL_RG32UI:
    case GL_RG32I:
    case GL_RGBA8UI:
    case GL_RGBA8I:
    case GL_RGB10_A2UI:
    case GL_RGBA16UI:
    case GL_RGBA16I:
    case GL_RGBA32UI:
    case GL_RGBA32I:
      if (samples > 0) {
        SynthesizeGLError(GL_INVALID_OPERATION, function_name,
                          "for integer formats, samples > 0");
        return;
      }
      FALLTHROUGH;
    case GL_R8:
    case GL_RG8:
    case GL_RGB8:
    case GL_RGB565:
    case GL_RGBA8:
    case GL_SRGB8_ALPHA8:
    case GL_RGB5_A1:
    case GL_RGBA4:
    case GL_RGB10_A2:
    case GL_DEPTH_COMPONENT16:
    case GL_DEPTH_COMPONENT24:
    case GL_DEPTH_COMPONENT32F:
    case GL_DEPTH24_STENCIL8:
    case GL_DEPTH32F_STENCIL8:
    case GL_STENCIL_INDEX8:
      RenderbufferStorageHelper(target, samples, internalformat, width, height,
                                function_name);
      break;
    case GL_DEPTH_STENCIL:
      // To be WebGL 1 backward compatible.
      if (samples > 0) {
        SynthesizeGLError(GL_INVALID_ENUM, function_name,
                          "invalid internalformat");
        return;
      }
      RenderbufferStorageHelper(target, 0, GL_DEPTH24_STENCIL8, width, height,
                                function_name);
      break;
    case GL_R16F:
    case GL_RG16F:
    case GL_RGBA16F:
    case GL_R32F:
    case GL_RG32F:
    case GL_RGBA32F:
    case GL_R11F_G11F_B10F:
      if (!ExtensionEnabled(kEXTColorBufferFloatName)) {
        SynthesizeGLError(GL_INVALID_ENUM, function_name,
                          "EXT_color_buffer_float not enabled");
        return;
      }
      RenderbufferStorageHelper(target, samples, internalformat, width, height,
                                function_name);
      break;
    case GL_R16_EXT:
    case GL_RG16_EXT:
    case GL_RGBA16_EXT:
      if (!ExtensionEnabled(kEXTTextureNorm16Name)) {
        SynthesizeGLError(GL_INVALID_ENUM, function_name,
                          "EXT_texture_norm16 not enabled");
        return;
      }
      RenderbufferStorageHelper(target, samples, internalformat, width, height,
                                function_name);
      break;
    default:
      SynthesizeGLError(GL_INVALID_ENUM, function_name,
                        "invalid internalformat");
      return;
  }
  renderbuffer_binding_->SetInternalFormat(internalformat);
  renderbuffer_binding_->SetSize(width, height);
  UpdateNumberOfUserAllocatedMultisampledRenderbuffers(
      renderbuffer_binding_->UpdateMultisampleState(samples > 0));
}

void WebGL2RenderingContextBase::renderbufferStorageMultisample(
    GLenum target,
    GLsizei samples,
    GLenum internalformat,
    GLsizei width,
    GLsizei height) {
  const char* function_name = "renderbufferStorageMultisample";
  if (isContextLost())
    return;
  if (target != GL_RENDERBUFFER) {
    SynthesizeGLError(GL_INVALID_ENUM, function_name, "invalid target");
    return;
  }
  if (!renderbuffer_binding_ || !renderbuffer_binding_->Object()) {
    SynthesizeGLError(GL_INVALID_OPERATION, function_name,
                      "no bound renderbuffer");
    return;
  }
  if (!ValidateSize("renderbufferStorage", width, height))
    return;
  if (samples < 0) {
    SynthesizeGLError(GL_INVALID_VALUE, function_name, "samples < 0");
    return;
  }
  RenderbufferStorageImpl(target, samples, internalformat, width, height,
                          function_name);
  ApplyStencilTest();
}

void WebGL2RenderingContextBase::ResetUnpackParameters() {
  WebGLRenderingContextBase::ResetUnpackParameters();

  if (unpack_row_length_)
    ContextGL()->PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  if (unpack_image_height_)
    ContextGL()->PixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
  if (unpack_skip_pixels_)
    ContextGL()->PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
  if (unpack_skip_rows_)
    ContextGL()->PixelStorei(GL_UNPACK_SKIP_ROWS, 0);
  if (unpack_skip_images_)
    ContextGL()->PixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
}

void WebGL2RenderingContextBase::RestoreUnpackParameters() {
  WebGLRenderingContextBase::RestoreUnpackParameters();

  if (unpack_row_length_)
    ContextGL()->PixelStorei(GL_UNPACK_ROW_LENGTH, unpack_row_length_);
  if (unpack_image_height_)
    ContextGL()->PixelStorei(GL_UNPACK_IMAGE_HEIGHT, unpack_image_height_);
  if (unpack_skip_pixels_)
    ContextGL()->PixelStorei(GL_UNPACK_SKIP_PIXELS, unpack_skip_pixels_);
  if (unpack_skip_rows_)
    ContextGL()->PixelStorei(GL_UNPACK_SKIP_ROWS, unpack_skip_rows_);
  if (unpack_skip_images_)
    ContextGL()->PixelStorei(GL_UNPACK_SKIP_IMAGES, unpack_skip_images_);
}

/* Texture objects */
bool WebGL2RenderingContextBase::ValidateTexStorage(
    const char* function_name,
    GLenum target,
    GLsizei levels,
    GLenum internalformat,
    GLsizei width,
    GLsizei height,
    GLsizei depth,
    TexStorageType function_type) {
  if (function_type == kTexStorageType2D) {
    if (target != GL_TEXTURE_2D && target != GL_TEXTURE_CUBE_MAP) {
      SynthesizeGLError(GL_INVALID_ENUM, function_name, "invalid 2D target");
      return false;
    }
  } else {
    if (target != GL_TEXTURE_3D && target != GL_TEXTURE_2D_ARRAY) {
      SynthesizeGLError(GL_INVALID_ENUM, function_name, "invalid 3D target");
      return false;
    }
  }

  if (function_type == kTexStorageType3D && target != GL_TEXTURE_2D_ARRAY &&
      compressed_texture_formats_etc2eac_.find(internalformat) !=
          compressed_texture_formats_etc2eac_.end()) {
    SynthesizeGLError(
        GL_INVALID_OPERATION, function_name,
        "target for ETC2/EAC internal formats must be TEXTURE_2D_ARRAY");
    return false;
  }

  if (supported_internal_formats_storage_.find(internalformat) ==
          supported_internal_formats_storage_.end() &&
      (function_type == kTexStorageType2D &&
       !compressed_texture_formats_.Contains(internalformat))) {
    SynthesizeGLError(GL_INVALID_ENUM, function_name, "invalid internalformat");
    return false;
  }

  if (width <= 0 || height <= 0 || depth <= 0) {
    SynthesizeGLError(GL_INVALID_VALUE, function_name, "invalid dimensions");
    return false;
  }

  if (levels <= 0) {
    SynthesizeGLError(GL_INVALID_VALUE, function_name, "invalid levels");
    return false;
  }

  if (target == GL_TEXTURE_3D) {
    if (levels > log2(std::max(std::max(width, height), depth)) + 1) {
      SynthesizeGLError(GL_INVALID_OPERATION, function_name, "to many levels");
      return false;
    }
  } else {
    if (levels > log2(std::max(width, height)) + 1) {
      SynthesizeGLError(GL_INVALID_OPERATION, function_name, "to many levels");
      return false;
    }
  }

  return true;
}

void WebGL2RenderingContextBase::texImage2D(GLenum target,
                                            GLint level,
                                            GLint internalformat,
                                            GLsizei width,
                                            GLsizei height,
                                            GLint border,
                                            GLenum format,
                                            GLenum type,
                                            int64_t offset) {
  if (isContextLost())
    return;
  if (!ValidateTexture2DBinding("texImage2D", target))
    return;
  if (!bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texImage2D",
                      "no bound PIXEL_UNPACK_BUFFER");
    return;
  }
  if (unpack_flip_y_ || unpack_premultiply_alpha_) {
    SynthesizeGLError(
        GL_INVALID_OPERATION, "texImage2D",
        "FLIP_Y or PREMULTIPLY_ALPHA isn't allowed while uploading from PBO");
    return;
  }
  if (!ValidateTexFunc("texImage2D", kTexImage, kSourceUnpackBuffer, target,
                       level, internalformat, width, height, 1, border, format,
                       type, 0, 0, 0))
    return;
  if (!ValidateValueFitNonNegInt32("texImage2D", "offset", offset))
    return;

  ContextGL()->TexImage2D(
      target, level, ConvertTexInternalFormat(internalformat, type), width,
      height, border, format, type, reinterpret_cast<const void*>(offset));
}

void WebGL2RenderingContextBase::texSubImage2D(GLenum target,
                                               GLint level,
                                               GLint xoffset,
                                               GLint yoffset,
                                               GLsizei width,
                                               GLsizei height,
                                               GLenum format,
                                               GLenum type,
                                               int64_t offset) {
  if (isContextLost())
    return;
  if (!ValidateTexture2DBinding("texSubImage2D", target))
    return;
  if (!bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texSubImage2D",
                      "no bound PIXEL_UNPACK_BUFFER");
    return;
  }
  if (unpack_flip_y_ || unpack_premultiply_alpha_) {
    SynthesizeGLError(
        GL_INVALID_OPERATION, "texSubImage2D",
        "FLIP_Y or PREMULTIPLY_ALPHA isn't allowed while uploading from PBO");
    return;
  }
  if (!ValidateTexFunc("texSubImage2D", kTexSubImage, kSourceUnpackBuffer,
                       target, level, 0, width, height, 1, 0, format, type,
                       xoffset, yoffset, 0))
    return;
  if (!ValidateValueFitNonNegInt32("texSubImage2D", "offset", offset))
    return;

  ContextGL()->TexSubImage2D(target, level, xoffset, yoffset, width, height,
                             format, type,
                             reinterpret_cast<const void*>(offset));
}

void WebGL2RenderingContextBase::texImage2D(
    GLenum target,
    GLint level,
    GLint internalformat,
    GLsizei width,
    GLsizei height,
    GLint border,
    GLenum format,
    GLenum type,
    MaybeShared<DOMArrayBufferView> data) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texImage2D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }
  WebGLRenderingContextBase::texImage2D(target, level, internalformat, width,
                                        height, border, format, type, data);
}

void WebGL2RenderingContextBase::texImage2D(
    GLenum target,
    GLint level,
    GLint internalformat,
    GLsizei width,
    GLsizei height,
    GLint border,
    GLenum format,
    GLenum type,
    MaybeShared<DOMArrayBufferView> data,
    GLuint src_offset) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texImage2D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }
  TexImageHelperDOMArrayBufferView(
      kTexImage2D, target, level, internalformat, width, height, 1, border,
      format, type, 0, 0, 0, data.View(), kNullNotReachable, src_offset);
}

void WebGL2RenderingContextBase::texImage2D(GLenum target,
                                            GLint level,
                                            GLint internalformat,
                                            GLsizei width,
                                            GLsizei height,
                                            GLint border,
                                            GLenum format,
                                            GLenum type,
                                            ImageData* pixels) {
  DCHECK(pixels);
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texImage2D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }
  TexImageHelperImageData(kTexImage2D, target, level, internalformat, 0, format,
                          type, 1, 0, 0, 0, pixels,
                          GetTextureSourceSubRectangle(width, height), 0);
}

void WebGL2RenderingContextBase::texImage2D(ExecutionContext* execution_context,
                                            GLenum target,
                                            GLint level,
                                            GLint internalformat,
                                            GLsizei width,
                                            GLsizei height,
                                            GLint border,
                                            GLenum format,
                                            GLenum type,
                                            HTMLImageElement* image,
                                            ExceptionState& exception_state) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texImage2D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }
  TexImageHelperHTMLImageElement(execution_context->GetSecurityOrigin(),
                                 kTexImage2D, target, level, internalformat,
                                 format, type, 0, 0, 0, image,
                                 GetTextureSourceSubRectangle(width, height), 1,
                                 unpack_image_height_, exception_state);
}

void WebGL2RenderingContextBase::texImage2D(ExecutionContext* execution_context,
                                            GLenum target,
                                            GLint level,
                                            GLint internalformat,
                                            GLsizei width,
                                            GLsizei height,
                                            GLint border,
                                            GLenum format,
                                            GLenum type,
                                            CanvasRenderingContextHost* canvas,
                                            ExceptionState& exception_state) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texImage2D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }

  TexImageHelperCanvasRenderingContextHost(
      execution_context->GetSecurityOrigin(), kTexImage2D, target, level,
      internalformat, format, type, 0, 0, 0, canvas,
      GetTextureSourceSubRectangle(width, height), 1, 0, exception_state);
}

void WebGL2RenderingContextBase::texImage2D(ExecutionContext* execution_context,
                                            GLenum target,
                                            GLint level,
                                            GLint internalformat,
                                            GLsizei width,
                                            GLsizei height,
                                            GLint border,
                                            GLenum format,
                                            GLenum type,
                                            HTMLVideoElement* video,
                                            ExceptionState& exception_state) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texImage2D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }

  TexImageHelperHTMLVideoElement(
      execution_context->GetSecurityOrigin(), kTexImage2D, target, level,
      internalformat, format, type, 0, 0, 0, video,
      GetTextureSourceSubRectangle(width, height), 1, 0, exception_state);
}

void WebGL2RenderingContextBase::texImage2D(GLenum target,
                                            GLint level,
                                            GLint internalformat,
                                            GLsizei width,
                                            GLsizei height,
                                            GLint border,
                                            GLenum format,
                                            GLenum type,
                                            ImageBitmap* bitmap,
                                            ExceptionState& exception_state) {
  DCHECK(bitmap);
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texImage2D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }
  TexImageHelperImageBitmap(
      kTexImage2D, target, level, internalformat, format, type, 0, 0, 0, bitmap,
      GetTextureSourceSubRectangle(width, height), 1, 0, exception_state);
}

void WebGL2RenderingContextBase::texImage2D(GLenum target,
                                            GLint level,
                                            GLint internalformat,
                                            GLenum format,
                                            GLenum type,
                                            ImageData* image_data) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texImage2D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }
  WebGLRenderingContextBase::texImage2D(target, level, internalformat, format,
                                        type, image_data);
}

void WebGL2RenderingContextBase::texImage2D(ExecutionContext* execution_context,
                                            GLenum target,
                                            GLint level,
                                            GLint internalformat,
                                            GLenum format,
                                            GLenum type,
                                            HTMLImageElement* image,
                                            ExceptionState& exception_state) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texImage2D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }

  WebGLRenderingContextBase::texImage2D(execution_context, target, level,
                                        internalformat, format, type, image,
                                        exception_state);
}

void WebGL2RenderingContextBase::texImage2D(
    ExecutionContext* execution_context,
    GLenum target,
    GLint level,
    GLint internalformat,
    GLenum format,
    GLenum type,
    CanvasRenderingContextHost* context_host,
    ExceptionState& exception_state) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texImage2D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }

  WebGLRenderingContextBase::texImage2D(execution_context, target, level,
                                        internalformat, format, type,
                                        context_host, exception_state);
}

void WebGL2RenderingContextBase::texImage2D(ExecutionContext* execution_context,
                                            GLenum target,
                                            GLint level,
                                            GLint internalformat,
                                            GLenum format,
                                            GLenum type,
                                            HTMLVideoElement* video,
                                            ExceptionState& exception_state) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texImage2D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }

  WebGLRenderingContextBase::texImage2D(execution_context, target, level,
                                        internalformat, format, type, video,
                                        exception_state);
}

void WebGL2RenderingContextBase::texImage2D(GLenum target,
                                            GLint level,
                                            GLint internalformat,
                                            GLenum format,
                                            GLenum type,
                                            ImageBitmap* image_bit_map,
                                            ExceptionState& exception_state) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texImage2D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }
  WebGLRenderingContextBase::texImage2D(target, level, internalformat, format,
                                        type, image_bit_map, exception_state);
}

void WebGL2RenderingContextBase::texSubImage2D(
    GLenum target,
    GLint level,
    GLint xoffset,
    GLint yoffset,
    GLsizei width,
    GLsizei height,
    GLenum format,
    GLenum type,
    MaybeShared<DOMArrayBufferView> pixels) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texSubImage2D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }
  WebGLRenderingContextBase::texSubImage2D(target, level, xoffset, yoffset,
                                           width, height, format, type, pixels);
}

void WebGL2RenderingContextBase::texSubImage2D(
    GLenum target,
    GLint level,
    GLint xoffset,
    GLint yoffset,
    GLsizei width,
    GLsizei height,
    GLenum format,
    GLenum type,
    MaybeShared<DOMArrayBufferView> pixels,
    GLuint src_offset) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texSubImage2D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }
  TexImageHelperDOMArrayBufferView(
      kTexSubImage2D, target, level, 0, width, height, 1, 0, format, type,
      xoffset, yoffset, 0, pixels.View(), kNullNotReachable, src_offset);
}

void WebGL2RenderingContextBase::texSubImage2D(GLenum target,
                                               GLint level,
                                               GLint xoffset,
                                               GLint yoffset,
                                               GLsizei width,
                                               GLsizei height,
                                               GLenum format,
                                               GLenum type,
                                               ImageData* pixels) {
  DCHECK(pixels);
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texSubImage2D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }
  TexImageHelperImageData(kTexSubImage2D, target, level, 0, 0, format, type, 1,
                          xoffset, yoffset, 0, pixels,
                          GetTextureSourceSubRectangle(width, height), 0);
}

void WebGL2RenderingContextBase::texSubImage2D(
    ExecutionContext* execution_context,
    GLenum target,
    GLint level,
    GLint xoffset,
    GLint yoffset,
    GLsizei width,
    GLsizei height,
    GLenum format,
    GLenum type,
    HTMLImageElement* image,
    ExceptionState& exception_state) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texSubImage2D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }

  TexImageHelperHTMLImageElement(
      execution_context->GetSecurityOrigin(), kTexSubImage2D, target, level, 0,
      format, type, xoffset, yoffset, 0, image,
      GetTextureSourceSubRectangle(width, height), 1, 0, exception_state);
}

void WebGL2RenderingContextBase::texSubImage2D(
    ExecutionContext* execution_context,
    GLenum target,
    GLint level,
    GLint xoffset,
    GLint yoffset,
    GLsizei width,
    GLsizei height,
    GLenum format,
    GLenum type,
    CanvasRenderingContextHost* canvas,
    ExceptionState& exception_state) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texSubImage2D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }

  TexImageHelperCanvasRenderingContextHost(
      execution_context->GetSecurityOrigin(), kTexSubImage2D, target, level, 0,
      format, type, xoffset, yoffset, 0, canvas,
      GetTextureSourceSubRectangle(width, height), 1, 0, exception_state);
}

void WebGL2RenderingContextBase::texSubImage2D(
    ExecutionContext* execution_context,
    GLenum target,
    GLint level,
    GLint xoffset,
    GLint yoffset,
    GLsizei width,
    GLsizei height,
    GLenum format,
    GLenum type,
    HTMLVideoElement* video,
    ExceptionState& exception_state) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texSubImage2D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }

  TexImageHelperHTMLVideoElement(
      execution_context->GetSecurityOrigin(), kTexSubImage2D, target, level, 0,
      format, type, xoffset, yoffset, 0, video,
      GetTextureSourceSubRectangle(width, height), 1, 0, exception_state);
}

void WebGL2RenderingContextBase::texSubImage2D(
    GLenum target,
    GLint level,
    GLint xoffset,
    GLint yoffset,
    GLsizei width,
    GLsizei height,
    GLenum format,
    GLenum type,
    ImageBitmap* bitmap,
    ExceptionState& exception_state) {
  DCHECK(bitmap);
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texSubImage2D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }
  TexImageHelperImageBitmap(kTexSubImage2D, target, level, 0, format, type,
                            xoffset, yoffset, 0, bitmap,
                            GetTextureSourceSubRectangle(width, height), 1, 0,
                            exception_state);
}

void WebGL2RenderingContextBase::texSubImage2D(GLenum target,
                                               GLint level,
                                               GLint xoffset,
                                               GLint yoffset,
                                               GLenum format,
                                               GLenum type,
                                               ImageData* pixels) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texSubImage2D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }
  WebGLRenderingContextBase::texSubImage2D(target, level, xoffset, yoffset,
                                           format, type, pixels);
}

void WebGL2RenderingContextBase::texSubImage2D(
    ExecutionContext* execution_context,
    GLenum target,
    GLint level,
    GLint xoffset,
    GLint yoffset,
    GLenum format,
    GLenum type,
    HTMLImageElement* image,
    ExceptionState& exception_state) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texSubImage2D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }

  WebGLRenderingContextBase::texSubImage2D(execution_context, target, level,
                                           xoffset, yoffset, format, type,
                                           image, exception_state);
}

void WebGL2RenderingContextBase::texSubImage2D(
    ExecutionContext* execution_context,
    GLenum target,
    GLint level,
    GLint xoffset,
    GLint yoffset,
    GLenum format,
    GLenum type,
    CanvasRenderingContextHost* context_host,
    ExceptionState& exception_state) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texSubImage2D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }

  WebGLRenderingContextBase::texSubImage2D(execution_context, target, level,
                                           xoffset, yoffset, format, type,
                                           context_host, exception_state);
}

void WebGL2RenderingContextBase::texSubImage2D(
    ExecutionContext* execution_context,
    GLenum target,
    GLint level,
    GLint xoffset,
    GLint yoffset,
    GLenum format,
    GLenum type,
    HTMLVideoElement* video,
    ExceptionState& exception_state) {
  WebGLRenderingContextBase::texSubImage2D(execution_context, target, level,
                                           xoffset, yoffset, format, type,
                                           video, exception_state);
}

void WebGL2RenderingContextBase::texSubImage2D(
    GLenum target,
    GLint level,
    GLint xoffset,
    GLint yoffset,
    GLenum format,
    GLenum type,
    ImageBitmap* bitmap,
    ExceptionState& exception_state) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texSubImage2D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }
  WebGLRenderingContextBase::texSubImage2D(
      target, level, xoffset, yoffset, format, type, bitmap, exception_state);
}

void WebGL2RenderingContextBase::texStorage2D(GLenum target,
                                              GLsizei levels,
                                              GLenum internalformat,
                                              GLsizei width,
                                              GLsizei height) {
  if (isContextLost() ||
      !ValidateTexStorage("texStorage2D", target, levels, internalformat, width,
                          height, 1, kTexStorageType2D))
    return;

  ContextGL()->TexStorage2DEXT(target, levels, internalformat, width, height);
}

void WebGL2RenderingContextBase::texStorage3D(GLenum target,
                                              GLsizei levels,
                                              GLenum internalformat,
                                              GLsizei width,
                                              GLsizei height,
                                              GLsizei depth) {
  if (isContextLost() ||
      !ValidateTexStorage("texStorage3D", target, levels, internalformat, width,
                          height, depth, kTexStorageType3D))
    return;

  ContextGL()->TexStorage3D(target, levels, internalformat, width, height,
                            depth);
}

void WebGL2RenderingContextBase::texImage3D(
    GLenum target,
    GLint level,
    GLint internalformat,
    GLsizei width,
    GLsizei height,
    GLsizei depth,
    GLint border,
    GLenum format,
    GLenum type,
    MaybeShared<DOMArrayBufferView> pixels) {
  if ((unpack_flip_y_ || unpack_premultiply_alpha_) && pixels) {
    SynthesizeGLError(
        GL_INVALID_OPERATION, "texImage3D",
        "FLIP_Y or PREMULTIPLY_ALPHA isn't allowed for uploading 3D textures");
    return;
  }
  TexImageHelperDOMArrayBufferView(kTexImage3D, target, level, internalformat,
                                   width, height, depth, border, format, type,
                                   0, 0, 0, pixels.View(), kNullAllowed, 0);
}

void WebGL2RenderingContextBase::texImage3D(
    GLenum target,
    GLint level,
    GLint internalformat,
    GLsizei width,
    GLsizei height,
    GLsizei depth,
    GLint border,
    GLenum format,
    GLenum type,
    MaybeShared<DOMArrayBufferView> pixels,
    GLuint src_offset) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texImage3D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }
  if (unpack_flip_y_ || unpack_premultiply_alpha_) {
    DCHECK(pixels);
    SynthesizeGLError(
        GL_INVALID_OPERATION, "texImage3D",
        "FLIP_Y or PREMULTIPLY_ALPHA isn't allowed for uploading 3D textures");
    return;
  }
  TexImageHelperDOMArrayBufferView(
      kTexImage3D, target, level, internalformat, width, height, depth, border,
      format, type, 0, 0, 0, pixels.View(), kNullNotReachable, src_offset);
}

void WebGL2RenderingContextBase::texImage3D(GLenum target,
                                            GLint level,
                                            GLint internalformat,
                                            GLsizei width,
                                            GLsizei height,
                                            GLsizei depth,
                                            GLint border,
                                            GLenum format,
                                            GLenum type,
                                            int64_t offset) {
  if (isContextLost())
    return;
  if (!ValidateTexture3DBinding("texImage3D", target))
    return;
  if (!bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texImage3D",
                      "no bound PIXEL_UNPACK_BUFFER");
    return;
  }
  if (unpack_flip_y_ || unpack_premultiply_alpha_) {
    SynthesizeGLError(
        GL_INVALID_OPERATION, "texImage3D",
        "FLIP_Y or PREMULTIPLY_ALPHA isn't allowed for uploading 3D textures");
    return;
  }
  if (!ValidateTexFunc("texImage3D", kTexImage, kSourceUnpackBuffer, target,
                       level, internalformat, width, height, depth, border,
                       format, type, 0, 0, 0))
    return;
  if (!ValidateValueFitNonNegInt32("texImage3D", "offset", offset))
    return;

  ContextGL()->TexImage3D(target, level,
                          ConvertTexInternalFormat(internalformat, type), width,
                          height, depth, border, format, type,
                          reinterpret_cast<const void*>(offset));
}

void WebGL2RenderingContextBase::texImage3D(GLenum target,
                                            GLint level,
                                            GLint internalformat,
                                            GLsizei width,
                                            GLsizei height,
                                            GLsizei depth,
                                            GLint border,
                                            GLenum format,
                                            GLenum type,
                                            ImageData* pixels) {
  DCHECK(pixels);
  IntRect source_image_rect;
  source_image_rect.SetLocation(
      IntPoint(unpack_skip_pixels_, unpack_skip_rows_));
  source_image_rect.SetSize(IntSize(width, height));
  TexImageHelperImageData(kTexImage3D, target, level, internalformat, 0, format,
                          type, depth, 0, 0, 0, pixels, source_image_rect,
                          unpack_image_height_);
}

void WebGL2RenderingContextBase::texImage3D(ExecutionContext* execution_context,
                                            GLenum target,
                                            GLint level,
                                            GLint internalformat,
                                            GLsizei width,
                                            GLsizei height,
                                            GLsizei depth,
                                            GLint border,
                                            GLenum format,
                                            GLenum type,
                                            HTMLImageElement* image,
                                            ExceptionState& exception_state) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texImage3D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }

  TexImageHelperHTMLImageElement(execution_context->GetSecurityOrigin(),
                                 kTexImage3D, target, level, internalformat,
                                 format, type, 0, 0, 0, image,
                                 GetTextureSourceSubRectangle(width, height),
                                 depth, unpack_image_height_, exception_state);
}

void WebGL2RenderingContextBase::texImage3D(ExecutionContext* execution_context,
                                            GLenum target,
                                            GLint level,
                                            GLint internalformat,
                                            GLsizei width,
                                            GLsizei height,
                                            GLsizei depth,
                                            GLint border,
                                            GLenum format,
                                            GLenum type,
                                            CanvasRenderingContextHost* canvas,
                                            ExceptionState& exception_state) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texImage3D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }

  TexImageHelperCanvasRenderingContextHost(
      execution_context->GetSecurityOrigin(), kTexImage3D, target, level,
      internalformat, format, type, 0, 0, 0, canvas,
      GetTextureSourceSubRectangle(width, height), depth, unpack_image_height_,
      exception_state);
}

void WebGL2RenderingContextBase::texImage3D(ExecutionContext* execution_context,
                                            GLenum target,
                                            GLint level,
                                            GLint internalformat,
                                            GLsizei width,
                                            GLsizei height,
                                            GLsizei depth,
                                            GLint border,
                                            GLenum format,
                                            GLenum type,
                                            HTMLVideoElement* video,
                                            ExceptionState& exception_state) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texImage3D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }

  TexImageHelperHTMLVideoElement(execution_context->GetSecurityOrigin(),
                                 kTexImage3D, target, level, internalformat,
                                 format, type, 0, 0, 0, video,
                                 GetTextureSourceSubRectangle(width, height),
                                 depth, unpack_image_height_, exception_state);
}

void WebGL2RenderingContextBase::texImage3D(GLenum target,
                                            GLint level,
                                            GLint internalformat,
                                            GLsizei width,
                                            GLsizei height,
                                            GLsizei depth,
                                            GLint border,
                                            GLenum format,
                                            GLenum type,
                                            ImageBitmap* bitmap,
                                            ExceptionState& exception_state) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texImage3D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }
  TexImageHelperImageBitmap(kTexImage3D, target, level, internalformat, format,
                            type, 0, 0, 0, bitmap,
                            GetTextureSourceSubRectangle(width, height), depth,
                            unpack_image_height_, exception_state);
}

void WebGL2RenderingContextBase::texSubImage3D(
    GLenum target,
    GLint level,
    GLint xoffset,
    GLint yoffset,
    GLint zoffset,
    GLsizei width,
    GLsizei height,
    GLsizei depth,
    GLenum format,
    GLenum type,
    MaybeShared<DOMArrayBufferView> pixels,
    GLuint src_offset) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texSubImage3D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }
  if (unpack_flip_y_ || unpack_premultiply_alpha_) {
    DCHECK(pixels);
    SynthesizeGLError(
        GL_INVALID_OPERATION, "texSubImage3D",
        "FLIP_Y or PREMULTIPLY_ALPHA isn't allowed for uploading 3D textures");
    return;
  }

  TexImageHelperDOMArrayBufferView(
      kTexSubImage3D, target, level, 0, width, height, depth, 0, format, type,
      xoffset, yoffset, zoffset, pixels.View(), kNullNotReachable, src_offset);
}

void WebGL2RenderingContextBase::texSubImage3D(GLenum target,
                                               GLint level,
                                               GLint xoffset,
                                               GLint yoffset,
                                               GLint zoffset,
                                               GLsizei width,
                                               GLsizei height,
                                               GLsizei depth,
                                               GLenum format,
                                               GLenum type,
                                               int64_t offset) {
  if (isContextLost())
    return;
  if (!ValidateTexture3DBinding("texSubImage3D", target))
    return;
  if (!bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texSubImage3D",
                      "no bound PIXEL_UNPACK_BUFFER");
    return;
  }
  if (unpack_flip_y_ || unpack_premultiply_alpha_) {
    SynthesizeGLError(
        GL_INVALID_OPERATION, "texSubImage3D",
        "FLIP_Y or PREMULTIPLY_ALPHA isn't allowed for uploading 3D textures");
    return;
  }
  if (!ValidateTexFunc("texSubImage3D", kTexSubImage, kSourceUnpackBuffer,
                       target, level, 0, width, height, depth, 0, format, type,
                       xoffset, yoffset, zoffset))
    return;
  if (!ValidateValueFitNonNegInt32("texSubImage3D", "offset", offset))
    return;

  ContextGL()->TexSubImage3D(target, level, xoffset, yoffset, zoffset, width,
                             height, depth, format, type,
                             reinterpret_cast<const void*>(offset));
}

void WebGL2RenderingContextBase::texSubImage3D(GLenum target,
                                               GLint level,
                                               GLint xoffset,
                                               GLint yoffset,
                                               GLint zoffset,
                                               GLsizei width,
                                               GLsizei height,
                                               GLsizei depth,
                                               GLenum format,
                                               GLenum type,
                                               ImageData* pixels) {
  DCHECK(pixels);
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texSubImage3D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }
  TexImageHelperImageData(kTexSubImage3D, target, level, 0, 0, format, type,
                          depth, xoffset, yoffset, zoffset, pixels,
                          GetTextureSourceSubRectangle(width, height),
                          unpack_image_height_);
}

void WebGL2RenderingContextBase::texSubImage3D(
    ExecutionContext* execution_context,
    GLenum target,
    GLint level,
    GLint xoffset,
    GLint yoffset,
    GLint zoffset,
    GLsizei width,
    GLsizei height,
    GLsizei depth,
    GLenum format,
    GLenum type,
    HTMLImageElement* image,
    ExceptionState& exception_state) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texSubImage3D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }

  TexImageHelperHTMLImageElement(execution_context->GetSecurityOrigin(),
                                 kTexSubImage3D, target, level, 0, format, type,
                                 xoffset, yoffset, zoffset, image,
                                 GetTextureSourceSubRectangle(width, height),
                                 depth, unpack_image_height_, exception_state);
}

void WebGL2RenderingContextBase::texSubImage3D(
    ExecutionContext* execution_context,
    GLenum target,
    GLint level,
    GLint xoffset,
    GLint yoffset,
    GLint zoffset,
    GLsizei width,
    GLsizei height,
    GLsizei depth,
    GLenum format,
    GLenum type,
    CanvasRenderingContextHost* context_host,
    ExceptionState& exception_state) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texSubImage3D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }

  TexImageHelperCanvasRenderingContextHost(
      execution_context->GetSecurityOrigin(), kTexSubImage3D, target, level, 0,
      format, type, xoffset, yoffset, zoffset, context_host,
      GetTextureSourceSubRectangle(width, height), depth, unpack_image_height_,
      exception_state);
}

void WebGL2RenderingContextBase::texSubImage3D(
    ExecutionContext* execution_context,
    GLenum target,
    GLint level,
    GLint xoffset,
    GLint yoffset,
    GLint zoffset,
    GLsizei width,
    GLsizei height,
    GLsizei depth,
    GLenum format,
    GLenum type,
    HTMLVideoElement* video,
    ExceptionState& exception_state) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texSubImage3D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }

  TexImageHelperHTMLVideoElement(execution_context->GetSecurityOrigin(),
                                 kTexSubImage3D, target, level, 0, format, type,
                                 xoffset, yoffset, zoffset, video,
                                 GetTextureSourceSubRectangle(width, height),
                                 depth, unpack_image_height_, exception_state);
}

void WebGL2RenderingContextBase::texSubImage3D(
    GLenum target,
    GLint level,
    GLint xoffset,
    GLint yoffset,
    GLint zoffset,
    GLsizei width,
    GLsizei height,
    GLsizei depth,
    GLenum format,
    GLenum type,
    ImageBitmap* bitmap,
    ExceptionState& exception_state) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "texSubImage3D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }
  TexImageHelperImageBitmap(kTexSubImage3D, target, level, 0, format, type,
                            xoffset, yoffset, zoffset, bitmap,
                            GetTextureSourceSubRectangle(width, height), depth,
                            unpack_image_height_, exception_state);
}

void WebGL2RenderingContextBase::copyTexSubImage3D(GLenum target,
                                                   GLint level,
                                                   GLint xoffset,
                                                   GLint yoffset,
                                                   GLint zoffset,
                                                   GLint x,
                                                   GLint y,
                                                   GLsizei width,
                                                   GLsizei height) {
  if (isContextLost())
    return;
  if (!ValidateTexture3DBinding("copyTexSubImage3D", target))
    return;
  WebGLFramebuffer* read_framebuffer_binding = nullptr;
  if (!ValidateReadBufferAndGetInfo("copyTexSubImage3D",
                                    read_framebuffer_binding))
    return;
  ClearIfComposited();
  ScopedDrawingBufferBinder binder(GetDrawingBuffer(),
                                   read_framebuffer_binding);
  ContextGL()->CopyTexSubImage3D(target, level, xoffset, yoffset, zoffset, x, y,
                                 width, height);
}

void WebGL2RenderingContextBase::compressedTexImage2D(
    GLenum target,
    GLint level,
    GLenum internalformat,
    GLsizei width,
    GLsizei height,
    GLint border,
    MaybeShared<DOMArrayBufferView> data) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "compressedTexImage2D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }
  WebGLRenderingContextBase::compressedTexImage2D(target, level, internalformat,
                                                  width, height, border, data);
}

void WebGL2RenderingContextBase::compressedTexImage2D(
    GLenum target,
    GLint level,
    GLenum internalformat,
    GLsizei width,
    GLsizei height,
    GLint border,
    MaybeShared<DOMArrayBufferView> data,
    GLuint src_offset,
    GLuint src_length_override) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "compressedTexImage2D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }
  if (!ValidateTexture2DBinding("compressedTexImage2D", target))
    return;
  if (!ValidateCompressedTexFormat("compressedTexImage2D", internalformat))
    return;
  GLuint data_length;
  if (!ExtractDataLengthIfValid("compressedTexImage2D", data, &data_length))
    return;
  if (src_offset > data_length) {
    SynthesizeGLError(GL_INVALID_VALUE, "compressedTexImage2D",
                      "srcOffset is out of range");
    return;
  }
  if (src_length_override == 0) {
    src_length_override = data_length - src_offset;
  } else if (src_length_override > data_length - src_offset) {
    SynthesizeGLError(GL_INVALID_VALUE, "compressedTexImage2D",
                      "srcLengthOverride is out of range");
    return;
  }
  ContextGL()->CompressedTexImage2D(
      target, level, internalformat, width, height, border, src_length_override,
      static_cast<uint8_t*>(data.View()->BaseAddressMaybeShared()) +
          src_offset);
}

void WebGL2RenderingContextBase::compressedTexImage2D(GLenum target,
                                                      GLint level,
                                                      GLenum internalformat,
                                                      GLsizei width,
                                                      GLsizei height,
                                                      GLint border,
                                                      GLsizei image_size,
                                                      int64_t offset) {
  if (isContextLost())
    return;
  if (!bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "compressedTexImage2D",
                      "no bound PIXEL_UNPACK_BUFFER");
    return;
  }
  ContextGL()->CompressedTexImage2D(target, level, internalformat, width,
                                    height, border, image_size,
                                    reinterpret_cast<uint8_t*>(offset));
}

void WebGL2RenderingContextBase::compressedTexSubImage2D(
    GLenum target,
    GLint level,
    GLint xoffset,
    GLint yoffset,
    GLsizei width,
    GLsizei height,
    GLenum format,
    MaybeShared<DOMArrayBufferView> data) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "compressedTexSubImage2D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }
  WebGLRenderingContextBase::compressedTexSubImage2D(
      target, level, xoffset, yoffset, width, height, format, data);
}

void WebGL2RenderingContextBase::compressedTexSubImage2D(
    GLenum target,
    GLint level,
    GLint xoffset,
    GLint yoffset,
    GLsizei width,
    GLsizei height,
    GLenum format,
    MaybeShared<DOMArrayBufferView> data,
    GLuint src_offset,
    GLuint src_length_override) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "compressedTexSubImage2D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }
  if (!ValidateTexture2DBinding("compressedTexSubImage2D", target))
    return;
  if (!ValidateCompressedTexFormat("compressedTexSubImage2D", format))
    return;
  GLuint data_length;
  if (!ExtractDataLengthIfValid("compressedTexSubImage2D", data, &data_length))
    return;
  if (src_offset > data_length) {
    SynthesizeGLError(GL_INVALID_VALUE, "compressedTexSubImage2D",
                      "srcOffset is out of range");
    return;
  }
  if (src_length_override == 0) {
    src_length_override = data_length - src_offset;
  } else if (src_length_override > data_length - src_offset) {
    SynthesizeGLError(GL_INVALID_VALUE, "compressedTexImage2D",
                      "srcLengthOverride is out of range");
    return;
  }
  ContextGL()->CompressedTexSubImage2D(
      target, level, xoffset, yoffset, width, height, format,
      src_length_override,
      static_cast<uint8_t*>(data.View()->BaseAddressMaybeShared()) +
          src_offset);
}

void WebGL2RenderingContextBase::compressedTexSubImage2D(GLenum target,
                                                         GLint level,
                                                         GLint xoffset,
                                                         GLint yoffset,
                                                         GLsizei width,
                                                         GLsizei height,
                                                         GLenum format,
                                                         GLsizei image_size,
                                                         int64_t offset) {
  if (isContextLost())
    return;
  if (!bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "compressedTexSubImage2D",
                      "no bound PIXEL_UNPACK_BUFFER");
    return;
  }
  ContextGL()->CompressedTexSubImage2D(target, level, xoffset, yoffset, width,
                                       height, format, image_size,
                                       reinterpret_cast<uint8_t*>(offset));
}

void WebGL2RenderingContextBase::compressedTexImage3D(
    GLenum target,
    GLint level,
    GLenum internalformat,
    GLsizei width,
    GLsizei height,
    GLsizei depth,
    GLint border,
    MaybeShared<DOMArrayBufferView> data,
    GLuint src_offset,
    GLuint src_length_override) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "compressedTexImage3D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }
  if (!ValidateTexture3DBinding("compressedTexImage3D", target))
    return;
  if (!ValidateCompressedTexFormat("compressedTexImage3D", internalformat))
    return;
  GLuint data_length;
  if (!ExtractDataLengthIfValid("compressedTexImage3D", data, &data_length))
    return;
  if (src_offset > data_length) {
    SynthesizeGLError(GL_INVALID_VALUE, "compressedTexImage3D",
                      "srcOffset is out of range");
    return;
  }
  if (src_length_override == 0) {
    src_length_override = data_length - src_offset;
  } else if (src_length_override > data_length - src_offset) {
    SynthesizeGLError(GL_INVALID_VALUE, "compressedTexImage3D",
                      "srcLengthOverride is out of range");
    return;
  }
  ContextGL()->CompressedTexImage3D(
      target, level, internalformat, width, height, depth, border,
      src_length_override,
      static_cast<uint8_t*>(data.View()->BaseAddressMaybeShared()) +
          src_offset);
}

void WebGL2RenderingContextBase::compressedTexImage3D(GLenum target,
                                                      GLint level,
                                                      GLenum internalformat,
                                                      GLsizei width,
                                                      GLsizei height,
                                                      GLsizei depth,
                                                      GLint border,
                                                      GLsizei image_size,
                                                      int64_t offset) {
  if (isContextLost())
    return;
  if (!bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "compressedTexImage3D",
                      "no bound PIXEL_UNPACK_BUFFER");
    return;
  }
  ContextGL()->CompressedTexImage3D(target, level, internalformat, width,
                                    height, depth, border, image_size,
                                    reinterpret_cast<uint8_t*>(offset));
}

void WebGL2RenderingContextBase::compressedTexSubImage3D(
    GLenum target,
    GLint level,
    GLint xoffset,
    GLint yoffset,
    GLint zoffset,
    GLsizei width,
    GLsizei height,
    GLsizei depth,
    GLenum format,
    MaybeShared<DOMArrayBufferView> data,
    GLuint src_offset,
    GLuint src_length_override) {
  if (isContextLost())
    return;
  if (bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "compressedTexSubImage3D",
                      "a buffer is bound to PIXEL_UNPACK_BUFFER");
    return;
  }
  if (!ValidateTexture3DBinding("compressedTexSubImage3D", target))
    return;
  if (!ValidateCompressedTexFormat("compressedTexSubImage3D", format))
    return;
  GLuint data_length;
  if (!ExtractDataLengthIfValid("compressedTexSubImage3D", data, &data_length))
    return;
  if (src_offset > data_length) {
    SynthesizeGLError(GL_INVALID_VALUE, "compressedTexSubImage3D",
                      "srcOffset is out of range");
    return;
  }
  if (src_length_override == 0) {
    src_length_override = data_length - src_offset;
  } else if (src_length_override > data_length - src_offset) {
    SynthesizeGLError(GL_INVALID_VALUE, "compressedTexSubImage3D",
                      "srcLengthOverride is out of range");
    return;
  }
  ContextGL()->CompressedTexSubImage3D(
      target, level, xoffset, yoffset, zoffset, width, height, depth, format,
      src_length_override,
      static_cast<uint8_t*>(data.View()->BaseAddressMaybeShared()) +
          src_offset);
}

void WebGL2RenderingContextBase::compressedTexSubImage3D(GLenum target,
                                                         GLint level,
                                                         GLint xoffset,
                                                         GLint yoffset,
                                                         GLint zoffset,
                                                         GLsizei width,
                                                         GLsizei height,
                                                         GLsizei depth,
                                                         GLenum format,
                                                         GLsizei image_size,
                                                         int64_t offset) {
  if (isContextLost())
    return;
  if (!bound_pixel_unpack_buffer_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "compressedTexSubImage3D",
                      "no bound PIXEL_UNPACK_BUFFER");
    return;
  }
  ContextGL()->CompressedTexSubImage3D(target, level, xoffset, yoffset, zoffset,
                                       width, height, depth, format, image_size,
                                       reinterpret_cast<uint8_t*>(offset));
}

GLint WebGL2RenderingContextBase::getFragDataLocation(WebGLProgram* program,
                                                      const String& name) {
  if (!ValidateWebGLProgramOrShader("getFragDataLocation", program))
    return -1;

  return ContextGL()->GetFragDataLocation(ObjectOrZero(program),
                                          name.Utf8().c_str());
}

void WebGL2RenderingContextBase::uniform1ui(
    const WebGLUniformLocation* location,
    GLuint v0) {
  if (isContextLost() || !location)
    return;

  if (location->Program() != current_program_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "uniform1ui",
                      "location not for current program");
    return;
  }

  ContextGL()->Uniform1ui(location->Location(), v0);
}

void WebGL2RenderingContextBase::uniform2ui(
    const WebGLUniformLocation* location,
    GLuint v0,
    GLuint v1) {
  if (isContextLost() || !location)
    return;

  if (location->Program() != current_program_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "uniform2ui",
                      "location not for current program");
    return;
  }

  ContextGL()->Uniform2ui(location->Location(), v0, v1);
}

void WebGL2RenderingContextBase::uniform3ui(
    const WebGLUniformLocation* location,
    GLuint v0,
    GLuint v1,
    GLuint v2) {
  if (isContextLost() || !location)
    return;

  if (location->Program() != current_program_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "uniform3ui",
                      "location not for current program");
    return;
  }

  ContextGL()->Uniform3ui(location->Location(), v0, v1, v2);
}

void WebGL2RenderingContextBase::uniform4ui(
    const WebGLUniformLocation* location,
    GLuint v0,
    GLuint v1,
    GLuint v2,
    GLuint v3) {
  if (isContextLost() || !location)
    return;

  if (location->Program() != current_program_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "uniform4ui",
                      "location not for current program");
    return;
  }

  ContextGL()->Uniform4ui(location->Location(), v0, v1, v2, v3);
}

void WebGL2RenderingContextBase::uniform1fv(
    const WebGLUniformLocation* location,
    const FlexibleFloat32ArrayView& v,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() || !ValidateUniformParameters("uniform1fv", location, v,
                                                    1, src_offset, src_length))
    return;

  ContextGL()->Uniform1fv(
      location->Location(),
      src_length ? src_length
                 : (base::checked_cast<GLuint>(v.lengthAsSizeT()) - src_offset),
      v.DataMaybeOnStack() + src_offset);
}

void WebGL2RenderingContextBase::uniform1fv(
    const WebGLUniformLocation* location,
    Vector<GLfloat>& v,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() ||
      !ValidateUniformParameters("uniform1fv", location, v.data(), v.size(), 1,
                                 src_offset, src_length))
    return;

  ContextGL()->Uniform1fv(location->Location(),
                          src_length ? src_length : (v.size() - src_offset),
                          v.data() + src_offset);
}

void WebGL2RenderingContextBase::uniform2fv(
    const WebGLUniformLocation* location,
    const FlexibleFloat32ArrayView& v,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() || !ValidateUniformParameters("uniform2fv", location, v,
                                                    2, src_offset, src_length))
    return;

  ContextGL()->Uniform2fv(
      location->Location(),
      (src_length
           ? src_length
           : (base::checked_cast<GLuint>(v.lengthAsSizeT()) - src_offset)) >>
          1,
      v.DataMaybeOnStack() + src_offset);
}

void WebGL2RenderingContextBase::uniform2fv(
    const WebGLUniformLocation* location,
    Vector<GLfloat>& v,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() ||
      !ValidateUniformParameters("uniform2fv", location, v.data(), v.size(), 2,
                                 src_offset, src_length))
    return;

  ContextGL()->Uniform2fv(
      location->Location(),
      (src_length ? src_length : (v.size() - src_offset)) >> 1,
      v.data() + src_offset);
}

void WebGL2RenderingContextBase::uniform3fv(
    const WebGLUniformLocation* location,
    const FlexibleFloat32ArrayView& v,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() || !ValidateUniformParameters("uniform3fv", location, v,
                                                    3, src_offset, src_length))
    return;

  ContextGL()->Uniform3fv(
      location->Location(),
      (src_length
           ? src_length
           : (base::checked_cast<GLuint>(v.lengthAsSizeT()) - src_offset)) /
          3,
      v.DataMaybeOnStack() + src_offset);
}

void WebGL2RenderingContextBase::uniform3fv(
    const WebGLUniformLocation* location,
    Vector<GLfloat>& v,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() ||
      !ValidateUniformParameters("uniform3fv", location, v.data(), v.size(), 3,
                                 src_offset, src_length))
    return;

  ContextGL()->Uniform3fv(
      location->Location(),
      (src_length ? src_length : (v.size() - src_offset)) / 3,
      v.data() + src_offset);
}

void WebGL2RenderingContextBase::uniform4fv(
    const WebGLUniformLocation* location,
    const FlexibleFloat32ArrayView& v,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() || !ValidateUniformParameters("uniform4fv", location, v,
                                                    4, src_offset, src_length))
    return;

  ContextGL()->Uniform4fv(
      location->Location(),
      (src_length
           ? src_length
           : (base::checked_cast<GLuint>(v.lengthAsSizeT()) - src_offset)) >>
          2,
      v.DataMaybeOnStack() + src_offset);
}

void WebGL2RenderingContextBase::uniform4fv(
    const WebGLUniformLocation* location,
    Vector<GLfloat>& v,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() ||
      !ValidateUniformParameters("uniform4fv", location, v.data(), v.size(), 4,
                                 src_offset, src_length))
    return;

  ContextGL()->Uniform4fv(
      location->Location(),
      (src_length ? src_length : (v.size() - src_offset)) >> 2,
      v.data() + src_offset);
}

void WebGL2RenderingContextBase::uniform1iv(
    const WebGLUniformLocation* location,
    const FlexibleInt32ArrayView& v,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() || !ValidateUniformParameters("uniform1iv", location, v,
                                                    1, src_offset, src_length))
    return;

  ContextGL()->Uniform1iv(
      location->Location(),
      src_length ? src_length
                 : (base::checked_cast<GLuint>(v.lengthAsSizeT()) - src_offset),
      v.DataMaybeOnStack() + src_offset);
}

void WebGL2RenderingContextBase::uniform1iv(
    const WebGLUniformLocation* location,
    Vector<GLint>& v,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() ||
      !ValidateUniformParameters("uniform1iv", location, v.data(), v.size(), 1,
                                 src_offset, src_length))
    return;

  ContextGL()->Uniform1iv(location->Location(),
                          src_length ? src_length : (v.size() - src_offset),
                          v.data() + src_offset);
}

void WebGL2RenderingContextBase::uniform2iv(
    const WebGLUniformLocation* location,
    const FlexibleInt32ArrayView& v,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() || !ValidateUniformParameters("uniform2iv", location, v,
                                                    2, src_offset, src_length))
    return;

  ContextGL()->Uniform2iv(
      location->Location(),
      (src_length
           ? src_length
           : (base::checked_cast<GLuint>(v.lengthAsSizeT()) - src_offset)) >>
          1,
      v.DataMaybeOnStack() + src_offset);
}

void WebGL2RenderingContextBase::uniform2iv(
    const WebGLUniformLocation* location,
    Vector<GLint>& v,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() ||
      !ValidateUniformParameters("uniform2iv", location, v.data(), v.size(), 2,
                                 src_offset, src_length))
    return;

  ContextGL()->Uniform2iv(
      location->Location(),
      (src_length ? src_length : (v.size() - src_offset)) >> 1,
      v.data() + src_offset);
}

void WebGL2RenderingContextBase::uniform3iv(
    const WebGLUniformLocation* location,
    const FlexibleInt32ArrayView& v,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() || !ValidateUniformParameters("uniform3iv", location, v,
                                                    3, src_offset, src_length))
    return;

  ContextGL()->Uniform3iv(
      location->Location(),
      (src_length
           ? src_length
           : (base::checked_cast<GLuint>(v.lengthAsSizeT()) - src_offset)) /
          3,
      v.DataMaybeOnStack() + src_offset);
}

void WebGL2RenderingContextBase::uniform3iv(
    const WebGLUniformLocation* location,
    Vector<GLint>& v,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() ||
      !ValidateUniformParameters("uniform3iv", location, v.data(), v.size(), 3,
                                 src_offset, src_length))
    return;

  ContextGL()->Uniform3iv(
      location->Location(),
      (src_length ? src_length : (v.size() - src_offset)) / 3,
      v.data() + src_offset);
}

void WebGL2RenderingContextBase::uniform4iv(
    const WebGLUniformLocation* location,
    const FlexibleInt32ArrayView& v,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() || !ValidateUniformParameters("uniform4iv", location, v,
                                                    4, src_offset, src_length))
    return;

  ContextGL()->Uniform4iv(
      location->Location(),
      (src_length
           ? src_length
           : (base::checked_cast<GLuint>(v.lengthAsSizeT()) - src_offset)) >>
          2,
      v.DataMaybeOnStack() + src_offset);
}

void WebGL2RenderingContextBase::uniform4iv(
    const WebGLUniformLocation* location,
    Vector<GLint>& v,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() ||
      !ValidateUniformParameters("uniform4iv", location, v.data(), v.size(), 4,
                                 src_offset, src_length))
    return;

  ContextGL()->Uniform4iv(
      location->Location(),
      (src_length ? src_length : (v.size() - src_offset)) >> 2,
      v.data() + src_offset);
}

void WebGL2RenderingContextBase::uniform1uiv(
    const WebGLUniformLocation* location,
    const FlexibleUint32ArrayView& v,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() || !ValidateUniformParameters("uniform1uiv", location, v,
                                                    1, src_offset, src_length))
    return;

  ContextGL()->Uniform1uiv(
      location->Location(),
      src_length ? src_length
                 : (base::checked_cast<GLuint>(v.lengthAsSizeT()) - src_offset),
      v.DataMaybeOnStack() + src_offset);
}

void WebGL2RenderingContextBase::uniform1uiv(
    const WebGLUniformLocation* location,
    Vector<GLuint>& value,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() ||
      !ValidateUniformParameters("uniform1uiv", location, value.data(),
                                 value.size(), 1, src_offset, src_length))
    return;

  ContextGL()->Uniform1uiv(
      location->Location(),
      src_length ? src_length : (value.size() - src_offset),
      value.data() + src_offset);
}

void WebGL2RenderingContextBase::uniform2uiv(
    const WebGLUniformLocation* location,
    const FlexibleUint32ArrayView& v,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() || !ValidateUniformParameters("uniform2uiv", location, v,
                                                    2, src_offset, src_length))
    return;

  ContextGL()->Uniform2uiv(
      location->Location(),
      (src_length
           ? src_length
           : (base::checked_cast<GLuint>(v.lengthAsSizeT()) - src_offset)) >>
          1,
      v.DataMaybeOnStack() + src_offset);
}

void WebGL2RenderingContextBase::uniform2uiv(
    const WebGLUniformLocation* location,
    Vector<GLuint>& value,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() ||
      !ValidateUniformParameters("uniform2uiv", location, value.data(),
                                 value.size(), 2, src_offset, src_length))
    return;

  ContextGL()->Uniform2uiv(
      location->Location(),
      (src_length ? src_length : (value.size() - src_offset)) >> 1,
      value.data() + src_offset);
}

void WebGL2RenderingContextBase::uniform3uiv(
    const WebGLUniformLocation* location,
    const FlexibleUint32ArrayView& v,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() || !ValidateUniformParameters("uniform3uiv", location, v,
                                                    3, src_offset, src_length))
    return;

  ContextGL()->Uniform3uiv(
      location->Location(),
      (src_length
           ? src_length
           : (base::checked_cast<GLuint>(v.lengthAsSizeT()) - src_offset)) /
          3,
      v.DataMaybeOnStack() + src_offset);
}

void WebGL2RenderingContextBase::uniform3uiv(
    const WebGLUniformLocation* location,
    Vector<GLuint>& value,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() ||
      !ValidateUniformParameters("uniform3uiv", location, value.data(),
                                 value.size(), 3, src_offset, src_length))
    return;

  ContextGL()->Uniform3uiv(
      location->Location(),
      (src_length ? src_length : (value.size() - src_offset)) / 3,
      value.data() + src_offset);
}

void WebGL2RenderingContextBase::uniform4uiv(
    const WebGLUniformLocation* location,
    const FlexibleUint32ArrayView& v,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() || !ValidateUniformParameters("uniform4uiv", location, v,
                                                    4, src_offset, src_length))
    return;

  ContextGL()->Uniform4uiv(
      location->Location(),
      (src_length
           ? src_length
           : (base::checked_cast<GLuint>(v.lengthAsSizeT()) - src_offset)) >>
          2,
      v.DataMaybeOnStack() + src_offset);
}

void WebGL2RenderingContextBase::uniform4uiv(
    const WebGLUniformLocation* location,
    Vector<GLuint>& value,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() ||
      !ValidateUniformParameters("uniform4uiv", location, value.data(),
                                 value.size(), 4, src_offset, src_length))
    return;

  ContextGL()->Uniform4uiv(
      location->Location(),
      (src_length ? src_length : (value.size() - src_offset)) >> 2,
      value.data() + src_offset);
}

void WebGL2RenderingContextBase::uniformMatrix2fv(
    const WebGLUniformLocation* location,
    GLboolean transpose,
    MaybeShared<DOMFloat32Array> v,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() ||
      !ValidateUniformMatrixParameters("uniformMatrix2fv", location, transpose,
                                       v.View(), 4, src_offset, src_length))
    return;
  ContextGL()->UniformMatrix2fv(
      location->Location(),
      (src_length ? src_length
                  : (base::checked_cast<GLuint>(v.View()->lengthAsSizeT()) -
                     src_offset)) >>
          2,
      transpose, v.View()->DataMaybeShared() + src_offset);
}

void WebGL2RenderingContextBase::uniformMatrix2fv(
    const WebGLUniformLocation* location,
    GLboolean transpose,
    Vector<GLfloat>& v,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() || !ValidateUniformMatrixParameters(
                             "uniformMatrix2fv", location, transpose, v.data(),
                             v.size(), 4, src_offset, src_length))
    return;
  ContextGL()->UniformMatrix2fv(
      location->Location(),
      (src_length ? src_length : (v.size() - src_offset)) >> 2, transpose,
      v.data() + src_offset);
}

void WebGL2RenderingContextBase::uniformMatrix3fv(
    const WebGLUniformLocation* location,
    GLboolean transpose,
    MaybeShared<DOMFloat32Array> v,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() ||
      !ValidateUniformMatrixParameters("uniformMatrix3fv", location, transpose,
                                       v.View(), 9, src_offset, src_length))
    return;
  ContextGL()->UniformMatrix3fv(
      location->Location(),
      (src_length ? src_length
                  : (base::checked_cast<GLuint>(v.View()->lengthAsSizeT()) -
                     src_offset)) /
          9,
      transpose, v.View()->DataMaybeShared() + src_offset);
}

void WebGL2RenderingContextBase::uniformMatrix3fv(
    const WebGLUniformLocation* location,
    GLboolean transpose,
    Vector<GLfloat>& v,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() || !ValidateUniformMatrixParameters(
                             "uniformMatrix3fv", location, transpose, v.data(),
                             v.size(), 9, src_offset, src_length))
    return;
  ContextGL()->UniformMatrix3fv(
      location->Location(),
      (src_length ? src_length : (v.size() - src_offset)) / 9, transpose,
      v.data() + src_offset);
}

void WebGL2RenderingContextBase::uniformMatrix4fv(
    const WebGLUniformLocation* location,
    GLboolean transpose,
    MaybeShared<DOMFloat32Array> v,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() ||
      !ValidateUniformMatrixParameters("uniformMatrix4fv", location, transpose,
                                       v.View(), 16, src_offset, src_length))
    return;
  ContextGL()->UniformMatrix4fv(
      location->Location(),
      (src_length ? src_length
                  : (base::checked_cast<GLuint>(v.View()->lengthAsSizeT()) -
                     src_offset)) >>
          4,
      transpose, v.View()->DataMaybeShared() + src_offset);
}

void WebGL2RenderingContextBase::uniformMatrix4fv(
    const WebGLUniformLocation* location,
    GLboolean transpose,
    Vector<GLfloat>& v,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() || !ValidateUniformMatrixParameters(
                             "uniformMatrix4fv", location, transpose, v.data(),
                             v.size(), 16, src_offset, src_length))
    return;
  ContextGL()->UniformMatrix4fv(
      location->Location(),
      (src_length ? src_length : (v.size() - src_offset)) >> 4, transpose,
      v.data() + src_offset);
}

void WebGL2RenderingContextBase::uniformMatrix2x3fv(
    const WebGLUniformLocation* location,
    GLboolean transpose,
    MaybeShared<DOMFloat32Array> value,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() || !ValidateUniformMatrixParameters(
                             "uniformMatrix2x3fv", location, transpose,
                             value.View(), 6, src_offset, src_length))
    return;
  ContextGL()->UniformMatrix2x3fv(
      location->Location(),
      (src_length ? src_length
                  : (base::checked_cast<GLuint>(value.View()->lengthAsSizeT()) -
                     src_offset)) /
          6,
      transpose, value.View()->DataMaybeShared() + src_offset);
}

void WebGL2RenderingContextBase::uniformMatrix2x3fv(
    const WebGLUniformLocation* location,
    GLboolean transpose,
    Vector<GLfloat>& value,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() ||
      !ValidateUniformMatrixParameters("uniformMatrix2x3fv", location,
                                       transpose, value.data(), value.size(), 6,
                                       src_offset, src_length))
    return;
  ContextGL()->UniformMatrix2x3fv(
      location->Location(),
      (src_length ? src_length : (value.size() - src_offset)) / 6, transpose,
      value.data() + src_offset);
}

void WebGL2RenderingContextBase::uniformMatrix3x2fv(
    const WebGLUniformLocation* location,
    GLboolean transpose,
    MaybeShared<DOMFloat32Array> value,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() || !ValidateUniformMatrixParameters(
                             "uniformMatrix3x2fv", location, transpose,
                             value.View(), 6, src_offset, src_length))
    return;
  ContextGL()->UniformMatrix3x2fv(
      location->Location(),
      (src_length ? src_length
                  : (base::checked_cast<GLuint>(value.View()->lengthAsSizeT()) -
                     src_offset)) /
          6,
      transpose, value.View()->DataMaybeShared() + src_offset);
}

void WebGL2RenderingContextBase::uniformMatrix3x2fv(
    const WebGLUniformLocation* location,
    GLboolean transpose,
    Vector<GLfloat>& value,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() ||
      !ValidateUniformMatrixParameters("uniformMatrix3x2fv", location,
                                       transpose, value.data(), value.size(), 6,
                                       src_offset, src_length))
    return;
  ContextGL()->UniformMatrix3x2fv(
      location->Location(),
      (src_length ? src_length : (value.size() - src_offset)) / 6, transpose,
      value.data() + src_offset);
}

void WebGL2RenderingContextBase::uniformMatrix2x4fv(
    const WebGLUniformLocation* location,
    GLboolean transpose,
    MaybeShared<DOMFloat32Array> value,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() || !ValidateUniformMatrixParameters(
                             "uniformMatrix2x4fv", location, transpose,
                             value.View(), 8, src_offset, src_length))
    return;
  ContextGL()->UniformMatrix2x4fv(
      location->Location(),
      (src_length ? src_length
                  : (base::checked_cast<GLuint>(value.View()->lengthAsSizeT()) -
                     src_offset)) >>
          3,
      transpose, value.View()->DataMaybeShared() + src_offset);
}

void WebGL2RenderingContextBase::uniformMatrix2x4fv(
    const WebGLUniformLocation* location,
    GLboolean transpose,
    Vector<GLfloat>& value,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() ||
      !ValidateUniformMatrixParameters("uniformMatrix2x4fv", location,
                                       transpose, value.data(), value.size(), 8,
                                       src_offset, src_length))
    return;
  ContextGL()->UniformMatrix2x4fv(
      location->Location(),
      (src_length ? src_length : (value.size() - src_offset)) >> 3, transpose,
      value.data() + src_offset);
}

void WebGL2RenderingContextBase::uniformMatrix4x2fv(
    const WebGLUniformLocation* location,
    GLboolean transpose,
    MaybeShared<DOMFloat32Array> value,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() || !ValidateUniformMatrixParameters(
                             "uniformMatrix4x2fv", location, transpose,
                             value.View(), 8, src_offset, src_length))
    return;
  ContextGL()->UniformMatrix4x2fv(
      location->Location(),
      (src_length ? src_length
                  : (base::checked_cast<GLuint>(value.View()->lengthAsSizeT()) -
                     src_offset)) >>
          3,
      transpose, value.View()->DataMaybeShared() + src_offset);
}

void WebGL2RenderingContextBase::uniformMatrix4x2fv(
    const WebGLUniformLocation* location,
    GLboolean transpose,
    Vector<GLfloat>& value,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() ||
      !ValidateUniformMatrixParameters("uniformMatrix4x2fv", location,
                                       transpose, value.data(), value.size(), 8,
                                       src_offset, src_length))
    return;
  ContextGL()->UniformMatrix4x2fv(
      location->Location(),
      (src_length ? src_length : (value.size() - src_offset)) >> 3, transpose,
      value.data() + src_offset);
}

void WebGL2RenderingContextBase::uniformMatrix3x4fv(
    const WebGLUniformLocation* location,
    GLboolean transpose,
    MaybeShared<DOMFloat32Array> value,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() || !ValidateUniformMatrixParameters(
                             "uniformMatrix3x4fv", location, transpose,
                             value.View(), 12, src_offset, src_length))
    return;
  ContextGL()->UniformMatrix3x4fv(
      location->Location(),
      (src_length ? src_length
                  : (base::checked_cast<GLuint>(value.View()->lengthAsSizeT()) -
                     src_offset)) /
          12,
      transpose, value.View()->DataMaybeShared() + src_offset);
}

void WebGL2RenderingContextBase::uniformMatrix3x4fv(
    const WebGLUniformLocation* location,
    GLboolean transpose,
    Vector<GLfloat>& value,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() ||
      !ValidateUniformMatrixParameters("uniformMatrix3x4fv", location,
                                       transpose, value.data(), value.size(),
                                       12, src_offset, src_length))
    return;
  ContextGL()->UniformMatrix3x4fv(
      location->Location(),
      (src_length ? src_length : (value.size() - src_offset)) / 12, transpose,
      value.data() + src_offset);
}

void WebGL2RenderingContextBase::uniformMatrix4x3fv(
    const WebGLUniformLocation* location,
    GLboolean transpose,
    MaybeShared<DOMFloat32Array> value,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() || !ValidateUniformMatrixParameters(
                             "uniformMatrix4x3fv", location, transpose,
                             value.View(), 12, src_offset, src_length))
    return;
  ContextGL()->UniformMatrix4x3fv(
      location->Location(),
      (src_length ? src_length
                  : (base::checked_cast<GLuint>(value.View()->lengthAsSizeT()) -
                     src_offset)) /
          12,
      transpose, value.View()->DataMaybeShared() + src_offset);
}

void WebGL2RenderingContextBase::uniformMatrix4x3fv(
    const WebGLUniformLocation* location,
    GLboolean transpose,
    Vector<GLfloat>& value,
    GLuint src_offset,
    GLuint src_length) {
  if (isContextLost() ||
      !ValidateUniformMatrixParameters("uniformMatrix4x3fv", location,
                                       transpose, value.data(), value.size(),
                                       12, src_offset, src_length))
    return;
  ContextGL()->UniformMatrix4x3fv(
      location->Location(),
      (src_length ? src_length : (value.size() - src_offset)) / 12, transpose,
      value.data() + src_offset);
}

void WebGL2RenderingContextBase::uniform1fv(
    const WebGLUniformLocation* location,
    const FlexibleFloat32ArrayView& v) {
  WebGLRenderingContextBase::uniform1fv(location, v);
}

void WebGL2RenderingContextBase::uniform1fv(
    const WebGLUniformLocation* location,
    Vector<GLfloat>& v) {
  WebGLRenderingContextBase::uniform1fv(location, v);
}

void WebGL2RenderingContextBase::uniform2fv(
    const WebGLUniformLocation* location,
    const FlexibleFloat32ArrayView& v) {
  WebGLRenderingContextBase::uniform2fv(location, v);
}

void WebGL2RenderingContextBase::uniform2fv(
    const WebGLUniformLocation* location,
    Vector<GLfloat>& v) {
  WebGLRenderingContextBase::uniform2fv(location, v);
}

void WebGL2RenderingContextBase::uniform3fv(
    const WebGLUniformLocation* location,
    const FlexibleFloat32ArrayView& v) {
  WebGLRenderingContextBase::uniform3fv(location, v);
}

void WebGL2RenderingContextBase::uniform3fv(
    const WebGLUniformLocation* location,
    Vector<GLfloat>& v) {
  WebGLRenderingContextBase::uniform3fv(location, v);
}

void WebGL2RenderingContextBase::uniform4fv(
    const WebGLUniformLocation* location,
    const FlexibleFloat32ArrayView& v) {
  WebGLRenderingContextBase::uniform4fv(location, v);
}

void WebGL2RenderingContextBase::uniform4fv(
    const WebGLUniformLocation* location,
    Vector<GLfloat>& v) {
  WebGLRenderingContextBase::uniform4fv(location, v);
}

void WebGL2RenderingContextBase::uniform1iv(
    const WebGLUniformLocation* location,
    const FlexibleInt32ArrayView& v) {
  WebGLRenderingContextBase::uniform1iv(location, v);
}

void WebGL2RenderingContextBase::uniform1iv(
    const WebGLUniformLocation* location,
    Vector<GLint>& v) {
  WebGLRenderingContextBase::uniform1iv(location, v);
}

void WebGL2RenderingContextBase::uniform2iv(
    const WebGLUniformLocation* location,
    const FlexibleInt32ArrayView& v) {
  WebGLRenderingContextBase::uniform2iv(location, v);
}

void WebGL2RenderingContextBase::uniform2iv(
    const WebGLUniformLocation* location,
    Vector<GLint>& v) {
  WebGLRenderingContextBase::uniform2iv(location, v);
}

void WebGL2RenderingContextBase::uniform3iv(
    const WebGLUniformLocation* location,
    const FlexibleInt32ArrayView& v) {
  WebGLRenderingContextBase::uniform3iv(location, v);
}

void WebGL2RenderingContextBase::uniform3iv(
    const WebGLUniformLocation* location,
    Vector<GLint>& v) {
  WebGLRenderingContextBase::uniform3iv(location, v);
}

void WebGL2RenderingContextBase::uniform4iv(
    const WebGLUniformLocation* location,
    const FlexibleInt32ArrayView& v) {
  WebGLRenderingContextBase::uniform4iv(location, v);
}

void WebGL2RenderingContextBase::uniform4iv(
    const WebGLUniformLocation* location,
    Vector<GLint>& v) {
  WebGLRenderingContextBase::uniform4iv(location, v);
}

void WebGL2RenderingContextBase::uniformMatrix2fv(
    const WebGLUniformLocation* location,
    GLboolean transpose,
    MaybeShared<DOMFloat32Array> v) {
  WebGLRenderingContextBase::uniformMatrix2fv(location, transpose, v);
}

void WebGL2RenderingContextBase::uniformMatrix2fv(
    const WebGLUniformLocation* location,
    GLboolean transpose,
    Vector<GLfloat>& v) {
  WebGLRenderingContextBase::uniformMatrix2fv(location, transpose, v);
}

void WebGL2RenderingContextBase::uniformMatrix3fv(
    const WebGLUniformLocation* location,
    GLboolean transpose,
    MaybeShared<DOMFloat32Array> v) {
  WebGLRenderingContextBase::uniformMatrix3fv(location, transpose, v);
}

void WebGL2RenderingContextBase::uniformMatrix3fv(
    const WebGLUniformLocation* location,
    GLboolean transpose,
    Vector<GLfloat>& v) {
  WebGLRenderingContextBase::uniformMatrix3fv(location, transpose, v);
}

void WebGL2RenderingContextBase::uniformMatrix4fv(
    const WebGLUniformLocation* location,
    GLboolean transpose,
    MaybeShared<DOMFloat32Array> v) {
  WebGLRenderingContextBase::uniformMatrix4fv(location, transpose, v);
}

void WebGL2RenderingContextBase::uniformMatrix4fv(
    const WebGLUniformLocation* location,
    GLboolean transpose,
    Vector<GLfloat>& v) {
  WebGLRenderingContextBase::uniformMatrix4fv(location, transpose, v);
}

void WebGL2RenderingContextBase::vertexAttribI4i(GLuint index,
                                                 GLint x,
                                                 GLint y,
                                                 GLint z,
                                                 GLint w) {
  if (isContextLost())
    return;
  ContextGL()->VertexAttribI4i(index, x, y, z, w);
  SetVertexAttribType(index, kInt32ArrayType);
}

void WebGL2RenderingContextBase::vertexAttribI4iv(
    GLuint index,
    MaybeShared<const DOMInt32Array> v) {
  if (isContextLost())
    return;
  if (!v.View() || v.View()->lengthAsSizeT() < 4) {
    SynthesizeGLError(GL_INVALID_VALUE, "vertexAttribI4iv", "invalid array");
    return;
  }
  ContextGL()->VertexAttribI4iv(index, v.View()->DataMaybeShared());
  SetVertexAttribType(index, kInt32ArrayType);
}

void WebGL2RenderingContextBase::vertexAttribI4iv(GLuint index,
                                                  const Vector<GLint>& v) {
  if (isContextLost())
    return;
  if (v.size() < 4) {
    SynthesizeGLError(GL_INVALID_VALUE, "vertexAttribI4iv", "invalid array");
    return;
  }
  ContextGL()->VertexAttribI4iv(index, v.data());
  SetVertexAttribType(index, kInt32ArrayType);
}

void WebGL2RenderingContextBase::vertexAttribI4ui(GLuint index,
                                                  GLuint x,
                                                  GLuint y,
                                                  GLuint z,
                                                  GLuint w) {
  if (isContextLost())
    return;
  ContextGL()->VertexAttribI4ui(index, x, y, z, w);
  SetVertexAttribType(index, kUint32ArrayType);
}

void WebGL2RenderingContextBase::vertexAttribI4uiv(
    GLuint index,
    MaybeShared<const DOMUint32Array> v) {
  if (isContextLost())
    return;
  if (!v.View() || v.View()->lengthAsSizeT() < 4) {
    SynthesizeGLError(GL_INVALID_VALUE, "vertexAttribI4uiv", "invalid array");
    return;
  }
  ContextGL()->VertexAttribI4uiv(index, v.View()->DataMaybeShared());
  SetVertexAttribType(index, kUint32ArrayType);
}

void WebGL2RenderingContextBase::vertexAttribI4uiv(GLuint index,
                                                   const Vector<GLuint>& v) {
  if (isContextLost())
    return;
  if (v.size() < 4) {
    SynthesizeGLError(GL_INVALID_VALUE, "vertexAttribI4uiv", "invalid array");
    return;
  }
  ContextGL()->VertexAttribI4uiv(index, v.data());
  SetVertexAttribType(index, kUint32ArrayType);
}

void WebGL2RenderingContextBase::vertexAttribIPointer(GLuint index,
                                                      GLint size,
                                                      GLenum type,
                                                      GLsizei stride,
                                                      int64_t offset) {
  if (isContextLost())
    return;
  if (index >= max_vertex_attribs_) {
    SynthesizeGLError(GL_INVALID_VALUE, "vertexAttribIPointer",
                      "index out of range");
    return;
  }
  if (!ValidateValueFitNonNegInt32("vertexAttribIPointer", "offset", offset))
    return;
  if (!bound_array_buffer_ && offset != 0) {
    SynthesizeGLError(GL_INVALID_OPERATION, "vertexAttribIPointer",
                      "no ARRAY_BUFFER is bound and offset is non-zero");
    return;
  }

  bound_vertex_array_object_->SetArrayBufferForAttrib(index,
                                                      bound_array_buffer_);
  ContextGL()->VertexAttribIPointer(
      index, size, type, stride,
      reinterpret_cast<void*>(static_cast<intptr_t>(offset)));
}

/* Writing to the drawing buffer */
void WebGL2RenderingContextBase::vertexAttribDivisor(GLuint index,
                                                     GLuint divisor) {
  if (isContextLost())
    return;

  if (index >= max_vertex_attribs_) {
    SynthesizeGLError(GL_INVALID_VALUE, "vertexAttribDivisor",
                      "index out of range");
    return;
  }

  ContextGL()->VertexAttribDivisorANGLE(index, divisor);
}

void WebGL2RenderingContextBase::drawArraysInstanced(GLenum mode,
                                                     GLint first,
                                                     GLsizei count,
                                                     GLsizei instance_count) {
  if (!ValidateDrawArrays("drawArraysInstanced"))
    return;

  if (!bound_vertex_array_object_->IsAllEnabledAttribBufferBound()) {
    SynthesizeGLError(GL_INVALID_OPERATION, "drawArraysInstanced",
                      "no buffer is bound to enabled attribute");
    return;
  }

  ScopedRGBEmulationColorMask emulation_color_mask(this, color_mask_,
                                                   drawing_buffer_.get());
  OnBeforeDrawCall();
  ContextGL()->DrawArraysInstancedANGLE(mode, first, count, instance_count);
}

void WebGL2RenderingContextBase::drawElementsInstanced(GLenum mode,
                                                       GLsizei count,
                                                       GLenum type,
                                                       int64_t offset,
                                                       GLsizei instance_count) {
  if (!ValidateDrawElements("drawElementsInstanced", type, offset))
    return;

  if (!bound_vertex_array_object_->IsAllEnabledAttribBufferBound()) {
    SynthesizeGLError(GL_INVALID_OPERATION, "drawElementsInstanced",
                      "no buffer is bound to enabled attribute");
    return;
  }

  ScopedRGBEmulationColorMask emulation_color_mask(this, color_mask_,
                                                   drawing_buffer_.get());
  OnBeforeDrawCall();
  ContextGL()->DrawElementsInstancedANGLE(
      mode, count, type, reinterpret_cast<void*>(static_cast<intptr_t>(offset)),
      instance_count);
}

void WebGL2RenderingContextBase::drawRangeElements(GLenum mode,
                                                   GLuint start,
                                                   GLuint end,
                                                   GLsizei count,
                                                   GLenum type,
                                                   int64_t offset) {
  if (!ValidateDrawElements("drawRangeElements", type, offset))
    return;

  if (!bound_vertex_array_object_->IsAllEnabledAttribBufferBound()) {
    SynthesizeGLError(GL_INVALID_OPERATION, "drawRangeElements",
                      "no buffer is bound to enabled attribute");
    return;
  }

  ScopedRGBEmulationColorMask emulation_color_mask(this, color_mask_,
                                                   drawing_buffer_.get());
  OnBeforeDrawCall();
  ContextGL()->DrawRangeElements(
      mode, start, end, count, type,
      reinterpret_cast<void*>(static_cast<intptr_t>(offset)));
}

void WebGL2RenderingContextBase::drawBuffers(const Vector<GLenum>& buffers) {
  if (isContextLost())
    return;

  ScopedRGBEmulationColorMask emulation_color_mask(this, color_mask_,
                                                   drawing_buffer_.get());
  GLsizei n = buffers.size();
  const GLenum* bufs = buffers.data();
  for (GLsizei i = 0; i < n; ++i) {
    switch (bufs[i]) {
      case GL_NONE:
      case GL_BACK:
      case GL_COLOR_ATTACHMENT0:
        break;
      default:
        if (bufs[i] > GL_COLOR_ATTACHMENT0 &&
            bufs[i] < static_cast<GLenum>(GL_COLOR_ATTACHMENT0 +
                                          MaxColorAttachments()))
          break;
        SynthesizeGLError(GL_INVALID_ENUM, "drawBuffers", "invalid buffer");
        return;
    }
  }
  if (!framebuffer_binding_) {
    if (n != 1) {
      SynthesizeGLError(GL_INVALID_OPERATION, "drawBuffers",
                        "the number of buffers is not 1");
      return;
    }
    if (bufs[0] != GL_BACK && bufs[0] != GL_NONE) {
      SynthesizeGLError(GL_INVALID_OPERATION, "drawBuffers", "BACK or NONE");
      return;
    }
    // Because the backbuffer is simulated on all current WebKit ports, we need
    // to change BACK to COLOR_ATTACHMENT0.
    GLenum value = (bufs[0] == GL_BACK) ? GL_COLOR_ATTACHMENT0 : GL_NONE;
    ContextGL()->DrawBuffersEXT(1, &value);
    SetBackDrawBuffer(bufs[0]);
  } else {
    if (n > MaxDrawBuffers()) {
      SynthesizeGLError(GL_INVALID_VALUE, "drawBuffers",
                        "more than max draw buffers");
      return;
    }
    for (GLsizei i = 0; i < n; ++i) {
      if (bufs[i] != GL_NONE &&
          bufs[i] != static_cast<GLenum>(GL_COLOR_ATTACHMENT0_EXT + i)) {
        SynthesizeGLError(GL_INVALID_OPERATION, "drawBuffers",
                          "COLOR_ATTACHMENTi_EXT or NONE");
        return;
      }
    }
    framebuffer_binding_->DrawBuffers(buffers);
  }
}

bool WebGL2RenderingContextBase::ValidateClearBuffer(const char* function_name,
                                                     GLenum buffer,
                                                     size_t size,
                                                     GLuint src_offset) {
  base::CheckedNumeric<GLsizei> checked_size(size);
  checked_size -= src_offset;
  if (!checked_size.IsValid()) {
    SynthesizeGLError(GL_INVALID_VALUE, function_name,
                      "invalid array size / srcOffset");
    return false;
  }
  switch (buffer) {
    case GL_COLOR:
      if (checked_size.ValueOrDie() < 4) {
        SynthesizeGLError(GL_INVALID_VALUE, function_name,
                          "invalid array size / srcOffset");
        return false;
      }
      break;
    case GL_DEPTH:
    case GL_STENCIL:
      if (checked_size.ValueOrDie() < 1) {
        SynthesizeGLError(GL_INVALID_VALUE, function_name,
                          "invalid array size / srcOffset");
        return false;
      }
      break;
    default:
      SynthesizeGLError(GL_INVALID_ENUM, function_name, "invalid buffer");
      return false;
  }
  return true;
}

WebGLTexture* WebGL2RenderingContextBase::ValidateTexImageBinding(
    const char* func_name,
    TexImageFunctionID function_id,
    GLenum target) {
  if (function_id == kTexImage3D || function_id == kTexSubImage3D)
    return ValidateTexture3DBinding(func_name, target);
  return ValidateTexture2DBinding(func_name, target);
}

void WebGL2RenderingContextBase::clearBufferiv(GLenum buffer,
                                               GLint drawbuffer,
                                               MaybeShared<DOMInt32Array> value,
                                               GLuint src_offset) {
  if (isContextLost() ||
      !ValidateClearBuffer("clearBufferiv", buffer,
                           value.View()->lengthAsSizeT(), src_offset))
    return;

  ScopedRGBEmulationColorMask emulation_color_mask(this, color_mask_,
                                                   drawing_buffer_.get());

  ContextGL()->ClearBufferiv(buffer, drawbuffer,
                             value.View()->DataMaybeShared() + src_offset);
  UpdateBuffersToAutoClear(kClearBufferiv, buffer, drawbuffer);
}

void WebGL2RenderingContextBase::clearBufferiv(GLenum buffer,
                                               GLint drawbuffer,
                                               const Vector<GLint>& value,
                                               GLuint src_offset) {
  if (isContextLost() ||
      !ValidateClearBuffer("clearBufferiv", buffer, value.size(), src_offset))
    return;

  ScopedRGBEmulationColorMask emulation_color_mask(this, color_mask_,
                                                   drawing_buffer_.get());

  ContextGL()->ClearBufferiv(buffer, drawbuffer, value.data() + src_offset);
  UpdateBuffersToAutoClear(kClearBufferiv, buffer, drawbuffer);
}

void WebGL2RenderingContextBase::clearBufferuiv(
    GLenum buffer,
    GLint drawbuffer,
    MaybeShared<DOMUint32Array> value,
    GLuint src_offset) {
  if (isContextLost() ||
      !ValidateClearBuffer("clearBufferuiv", buffer,
                           value.View()->lengthAsSizeT(), src_offset))
    return;

  ScopedRGBEmulationColorMask emulation_color_mask(this, color_mask_,
                                                   drawing_buffer_.get());

  ContextGL()->ClearBufferuiv(buffer, drawbuffer,
                              value.View()->DataMaybeShared() + src_offset);
  UpdateBuffersToAutoClear(kClearBufferuiv, buffer, drawbuffer);
}

void WebGL2RenderingContextBase::clearBufferuiv(GLenum buffer,
                                                GLint drawbuffer,
                                                const Vector<GLuint>& value,
                                                GLuint src_offset) {
  if (isContextLost() ||
      !ValidateClearBuffer("clearBufferuiv", buffer, value.size(), src_offset))
    return;

  ScopedRGBEmulationColorMask emulation_color_mask(this, color_mask_,
                                                   drawing_buffer_.get());

  ContextGL()->ClearBufferuiv(buffer, drawbuffer, value.data() + src_offset);
  UpdateBuffersToAutoClear(kClearBufferuiv, buffer, drawbuffer);
}

void WebGL2RenderingContextBase::clearBufferfv(
    GLenum buffer,
    GLint drawbuffer,
    MaybeShared<DOMFloat32Array> value,
    GLuint src_offset) {
  if (isContextLost() ||
      !ValidateClearBuffer("clearBufferfv", buffer,
                           value.View()->lengthAsSizeT(), src_offset))
    return;

  // As of this writing the default back buffer will always have an
  // RGB(A)/UNSIGNED_BYTE color attachment, so only clearBufferfv can
  // be used with it and consequently the emulation should only be
  // needed here. However, as support for extended color spaces is
  // added, the type of the back buffer might change, so do the
  // emulation for all clearBuffer entry points instead of just here.
  ScopedRGBEmulationColorMask emulation_color_mask(this, color_mask_,
                                                   drawing_buffer_.get());

  ContextGL()->ClearBufferfv(buffer, drawbuffer,
                             value.View()->DataMaybeShared() + src_offset);
  // clearBufferiv and clearBufferuiv will currently generate an error
  // if they're called against the default back buffer. If support for
  // extended canvas color spaces is added, this call might need to be
  // added to the other versions.
  MarkContextChanged(kCanvasChanged);
  UpdateBuffersToAutoClear(kClearBufferfv, buffer, drawbuffer);
}

void WebGL2RenderingContextBase::clearBufferfv(GLenum buffer,
                                               GLint drawbuffer,
                                               const Vector<GLfloat>& value,
                                               GLuint src_offset) {
  if (isContextLost() ||
      !ValidateClearBuffer("clearBufferfv", buffer, value.size(), src_offset))
    return;

  // As of this writing the default back buffer will always have an
  // RGB(A)/UNSIGNED_BYTE color attachment, so only clearBufferfv can
  // be used with it and consequently the emulation should only be
  // needed here. However, as support for extended color spaces is
  // added, the type of the back buffer might change, so do the
  // emulation for all clearBuffer entry points instead of just here.
  ScopedRGBEmulationColorMask emulation_color_mask(this, color_mask_,
                                                   drawing_buffer_.get());

  ContextGL()->ClearBufferfv(buffer, drawbuffer, value.data() + src_offset);
  // clearBufferiv and clearBufferuiv will currently generate an error
  // if they're called against the default back buffer. If support for
  // extended canvas color spaces is added, this call might need to be
  // added to the other versions.
  MarkContextChanged(kCanvasChanged);
  UpdateBuffersToAutoClear(kClearBufferfv, buffer, drawbuffer);
}

void WebGL2RenderingContextBase::clearBufferfi(GLenum buffer,
                                               GLint drawbuffer,
                                               GLfloat depth,
                                               GLint stencil) {
  if (isContextLost())
    return;

  ContextGL()->ClearBufferfi(buffer, drawbuffer, depth, stencil);
  // This might have been used to clear the depth and stencil buffers
  // of the default back buffer.
  MarkContextChanged(kCanvasChanged);
  UpdateBuffersToAutoClear(kClearBufferfi, buffer, drawbuffer);
}

void WebGL2RenderingContextBase::UpdateBuffersToAutoClear(
    WebGL2RenderingContextBase::ClearBufferCaller caller,
    GLenum buffer,
    GLint drawbuffer) {
  // This method makes sure that we don't auto-clear any buffers which the
  // user has manually cleared using the new ES 3.0 clearBuffer* APIs.

  // If the user has a framebuffer bound, don't update the auto-clear
  // state of the built-in back buffer.
  if (framebuffer_binding_)
    return;

  // If the scissor test is on, assume that we can't short-circuit
  // these clears.
  if (scissor_enabled_)
    return;

  // The default back buffer only has one color attachment.
  if (drawbuffer != 0)
    return;

  // If the call to the driver generated an error, don't claim that
  // we've auto-cleared these buffers. The early returns below are for
  // cases where errors will be produced.

  // The default back buffer is currently always RGB(A)8, which
  // restricts the variants which can legally be used to clear the
  // color buffer. TODO(crbug.com/829632): this needs to be
  // generalized.
  switch (caller) {
    case kClearBufferiv:
      if (buffer != GL_STENCIL)
        return;
      break;
    case kClearBufferfv:
      if (buffer != GL_COLOR && buffer != GL_DEPTH)
        return;
      break;
    case kClearBufferuiv:
      return;
    case kClearBufferfi:
      if (buffer != GL_DEPTH_STENCIL)
        return;
      break;
  }

  GLbitfield buffers_to_clear = 0;

  // Turn it into a bitfield and mask it off.
  switch (buffer) {
    case GL_COLOR:
      buffers_to_clear = GL_COLOR_BUFFER_BIT;
      break;
    case GL_DEPTH:
      buffers_to_clear = GL_DEPTH_BUFFER_BIT;
      break;
    case GL_STENCIL:
      buffers_to_clear = GL_STENCIL_BUFFER_BIT;
      break;
    case GL_DEPTH_STENCIL:
      buffers_to_clear = GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
      break;
    default:
      // Illegal value.
      return;
  }

  GetDrawingBuffer()->SetBuffersToAutoClear(
      GetDrawingBuffer()->GetBuffersToAutoClear() & (~buffers_to_clear));
}

WebGLQuery* WebGL2RenderingContextBase::createQuery() {
  if (isContextLost())
    return nullptr;
  return MakeGarbageCollected<WebGLQuery>(this);
}

void WebGL2RenderingContextBase::deleteQuery(WebGLQuery* query) {
  if (isContextLost() || !query)
    return;

  if (current_boolean_occlusion_query_ == query) {
    ContextGL()->EndQueryEXT(current_boolean_occlusion_query_->GetTarget());
    current_boolean_occlusion_query_ = nullptr;
  }

  if (current_transform_feedback_primitives_written_query_ == query) {
    ContextGL()->EndQueryEXT(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
    current_transform_feedback_primitives_written_query_ = nullptr;
  }

  if (current_elapsed_query_ == query) {
    ContextGL()->EndQueryEXT(current_elapsed_query_->GetTarget());
    current_elapsed_query_ = nullptr;
  }

  DeleteObject(query);
}

GLboolean WebGL2RenderingContextBase::isQuery(WebGLQuery* query) {
  if (!query || isContextLost() || !query->Validate(ContextGroup(), this))
    return 0;

  if (query->MarkedForDeletion())
    return 0;

  return ContextGL()->IsQueryEXT(query->Object());
}

void WebGL2RenderingContextBase::beginQuery(GLenum target, WebGLQuery* query) {
  if (!ValidateWebGLObject("beginQuery", query))
    return;

  if (query->GetTarget() && query->GetTarget() != target) {
    SynthesizeGLError(GL_INVALID_OPERATION, "beginQuery",
                      "query type does not match target");
    return;
  }

  switch (target) {
    case GL_ANY_SAMPLES_PASSED:
    case GL_ANY_SAMPLES_PASSED_CONSERVATIVE: {
      if (current_boolean_occlusion_query_) {
        SynthesizeGLError(GL_INVALID_OPERATION, "beginQuery",
                          "a query is already active for target");
        return;
      }
      current_boolean_occlusion_query_ = query;
    } break;
    case GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN: {
      if (current_transform_feedback_primitives_written_query_) {
        SynthesizeGLError(GL_INVALID_OPERATION, "beginQuery",
                          "a query is already active for target");
        return;
      }
      current_transform_feedback_primitives_written_query_ = query;
    } break;
    case GL_TIME_ELAPSED_EXT: {
      if (!ExtensionEnabled(kEXTDisjointTimerQueryWebGL2Name)) {
        SynthesizeGLError(GL_INVALID_ENUM, "beginQuery", "invalid target");
        return;
      }
      if (current_elapsed_query_) {
        SynthesizeGLError(GL_INVALID_OPERATION, "beginQuery",
                          "a query is already active for target");
        return;
      }
      current_elapsed_query_ = query;
    } break;
    default:
      SynthesizeGLError(GL_INVALID_ENUM, "beginQuery", "invalid target");
      return;
  }

  if (!query->GetTarget())
    query->SetTarget(target);

  ContextGL()->BeginQueryEXT(target, query->Object());
}

void WebGL2RenderingContextBase::endQuery(GLenum target) {
  if (isContextLost())
    return;

  switch (target) {
    case GL_ANY_SAMPLES_PASSED:
    case GL_ANY_SAMPLES_PASSED_CONSERVATIVE: {
      if (current_boolean_occlusion_query_ &&
          current_boolean_occlusion_query_->GetTarget() == target) {
        current_boolean_occlusion_query_->ResetCachedResult();
        current_boolean_occlusion_query_ = nullptr;
      } else {
        SynthesizeGLError(GL_INVALID_OPERATION, "endQuery",
                          "target query is not active");
        return;
      }
    } break;
    case GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN: {
      if (current_transform_feedback_primitives_written_query_) {
        current_transform_feedback_primitives_written_query_
            ->ResetCachedResult();
        current_transform_feedback_primitives_written_query_ = nullptr;
      } else {
        SynthesizeGLError(GL_INVALID_OPERATION, "endQuery",
                          "target query is not active");
        return;
      }
    } break;
    case GL_TIME_ELAPSED_EXT: {
      if (!ExtensionEnabled(kEXTDisjointTimerQueryWebGL2Name)) {
        SynthesizeGLError(GL_INVALID_ENUM, "endQuery", "invalid target");
        return;
      }
      if (current_elapsed_query_) {
        current_elapsed_query_->ResetCachedResult();
        current_elapsed_query_ = nullptr;
      } else {
        SynthesizeGLError(GL_INVALID_OPERATION, "endQuery",
                          "target query is not active");
        return;
      }
    } break;
    default:
      SynthesizeGLError(GL_INVALID_ENUM, "endQuery", "invalid target");
      return;
  }

  ContextGL()->EndQueryEXT(target);
}

ScriptValue WebGL2RenderingContextBase::getQuery(ScriptState* script_state,
                                                 GLenum target,
                                                 GLenum pname) {
  if (isContextLost())
    return ScriptValue::CreateNull(script_state->GetIsolate());

  if (ExtensionEnabled(kEXTDisjointTimerQueryWebGL2Name)) {
    if (pname == GL_QUERY_COUNTER_BITS_EXT) {
      if (target == GL_TIMESTAMP_EXT || target == GL_TIME_ELAPSED_EXT) {
        GLint value = 0;
        ContextGL()->GetQueryivEXT(target, pname, &value);
        return WebGLAny(script_state, value);
      }
      SynthesizeGLError(GL_INVALID_ENUM, "getQuery",
                        "invalid target/pname combination");
      return ScriptValue::CreateNull(script_state->GetIsolate());
    }

    if (target == GL_TIME_ELAPSED_EXT && pname == GL_CURRENT_QUERY) {
      return current_elapsed_query_
                 ? WebGLAny(script_state, current_elapsed_query_)
                 : ScriptValue::CreateNull(script_state->GetIsolate());
    }

    if (target == GL_TIMESTAMP_EXT && pname == GL_CURRENT_QUERY) {
      return ScriptValue::CreateNull(script_state->GetIsolate());
    }
  }

  if (pname != GL_CURRENT_QUERY) {
    SynthesizeGLError(GL_INVALID_ENUM, "getQuery", "invalid parameter name");
    return ScriptValue::CreateNull(script_state->GetIsolate());
  }

  switch (target) {
    case GL_ANY_SAMPLES_PASSED:
    case GL_ANY_SAMPLES_PASSED_CONSERVATIVE:
      if (current_boolean_occlusion_query_ &&
          current_boolean_occlusion_query_->GetTarget() == target)
        return WebGLAny(script_state, current_boolean_occlusion_query_);
      break;
    case GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN:
      return WebGLAny(script_state,
                      current_transform_feedback_primitives_written_query_);
    default:
      SynthesizeGLError(GL_INVALID_ENUM, "getQuery", "invalid target");
      return ScriptValue::CreateNull(script_state->GetIsolate());
  }
  return ScriptValue::CreateNull(script_state->GetIsolate());
}

ScriptValue WebGL2RenderingContextBase::getQueryParameter(
    ScriptState* script_state,
    WebGLQuery* query,
    GLenum pname) {
  if (!ValidateWebGLObject("getQueryParameter", query))
    return ScriptValue::CreateNull(script_state->GetIsolate());

  // Query is non-null at this point.
  if (!query->GetTarget()) {
    SynthesizeGLError(GL_INVALID_OPERATION, "getQueryParameter",
                      "'query' is not a query object yet, since it has't been "
                      "used by beginQuery");
    return ScriptValue::CreateNull(script_state->GetIsolate());
  }
  if (query == current_boolean_occlusion_query_ ||
      query == current_transform_feedback_primitives_written_query_ ||
      query == current_elapsed_query_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "getQueryParameter",
                      "query is currently active");
    return ScriptValue::CreateNull(script_state->GetIsolate());
  }

  switch (pname) {
    case GL_QUERY_RESULT: {
      query->UpdateCachedResult(ContextGL());
      return WebGLAny(script_state, query->GetQueryResult());
    }
    case GL_QUERY_RESULT_AVAILABLE: {
      query->UpdateCachedResult(ContextGL());
      return WebGLAny(script_state, query->IsQueryResultAvailable());
    }
    default:
      SynthesizeGLError(GL_INVALID_ENUM, "getQueryParameter",
                        "invalid parameter name");
      return ScriptValue::CreateNull(script_state->GetIsolate());
  }
}

WebGLSampler* WebGL2RenderingContextBase::createSampler() {
  if (isContextLost())
    return nullptr;
  return MakeGarbageCollected<WebGLSampler>(this);
}

void WebGL2RenderingContextBase::deleteSampler(WebGLSampler* sampler) {
  if (isContextLost())
    return;

  for (wtf_size_t i = 0; i < sampler_units_.size(); ++i) {
    if (sampler == sampler_units_[i]) {
      sampler_units_[i] = nullptr;
      ContextGL()->BindSampler(i, 0);
    }
  }

  DeleteObject(sampler);
}

GLboolean WebGL2RenderingContextBase::isSampler(WebGLSampler* sampler) {
  if (!sampler || isContextLost() || !sampler->Validate(ContextGroup(), this))
    return 0;

  if (sampler->MarkedForDeletion())
    return 0;

  return ContextGL()->IsSampler(sampler->Object());
}

void WebGL2RenderingContextBase::bindSampler(GLuint unit,
                                             WebGLSampler* sampler) {
  if (!ValidateNullableWebGLObject("bindSampler", sampler))
    return;

  if (unit >= sampler_units_.size()) {
    SynthesizeGLError(GL_INVALID_VALUE, "bindSampler",
                      "texture unit out of range");
    return;
  }

  sampler_units_[unit] = sampler;

  ContextGL()->BindSampler(unit, ObjectOrZero(sampler));
}

void WebGL2RenderingContextBase::SamplerParameter(WebGLSampler* sampler,
                                                  GLenum pname,
                                                  GLfloat paramf,
                                                  GLint parami,
                                                  bool is_float) {
  if (!ValidateWebGLObject("samplerParameter", sampler))
    return;

  GLint param;
  if (is_float) {
    param = base::saturated_cast<GLint>(paramf);
  } else {
    param = parami;
  }
  switch (pname) {
    case GL_TEXTURE_MAX_LOD:
    case GL_TEXTURE_MIN_LOD:
      break;
    case GL_TEXTURE_COMPARE_FUNC:
      switch (param) {
        case GL_LEQUAL:
        case GL_GEQUAL:
        case GL_LESS:
        case GL_GREATER:
        case GL_EQUAL:
        case GL_NOTEQUAL:
        case GL_ALWAYS:
        case GL_NEVER:
          break;
        default:
          SynthesizeGLError(GL_INVALID_ENUM, "samplerParameter",
                            "invalid parameter");
          return;
      }
      break;
    case GL_TEXTURE_COMPARE_MODE:
      switch (param) {
        case GL_COMPARE_REF_TO_TEXTURE:
        case GL_NONE:
          break;
        default:
          SynthesizeGLError(GL_INVALID_ENUM, "samplerParameter",
                            "invalid parameter");
          return;
      }
      break;
    case GL_TEXTURE_MAG_FILTER:
      switch (param) {
        case GL_NEAREST:
        case GL_LINEAR:
          break;
        default:
          SynthesizeGLError(GL_INVALID_ENUM, "samplerParameter",
                            "invalid parameter");
          return;
      }
      break;
    case GL_TEXTURE_MIN_FILTER:
      switch (param) {
        case GL_NEAREST:
        case GL_LINEAR:
        case GL_NEAREST_MIPMAP_NEAREST:
        case GL_LINEAR_MIPMAP_NEAREST:
        case GL_NEAREST_MIPMAP_LINEAR:
        case GL_LINEAR_MIPMAP_LINEAR:
          break;
        default:
          SynthesizeGLError(GL_INVALID_ENUM, "samplerParameter",
                            "invalid parameter");
          return;
      }
      break;
    case GL_TEXTURE_WRAP_R:
    case GL_TEXTURE_WRAP_S:
    case GL_TEXTURE_WRAP_T:
      switch (param) {
        case GL_CLAMP_TO_EDGE:
        case GL_MIRRORED_REPEAT:
        case GL_REPEAT:
          break;
        default:
          SynthesizeGLError(GL_INVALID_ENUM, "samplerParameter",
                            "invalid parameter");
          return;
      }
      break;
    case GL_TEXTURE_MAX_ANISOTROPY_EXT:
      if (!ExtensionEnabled(kEXTTextureFilterAnisotropicName)) {
        SynthesizeGLError(GL_INVALID_ENUM, "samplerParameter",
                          "EXT_texture_filter_anisotropic not enabled");
        return;
      }
      break;
    default:
      SynthesizeGLError(GL_INVALID_ENUM, "samplerParameter",
                        "invalid parameter name");
      return;
  }

  if (is_float) {
    ContextGL()->SamplerParameterf(ObjectOrZero(sampler), pname, paramf);
  } else {
    ContextGL()->SamplerParameteri(ObjectOrZero(sampler), pname, parami);
  }
}

void WebGL2RenderingContextBase::samplerParameteri(WebGLSampler* sampler,
                                                   GLenum pname,
                                                   GLint param) {
  SamplerParameter(sampler, pname, 0, param, false);
}

void WebGL2RenderingContextBase::samplerParameterf(WebGLSampler* sampler,
                                                   GLenum pname,
                                                   GLfloat param) {
  SamplerParameter(sampler, pname, param, 0, true);
}

ScriptValue WebGL2RenderingContextBase::getSamplerParameter(
    ScriptState* script_state,
    WebGLSampler* sampler,
    GLenum pname) {
  if (!ValidateWebGLObject("getSamplerParameter", sampler))
    return ScriptValue::CreateNull(script_state->GetIsolate());

  switch (pname) {
    case GL_TEXTURE_COMPARE_FUNC:
    case GL_TEXTURE_COMPARE_MODE:
    case GL_TEXTURE_MAG_FILTER:
    case GL_TEXTURE_MIN_FILTER:
    case GL_TEXTURE_WRAP_R:
    case GL_TEXTURE_WRAP_S:
    case GL_TEXTURE_WRAP_T: {
      GLint value = 0;
      ContextGL()->GetSamplerParameteriv(ObjectOrZero(sampler), pname, &value);
      return WebGLAny(script_state, static_cast<unsigned>(value));
    }
    case GL_TEXTURE_MAX_LOD:
    case GL_TEXTURE_MIN_LOD: {
      GLfloat value = 0.f;
      ContextGL()->GetSamplerParameterfv(ObjectOrZero(sampler), pname, &value);
      return WebGLAny(script_state, value);
    }
    case GL_TEXTURE_MAX_ANISOTROPY_EXT: {
      if (!ExtensionEnabled(kEXTTextureFilterAnisotropicName)) {
        SynthesizeGLError(GL_INVALID_ENUM, "samplerParameter",
                          "EXT_texture_filter_anisotropic not enabled");
        return ScriptValue::CreateNull(script_state->GetIsolate());
      }
      GLfloat value = 0.f;
      ContextGL()->GetSamplerParameterfv(ObjectOrZero(sampler), pname, &value);
      return WebGLAny(script_state, value);
    }
    default:
      SynthesizeGLError(GL_INVALID_ENUM, "getSamplerParameter",
                        "invalid parameter name");
      return ScriptValue::CreateNull(script_state->GetIsolate());
  }
}

WebGLSync* WebGL2RenderingContextBase::fenceSync(GLenum condition,
                                                 GLbitfield flags) {
  if (isContextLost())
    return nullptr;

  if (condition != GL_SYNC_GPU_COMMANDS_COMPLETE) {
    SynthesizeGLError(GL_INVALID_ENUM, "fenceSync",
                      "condition must be SYNC_GPU_COMMANDS_COMPLETE");
    return nullptr;
  }
  if (flags != 0) {
    SynthesizeGLError(GL_INVALID_VALUE, "fenceSync", "flags must be zero");
    return nullptr;
  }
  return MakeGarbageCollected<WebGLFenceSync>(this, condition, flags);
}

GLboolean WebGL2RenderingContextBase::isSync(WebGLSync* sync) {
  if (!sync || isContextLost() || !sync->Validate(ContextGroup(), this))
    return 0;

  if (sync->MarkedForDeletion())
    return 0;

  return sync->Object() != 0;
}

void WebGL2RenderingContextBase::deleteSync(WebGLSync* sync) {
  DeleteObject(sync);
}

GLenum WebGL2RenderingContextBase::clientWaitSync(WebGLSync* sync,
                                                  GLbitfield flags,
                                                  GLuint64 timeout) {
  if (!ValidateWebGLObject("clientWaitSync", sync))
    return GL_WAIT_FAILED;

  if (timeout > kMaxClientWaitTimeout) {
    SynthesizeGLError(GL_INVALID_OPERATION, "clientWaitSync",
                      "timeout > MAX_CLIENT_WAIT_TIMEOUT_WEBGL");
    return GL_WAIT_FAILED;
  }

  // clientWaitSync must poll for updates no more than once per
  // requestAnimationFrame, so all validation, and the implementation,
  // must be done inline.
  if (!(flags == 0 || flags == GL_SYNC_FLUSH_COMMANDS_BIT)) {
    SynthesizeGLError(GL_INVALID_VALUE, "clientWaitSync", "invalid flags");
    return GL_WAIT_FAILED;
  }

  if (sync->IsSignaled()) {
    return GL_ALREADY_SIGNALED;
  }

  sync->UpdateCache(ContextGL());

  if (sync->IsSignaled()) {
    return GL_CONDITION_SATISFIED;
  }

  return GL_TIMEOUT_EXPIRED;
}

void WebGL2RenderingContextBase::waitSync(WebGLSync* sync,
                                          GLbitfield flags,
                                          GLint64 timeout) {
  if (!ValidateWebGLObject("waitSync", sync))
    return;

  if (flags) {
    SynthesizeGLError(GL_INVALID_VALUE, "waitSync", "invalid flags");
    return;
  }

  if (timeout != -1) {
    SynthesizeGLError(GL_INVALID_VALUE, "waitSync", "invalid timeout");
    return;
  }

  // This is intentionally changed to an no-op in WebGL2.
}

ScriptValue WebGL2RenderingContextBase::getSyncParameter(
    ScriptState* script_state,
    WebGLSync* sync,
    GLenum pname) {
  if (!ValidateWebGLObject("getSyncParameter", sync))
    return ScriptValue::CreateNull(script_state->GetIsolate());

  switch (pname) {
    case GL_OBJECT_TYPE:
    case GL_SYNC_STATUS:
    case GL_SYNC_CONDITION:
    case GL_SYNC_FLAGS: {
      sync->UpdateCache(ContextGL());
      return WebGLAny(script_state, sync->GetCachedResult(pname));
    }
    default:
      SynthesizeGLError(GL_INVALID_ENUM, "getSyncParameter",
                        "invalid parameter name");
      return ScriptValue::CreateNull(script_state->GetIsolate());
  }
}

WebGLTransformFeedback* WebGL2RenderingContextBase::createTransformFeedback() {
  if (isContextLost())
    return nullptr;
  return MakeGarbageCollected<WebGLTransformFeedback>(
      this, WebGLTransformFeedback::TFTypeUser);
}

void WebGL2RenderingContextBase::deleteTransformFeedback(
    WebGLTransformFeedback* feedback) {
  // We have to short-circuit the deletion process if the transform feedback is
  // active. This requires duplication of some validation logic.
  if (!isContextLost() && feedback &&
      feedback->Validate(ContextGroup(), this)) {
    if (feedback->active()) {
      SynthesizeGLError(
          GL_INVALID_OPERATION, "deleteTransformFeedback",
          "attempt to delete an active transform feedback object");
      return;
    }
  }

  if (!DeleteObject(feedback))
    return;

  if (feedback == transform_feedback_binding_)
    transform_feedback_binding_ = default_transform_feedback_;
}

GLboolean WebGL2RenderingContextBase::isTransformFeedback(
    WebGLTransformFeedback* feedback) {
  if (!feedback || isContextLost() || !feedback->Validate(ContextGroup(), this))
    return 0;

  if (!feedback->HasEverBeenBound())
    return 0;

  if (feedback->MarkedForDeletion())
    return 0;

  return ContextGL()->IsTransformFeedback(feedback->Object());
}

void WebGL2RenderingContextBase::bindTransformFeedback(
    GLenum target,
    WebGLTransformFeedback* feedback) {
  if (!ValidateNullableWebGLObject("bindTransformFeedback", feedback))
    return;

  if (target != GL_TRANSFORM_FEEDBACK) {
    SynthesizeGLError(GL_INVALID_ENUM, "bindTransformFeedback",
                      "target must be TRANSFORM_FEEDBACK");
    return;
  }

  if (transform_feedback_binding_->active() &&
      !transform_feedback_binding_->paused()) {
    SynthesizeGLError(GL_INVALID_OPERATION, "bindTransformFeedback",
                      "transform feedback is active and not paused");
    return;
  }

  WebGLTransformFeedback* feedback_to_be_bound;
  if (feedback) {
    feedback_to_be_bound = feedback;
    feedback_to_be_bound->SetTarget(target);
  } else {
    feedback_to_be_bound = default_transform_feedback_.Get();
  }

  transform_feedback_binding_ = feedback_to_be_bound;
  ContextGL()->BindTransformFeedback(target,
                                     ObjectOrZero(feedback_to_be_bound));
}

void WebGL2RenderingContextBase::beginTransformFeedback(GLenum primitive_mode) {
  if (isContextLost())
    return;
  if (!ValidateTransformFeedbackPrimitiveMode("beginTransformFeedback",
                                              primitive_mode))
    return;
  if (!current_program_) {
    SynthesizeGLError(GL_INVALID_OPERATION, "beginTransformFeedback",
                      "no program object is active");
    return;
  }
  if (transform_feedback_binding_->active()) {
    SynthesizeGLError(GL_INVALID_OPERATION, "beginTransformFeedback",
                      "transform feedback is already active");
    return;
  }
  int required_buffer_count =
      current_program_->GetRequiredTransformFeedbackBufferCount(this);
  if (required_buffer_count == 0) {
    SynthesizeGLError(GL_INVALID_OPERATION, "beginTransformFeedback",
                      "current active program does not specify any transform "
                      "feedback varyings to record");
    return;
  }
  if (!transform_feedback_binding_->HasEnoughBuffers(required_buffer_count)) {
    SynthesizeGLError(GL_INVALID_OPERATION, "beginTransformFeedback",
                      "not enough transform feedback buffers bound");
    return;
  }

  ContextGL()->BeginTransformFeedback(primitive_mode);
  current_program_->IncreaseActiveTransformFeedbackCount();
  transform_feedback_binding_->SetProgram(current_program_);
  transform_feedback_binding_->SetActive(true);
  transform_feedback_binding_->SetPaused(false);
}

void WebGL2RenderingContextBase::endTransformFeedback() {
  if (isContextLost())
    return;
  if (!transform_feedback_binding_->active()) {
    SynthesizeGLError(GL_INVALID_OPERATION, "endTransformFeedback",
                      "transform feedback is not active");
    return;
  }

  ContextGL()->EndTransformFeedback();

  transform_feedback_binding_->SetPaused(false);
  transform_feedback_binding_->SetActive(false);
  if (current_program_)
    current_program_->DecreaseActiveTransformFeedbackCount();
}

void WebGL2RenderingContextBase::transformFeedbackVaryings(
    WebGLProgram* program,
    const Vector<String>& varyings,
    GLenum buffer_mode) {
  if (!ValidateWebGLProgramOrShader("transformFeedbackVaryings", program))
    return;

  switch (buffer_mode) {
    case GL_SEPARATE_ATTRIBS:
      if (varyings.size() >
          static_cast<size_t>(max_transform_feedback_separate_attribs_)) {
        SynthesizeGLError(GL_INVALID_VALUE, "transformFeedbackVaryings",
                          "too many varyings");
        return;
      }
      break;
    case GL_INTERLEAVED_ATTRIBS:
      break;
    default:
      SynthesizeGLError(GL_INVALID_ENUM, "transformFeedbackVaryings",
                        "invalid buffer mode");
      return;
  }

  PointableStringArray varying_strings(varyings);

  program->SetRequiredTransformFeedbackBufferCount(
      buffer_mode == GL_INTERLEAVED_ATTRIBS ? 1 : varyings.size());

  ContextGL()->TransformFeedbackVaryings(ObjectOrZero(program), varyings.size(),
                                         varying_strings.data(), buffer_mode);
}

WebGLActiveInfo* WebGL2RenderingContextBase::getTransformFeedbackVarying(
    WebGLProgram* program,
    GLuint index) {
  if (!ValidateWebGLProgramOrShader("getTransformFeedbackVarying", program))
    return nullptr;

  if (!program->LinkStatus(this)) {
    SynthesizeGLError(GL_INVALID_OPERATION, "getTransformFeedbackVarying",
                      "program not linked");
    return nullptr;
  }
  GLint max_index = 0;
  ContextGL()->GetProgramiv(ObjectOrZero(program),
                            GL_TRANSFORM_FEEDBACK_VARYINGS, &max_index);
  if (index >= static_cast<GLuint>(max_index)) {
    SynthesizeGLError(GL_INVALID_VALUE, "getTransformFeedbackVarying",
                      "invalid index");
    return nullptr;
  }

  GLint max_name_length = -1;
  ContextGL()->GetProgramiv(ObjectOrZero(program),
                            GL_TRANSFORM_FEEDBACK_VARYING_MAX_LENGTH,
                            &max_name_length);
  if (max_name_length <= 0) {
    return nullptr;
  }
  auto name = std::make_unique<GLchar[]>(max_name_length);
  GLsizei length = 0;
  GLsizei size = 0;
  GLenum type = 0;
  ContextGL()->GetTransformFeedbackVarying(ObjectOrZero(program), index,
                                           max_name_length, &length, &size,
                                           &type, name.get());

  if (length <= 0 || size == 0 || type == 0) {
    return nullptr;
  }

  return MakeGarbageCollected<WebGLActiveInfo>(
      String(name.get(), static_cast<uint32_t>(length)), type, size);
}

void WebGL2RenderingContextBase::pauseTransformFeedback() {
  if (isContextLost())
    return;

  if (!transform_feedback_binding_->active()) {
    SynthesizeGLError(GL_INVALID_OPERATION, "pauseTransformFeedback",
                      "transform feedback is not active");
    return;
  }
  if (transform_feedback_binding_->paused()) {
    SynthesizeGLError(GL_INVALID_OPERATION, "pauseTransformFeedback",
                      "transform feedback is already paused");
    return;
  }

  transform_feedback_binding_->SetPaused(true);
  ContextGL()->PauseTransformFeedback();
}

void WebGL2RenderingContextBase::resumeTransformFeedback() {
  if (isContextLost())
    return;

  if (!transform_feedback_binding_->ValidateProgramForResume(
          current_program_)) {
    SynthesizeGLError(GL_INVALID_OPERATION, "resumeTransformFeedback",
                      "the current program is not the same as when "
                      "beginTransformFeedback was called");
    return;
  }
  if (!transform_feedback_binding_->active() ||
      !transform_feedback_binding_->paused()) {
    SynthesizeGLError(GL_INVALID_OPERATION, "resumeTransformFeedback",
                      "transform feedback is not active or not paused");
    return;
  }

  transform_feedback_binding_->SetPaused(false);
  ContextGL()->ResumeTransformFeedback();
}

bool WebGL2RenderingContextBase::ValidateTransformFeedbackPrimitiveMode(
    const char* function_name,
    GLenum primitive_mode) {
  switch (primitive_mode) {
    case GL_POINTS:
    case GL_LINES:
    case GL_TRIANGLES:
      return true;
    default:
      SynthesizeGLError(GL_INVALID_ENUM, function_name,
                        "invalid transform feedback primitive mode");
      return false;
  }
}

void WebGL2RenderingContextBase::OnBeforeDrawCall() {
  if (transform_feedback_binding_->active() &&
      !transform_feedback_binding_->paused()) {
    for (WebGLBuffer* buffer :
         transform_feedback_binding_
             ->bound_indexed_transform_feedback_buffers()) {
      if (buffer) {
        ContextGL()->InvalidateReadbackBufferShadowDataCHROMIUM(
            buffer->Object());
      }
    }
  }

  WebGLRenderingContextBase::OnBeforeDrawCall();
}

void WebGL2RenderingContextBase::bindBufferBase(GLenum target,
                                                GLuint index,
                                                WebGLBuffer* buffer) {
  if (isContextLost())
    return;
  if (!ValidateNullableWebGLObject("bindBufferBase", buffer))
    return;
  if (target == GL_TRANSFORM_FEEDBACK_BUFFER &&
      transform_feedback_binding_->active()) {
    SynthesizeGLError(GL_INVALID_OPERATION, "bindBufferBase",
                      "transform feedback is active");
    return;
  }
  if (!ValidateAndUpdateBufferBindBaseTarget("bindBufferBase", target, index,
                                             buffer))
    return;

  ContextGL()->BindBufferBase(target, index, ObjectOrZero(buffer));
}

void WebGL2RenderingContextBase::bindBufferRange(GLenum target,
                                                 GLuint index,
                                                 WebGLBuffer* buffer,
                                                 int64_t offset,
                                                 int64_t size) {
  if (isContextLost())
    return;
  if (!ValidateNullableWebGLObject("bindBufferRange", buffer))
    return;
  if (target == GL_TRANSFORM_FEEDBACK_BUFFER &&
      transform_feedback_binding_->active()) {
    SynthesizeGLError(GL_INVALID_OPERATION, "bindBufferBase",
                      "transform feedback is active");
    return;
  }
  if (!ValidateValueFitNonNegInt32("bindBufferRange", "offset", offset) ||
      !ValidateValueFitNonNegInt32("bindBufferRange", "size", size)) {
    return;
  }

  if (!ValidateAndUpdateBufferBindBaseTarget("bindBufferRange", target, index,
                                             buffer))
    return;

  ContextGL()->BindBufferRange(target, index, ObjectOrZero(buffer),
                               static_cast<GLintptr>(offset),
                               static_cast<GLsizeiptr>(size));
}

ScriptValue WebGL2RenderingContextBase::getIndexedParameter(
    ScriptState* script_state,
    GLenum target,
    GLuint index) {
  if (isContextLost())
    return ScriptValue::CreateNull(script_state->GetIsolate());

  switch (target) {
    case GL_TRANSFORM_FEEDBACK_BUFFER_BINDING: {
      WebGLBuffer* buffer = nullptr;
      if (!transform_feedback_binding_->GetBoundIndexedTransformFeedbackBuffer(
              index, &buffer)) {
        SynthesizeGLError(GL_INVALID_VALUE, "getIndexedParameter",
                          "index out of range");
        return ScriptValue::CreateNull(script_state->GetIsolate());
      }
      return WebGLAny(script_state, buffer);
    }
    case GL_UNIFORM_BUFFER_BINDING:
      if (index >= bound_indexed_uniform_buffers_.size()) {
        SynthesizeGLError(GL_INVALID_VALUE, "getIndexedParameter",
                          "index out of range");
        return ScriptValue::CreateNull(script_state->GetIsolate());
      }
      return WebGLAny(script_state,
                      bound_indexed_uniform_buffers_[index].Get());
    case GL_TRANSFORM_FEEDBACK_BUFFER_SIZE:
    case GL_TRANSFORM_FEEDBACK_BUFFER_START:
    case GL_UNIFORM_BUFFER_SIZE:
    case GL_UNIFORM_BUFFER_START: {
      GLint64 value = -1;
      ContextGL()->GetInteger64i_v(target, index, &value);
      return WebGLAny(script_state, value);
    }
    default:
      SynthesizeGLError(GL_INVALID_ENUM, "getIndexedParameter",
                        "invalid parameter name");
      return ScriptValue::CreateNull(script_state->GetIsolate());
  }
}

Vector<GLuint> WebGL2RenderingContextBase::getUniformIndices(
    WebGLProgram* program,
    const Vector<String>& uniform_names) {
  Vector<GLuint> result;
  if (!ValidateWebGLProgramOrShader("getUniformIndices", program))
    return result;

  PointableStringArray uniform_strings(uniform_names);

  result.resize(uniform_names.size());
  ContextGL()->GetUniformIndices(ObjectOrZero(program), uniform_strings.size(),
                                 uniform_strings.data(), result.data());
  return result;
}

ScriptValue WebGL2RenderingContextBase::getActiveUniforms(
    ScriptState* script_state,
    WebGLProgram* program,
    const Vector<GLuint>& uniform_indices,
    GLenum pname) {
  if (!ValidateWebGLProgramOrShader("getActiveUniforms", program))
    return ScriptValue::CreateNull(script_state->GetIsolate());

  enum ReturnType { kEnumType, kUnsignedIntType, kIntType, kBoolType };

  int return_type;
  switch (pname) {
    case GL_UNIFORM_TYPE:
      return_type = kEnumType;
      break;
    case GL_UNIFORM_SIZE:
      return_type = kUnsignedIntType;
      break;
    case GL_UNIFORM_BLOCK_INDEX:
    case GL_UNIFORM_OFFSET:
    case GL_UNIFORM_ARRAY_STRIDE:
    case GL_UNIFORM_MATRIX_STRIDE:
      return_type = kIntType;
      break;
    case GL_UNIFORM_IS_ROW_MAJOR:
      return_type = kBoolType;
      break;
    default:
      SynthesizeGLError(GL_INVALID_ENUM, "getActiveUniforms",
                        "invalid parameter name");
      return ScriptValue::CreateNull(script_state->GetIsolate());
  }

  GLint active_uniforms = -1;
  ContextGL()->GetProgramiv(ObjectOrZero(program), GL_ACTIVE_UNIFORMS,
                            &active_uniforms);

  GLuint active_uniforms_unsigned = active_uniforms;
  wtf_size_t size = uniform_indices.size();
  for (GLuint index : uniform_indices) {
    if (index >= active_uniforms_unsigned) {
      SynthesizeGLError(GL_INVALID_VALUE, "getActiveUniforms",
                        "uniform index greater than ACTIVE_UNIFORMS");
      return ScriptValue::CreateNull(script_state->GetIsolate());
    }
  }

  Vector<GLint> result(size);
  ContextGL()->GetActiveUniformsiv(
      ObjectOrZero(program), uniform_indices.size(), uniform_indices.data(),
      pname, result.data());
  switch (return_type) {
    case kEnumType: {
      Vector<GLenum> enum_result(size);
      for (wtf_size_t i = 0; i < size; ++i)
        enum_result[i] = static_cast<GLenum>(result[i]);
      return WebGLAny(script_state, enum_result);
    }
    case kUnsignedIntType: {
      Vector<GLuint> uint_result(size);
      for (wtf_size_t i = 0; i < size; ++i)
        uint_result[i] = static_cast<GLuint>(result[i]);
      return WebGLAny(script_state, uint_result);
    }
    case kIntType: {
      return WebGLAny(script_state, result);
    }
    case kBoolType: {
      Vector<bool> bool_result(size);
      for (wtf_size_t i = 0; i < size; ++i)
        bool_result[i] = static_cast<bool>(result[i]);
      return WebGLAny(script_state, bool_result);
    }
    default:
      NOTREACHED();
      return ScriptValue::CreateNull(script_state->GetIsolate());
  }
}

GLuint WebGL2RenderingContextBase::getUniformBlockIndex(
    WebGLProgram* program,
    const String& uniform_block_name) {
  if (!ValidateWebGLProgramOrShader("getUniformBlockIndex", program))
    return 0;
  if (!ValidateString("getUniformBlockIndex", uniform_block_name))
    return 0;

  return ContextGL()->GetUniformBlockIndex(ObjectOrZero(program),
                                           uniform_block_name.Utf8().c_str());
}

bool WebGL2RenderingContextBase::ValidateUniformBlockIndex(
    const char* function_name,
    WebGLProgram* program,
    GLuint block_index) {
  DCHECK(program);
  if (!program->LinkStatus(this)) {
    SynthesizeGLError(GL_INVALID_OPERATION, function_name,
                      "program not linked");
    return false;
  }
  GLint active_uniform_blocks = 0;
  ContextGL()->GetProgramiv(ObjectOrZero(program), GL_ACTIVE_UNIFORM_BLOCKS,
                            &active_uniform_blocks);
  if (block_index >= static_cast<GLuint>(active_uniform_blocks)) {
    SynthesizeGLError(GL_INVALID_VALUE, function_name,
                      "invalid uniform block index");
    return false;
  }
  return true;
}

ScriptValue WebGL2RenderingContextBase::getActiveUniformBlockParameter(
    ScriptState* script_state,
    WebGLProgram* program,
    GLuint uniform_block_index,
    GLenum pname) {
  if (!ValidateWebGLProgramOrShader("getActiveUniformBlockParameter", program))
    return ScriptValue::CreateNull(script_state->GetIsolate());

  if (!ValidateUniformBlockIndex("getActiveUniformBlockParameter", program,
                                 uniform_block_index))
    return ScriptValue::CreateNull(script_state->GetIsolate());

  switch (pname) {
    case GL_UNIFORM_BLOCK_BINDING:
    case GL_UNIFORM_BLOCK_DATA_SIZE:
    case GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS: {
      GLint int_value = 0;
      ContextGL()->GetActiveUniformBlockiv(
          ObjectOrZero(program), uniform_block_index, pname, &int_value);
      return WebGLAny(script_state, static_cast<unsigned>(int_value));
    }
    case GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES: {
      GLint uniform_count = 0;
      ContextGL()->GetActiveUniformBlockiv(
          ObjectOrZero(program), uniform_block_index,
          GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS, &uniform_count);

      Vector<GLint> indices(uniform_count);
      ContextGL()->GetActiveUniformBlockiv(
          ObjectOrZero(program), uniform_block_index, pname, indices.data());
      return WebGLAny(
          script_state,
          DOMUint32Array::Create(reinterpret_cast<GLuint*>(indices.data()),
                                 indices.size()));
    }
    case GL_UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER:
    case GL_UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER: {
      GLint bool_value = 0;
      ContextGL()->GetActiveUniformBlockiv(
          ObjectOrZero(program), uniform_block_index, pname, &bool_value);
      return WebGLAny(script_state, static_cast<bool>(bool_value));
    }
    default:
      SynthesizeGLError(GL_INVALID_ENUM, "getActiveUniformBlockParameter",
                        "invalid parameter name");
      return ScriptValue::CreateNull(script_state->GetIsolate());
  }
}

String WebGL2RenderingContextBase::getActiveUniformBlockName(
    WebGLProgram* program,
    GLuint uniform_block_index) {
  if (!ValidateWebGLProgramOrShader("getActiveUniformBlockName", program))
    return String();

  if (!ValidateUniformBlockIndex("getActiveUniformBlockName", program,
                                 uniform_block_index))
    return String();

  GLint max_name_length = -1;
  ContextGL()->GetProgramiv(ObjectOrZero(program),
                            GL_ACTIVE_UNIFORM_BLOCK_MAX_NAME_LENGTH,
                            &max_name_length);
  if (max_name_length <= 0) {
    // This state indicates that there are no active uniform blocks
    SynthesizeGLError(GL_INVALID_VALUE, "getActiveUniformBlockName",
                      "invalid uniform block index");
    return String();
  }
  auto name = std::make_unique<GLchar[]>(max_name_length);

  GLsizei length = 0;
  ContextGL()->GetActiveUniformBlockName(ObjectOrZero(program),
                                         uniform_block_index, max_name_length,
                                         &length, name.get());

  if (length <= 0)
    return String();
  return String(name.get(), static_cast<uint32_t>(length));
}

void WebGL2RenderingContextBase::uniformBlockBinding(
    WebGLProgram* program,
    GLuint uniform_block_index,
    GLuint uniform_block_binding) {
  if (!ValidateWebGLProgramOrShader("uniformBlockBinding", program))
    return;

  if (!ValidateUniformBlockIndex("uniformBlockBinding", program,
                                 uniform_block_index))
    return;

  ContextGL()->UniformBlockBinding(ObjectOrZero(program), uniform_block_index,
                                   uniform_block_binding);
}

WebGLVertexArrayObject* WebGL2RenderingContextBase::createVertexArray() {
  if (isContextLost())
    return nullptr;

  return MakeGarbageCollected<WebGLVertexArrayObject>(
      this, WebGLVertexArrayObjectBase::kVaoTypeUser);
}

void WebGL2RenderingContextBase::deleteVertexArray(
    WebGLVertexArrayObject* vertex_array) {
  // ValidateWebGLObject generates an error if the object has already been
  // deleted, so we must replicate most of its checks here.
  if (isContextLost() || !vertex_array)
    return;
  if (!vertex_array->Validate(ContextGroup(), this)) {
    SynthesizeGLError(GL_INVALID_OPERATION, "deleteVertexArray",
                      "object does not belong to this context");
    return;
  }
  if (vertex_array->MarkedForDeletion())
    return;

  if (!vertex_array->IsDefaultObject() &&
      vertex_array == bound_vertex_array_object_)
    SetBoundVertexArrayObject(nullptr);

  vertex_array->DeleteObject(ContextGL());
}

GLboolean WebGL2RenderingContextBase::isVertexArray(
    WebGLVertexArrayObject* vertex_array) {
  if (isContextLost() || !vertex_array ||
      !vertex_array->Validate(ContextGroup(), this))
    return 0;

  if (!vertex_array->HasEverBeenBound())
    return 0;
  if (vertex_array->MarkedForDeletion())
    return 0;

  return ContextGL()->IsVertexArrayOES(vertex_array->Object());
}

void WebGL2RenderingContextBase::bindVertexArray(
    WebGLVertexArrayObject* vertex_array) {
  if (!ValidateNullableWebGLObject("bindVertexArray", vertex_array))
    return;

  if (vertex_array && !vertex_array->IsDefaultObject() &&
      vertex_array->Object()) {
    ContextGL()->BindVertexArrayOES(ObjectOrZero(vertex_array));

    vertex_array->SetHasEverBeenBound();
    SetBoundVertexArrayObject(vertex_array);
  } else {
    ContextGL()->BindVertexArrayOES(0);
    SetBoundVertexArrayObject(nullptr);
  }
}

void WebGL2RenderingContextBase::bindFramebuffer(GLenum target,
                                                 WebGLFramebuffer* buffer) {
  if (!ValidateNullableWebGLObject("bindFramebuffer", buffer))
    return;

  switch (target) {
    case GL_DRAW_FRAMEBUFFER:
      break;
    case GL_FRAMEBUFFER:
    case GL_READ_FRAMEBUFFER:
      read_framebuffer_binding_ = buffer;
      break;
    default:
      SynthesizeGLError(GL_INVALID_ENUM, "bindFramebuffer", "invalid target");
      return;
  }

  SetFramebuffer(target, buffer);
}

void WebGL2RenderingContextBase::deleteFramebuffer(
    WebGLFramebuffer* framebuffer) {
  // Don't allow the application to delete an opaque framebuffer.
  if (framebuffer && framebuffer->Opaque()) {
    SynthesizeGLError(GL_INVALID_OPERATION, "deleteFramebuffer",
                      "cannot delete an opaque framebuffer");
    return;
  }
  if (!DeleteObject(framebuffer))
    return;
  GLenum target = 0;
  if (framebuffer == framebuffer_binding_) {
    if (framebuffer == read_framebuffer_binding_) {
      target = GL_FRAMEBUFFER;
      framebuffer_binding_ = nullptr;
      read_framebuffer_binding_ = nullptr;
    } else {
      target = GL_DRAW_FRAMEBUFFER;
      framebuffer_binding_ = nullptr;
    }
  } else if (framebuffer == read_framebuffer_binding_) {
    target = GL_READ_FRAMEBUFFER;
    read_framebuffer_binding_ = nullptr;
  }
  if (target) {
    // Have to call drawingBuffer()->bind() here to bind back to internal fbo.
    GetDrawingBuffer()->Bind(target);
  }
}

ScriptValue WebGL2RenderingContextBase::getParameter(ScriptState* script_state,
                                                     GLenum pname) {
  if (isContextLost())
    return ScriptValue::CreateNull(script_state->GetIsolate());
  switch (pname) {
    case GL_SHADING_LANGUAGE_VERSION: {
      return WebGLAny(
          script_state,
          "WebGL GLSL ES 3.00 (" +
              String(ContextGL()->GetString(GL_SHADING_LANGUAGE_VERSION)) +
              ")");
    }
    case GL_VERSION:
      return WebGLAny(
          script_state,
          "WebGL 2.0 (" + String(ContextGL()->GetString(GL_VERSION)) + ")");

    case GL_COPY_READ_BUFFER_BINDING:
      return WebGLAny(script_state, bound_copy_read_buffer_.Get());
    case GL_COPY_WRITE_BUFFER_BINDING:
      return WebGLAny(script_state, bound_copy_write_buffer_.Get());
    case GL_DRAW_FRAMEBUFFER_BINDING:
      return WebGLAny(script_state, framebuffer_binding_.Get());
    case GL_FRAGMENT_SHADER_DERIVATIVE_HINT:
      return GetUnsignedIntParameter(script_state, pname);
    case GL_MAX_3D_TEXTURE_SIZE:
      return GetIntParameter(script_state, pname);
    case GL_MAX_ARRAY_TEXTURE_LAYERS:
      return GetIntParameter(script_state, pname);
    case GC3D_MAX_CLIENT_WAIT_TIMEOUT_WEBGL:
      return WebGLAny(script_state, kMaxClientWaitTimeout);
    case GL_MAX_COLOR_ATTACHMENTS:
      return GetIntParameter(script_state, pname);
    case GL_MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS:
      return GetInt64Parameter(script_state, pname);
    case GL_MAX_COMBINED_UNIFORM_BLOCKS:
      return GetIntParameter(script_state, pname);
    case GL_MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS:
      return GetInt64Parameter(script_state, pname);
    case GL_MAX_DRAW_BUFFERS:
      return GetIntParameter(script_state, pname);
    case GL_MAX_ELEMENT_INDEX:
      return GetInt64Parameter(script_state, pname);
    case GL_MAX_ELEMENTS_INDICES:
      return GetIntParameter(script_state, pname);
    case GL_MAX_ELEMENTS_VERTICES:
      return GetIntParameter(script_state, pname);
    case GL_MAX_FRAGMENT_INPUT_COMPONENTS:
      return GetIntParameter(script_state, pname);
    case GL_MAX_FRAGMENT_UNIFORM_BLOCKS:
      return GetIntParameter(script_state, pname);
    case GL_MAX_FRAGMENT_UNIFORM_COMPONENTS:
      return GetIntParameter(script_state, pname);
    case GL_MAX_PROGRAM_TEXEL_OFFSET:
      return GetIntParameter(script_state, pname);
    case GL_MAX_SAMPLES:
      return GetIntParameter(script_state, pname);
    case GL_MAX_SERVER_WAIT_TIMEOUT:
      return GetInt64Parameter(script_state, pname);
    case GL_MAX_TEXTURE_LOD_BIAS:
      return GetFloatParameter(script_state, pname);
    case GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS:
      return GetIntParameter(script_state, pname);
    case GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS:
      return GetIntParameter(script_state, pname);
    case GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS:
      return GetIntParameter(script_state, pname);
    case GL_MAX_UNIFORM_BLOCK_SIZE:
      return GetInt64Parameter(script_state, pname);
    case GL_MAX_UNIFORM_BUFFER_BINDINGS:
      return GetIntParameter(script_state, pname);
    case GL_MAX_VARYING_COMPONENTS:
      return GetIntParameter(script_state, pname);
    case GL_MAX_VERTEX_OUTPUT_COMPONENTS:
      return GetIntParameter(script_state, pname);
    case GL_MAX_VERTEX_UNIFORM_BLOCKS:
      return GetIntParameter(script_state, pname);
    case GL_MAX_VERTEX_UNIFORM_COMPONENTS:
      return GetIntParameter(script_state, pname);
    case GL_MIN_PROGRAM_TEXEL_OFFSET:
      return GetIntParameter(script_state, pname);
    case GL_PACK_ROW_LENGTH:
      return GetIntParameter(script_state, pname);
    case GL_PACK_SKIP_PIXELS:
      return GetIntParameter(script_state, pname);
    case GL_PACK_SKIP_ROWS:
      return GetIntParameter(script_state, pname);
    case GL_PIXEL_PACK_BUFFER_BINDING:
      return WebGLAny(script_state, bound_pixel_pack_buffer_.Get());
    case GL_PIXEL_UNPACK_BUFFER_BINDING:
      return WebGLAny(script_state, bound_pixel_unpack_buffer_.Get());
    case GL_RASTERIZER_DISCARD:
      return GetBooleanParameter(script_state, pname);
    case GL_READ_BUFFER: {
      GLenum value = 0;
      if (!isContextLost()) {
        WebGLFramebuffer* read_framebuffer_binding =
            GetFramebufferBinding(GL_READ_FRAMEBUFFER);
        if (!read_framebuffer_binding)
          value = read_buffer_of_default_framebuffer_;
        else
          value = read_framebuffer_binding->GetReadBuffer();
      }
      return WebGLAny(script_state, value);
    }
    case GL_READ_FRAMEBUFFER_BINDING:
      return WebGLAny(script_state, read_framebuffer_binding_.Get());
    case GL_SAMPLER_BINDING:
      return WebGLAny(script_state, sampler_units_[active_texture_unit_].Get());
    case GL_TEXTURE_BINDING_2D_ARRAY:
      return WebGLAny(
          script_state,
          texture_units_[active_texture_unit_].texture2d_array_binding_.Get());
    case GL_TEXTURE_BINDING_3D:
      return WebGLAny(
          script_state,
          texture_units_[active_texture_unit_].texture3d_binding_.Get());
    case GL_TRANSFORM_FEEDBACK_ACTIVE:
      return GetBooleanParameter(script_state, pname);
    case GL_TRANSFORM_FEEDBACK_BUFFER_BINDING:
      return WebGLAny(script_state, bound_transform_feedback_buffer_.Get());
    case GL_TRANSFORM_FEEDBACK_BINDING:
      if (!transform_feedback_binding_->IsDefaultObject()) {
        return WebGLAny(script_state, transform_feedback_binding_.Get());
      }
      return ScriptValue::CreateNull(script_state->GetIsolate());
    case GL_TRANSFORM_FEEDBACK_PAUSED:
      return GetBooleanParameter(script_state, pname);
    case GL_UNIFORM_BUFFER_BINDING:
      return WebGLAny(script_state, bound_uniform_buffer_.Get());
    case GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT:
      return GetIntParameter(script_state, pname);
    case GL_UNPACK_IMAGE_HEIGHT:
      return GetIntParameter(script_state, pname);
    case GL_UNPACK_ROW_LENGTH:
      return GetIntParameter(script_state, pname);
    case GL_UNPACK_SKIP_IMAGES:
      return GetIntParameter(script_state, pname);
    case GL_UNPACK_SKIP_PIXELS:
      return GetIntParameter(script_state, pname);
    case GL_UNPACK_SKIP_ROWS:
      return GetIntParameter(script_state, pname);
    case GL_TIMESTAMP_EXT:
      if (ExtensionEnabled(kEXTDisjointTimerQueryWebGL2Name)) {
        return WebGLAny(script_state, 0);
      }
      SynthesizeGLError(GL_INVALID_ENUM, "getParameter",
                        "invalid parameter name, "
                        "EXT_disjoint_timer_query_webgl2 not enabled");
      return ScriptValue::CreateNull(script_state->GetIsolate());
    case GL_GPU_DISJOINT_EXT:
      if (ExtensionEnabled(kEXTDisjointTimerQueryWebGL2Name)) {
        return GetBooleanParameter(script_state, GL_GPU_DISJOINT_EXT);
      }
      SynthesizeGLError(GL_INVALID_ENUM, "getParameter",
                        "invalid parameter name, "
                        "EXT_disjoint_timer_query_webgl2 not enabled");
      return ScriptValue::CreateNull(script_state->GetIsolate());

    default:
      return WebGLRenderingContextBase::getParameter(script_state, pname);
  }
}

ScriptValue WebGL2RenderingContextBase::GetInt64Parameter(
    ScriptState* script_state,
    GLenum pname) {
  GLint64 value = 0;
  if (!isContextLost())
    ContextGL()->GetInteger64v(pname, &value);
  return WebGLAny(script_state, value);
}

bool WebGL2RenderingContextBase::ValidateCapability(const char* function_name,
                                                    GLenum cap) {
  switch (cap) {
    case GL_RASTERIZER_DISCARD:
      return true;
    default:
      return WebGLRenderingContextBase::ValidateCapability(function_name, cap);
  }
}

bool WebGL2RenderingContextBase::ValidateBufferTargetCompatibility(
    const char* function_name,
    GLenum target,
    WebGLBuffer* buffer) {
  DCHECK(buffer);

  switch (buffer->GetInitialTarget()) {
    case GL_ELEMENT_ARRAY_BUFFER:
      switch (target) {
        case GL_ARRAY_BUFFER:
        case GL_PIXEL_PACK_BUFFER:
        case GL_PIXEL_UNPACK_BUFFER:
        case GL_TRANSFORM_FEEDBACK_BUFFER:
        case GL_UNIFORM_BUFFER:
          SynthesizeGLError(
              GL_INVALID_OPERATION, function_name,
              "element array buffers can not be bound to a different target");

          return false;
        default:
          break;
      }
      break;
    case GL_ARRAY_BUFFER:
    case GL_COPY_READ_BUFFER:
    case GL_COPY_WRITE_BUFFER:
    case GL_PIXEL_PACK_BUFFER:
    case GL_PIXEL_UNPACK_BUFFER:
    case GL_UNIFORM_BUFFER:
    case GL_TRANSFORM_FEEDBACK_BUFFER:
      if (target == GL_ELEMENT_ARRAY_BUFFER) {
        SynthesizeGLError(GL_INVALID_OPERATION, function_name,
                          "buffers bound to non ELEMENT_ARRAY_BUFFER targets "
                          "can not be bound to ELEMENT_ARRAY_BUFFER target");
        return false;
      }
      break;
    default:
      break;
  }

  return true;
}

bool WebGL2RenderingContextBase::ValidateBufferTarget(const char* function_name,
                                                      GLenum target) {
  switch (target) {
    case GL_ARRAY_BUFFER:
    case GL_COPY_READ_BUFFER:
    case GL_COPY_WRITE_BUFFER:
    case GL_ELEMENT_ARRAY_BUFFER:
    case GL_PIXEL_PACK_BUFFER:
    case GL_PIXEL_UNPACK_BUFFER:
    case GL_TRANSFORM_FEEDBACK_BUFFER:
    case GL_UNIFORM_BUFFER:
      return true;
    default:
      SynthesizeGLError(GL_INVALID_ENUM, function_name, "invalid target");
      return false;
  }
}

bool WebGL2RenderingContextBase::ValidateAndUpdateBufferBindTarget(
    const char* function_name,
    GLenum target,
    WebGLBuffer* buffer) {
  if (!ValidateBufferTarget(function_name, target))
    return false;

  if (buffer &&
      !ValidateBufferTargetCompatibility(function_name, target, buffer))
    return false;

  switch (target) {
    case GL_ARRAY_BUFFER:
      bound_array_buffer_ = buffer;
      break;
    case GL_COPY_READ_BUFFER:
      bound_copy_read_buffer_ = buffer;
      break;
    case GL_COPY_WRITE_BUFFER:
      bound_copy_write_buffer_ = buffer;
      break;
    case GL_ELEMENT_ARRAY_BUFFER:
      bound_vertex_array_object_->SetElementArrayBuffer(buffer);
      break;
    case GL_PIXEL_PACK_BUFFER:
      bound_pixel_pack_buffer_ = buffer;
      break;
    case GL_PIXEL_UNPACK_BUFFER:
      bound_pixel_unpack_buffer_ = buffer;
      break;
    case GL_TRANSFORM_FEEDBACK_BUFFER:
      bound_transform_feedback_buffer_ = buffer;
      break;
    case GL_UNIFORM_BUFFER:
      bound_uniform_buffer_ = buffer;
      break;
    default:
      NOTREACHED();
      break;
  }

  if (buffer && !buffer->GetInitialTarget())
    buffer->SetInitialTarget(target);
  return true;
}

bool WebGL2RenderingContextBase::ValidateBufferBaseTarget(
    const char* function_name,
    GLenum target) {
  switch (target) {
    case GL_TRANSFORM_FEEDBACK_BUFFER:
    case GL_UNIFORM_BUFFER:
      return true;
    default:
      SynthesizeGLError(GL_INVALID_ENUM, function_name, "invalid target");
      return false;
  }
}

bool WebGL2RenderingContextBase::ValidateAndUpdateBufferBindBaseTarget(
    const char* function_name,
    GLenum target,
    GLuint index,
    WebGLBuffer* buffer) {
  if (!ValidateBufferBaseTarget(function_name, target))
    return false;

  if (buffer &&
      !ValidateBufferTargetCompatibility(function_name, target, buffer))
    return false;

  switch (target) {
    case GL_TRANSFORM_FEEDBACK_BUFFER:
      if (!transform_feedback_binding_->SetBoundIndexedTransformFeedbackBuffer(
              index, buffer)) {
        SynthesizeGLError(GL_INVALID_VALUE, function_name,
                          "index out of range");
        return false;
      }
      bound_transform_feedback_buffer_ = buffer;
      break;
    case GL_UNIFORM_BUFFER:
      if (index >= bound_indexed_uniform_buffers_.size()) {
        SynthesizeGLError(GL_INVALID_VALUE, function_name,
                          "index out of range");
        return false;
      }
      bound_indexed_uniform_buffers_[index] = buffer;
      bound_uniform_buffer_ = buffer;

      // Keep track of what the maximum bound uniform buffer index is
      if (buffer) {
        if (index > max_bound_uniform_buffer_index_)
          max_bound_uniform_buffer_index_ = index;
      } else if (max_bound_uniform_buffer_index_ > 0 &&
                 index == max_bound_uniform_buffer_index_) {
        wtf_size_t i = max_bound_uniform_buffer_index_ - 1;
        for (; i > 0; --i) {
          if (bound_indexed_uniform_buffers_[i].Get())
            break;
        }
        max_bound_uniform_buffer_index_ = i;
      }
      break;
    default:
      NOTREACHED();
      break;
  }

  if (buffer && !buffer->GetInitialTarget())
    buffer->SetInitialTarget(target);
  return true;
}

bool WebGL2RenderingContextBase::ValidateFramebufferTarget(GLenum target) {
  switch (target) {
    case GL_FRAMEBUFFER:
    case GL_READ_FRAMEBUFFER:
    case GL_DRAW_FRAMEBUFFER:
      return true;
    default:
      return false;
  }
}

bool WebGL2RenderingContextBase::ValidateReadPixelsFormatAndType(
    GLenum format,
    GLenum type,
    DOMArrayBufferView* buffer) {
  switch (format) {
    case GL_RED:
    case GL_RED_INTEGER:
    case GL_RG:
    case GL_RG_INTEGER:
    case GL_RGB:
    case GL_RGB_INTEGER:
    case GL_RGBA:
    case GL_RGBA_INTEGER:
    case GL_LUMINANCE_ALPHA:
    case GL_LUMINANCE:
    case GL_ALPHA:
      break;
    default:
      SynthesizeGLError(GL_INVALID_ENUM, "readPixels", "invalid format");
      return false;
  }

  switch (type) {
    case GL_UNSIGNED_BYTE:
      if (buffer) {
        auto bufferType = buffer->GetType();
        if (bufferType != DOMArrayBufferView::kTypeUint8 &&
            bufferType != DOMArrayBufferView::kTypeUint8Clamped) {
          SynthesizeGLError(
              GL_INVALID_OPERATION, "readPixels",
              "type UNSIGNED_BYTE but ArrayBufferView not Uint8Array or "
              "Uint8ClampedArray");
          return false;
        }
      }
      return true;
    case GL_BYTE:
      if (buffer && buffer->GetType() != DOMArrayBufferView::kTypeInt8) {
        SynthesizeGLError(GL_INVALID_OPERATION, "readPixels",
                          "type BYTE but ArrayBufferView not Int8Array");
        return false;
      }
      return true;
    case GL_HALF_FLOAT:
      if (buffer && buffer->GetType() != DOMArrayBufferView::kTypeUint16) {
        SynthesizeGLError(
            GL_INVALID_OPERATION, "readPixels",
            "type HALF_FLOAT but ArrayBufferView not Uint16Array");
        return false;
      }
      return true;
    case GL_FLOAT:
      if (buffer && buffer->GetType() != DOMArrayBufferView::kTypeFloat32) {
        SynthesizeGLError(GL_INVALID_OPERATION, "readPixels",
                          "type FLOAT but ArrayBufferView not Float32Array");
        return false;
      }
      return true;
    case GL_UNSIGNED_SHORT:
    case GL_UNSIGNED_SHORT_5_6_5:
    case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1:
      if (buffer && buffer->GetType() != DOMArrayBufferView::kTypeUint16) {
        SynthesizeGLError(
            GL_INVALID_OPERATION, "readPixels",
            "type UNSIGNED_SHORT but ArrayBufferView not Uint16Array");
        return false;
      }
      return true;
    case GL_SHORT:
      if (buffer && buffer->GetType() != DOMArrayBufferView::kTypeInt16) {
        SynthesizeGLError(GL_INVALID_OPERATION, "readPixels",
                          "type SHORT but ArrayBufferView not Int16Array");
        return false;
      }
      return true;
    case GL_UNSIGNED_INT:
    case GL_UNSIGNED_INT_2_10_10_10_REV:
    case GL_UNSIGNED_INT_10F_11F_11F_REV:
    case GL_UNSIGNED_INT_5_9_9_9_REV:
      if (buffer && buffer->GetType() != DOMArrayBufferView::kTypeUint32) {
        SynthesizeGLError(
            GL_INVALID_OPERATION, "readPixels",
            "type UNSIGNED_INT but ArrayBufferView not Uint32Array");
        return false;
      }
      return true;
    case GL_INT:
      if (buffer && buffer->GetType() != DOMArrayBufferView::kTypeInt32) {
        SynthesizeGLError(GL_INVALID_OPERATION, "readPixels",
                          "type INT but ArrayBufferView not Int32Array");
        return false;
      }
      return true;
    default:
      SynthesizeGLError(GL_INVALID_ENUM, "readPixels", "invalid type");
      return false;
  }
}

WebGLFramebuffer* WebGL2RenderingContextBase::GetFramebufferBinding(
    GLenum target) {
  switch (target) {
    case GL_READ_FRAMEBUFFER:
      return read_framebuffer_binding_.Get();
    case GL_DRAW_FRAMEBUFFER:
      return framebuffer_binding_.Get();
    default:
      return WebGLRenderingContextBase::GetFramebufferBinding(target);
  }
}

WebGLFramebuffer* WebGL2RenderingContextBase::GetReadFramebufferBinding() {
  return read_framebuffer_binding_.Get();
}

bool WebGL2RenderingContextBase::ValidateGetFramebufferAttachmentParameterFunc(
    const char* function_name,
    GLenum target,
    GLenum attachment) {
  if (!ValidateFramebufferTarget(target)) {
    SynthesizeGLError(GL_INVALID_ENUM, function_name, "invalid target");
    return false;
  }

  WebGLFramebuffer* framebuffer_binding = GetFramebufferBinding(target);
  DCHECK(framebuffer_binding || GetDrawingBuffer());
  if (!framebuffer_binding) {
    // for the default framebuffer
    switch (attachment) {
      case GL_BACK:
      case GL_DEPTH:
      case GL_STENCIL:
        break;
      default:
        SynthesizeGLError(GL_INVALID_ENUM, function_name, "invalid attachment");
        return false;
    }
  } else {
    // for the FBO
    switch (attachment) {
      case GL_COLOR_ATTACHMENT0:
      case GL_DEPTH_ATTACHMENT:
      case GL_STENCIL_ATTACHMENT:
        break;
      case GL_DEPTH_STENCIL_ATTACHMENT:
        if (framebuffer_binding->GetAttachmentObject(GL_DEPTH_ATTACHMENT) !=
            framebuffer_binding->GetAttachmentObject(GL_STENCIL_ATTACHMENT)) {
          SynthesizeGLError(GL_INVALID_OPERATION, function_name,
                            "different objects are bound to the depth and "
                            "stencil attachment points");
          return false;
        }
        break;
      default:
        if (attachment > GL_COLOR_ATTACHMENT0 &&
            attachment < static_cast<GLenum>(GL_COLOR_ATTACHMENT0 +
                                             MaxColorAttachments()))
          break;
        SynthesizeGLError(GL_INVALID_ENUM, function_name, "invalid attachment");
        return false;
    }
  }
  return true;
}

ScriptValue WebGL2RenderingContextBase::getFramebufferAttachmentParameter(
    ScriptState* script_state,
    GLenum target,
    GLenum attachment,
    GLenum pname) {
  const char kFunctionName[] = "getFramebufferAttachmentParameter";
  if (isContextLost() || !ValidateGetFramebufferAttachmentParameterFunc(
                             kFunctionName, target, attachment))
    return ScriptValue::CreateNull(script_state->GetIsolate());

  WebGLFramebuffer* framebuffer_binding = GetFramebufferBinding(target);
  DCHECK(!framebuffer_binding || framebuffer_binding->Object());

  // Default framebuffer (an internal fbo)
  if (!framebuffer_binding) {
    // We can use creationAttributes() because in WebGL 2, they are required to
    // be honored.
    bool has_depth = CreationAttributes().depth;
    bool has_stencil = CreationAttributes().stencil;
    bool has_alpha = CreationAttributes().alpha;
    bool missing_image = (attachment == GL_DEPTH && !has_depth) ||
                         (attachment == GL_STENCIL && !has_stencil);
    if (missing_image) {
      switch (pname) {
        case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE:
          return WebGLAny(script_state, GL_NONE);
        default:
          SynthesizeGLError(GL_INVALID_OPERATION, kFunctionName,
                            "invalid parameter name");
          return ScriptValue::CreateNull(script_state->GetIsolate());
      }
    }
    switch (pname) {
      case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE:
        return WebGLAny(script_state, GL_FRAMEBUFFER_DEFAULT);
      case GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE:
      case GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE:
      case GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE: {
        GLint value = attachment == GL_BACK ? 8 : 0;
        return WebGLAny(script_state, value);
      }
      case GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE: {
        GLint value = (attachment == GL_BACK && has_alpha) ? 8 : 0;
        return WebGLAny(script_state, value);
      }
      case GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE: {
        // For ES3 capable backend, DEPTH24_STENCIL8 has to be supported.
        GLint value = attachment == GL_DEPTH ? 24 : 0;
        return WebGLAny(script_state, value);
      }
      case GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE: {
        GLint value = attachment == GL_STENCIL ? 8 : 0;
        return WebGLAny(script_state, value);
      }
      case GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE:
        return WebGLAny(script_state, GL_UNSIGNED_NORMALIZED);
      case GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING:
        return WebGLAny(script_state, GL_LINEAR);
      case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_BASE_VIEW_INDEX_OVR:
        if (ExtensionEnabled(kOVRMultiview2Name))
          return WebGLAny(script_state, 0);
        SynthesizeGLError(GL_INVALID_ENUM, kFunctionName,
                          "invalid parameter name, OVR_multiview2 not enabled");
        return ScriptValue::CreateNull(script_state->GetIsolate());
      case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_NUM_VIEWS_OVR:
        if (ExtensionEnabled(kOVRMultiview2Name))
          return WebGLAny(script_state, 0);
        SynthesizeGLError(GL_INVALID_ENUM, kFunctionName,
                          "invalid parameter name, OVR_multiview2 not enabled");
        return ScriptValue::CreateNull(script_state->GetIsolate());
      default:
        SynthesizeGLError(GL_INVALID_ENUM, kFunctionName,
                          "invalid parameter name");
        return ScriptValue::CreateNull(script_state->GetIsolate());
    }
  }

  WebGLSharedObject* attachment_object = nullptr;
  if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
    WebGLSharedObject* depth_attachment =
        framebuffer_binding->GetAttachmentObject(GL_DEPTH_ATTACHMENT);
    WebGLSharedObject* stencil_attachment =
        framebuffer_binding->GetAttachmentObject(GL_STENCIL_ATTACHMENT);
    if (depth_attachment != stencil_attachment) {
      SynthesizeGLError(
          GL_INVALID_OPERATION, kFunctionName,
          "different objects bound to DEPTH_ATTACHMENT and STENCIL_ATTACHMENT");
      return ScriptValue::CreateNull(script_state->GetIsolate());
    }
    attachment_object = depth_attachment;
  } else {
    attachment_object = framebuffer_binding->GetAttachmentObject(attachment);
  }

  if (!attachment_object) {
    switch (pname) {
      case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE:
        return WebGLAny(script_state, GL_NONE);
      case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME:
        return ScriptValue::CreateNull(script_state->GetIsolate());
      default:
        SynthesizeGLError(GL_INVALID_OPERATION, kFunctionName,
                          "invalid parameter name");
        return ScriptValue::CreateNull(script_state->GetIsolate());
    }
  }
  DCHECK(attachment_object->IsTexture() || attachment_object->IsRenderbuffer());

  switch (pname) {
    case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE:
      if (attachment_object->IsTexture())
        return WebGLAny(script_state, GL_TEXTURE);
      return WebGLAny(script_state, GL_RENDERBUFFER);
    case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME:
      return WebGLAny(script_state, attachment_object);
    case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE:
    case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER:
    case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL:
      if (!attachment_object->IsTexture())
        break;
      FALLTHROUGH;
    case GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE:
    case GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE:
    case GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE:
    case GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE:
    case GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE:
    case GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE: {
      GLint value = 0;
      ContextGL()->GetFramebufferAttachmentParameteriv(target, attachment,
                                                       pname, &value);
      return WebGLAny(script_state, value);
    }
    case GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE:
      if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
        SynthesizeGLError(
            GL_INVALID_OPERATION, kFunctionName,
            "COMPONENT_TYPE can't be queried for DEPTH_STENCIL_ATTACHMENT");
        return ScriptValue::CreateNull(script_state->GetIsolate());
      }
      FALLTHROUGH;
    case GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING: {
      GLint value = 0;
      ContextGL()->GetFramebufferAttachmentParameteriv(target, attachment,
                                                       pname, &value);
      return WebGLAny(script_state, static_cast<unsigned>(value));
    }
    case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_BASE_VIEW_INDEX_OVR:
    case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_NUM_VIEWS_OVR: {
      if (!ExtensionEnabled(kOVRMultiview2Name)) {
        SynthesizeGLError(GL_INVALID_ENUM, kFunctionName,
                          "invalid parameter name, OVR_multiview2 not enabled");
        return ScriptValue::CreateNull(script_state->GetIsolate());
      }
      GLint value = 0;
      ContextGL()->GetFramebufferAttachmentParameteriv(target, attachment,
                                                       pname, &value);
      return WebGLAny(script_state, static_cast<unsigned>(value));
    }
    default:
      break;
  }
  SynthesizeGLError(GL_INVALID_ENUM, kFunctionName, "invalid parameter name");
  return ScriptValue::CreateNull(script_state->GetIsolate());
}

void WebGL2RenderingContextBase::Trace(blink::Visitor* visitor) {
  visitor->Trace(read_framebuffer_binding_);
  visitor->Trace(transform_feedback_binding_);
  visitor->Trace(default_transform_feedback_);
  visitor->Trace(bound_copy_read_buffer_);
  visitor->Trace(bound_copy_write_buffer_);
  visitor->Trace(bound_pixel_pack_buffer_);
  visitor->Trace(bound_pixel_unpack_buffer_);
  visitor->Trace(bound_transform_feedback_buffer_);
  visitor->Trace(bound_uniform_buffer_);
  visitor->Trace(bound_indexed_uniform_buffers_);
  visitor->Trace(current_boolean_occlusion_query_);
  visitor->Trace(current_transform_feedback_primitives_written_query_);
  visitor->Trace(current_elapsed_query_);
  visitor->Trace(sampler_units_);
  WebGLRenderingContextBase::Trace(visitor);
}

WebGLTexture* WebGL2RenderingContextBase::ValidateTexture3DBinding(
    const char* function_name,
    GLenum target) {
  WebGLTexture* tex = nullptr;
  switch (target) {
    case GL_TEXTURE_2D_ARRAY:
      tex = texture_units_[active_texture_unit_].texture2d_array_binding_.Get();
      break;
    case GL_TEXTURE_3D:
      tex = texture_units_[active_texture_unit_].texture3d_binding_.Get();
      break;
    default:
      SynthesizeGLError(GL_INVALID_ENUM, function_name,
                        "invalid texture target");
      return nullptr;
  }
  if (!tex)
    SynthesizeGLError(GL_INVALID_OPERATION, function_name,
                      "no texture bound to target");
  return tex;
}

GLint WebGL2RenderingContextBase::GetMaxTextureLevelForTarget(GLenum target) {
  switch (target) {
    case GL_TEXTURE_3D:
      return max3d_texture_level_;
    case GL_TEXTURE_2D_ARRAY:
      return max_texture_level_;
  }
  return WebGLRenderingContextBase::GetMaxTextureLevelForTarget(target);
}

ScriptValue WebGL2RenderingContextBase::getTexParameter(
    ScriptState* script_state,
    GLenum target,
    GLenum pname) {
  if (isContextLost() || !ValidateTextureBinding("getTexParameter", target))
    return ScriptValue::CreateNull(script_state->GetIsolate());

  switch (pname) {
    case GL_TEXTURE_WRAP_R:
    case GL_TEXTURE_COMPARE_FUNC:
    case GL_TEXTURE_COMPARE_MODE:
    case GL_TEXTURE_IMMUTABLE_LEVELS: {
      GLint value = 0;
      ContextGL()->GetTexParameteriv(target, pname, &value);
      return WebGLAny(script_state, static_cast<unsigned>(value));
    }
    case GL_TEXTURE_IMMUTABLE_FORMAT: {
      GLint value = 0;
      ContextGL()->GetTexParameteriv(target, pname, &value);
      return WebGLAny(script_state, static_cast<bool>(value));
    }
    case GL_TEXTURE_BASE_LEVEL:
    case GL_TEXTURE_MAX_LEVEL: {
      GLint value = 0;
      ContextGL()->GetTexParameteriv(target, pname, &value);
      return WebGLAny(script_state, value);
    }
    case GL_TEXTURE_MAX_LOD:
    case GL_TEXTURE_MIN_LOD: {
      GLfloat value = 0.f;
      ContextGL()->GetTexParameterfv(target, pname, &value);
      return WebGLAny(script_state, value);
    }
    default:
      return WebGLRenderingContextBase::getTexParameter(script_state, target,
                                                        pname);
  }
}

WebGLBuffer* WebGL2RenderingContextBase::ValidateBufferDataTarget(
    const char* function_name,
    GLenum target) {
  WebGLBuffer* buffer = nullptr;
  switch (target) {
    case GL_ELEMENT_ARRAY_BUFFER:
      buffer = bound_vertex_array_object_->BoundElementArrayBuffer();
      break;
    case GL_ARRAY_BUFFER:
      buffer = bound_array_buffer_.Get();
      break;
    case GL_COPY_READ_BUFFER:
      buffer = bound_copy_read_buffer_.Get();
      break;
    case GL_COPY_WRITE_BUFFER:
      buffer = bound_copy_write_buffer_.Get();
      break;
    case GL_PIXEL_PACK_BUFFER:
      buffer = bound_pixel_pack_buffer_.Get();
      break;
    case GL_PIXEL_UNPACK_BUFFER:
      buffer = bound_pixel_unpack_buffer_.Get();
      break;
    case GL_TRANSFORM_FEEDBACK_BUFFER:
      buffer = bound_transform_feedback_buffer_.Get();
      break;
    case GL_UNIFORM_BUFFER:
      buffer = bound_uniform_buffer_.Get();
      break;
    default:
      SynthesizeGLError(GL_INVALID_ENUM, function_name, "invalid target");
      return nullptr;
  }
  if (!buffer) {
    SynthesizeGLError(GL_INVALID_OPERATION, function_name, "no buffer");
    return nullptr;
  }
  return buffer;
}

bool WebGL2RenderingContextBase::ValidateBufferDataUsage(
    const char* function_name,
    GLenum usage) {
  switch (usage) {
    case GL_STREAM_READ:
    case GL_STREAM_COPY:
    case GL_STATIC_READ:
    case GL_STATIC_COPY:
    case GL_DYNAMIC_READ:
    case GL_DYNAMIC_COPY:
      return true;
    default:
      return WebGLRenderingContextBase::ValidateBufferDataUsage(function_name,
                                                                usage);
  }
}

const char* WebGL2RenderingContextBase::ValidateGetBufferSubData(
    const char* function_name,
    GLenum target,
    int64_t source_byte_offset,
    DOMArrayBufferView* destination_array_buffer_view,
    GLuint destination_offset,
    GLuint length,
    WebGLBuffer** out_source_buffer,
    void** out_destination_data_ptr,
    int64_t* out_destination_byte_length) {
  if (isContextLost()) {
    return "Context lost";
  }

  if (!ValidateValueFitNonNegInt32(function_name, "srcByteOffset",
                                   source_byte_offset)) {
    return "Invalid value: srcByteOffset";
  }

  WebGLBuffer* source_buffer = ValidateBufferDataTarget(function_name, target);
  if (!source_buffer) {
    return "Invalid operation: no buffer bound to target";
  }
  if (transform_feedback_binding_->active() &&
      transform_feedback_binding_->UsesBuffer(source_buffer)) {
    SynthesizeGLError(GL_INVALID_OPERATION, function_name,
                      "buffer in use for transform feedback");
    return "Invalid operation: buffer in use for transform feedback";
  }
  *out_source_buffer = source_buffer;

  if (!ValidateSubSourceAndGetData(
          destination_array_buffer_view, destination_offset, length,
          out_destination_data_ptr, out_destination_byte_length)) {
    SynthesizeGLError(GL_INVALID_VALUE, function_name, "overflow of dstData");
    return "Invalid value: overflow of dstData";
  }

  return nullptr;
}

const char* WebGL2RenderingContextBase::ValidateGetBufferSubDataBounds(
    const char* function_name,
    WebGLBuffer* source_buffer,
    GLintptr source_byte_offset,
    int64_t destination_byte_length) {
  base::CheckedNumeric<int64_t> src_end = source_byte_offset;
  src_end += destination_byte_length;
  if (!src_end.IsValid() || src_end.ValueOrDie() > source_buffer->GetSize()) {
    SynthesizeGLError(GL_INVALID_VALUE, function_name,
                      "overflow of bound buffer");
    return "Invalid value: overflow of bound buffer";
  }

  return nullptr;
}

void WebGL2RenderingContextBase::RemoveBoundBuffer(WebGLBuffer* buffer) {
  if (bound_copy_read_buffer_ == buffer)
    bound_copy_read_buffer_ = nullptr;
  if (bound_copy_write_buffer_ == buffer)
    bound_copy_write_buffer_ = nullptr;
  if (bound_pixel_pack_buffer_ == buffer)
    bound_pixel_pack_buffer_ = nullptr;
  if (bound_pixel_unpack_buffer_ == buffer)
    bound_pixel_unpack_buffer_ = nullptr;
  if (bound_transform_feedback_buffer_ == buffer)
    bound_transform_feedback_buffer_ = nullptr;
  if (bound_uniform_buffer_ == buffer)
    bound_uniform_buffer_ = nullptr;

  transform_feedback_binding_->UnbindBuffer(buffer);

  WebGLRenderingContextBase::RemoveBoundBuffer(buffer);
}

void WebGL2RenderingContextBase::RestoreCurrentFramebuffer() {
  bindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer_binding_.Get());
  bindFramebuffer(GL_READ_FRAMEBUFFER, read_framebuffer_binding_.Get());
}

void WebGL2RenderingContextBase::useProgram(WebGLProgram* program) {
  if (transform_feedback_binding_->active() &&
      !transform_feedback_binding_->paused()) {
    SynthesizeGLError(GL_INVALID_OPERATION, "useProgram",
                      "transform feedback is active and not paused");
    return;
  }
  WebGLRenderingContextBase::useProgram(program);
}

GLint WebGL2RenderingContextBase::GetMaxTransformFeedbackSeparateAttribs()
    const {
  return max_transform_feedback_separate_attribs_;
}

WebGLImageConversion::PixelStoreParams
WebGL2RenderingContextBase::GetPackPixelStoreParams() {
  WebGLImageConversion::PixelStoreParams params;
  params.alignment = pack_alignment_;
  params.row_length = pack_row_length_;
  params.skip_pixels = pack_skip_pixels_;
  params.skip_rows = pack_skip_rows_;
  return params;
}

WebGLImageConversion::PixelStoreParams
WebGL2RenderingContextBase::GetUnpackPixelStoreParams(
    TexImageDimension dimension) {
  WebGLImageConversion::PixelStoreParams params;
  params.alignment = unpack_alignment_;
  params.row_length = unpack_row_length_;
  params.skip_pixels = unpack_skip_pixels_;
  params.skip_rows = unpack_skip_rows_;
  if (dimension == kTex3D) {
    params.image_height = unpack_image_height_;
    params.skip_images = unpack_skip_images_;
  }
  return params;
}

void WebGL2RenderingContextBase::
    DrawingBufferClientRestorePixelUnpackBufferBinding() {
  if (destruction_in_progress_)
    return;
  if (!ContextGL())
    return;
  ContextGL()->BindBuffer(GL_PIXEL_UNPACK_BUFFER,
                          ObjectOrZero(bound_pixel_unpack_buffer_.Get()));
}

void WebGL2RenderingContextBase::
    DrawingBufferClientRestorePixelPackBufferBinding() {
  if (destruction_in_progress_)
    return;
  if (!ContextGL())
    return;
  ContextGL()->BindBuffer(GL_PIXEL_PACK_BUFFER,
                          ObjectOrZero(bound_pixel_pack_buffer_.Get()));
}

void WebGL2RenderingContextBase::
    DrawingBufferClientRestorePixelPackParameters() {
  if (destruction_in_progress_)
    return;
  if (!ContextGL())
    return;

  ContextGL()->PixelStorei(GL_PACK_ROW_LENGTH, pack_row_length_);
  ContextGL()->PixelStorei(GL_PACK_SKIP_ROWS, pack_skip_rows_);
  ContextGL()->PixelStorei(GL_PACK_SKIP_PIXELS, pack_skip_pixels_);

  WebGLRenderingContextBase::DrawingBufferClientRestorePixelPackParameters();
}

}  // namespace blink
