// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

GEN_INCLUDE(['switch_access_e2e_test_base.js']);

UNDEFINED_INTERVAL_DELAY = -1;

/**
 * @constructor
 * @extends {SwitchAccessE2ETest}
 */
function SwitchAccessAutoScanManagerTest() {
  SwitchAccessE2ETest.call(this);
}

SwitchAccessAutoScanManagerTest.prototype = {
  __proto__: SwitchAccessE2ETest.prototype,

  /** @override */
  setUp() {
    // Use intervalCount and intervalDelay to check how many intervals are
    // currently running (should be no more than 1) and the current delay.
    window.intervalCount = 0;
    window.intervalDelay = UNDEFINED_INTERVAL_DELAY;
    window.defaultSetInterval = window.setInterval;
    window.defaultClearInterval = window.clearInterval;
    window.switchAccess.defaultMoveForward = window.switchAccess.moveForward;
    window.switchAccess.moveForwardCount = 0;


    window.setInterval = function(func, delay) {
      window.intervalCount++;
      window.intervalDelay = delay;

      // Override the delay to 1 ms to speed up the test.
      return window.defaultSetInterval(func, 1);
    };

    window.clearInterval = function(intervalId) {
      if (intervalId) {
        window.intervalCount--;
      }
      window.defaultClearInterval(intervalId);
    };

    window.switchAccess.moveForward = function() {
      window.switchAccess.moveForwardCount++;
      window.switchAccess.defaultMoveForward();
    };

    this.autoScanManager = window.switchAccess.autoScanManager_;
    switchAccess.onMoveForwardForTesting_ = null;
  }
};

TEST_F('SwitchAccessAutoScanManagerTest', 'SetEnabled', function() {
  this.runWithLoadedTree('', (desktop) => {
    assertFalse(
        this.autoScanManager.isRunning(),
        'Auto scan manager is running prematurely');
    assertEquals(
        0, switchAccess.moveForwardCount,
        'Incorrect initialization of moveForwardCount');
    assertEquals(0, intervalCount, 'Incorrect initialization of intervalCount');

    switchAccess.onMoveForwardForTesting_ = this.newCallback(() => {
      assertTrue(
          this.autoScanManager.isRunning(),
          'Auto scan manager has stopped running');
      assertGT(
          switchAccess.moveForwardCount, 0,
          'Switch Access has not moved forward');
      assertEquals(
          1, intervalCount, 'The number of intervals is no longer exactly 1');
    });

    this.autoScanManager.setEnabled(true);
    assertTrue(
        this.autoScanManager.isRunning(), 'Auto scan manager is not running');
    assertEquals(1, intervalCount, 'There is not exactly 1 interval');
  });
});

TEST_F('SwitchAccessAutoScanManagerTest', 'SetEnabledMultiple', function() {
  this.runWithLoadedTree('', (desktop) => {
    assertFalse(
        this.autoScanManager.isRunning(),
        'Auto scan manager is running prematurely');
    assertEquals(0, intervalCount, 'Incorrect initialization of intervalCount');

    this.autoScanManager.setEnabled(true);
    this.autoScanManager.setEnabled(true);
    this.autoScanManager.setEnabled(true);

    assertTrue(
        this.autoScanManager.isRunning(), 'Auto scan manager is not running');
    assertEquals(1, intervalCount, 'There is not exactly 1 interval');
  });
});

TEST_F('SwitchAccessAutoScanManagerTest', 'EnableAndDisable', function() {
  this.runWithLoadedTree('', (desktop) => {
    assertFalse(
        this.autoScanManager.isRunning(),
        'Auto scan manager is running prematurely');
    assertEquals(0, intervalCount, 'Incorrect initialization of intervalCount');

    this.autoScanManager.setEnabled(true);
    assertTrue(
        this.autoScanManager.isRunning(), 'Auto scan manager is not running');
    assertEquals(1, intervalCount, 'There is not exactly 1 interval');

    this.autoScanManager.setEnabled(false);
    assertFalse(
        this.autoScanManager.isRunning(),
        'Auto scan manager did not stop running');
    assertEquals(0, intervalCount, 'Interval was not removed');
  });
});

TEST_F(
    'SwitchAccessAutoScanManagerTest', 'RestartIfRunningMultiple', function() {
      this.runWithLoadedTree('', (desktop) => {
        assertFalse(
            this.autoScanManager.isRunning(),
            'Auto scan manager is running prematurely');
        assertEquals(
            0, switchAccess.moveForwardCount,
            'Incorrect initialization of moveForwardCount');
        assertEquals(
            0, intervalCount, 'Incorrect initialization of intervalCount');

        this.autoScanManager.setEnabled(true);
        this.autoScanManager.restartIfRunning();
        this.autoScanManager.restartIfRunning();
        this.autoScanManager.restartIfRunning();

        assertTrue(
            this.autoScanManager.isRunning(),
            'Auto scan manager is not running');
        assertEquals(1, intervalCount, 'There is not exactly 1 interval');
      });
    });

TEST_F(
    'SwitchAccessAutoScanManagerTest', 'RestartIfRunningWhenOff', function() {
      this.runWithLoadedTree('', (desktop) => {
        assertFalse(
            this.autoScanManager.isRunning(),
            'Auto scan manager is running at start.');
        this.autoScanManager.restartIfRunning();
        assertFalse(
            this.autoScanManager.isRunning(),
            'Auto scan manager enabled by restartIfRunning');
      });
    });

TEST_F('SwitchAccessAutoScanManagerTest', 'SetDefaultScanTime', function() {
  this.runWithLoadedTree('', (desktop) => {
    assertFalse(
        this.autoScanManager.isRunning(),
        'Auto scan manager is running prematurely');
    assertEquals(
        UNDEFINED_INTERVAL_DELAY, intervalDelay,
        'Interval delay improperly initialized');

    this.autoScanManager.setDefaultScanTime(2);
    assertFalse(
        this.autoScanManager.isRunning(),
        'Setting default scan time started auto-scanning');
    assertEquals(
        2, this.autoScanManager.defaultScanTime_,
        'Default scan time set improperly');
    assertEquals(
        UNDEFINED_INTERVAL_DELAY, intervalDelay,
        'Interval delay set prematurely');

    this.autoScanManager.setEnabled(true);
    assertTrue(this.autoScanManager.isRunning(), 'Auto scan did not start');
    assertEquals(
        2, this.autoScanManager.defaultScanTime_,
        'Default scan time has changed');
    assertEquals(2, intervalDelay, 'Interval delay not set');

    this.autoScanManager.setDefaultScanTime(5);
    assertTrue(this.autoScanManager.isRunning(), 'Auto scan stopped');
    assertEquals(
        5, this.autoScanManager.defaultScanTime_,
        'Default scan time did not change when set a second time');
    assertEquals(5, intervalDelay, 'Interval delay did not update');
  });
});
