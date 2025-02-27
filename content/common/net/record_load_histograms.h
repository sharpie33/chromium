// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_COMMON_NET_RECORD_LOAD_HISTOGRAMS_H_
#define CONTENT_COMMON_NET_RECORD_LOAD_HISTOGRAMS_H_

#include "third_party/blink/public/mojom/loader/resource_load_info.mojom-shared.h"
#include "url/origin.h"

namespace content {

// Logs histograms when a resource destined for a renderer (One with a
// blink::mojom::ResourceType) finishes loading, or when a load is aborted. Not
// used for internal network requests initiated by the browser itself.
void RecordLoadHistograms(const url::Origin& origin,
                          blink::mojom::ResourceType resource_type,
                          int net_error);

}  // namespace content

#endif  // CONTENT_COMMON_NET_RECORD_LOAD_HISTOGRAMS_H_
