/*
 * Copyright (C) 2006, 2007, 2008 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "third_party/blink/renderer/core/clipboard/data_transfer.h"

#include <memory>

#include "build/build_config.h"
#include "third_party/blink/public/platform/web_screen_info.h"
#include "third_party/blink/renderer/core/clipboard/clipboard_mime_types.h"
#include "third_party/blink/renderer/core/clipboard/clipboard_utilities.h"
#include "third_party/blink/renderer/core/clipboard/data_object.h"
#include "third_party/blink/renderer/core/clipboard/data_transfer_access_policy.h"
#include "third_party/blink/renderer/core/clipboard/data_transfer_item.h"
#include "third_party/blink/renderer/core/clipboard/data_transfer_item_list.h"
#include "third_party/blink/renderer/core/editing/ephemeral_range.h"
#include "third_party/blink/renderer/core/editing/frame_selection.h"
#include "third_party/blink/renderer/core/editing/serializers/serialization.h"
#include "third_party/blink/renderer/core/editing/visible_selection.h"
#include "third_party/blink/renderer/core/fileapi/file_list.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/visual_viewport.h"
#include "third_party/blink/renderer/core/html/forms/text_control_element.h"
#include "third_party/blink/renderer/core/html/html_image_element.h"
#include "third_party/blink/renderer/core/layout/layout_image.h"
#include "third_party/blink/renderer/core/layout/layout_object.h"
#include "third_party/blink/renderer/core/loader/resource/image_resource_content.h"
#include "third_party/blink/renderer/core/page/chrome_client.h"
#include "third_party/blink/renderer/core/page/drag_image.h"
#include "third_party/blink/renderer/core/page/page.h"
#include "third_party/blink/renderer/core/paint/paint_info.h"
#include "third_party/blink/renderer/core/paint/paint_layer.h"
#include "third_party/blink/renderer/core/paint/paint_layer_painter.h"
#include "third_party/blink/renderer/platform/graphics/paint/paint_canvas.h"
#include "third_party/blink/renderer/platform/graphics/paint/paint_record_builder.h"
#include "third_party/blink/renderer/platform/graphics/static_bitmap_image.h"
#include "third_party/blink/renderer/platform/graphics/unaccelerated_static_bitmap_image.h"
#include "third_party/blink/renderer/platform/network/mime/mime_type_registry.h"
#include "third_party/skia/include/core/SkSurface.h"

namespace blink {

namespace {

class DraggedNodeImageBuilder {
  STACK_ALLOCATED();

 public:
  DraggedNodeImageBuilder(LocalFrame& local_frame, Node& node)
      : local_frame_(&local_frame),
        node_(&node)
#if DCHECK_IS_ON()
        ,
        dom_tree_version_(node.GetDocument().DomTreeVersion())
#endif
  {
    for (Node& descendant : NodeTraversal::InclusiveDescendantsOf(*node_))
      descendant.SetDragged(true);
  }

  ~DraggedNodeImageBuilder() {
#if DCHECK_IS_ON()
    DCHECK_EQ(dom_tree_version_, node_->GetDocument().DomTreeVersion());
#endif
    for (Node& descendant : NodeTraversal::InclusiveDescendantsOf(*node_))
      descendant.SetDragged(false);
  }

  std::unique_ptr<DragImage> CreateImage() {
#if DCHECK_IS_ON()
    DCHECK_EQ(dom_tree_version_, node_->GetDocument().DomTreeVersion());
#endif
    // Construct layout object for |node_| with pseudo class "-webkit-drag"
    local_frame_->View()->UpdateAllLifecyclePhasesExceptPaint(
        DocumentUpdateReason::kDragImage);
    LayoutObject* const dragged_layout_object = node_->GetLayoutObject();
    if (!dragged_layout_object)
      return nullptr;
    // Paint starting at the nearest stacking context, clipped to the object
    // itself. This will also paint the contents behind the object if the
    // object contains transparency and there are other elements in the same
    // stacking context which stacked below.
    PaintLayer* layer = dragged_layout_object->EnclosingLayer();
    if (!layer->GetLayoutObject().StyleRef().IsStackingContext())
      layer = layer->AncestorStackingContext();

    IntRect absolute_bounding_box =
        dragged_layout_object->AbsoluteBoundingBoxRectIncludingDescendants();
    // TODO(chrishtr): consider using the root frame's visible rect instead
    // of the local frame, to avoid over-clipping.
    IntRect visible_rect(IntPoint(),
                         layer->GetLayoutObject().GetFrameView()->Size());
    // If the absolute bounding box is large enough to be possibly a memory
    // or IPC payload issue, clip it to the visible content rect.
    if (absolute_bounding_box.Size().Area() > visible_rect.Size().Area()) {
      absolute_bounding_box.Intersect(visible_rect);
    }

    FloatRect bounding_box =
        layer->GetLayoutObject()
            .AbsoluteToLocalQuad(FloatQuad(absolute_bounding_box))
            .BoundingBox();
    PaintLayerPaintingInfo painting_info(
        layer, CullRect(EnclosingIntRect(bounding_box)),
        kGlobalPaintFlattenCompositingLayers, PhysicalOffset());
    PaintLayerFlags flags = kPaintLayerHaveTransparency;
    PaintRecordBuilder builder;

    dragged_layout_object->GetDocument().Lifecycle().AdvanceTo(
        DocumentLifecycle::kInPaint);
    PaintLayerPainter(*layer).Paint(builder.Context(), painting_info, flags);
    dragged_layout_object->GetDocument().Lifecycle().AdvanceTo(
        DocumentLifecycle::kPaintClean);

    FloatPoint paint_offset = bounding_box.Location();
    PropertyTreeState border_box_properties =
        layer->GetLayoutObject().FirstFragment().LocalBorderBoxProperties();
    // We paint in the containing transform node's space. Add the offset from
    // the layer to this transform space.
    paint_offset +=
        FloatPoint(layer->GetLayoutObject().FirstFragment().PaintOffset());

    return DataTransfer::CreateDragImageForFrame(
        *local_frame_, 1.0f,
        LayoutObject::ShouldRespectImageOrientation(dragged_layout_object),
        bounding_box.Size(), paint_offset, builder, border_box_properties);
  }

 private:
  LocalFrame* const local_frame_;
  Node* const node_;
#if DCHECK_IS_ON()
  const uint64_t dom_tree_version_;
#endif
};
}  // namespace
static DragOperation ConvertEffectAllowedToDragOperation(const String& op) {
  // Values specified in
  // https://html.spec.whatwg.org/multipage/dnd.html#dom-datatransfer-effectallowed
  if (op == "uninitialized")
    return kDragOperationEvery;
  if (op == "none")
    return kDragOperationNone;
  if (op == "copy")
    return kDragOperationCopy;
  if (op == "link")
    return kDragOperationLink;
  if (op == "move")
    return (DragOperation)(kDragOperationGeneric | kDragOperationMove);
  if (op == "copyLink")
    return (DragOperation)(kDragOperationCopy | kDragOperationLink);
  if (op == "copyMove")
    return (DragOperation)(kDragOperationCopy | kDragOperationGeneric |
                           kDragOperationMove);
  if (op == "linkMove")
    return (DragOperation)(kDragOperationLink | kDragOperationGeneric |
                           kDragOperationMove);
  if (op == "all")
    return kDragOperationEvery;
  return kDragOperationPrivate;  // really a marker for "no conversion"
}

static String ConvertDragOperationToEffectAllowed(DragOperation op) {
  bool move_set = !!((kDragOperationGeneric | kDragOperationMove) & op);

  if ((move_set && (op & kDragOperationCopy) && (op & kDragOperationLink)) ||
      (op == kDragOperationEvery))
    return "all";
  if (move_set && (op & kDragOperationCopy))
    return "copyMove";
  if (move_set && (op & kDragOperationLink))
    return "linkMove";
  if ((op & kDragOperationCopy) && (op & kDragOperationLink))
    return "copyLink";
  if (move_set)
    return "move";
  if (op & kDragOperationCopy)
    return "copy";
  if (op & kDragOperationLink)
    return "link";
  return "none";
}

// We provide the IE clipboard types (URL and Text), and the clipboard types
// specified in the WHATWG Web Applications 1.0 draft see
// http://www.whatwg.org/specs/web-apps/current-work/ Section 6.3.5.3
static String NormalizeType(const String& type,
                            bool* convert_to_url = nullptr) {
  String clean_type = type.StripWhiteSpace().DeprecatedLower();
  if (clean_type == kMimeTypeText ||
      clean_type.StartsWith(kMimeTypeTextPlainEtc))
    return kMimeTypeTextPlain;
  if (clean_type == kMimeTypeURL) {
    if (convert_to_url)
      *convert_to_url = true;
    return kMimeTypeTextURIList;
  }
  return clean_type;
}

// static
DataTransfer* DataTransfer::Create() {
  DataTransfer* data = Create(
      kCopyAndPaste, DataTransferAccessPolicy::kWritable, DataObject::Create());
  data->drop_effect_ = "none";
  data->effect_allowed_ = "none";
  return data;
}

// static
DataTransfer* DataTransfer::Create(DataTransferType type,
                                   DataTransferAccessPolicy policy,
                                   DataObject* data_object) {
  return MakeGarbageCollected<DataTransfer>(type, policy, data_object);
}

DataTransfer::~DataTransfer() = default;

void DataTransfer::setDropEffect(const String& effect) {
  if (!IsForDragAndDrop())
    return;

  // The attribute must ignore any attempts to set it to a value other than
  // none, copy, link, and move.
  if (effect != "none" && effect != "copy" && effect != "link" &&
      effect != "move")
    return;

  // The specification states that dropEffect can be changed at all times, even
  // if the DataTransfer instance is protected or neutered.
  //
  // Allowing these changes seems inconsequential, but findDropZone() in
  // EventHandler.cpp relies on being able to call setDropEffect during
  // dragenter, when the DataTransfer policy is DataTransferTypesReadable.
  drop_effect_ = effect;
}

void DataTransfer::setEffectAllowed(const String& effect) {
  if (!IsForDragAndDrop())
    return;

  if (ConvertEffectAllowedToDragOperation(effect) == kDragOperationPrivate) {
    // This means that there was no conversion, and the effectAllowed that
    // we are passed isn't a valid effectAllowed, so we should ignore it,
    // and not set |effect_allowed_|.

    // The attribute must ignore any attempts to set it to a value other than
    // none, copy, copyLink, copyMove, link, linkMove, move, all, and
    // uninitialized.
    return;
  }

  if (CanWriteData())
    effect_allowed_ = effect;
}

void DataTransfer::clearData(const String& type) {
  if (!CanWriteData())
    return;

  if (type.IsNull())
    data_object_->ClearAll();
  else
    data_object_->ClearData(NormalizeType(type));
}

String DataTransfer::getData(const String& type) const {
  if (!CanReadData())
    return String();

  bool convert_to_url = false;
  String data = data_object_->GetData(NormalizeType(type, &convert_to_url));
  if (!convert_to_url)
    return data;
  return ConvertURIListToURL(data);
}

void DataTransfer::setData(const String& type, const String& data) {
  if (!CanWriteData())
    return;

  data_object_->SetData(NormalizeType(type), data);
}

bool DataTransfer::hasDataStoreItemListChanged() const {
  return data_store_item_list_changed_ || !CanReadTypes();
}

void DataTransfer::OnItemListChanged() {
  data_store_item_list_changed_ = true;
}

Vector<String> DataTransfer::types() {
  if (!CanReadTypes())
    return Vector<String>();

  data_store_item_list_changed_ = false;
  return data_object_->Types();
}

FileList* DataTransfer::files() const {
  auto* files = MakeGarbageCollected<FileList>();
  if (!CanReadData())
    return files;

  for (uint32_t i = 0; i < data_object_->length(); ++i) {
    if (data_object_->Item(i)->Kind() == DataObjectItem::kFileKind) {
      Blob* blob = data_object_->Item(i)->GetAsFile();
      if (auto* file = DynamicTo<File>(blob))
        files->Append(file);
    }
  }

  return files;
}

void DataTransfer::setDragImage(Element* image, int x, int y) {
  DCHECK(image);

  if (!IsForDragAndDrop())
    return;

  IntPoint location(x, y);
  auto* html_image_element = DynamicTo<HTMLImageElement>(image);
  if (html_image_element && !image->isConnected())
    SetDragImageResource(html_image_element->CachedImage(), location);
  else
    SetDragImageElement(image, location);
}

void DataTransfer::ClearDragImage() {
  setDragImage(nullptr, nullptr, IntPoint());
}

void DataTransfer::SetDragImageResource(ImageResourceContent* img,
                                        const IntPoint& loc) {
  setDragImage(img, nullptr, loc);
}

void DataTransfer::SetDragImageElement(Node* node, const IntPoint& loc) {
  setDragImage(nullptr, node, loc);
}

FloatRect DataTransfer::ClipByVisualViewport(const FloatRect& absolute_rect,
                                             const LocalFrame& frame) {
  IntRect viewport_in_root_frame =
      EnclosingIntRect(frame.GetPage()->GetVisualViewport().VisibleRect());
  FloatRect absolute_viewport =
      FloatRect(frame.View()->ConvertFromRootFrame(viewport_in_root_frame));
  return Intersection(absolute_viewport, absolute_rect);
}

// static
// Converts from size in CSS space to device space based on the given frame.
FloatSize DataTransfer::DeviceSpaceSize(const FloatSize& css_size,
                                        const LocalFrame& frame) {
  float device_scale_factor = frame.GetPage()->DeviceScaleFactorDeprecated();
  float page_scale_factor = frame.GetPage()->GetVisualViewport().Scale();
  FloatSize device_size(css_size);
  device_size.Scale(device_scale_factor * page_scale_factor);
  return device_size;
}

// static
// Returns a DragImage whose bitmap contains |contents|, positioned and scaled
// in device space.
std::unique_ptr<DragImage> DataTransfer::CreateDragImageForFrame(
    LocalFrame& frame,
    float opacity,
    RespectImageOrientationEnum image_orientation,
    const FloatSize& css_size,
    const FloatPoint& paint_offset,
    PaintRecordBuilder& builder,
    const PropertyTreeState& property_tree_state) {
  float device_scale_factor = frame.GetPage()->DeviceScaleFactorDeprecated();
  float page_scale_factor = frame.GetPage()->GetVisualViewport().Scale();

  FloatSize device_size = DeviceSpaceSize(css_size, frame);
  AffineTransform transform;
  FloatSize paint_offset_size =
      DeviceSpaceSize(FloatSize(paint_offset.X(), paint_offset.Y()), frame);
  transform.Translate(-paint_offset_size.Width(), -paint_offset_size.Height());
  transform.Scale(device_scale_factor * page_scale_factor);

  // Rasterize upfront, since DragImage::create() is going to do it anyway
  // (SkImage::asLegacyBitmap).
  SkSurfaceProps surface_props(0, kUnknown_SkPixelGeometry);
  sk_sp<SkSurface> surface = SkSurface::MakeRasterN32Premul(
      device_size.Width(), device_size.Height(), &surface_props);
  if (!surface)
    return nullptr;

  SkiaPaintCanvas skia_paint_canvas(surface->getCanvas());
  skia_paint_canvas.concat(AffineTransformToSkMatrix(transform));
  builder.EndRecording(skia_paint_canvas, property_tree_state);

  scoped_refptr<Image> image =
      UnacceleratedStaticBitmapImage::Create(surface->makeImageSnapshot());
  ChromeClient& chrome_client = frame.GetPage()->GetChromeClient();
  float screen_device_scale_factor =
      chrome_client.GetScreenInfo(frame).device_scale_factor;

  return DragImage::Create(image.get(), image_orientation,
                           screen_device_scale_factor, kInterpolationDefault,
                           opacity);
}

// static
std::unique_ptr<DragImage> DataTransfer::NodeImage(LocalFrame& frame,
                                                   Node& node) {
  DraggedNodeImageBuilder image_node(frame, node);
  return image_node.CreateImage();
}

std::unique_ptr<DragImage> DataTransfer::CreateDragImage(
    IntPoint& loc,
    LocalFrame* frame) const {
  if (drag_image_element_) {
    loc = drag_loc_;

    return NodeImage(*frame, *drag_image_element_);
  }
  if (drag_image_) {
    loc = drag_loc_;
    return DragImage::Create(drag_image_->GetImage());
  }
  return nullptr;
}

static ImageResourceContent* GetImageResourceContent(Element* element) {
  // Attempt to pull ImageResourceContent from element
  DCHECK(element);
  LayoutObject* layout_object = element->GetLayoutObject();
  if (!layout_object || !layout_object->IsImage())
    return nullptr;

  LayoutImage* image = ToLayoutImage(layout_object);
  if (image->CachedImage() && !image->CachedImage()->ErrorOccurred())
    return image->CachedImage();

  return nullptr;
}

static void WriteImageToDataObject(DataObject* data_object,
                                   Element* element,
                                   const KURL& image_url) {
  // Shove image data into a DataObject for use as a file
  ImageResourceContent* cached_image = GetImageResourceContent(element);
  if (!cached_image || !cached_image->GetImage() || !cached_image->IsLoaded())
    return;

  Image* image = cached_image->GetImage();
  scoped_refptr<SharedBuffer> image_buffer = image->Data();
  if (!image_buffer || !image_buffer->size())
    return;

  data_object->AddSharedBuffer(
      image_buffer, image_url, image->FilenameExtension(),
      cached_image->GetResponse().HttpHeaderFields().Get(
          http_names::kContentDisposition));
}

void DataTransfer::DeclareAndWriteDragImage(Element* element,
                                            const KURL& link_url,
                                            const KURL& image_url,
                                            const String& title) {
  if (!data_object_)
    return;

  data_object_->SetURLAndTitle(link_url.IsValid() ? link_url : image_url,
                               title);

  // Write the bytes in the image to the file format.
  WriteImageToDataObject(data_object_.Get(), element, image_url);

  // Put img tag on the clipboard referencing the image
  data_object_->SetData(kMimeTypeTextHTML,
                        CreateMarkup(element, kIncludeNode, kResolveAllURLs));
}

void DataTransfer::WriteURL(Node* node, const KURL& url, const String& title) {
  if (!data_object_)
    return;
  DCHECK(!url.IsEmpty());

  data_object_->SetURLAndTitle(url, title);

  // The URL can also be used as plain text.
  data_object_->SetData(kMimeTypeTextPlain, url.GetString());

  // The URL can also be used as an HTML fragment.
  data_object_->SetHTMLAndBaseURL(
      CreateMarkup(node, kIncludeNode, kResolveAllURLs), url);
}

void DataTransfer::WriteSelection(const FrameSelection& selection) {
  if (!data_object_)
    return;

  if (!EnclosingTextControl(
          selection.ComputeVisibleSelectionInDOMTreeDeprecated().Start())) {
    data_object_->SetHTMLAndBaseURL(selection.SelectedHTMLForClipboard(),
                                    selection.GetFrame()->GetDocument()->Url());
  }

  String str = selection.SelectedTextForClipboard();
#if defined(OS_WIN)
  ReplaceNewlinesWithWindowsStyleNewlines(str);
#endif
  ReplaceNBSPWithSpace(str);
  data_object_->SetData(kMimeTypeTextPlain, str);
}

void DataTransfer::SetAccessPolicy(DataTransferAccessPolicy policy) {
  // once you go numb, can never go back
  DCHECK(policy_ != DataTransferAccessPolicy::kNumb ||
         policy == DataTransferAccessPolicy::kNumb);
  policy_ = policy;
}

bool DataTransfer::CanReadTypes() const {
  return policy_ == DataTransferAccessPolicy::kReadable ||
         policy_ == DataTransferAccessPolicy::kTypesReadable ||
         policy_ == DataTransferAccessPolicy::kWritable;
}

bool DataTransfer::CanReadData() const {
  return policy_ == DataTransferAccessPolicy::kReadable ||
         policy_ == DataTransferAccessPolicy::kWritable;
}

bool DataTransfer::CanWriteData() const {
  return policy_ == DataTransferAccessPolicy::kWritable;
}

bool DataTransfer::CanSetDragImage() const {
  return policy_ == DataTransferAccessPolicy::kImageWritable ||
         policy_ == DataTransferAccessPolicy::kWritable;
}

DragOperation DataTransfer::SourceOperation() const {
  DragOperation op = ConvertEffectAllowedToDragOperation(effect_allowed_);
  DCHECK_NE(op, kDragOperationPrivate);
  return op;
}

DragOperation DataTransfer::DestinationOperation() const {
  DragOperation op = ConvertEffectAllowedToDragOperation(drop_effect_);
  DCHECK(op == kDragOperationCopy || op == kDragOperationNone ||
         op == kDragOperationLink ||
         op == (DragOperation)(kDragOperationGeneric | kDragOperationMove) ||
         op == kDragOperationEvery);
  return op;
}

void DataTransfer::SetSourceOperation(DragOperation op) {
  DCHECK_NE(op, kDragOperationPrivate);
  effect_allowed_ = ConvertDragOperationToEffectAllowed(op);
}

void DataTransfer::SetDestinationOperation(DragOperation op) {
  DCHECK(op == kDragOperationCopy || op == kDragOperationNone ||
         op == kDragOperationLink || op == kDragOperationGeneric ||
         op == kDragOperationMove ||
         op == static_cast<DragOperation>(kDragOperationGeneric |
                                          kDragOperationMove));
  drop_effect_ = ConvertDragOperationToEffectAllowed(op);
}

bool DataTransfer::HasDropZoneType(const String& keyword) {
  if (keyword.StartsWith("file:"))
    return HasFileOfType(keyword.Substring(5));

  if (keyword.StartsWith("string:"))
    return HasStringOfType(keyword.Substring(7));

  return false;
}

DataTransferItemList* DataTransfer::items() {
  // TODO: According to the spec, we are supposed to return the same collection
  // of items each time. We now return a wrapper that always wraps the *same*
  // set of items, so JS shouldn't be able to tell, but we probably still want
  // to fix this.
  return MakeGarbageCollected<DataTransferItemList>(this, data_object_);
}

DataObject* DataTransfer::GetDataObject() const {
  return data_object_;
}

DataTransfer::DataTransfer(DataTransferType type,
                           DataTransferAccessPolicy policy,
                           DataObject* data_object)
    : policy_(policy),
      drop_effect_("uninitialized"),
      effect_allowed_("uninitialized"),
      transfer_type_(type),
      data_object_(data_object),
      data_store_item_list_changed_(true) {
  data_object_->AddObserver(this);
}

void DataTransfer::setDragImage(ImageResourceContent* image,
                                Node* node,
                                const IntPoint& loc) {
  if (!CanSetDragImage())
    return;

  drag_image_ = image;
  drag_loc_ = loc;
  drag_image_element_ = node;
}

bool DataTransfer::HasFileOfType(const String& type) const {
  if (!CanReadTypes())
    return false;

  for (uint32_t i = 0; i < data_object_->length(); ++i) {
    if (data_object_->Item(i)->Kind() == DataObjectItem::kFileKind) {
      Blob* blob = data_object_->Item(i)->GetAsFile();
      if (blob && blob->IsFile() &&
          DeprecatedEqualIgnoringCase(blob->type(), type))
        return true;
    }
  }
  return false;
}

bool DataTransfer::HasStringOfType(const String& type) const {
  if (!CanReadTypes())
    return false;

  return data_object_->Types().Contains(type);
}

DragOperation ConvertDropZoneOperationToDragOperation(
    const String& drag_operation) {
  if (drag_operation == "copy")
    return kDragOperationCopy;
  if (drag_operation == "move")
    return kDragOperationMove;
  if (drag_operation == "link")
    return kDragOperationLink;
  return kDragOperationNone;
}

String ConvertDragOperationToDropZoneOperation(DragOperation operation) {
  switch (operation) {
    case kDragOperationCopy:
      return String("copy");
    case kDragOperationMove:
      return String("move");
    case kDragOperationLink:
      return String("link");
    default:
      return String("copy");
  }
}

void DataTransfer::Trace(blink::Visitor* visitor) {
  visitor->Trace(data_object_);
  visitor->Trace(drag_image_);
  visitor->Trace(drag_image_element_);
  ScriptWrappable::Trace(visitor);
}

}  // namespace blink
