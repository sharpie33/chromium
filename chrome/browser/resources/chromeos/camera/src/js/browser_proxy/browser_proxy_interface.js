// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * The abstract interface for the CCA's interaction with the browser.
 * @interface
 */
export class BrowserProxy {
  /** @param {function(!Array<!chrome.fileSystem.Volume>=)} callback */
  getVolumeList(callback) {}

  /**
   * @param {!chrome.fileSystem.RequestFileSystemOptions} options
   * @param {function(!FileSystem=)} callback
   */
  requestFileSystem(options, callback) {}

  /**
   * @param {(string|!Array<string>|!Object)} keys
   * @param {function(!Object)} callback
   */
  localStorageGet(keys, callback) {}

  /**
   * @param {!Object<string>} items
   * @param {function()=} callback
   */
  localStorageSet(items, callback) {}

  /**
   * @param {(string|!Array<string>)} items
   * @param {function()=} callback
   */
  localStorageRemove(items, callback) {}
}
