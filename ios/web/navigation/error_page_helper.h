// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IOS_WEB_NAVIGATION_ERROR_PAGE_HELPER_H_
#define IOS_WEB_NAVIGATION_ERROR_PAGE_HELPER_H_

#import <Foundation/Foundation.h>

class GURL;

// Class used to create an Error Page, constructing all the information needed
// based on the initial error.
@interface ErrorPageHelper : NSObject

// Failed URL of the failed navigation.
@property(nonatomic, strong, readonly) NSURL* failedNavigationURL;
// The error page file to be loaded as a new page.
@property(nonatomic, strong, readonly) NSURL* errorPageFileURL;
// The error page HTML content to be injected into current page.
@property(nonatomic, strong, readonly) NSString* scriptToInject;

- (instancetype)initWithError:(NSError*)error NS_DESIGNATED_INITIALIZER;

- (instancetype)init NS_UNAVAILABLE;

// Returns the failed URL if |URL| is an error page URL, otherwise empty URL.
+ (GURL)failedNavigationURLFromErrorPageFileURL:(const GURL&)URL;

// Returns YES if |URL| is a file URL for this error page.
- (BOOL)isErrorPageFileURLForFailedNavigationURL:(NSURL*)URL;

@end

#endif  // IOS_WEB_NAVIGATION_ERROR_PAGE_HELPER_H_
