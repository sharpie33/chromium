// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/compositor/surface_utils.h"

#include "build/build_config.h"
#include "components/viz/host/host_frame_sink_manager.h"

#if defined(OS_ANDROID)
#include "content/browser/renderer_host/compositor_dependencies_android.h"
#else
#include "content/browser/compositor/image_transport_factory.h"
#include "ui/compositor/compositor.h"  // nogncheck
#endif

namespace content {

viz::FrameSinkId AllocateFrameSinkId() {
#if defined(OS_ANDROID)
  return CompositorDependenciesAndroid::Get().AllocateFrameSinkId();
#else
  ImageTransportFactory* factory = ImageTransportFactory::GetInstance();
  return factory->GetContextFactoryPrivate()->AllocateFrameSinkId();
#endif
}

viz::HostFrameSinkManager* GetHostFrameSinkManager() {
#if defined(OS_ANDROID)
  return CompositorDependenciesAndroid::Get().host_frame_sink_manager();
#else
  ImageTransportFactory* factory = ImageTransportFactory::GetInstance();
  if (!factory)
    return nullptr;
  return factory->GetContextFactoryPrivate()->GetHostFrameSinkManager();
#endif
}

}  // namespace content
