// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.download.home.list;

import static org.mockito.Mockito.times;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;

import org.junit.After;
import org.junit.Assert;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.Mock;
import org.mockito.junit.MockitoJUnit;
import org.mockito.junit.MockitoRule;
import org.robolectric.annotation.Config;

import org.chromium.base.CollectionUtil;
import org.chromium.base.test.BaseRobolectricTestRunner;
import org.chromium.chrome.browser.download.home.DownloadManagerUiConfig;
import org.chromium.chrome.browser.download.home.JustNowProvider;
import org.chromium.chrome.browser.download.home.StableIds;
import org.chromium.chrome.browser.download.home.filter.OfflineItemFilterSource;
import org.chromium.chrome.browser.download.home.list.ListItem.OfflineItemListItem;
import org.chromium.chrome.browser.download.home.list.ListItem.SectionHeaderListItem;
import org.chromium.chrome.browser.download.home.list.mutator.DateOrderedListMutator;
import org.chromium.chrome.browser.download.home.list.mutator.ListMutationController;
import org.chromium.components.offline_items_collection.OfflineItem;
import org.chromium.components.offline_items_collection.OfflineItemFilter;
import org.chromium.components.offline_items_collection.OfflineItemState;
import org.chromium.ui.modelutil.ListObservable.ListObserver;

import java.util.Calendar;
import java.util.Collections;
import java.util.Date;

/** Unit tests for the DateOrderedListMutator class. */
@RunWith(BaseRobolectricTestRunner.class)
@Config(manifest = Config.NONE)
public class DateOrderedListMutatorTest {
    @Mock
    private OfflineItemFilterSource mSource;

    @Mock
    private ListObserver<Void> mObserver;

    private ListItemModel mModel;

    @Rule
    public MockitoRule mMockitoRule = MockitoJUnit.rule();

    @Before
    public void setUp() {
        mModel = new ListItemModel();
    }

    @After
    public void tearDown() {
        mModel = null;
    }

    /**
     * Action                               List
     * 1. Set()                             [ ]
     */
    @Test
    public void testNoItemsAndSetup() {
        when(mSource.getItems()).thenReturn(Collections.emptySet());
        DateOrderedListMutator list = createMutatorWithoutJustNowProvider();
        verify(mSource, times(1)).addObserver(list);

        Assert.assertEquals(0, mModel.size());
    }

    /**
     * Action                               List
     * 1. Set(item1 @ 1:00 1/1/2018)        [ DATE    @ 0:00 1/1/2018,
     *                                        item1   @ 1:00 1/1/2018 ]
     */
    @Test
    public void testSingleItem() {
        OfflineItem item1 = buildItem("1", buildCalendar(2018, 1, 1, 1), OfflineItemFilter.VIDEO);
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1));
        DateOrderedListMutator list = createMutatorWithoutJustNowProvider();

        Assert.assertEquals(2, mModel.size());
        assertSectionHeader(mModel.get(0), buildCalendar(2018, 1, 1, 0), false);
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 1, 1), item1);
    }

    /**
     * Action                               List
     * 1. Set(item1 @ 2:00 1/1/2018,        [ DATE    @ 0:00 1/1/2018,
     *        item2 @ 1:00 1/1/2018)
     *                                        item1   @ 2:00 1/1/2018,
     *                                        item2   @ 1:00 1/1/2018 ]
     */
    @Test
    public void testTwoItemsSameDay() {
        OfflineItem item1 = buildItem("1", buildCalendar(2018, 1, 1, 2), OfflineItemFilter.VIDEO);
        OfflineItem item2 = buildItem("2", buildCalendar(2018, 1, 1, 1), OfflineItemFilter.VIDEO);
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1, item2));
        DateOrderedListMutator list = createMutatorWithoutJustNowProvider();

        Assert.assertEquals(3, mModel.size());
        assertSectionHeader(mModel.get(0), buildCalendar(2018, 1, 1, 0), false);
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 1, 2), item1);
        assertOfflineItem(mModel.get(2), buildCalendar(2018, 1, 1, 1), item2);
    }

    /**
     * Action                                     List
     * 1. Set(item1 @ 2:00 1/1/2018 Video,        [ DATE    @ 0:00 1/1/2018,
     *        item2 @ 1:00 1/1/2018 Audio)          item1   @ 2:00 1/1/2018,
     *                                              item2   @ 1:00 1/1/2018 ]
     */
    @Test
    public void testTwoItemsSameDayDifferentSection() {
        OfflineItem item1 = buildItem("1", buildCalendar(2018, 1, 1, 2), OfflineItemFilter.VIDEO);
        OfflineItem item2 = buildItem("2", buildCalendar(2018, 1, 1, 1), OfflineItemFilter.AUDIO);
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1, item2));
        DateOrderedListMutator list = createMutatorWithoutJustNowProvider();

        Assert.assertEquals(3, mModel.size());
        assertSectionHeader(mModel.get(0), buildCalendar(2018, 1, 1, 0), false);
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 1, 2), item1);
        assertOfflineItem(mModel.get(2), buildCalendar(2018, 1, 1, 1), item2);
    }

    /**
     * Action                               List
     * 1. Set(item1 @ 1:00 1/1/2018         [ DATE    Just Now,
     *        IN_PROGRESS)
     *                                        item1   @ 1:00 1/1/2018 ]
     */
    @Test
    public void testSingleItemInJustNowSection() {
        OfflineItem item1 = buildItem("1", buildCalendar(2018, 1, 1, 1), OfflineItemFilter.VIDEO);
        item1.state = OfflineItemState.IN_PROGRESS;
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1));
        DateOrderedListMutator list = createMutatorWithJustNowProvider();

        Assert.assertEquals(2, mModel.size());
        assertJustNowSection(mModel.get(0));
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 1, 1), item1);
    }

    /**
     * Action                               List
     * 1. Set(item1 @ 1:00 1/1/2018         [ DATE    Just Now,
     *              Video IN_PROGRESS,
     *        item2 @ 1:00 1/1/2018           item1   @ 1:00 1/1/2018,
     *              Audio COMPLETE Recent)
     *                                        item2   @ 1:00 1/1/2018 ]
     */
    @Test
    public void testMultipleSectionsInJustNowSection() {
        Calendar calendar = buildCalendar(2018, 1, 1, 1);
        OfflineItem item1 = buildItem("1", calendar, OfflineItemFilter.VIDEO);
        OfflineItem item2 = buildItem("2", calendar, OfflineItemFilter.AUDIO);
        item1.state = OfflineItemState.IN_PROGRESS;
        item2.state = OfflineItemState.COMPLETE;
        item2.completionTimeMs = item2.creationTimeMs;
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1, item2));
        DateOrderedListMutator list =
                createMutatorWithJustNowProvider(buildJustNowProvider(calendar.getTime()));

        Assert.assertEquals(3, mModel.size());
        assertJustNowSection(mModel.get(0));
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 1, 1), item1);
        assertOfflineItem(mModel.get(2), buildCalendar(2018, 1, 1, 1), item2);
    }

    /**
     * Action                               List
     * 1. Set(item1 @ 0:10 1/2/2018         [ DATE    Just Now,
     *              Video COMPLETE,
     *        item2 @ 23:55 1/1/2018         [ DATE    Just Now,
     *              Video COMPLETE,
     *
     *        item3 @ 10:00 1/1/2018           DATE    1/1/2018
     *              Audio COMPLETE)           item3   @ 1:00 1/1/2018 ]
     */
    @Test
    public void testRecentItemBeforeMidnightShowsInJustNowSection() {
        Calendar calendar1 = CalendarFactory.get();
        calendar1.set(2018, 1, 2, 0, 10);
        Calendar calendar2 = CalendarFactory.get();
        calendar2.set(2018, 1, 1, 23, 50);
        OfflineItem item1 = buildItem("1", calendar1, OfflineItemFilter.VIDEO);
        OfflineItem item2 = buildItem("2", calendar2, OfflineItemFilter.AUDIO);
        OfflineItem item3 = buildItem("3", buildCalendar(2018, 1, 1, 10), OfflineItemFilter.AUDIO);
        item1.completionTimeMs = item1.creationTimeMs;
        item2.completionTimeMs = item2.creationTimeMs;
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1, item2, item3));

        Calendar now = CalendarFactory.get();
        now.set(2018, 1, 2, 0, 15);
        createMutatorWithJustNowProvider(buildJustNowProvider(now.getTime()));
        Assert.assertEquals(5, mModel.size());
        assertJustNowSection(mModel.get(0));
        assertOfflineItem(mModel.get(1), calendar1, item1);
        assertOfflineItem(mModel.get(2), calendar2, item2);
        assertSectionHeader(mModel.get(3), buildCalendar(2018, 1, 1, 0), true);
        assertOfflineItem(mModel.get(4), buildCalendar(2018, 1, 1, 10), item3);
    }

    /**
     * Action                               List
     * 1. Set(item1 @ 1:00 1/1/2018         [ DATE    Just Now,
     *        PAUSED)
     *                                        item1   @ 1:00 1/1/2018 ]
     * 2. Update(item1 @ 1:00 1/1/2018      [ DATE    Just Now,
     *        Resume --> IN_PROGRESS)
     *                                        item1   @ 1:00 1/1/2018 ]
     * 3. Update(item1 @ 1:00 1/1/2018      [ DATE    Just Now,
     *       COMPLETE, completion time now)
     *                                        item1   @ 1:00 1/1/2018 ]
     * 4. Update(item1 @ 1:00 1/1/2018      [ DATE    Just Now,
     *    COMPLETE, completion time 1/1/2017)
     *                                        item1   @ 1:00 1/1/2018 ]
     */
    @Test
    public void testItemDoesNotMoveOutOfJustNowSection() {
        OfflineItem item1 = buildItem("1", buildCalendar(2018, 1, 1, 1), OfflineItemFilter.VIDEO);
        item1.state = OfflineItemState.PAUSED;
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1));
        DateOrderedListMutator list = createMutatorWithJustNowProvider();
        mModel.addObserver(mObserver);

        Assert.assertEquals(2, mModel.size());
        assertJustNowSection(mModel.get(0));
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 1, 1), item1);

        // Resume the download.
        OfflineItem update1 = buildItem("1", buildCalendar(2018, 1, 1, 1), OfflineItemFilter.VIDEO);
        update1.state = OfflineItemState.IN_PROGRESS;
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(update1));
        list.onItemUpdated(item1, update1);

        Assert.assertEquals(2, mModel.size());
        assertJustNowSection(mModel.get(0));
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 1, 1), update1);

        // Complete the download.
        OfflineItem update2 = buildItem("1", buildCalendar(2018, 1, 1, 1), OfflineItemFilter.VIDEO);
        update2.state = OfflineItemState.COMPLETE;
        update2.completionTimeMs = new Date().getTime();
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(update2));
        list.onItemUpdated(update1, update2);

        Assert.assertEquals(2, mModel.size());
        assertJustNowSection(mModel.get(0));
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 1, 1), update2);

        // Too much time has passed since completion of the download.
        OfflineItem update3 = buildItem("1", buildCalendar(2018, 1, 1, 1), OfflineItemFilter.VIDEO);
        update3.state = OfflineItemState.COMPLETE;
        update3.completionTimeMs = buildCalendar(2017, 1, 1, 1).getTimeInMillis();
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(update3));
        list.onItemUpdated(update2, update3);

        Assert.assertEquals(2, mModel.size());
        assertJustNowSection(mModel.get(0));
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 1, 1), update3);
    }

    /**
     * Action                               List
     * 1. Set(item1 @ 1:00 2/1/2018         [ DATE    Just Now,
     *              Video IN_PROGRESS,
     *        item2 @ 1:00 1/1/2018           item1   @ 1:00 2/1/2018,
     *              Audio COMPLETE)           DATE    1/1/2018
     *                                        item2   @ 1:00 1/1/2018 ]
     */
    @Test
    public void testJustNowSectionWithOtherDates() {
        OfflineItem item1 = buildItem("1", buildCalendar(2018, 2, 1, 1), OfflineItemFilter.VIDEO);
        OfflineItem item2 = buildItem("2", buildCalendar(2018, 1, 1, 1), OfflineItemFilter.AUDIO);
        item1.state = OfflineItemState.IN_PROGRESS;
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1, item2));
        DateOrderedListMutator list = createMutatorWithJustNowProvider();

        Assert.assertEquals(4, mModel.size());
        assertJustNowSection(mModel.get(0));
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 2, 1, 1), item1);
        assertSectionHeader(mModel.get(2), buildCalendar(2018, 1, 1, 0), true);
        assertOfflineItem(mModel.get(3), buildCalendar(2018, 1, 1, 1), item2);
    }

    /**
     * Action                               List
     * 1. Set(item1 @ 0:00 1/2/2018,        [ DATE    @ 0:00 1/2/2018,
     *        item2 @ 0:00 1/1/2018)
     *                                        item1   @ 0:00 1/2/2018,
     *                                        DATE  @ 0:00 1/1/2018,
     *
     *                                        item2   @ 0:00 1/1/2018 ]
     */
    @Test
    public void testTwoItemsDifferentDayMatchHeader() {
        OfflineItem item1 = buildItem("1", buildCalendar(2018, 1, 2, 0), OfflineItemFilter.VIDEO);
        OfflineItem item2 = buildItem("2", buildCalendar(2018, 1, 1, 0), OfflineItemFilter.AUDIO);
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1, item2));
        DateOrderedListMutator list = createMutatorWithoutJustNowProvider();

        Assert.assertEquals(4, mModel.size());
        assertSectionHeader(mModel.get(0), buildCalendar(2018, 1, 2, 0), false);
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 2, 0), item1);
        assertSectionHeader(mModel.get(2), buildCalendar(2018, 1, 1, 0), true);
        assertOfflineItem(mModel.get(3), buildCalendar(2018, 1, 1, 0), item2);
    }

    /**
     * Action                               List
     * 1. Set(item1 @ 4:00 1/1/2018,        [ DATE    @ 0:00 1/1/2018,
     *        item2 @ 5:00 1/1/2018)
     *                                        item2   @ 5:00 1/1/2018,
     *                                        item1   @ 4:00 1/1/2018 ]
     */
    @Test
    public void testTwoItemsSameDayOutOfOrder() {
        OfflineItem item1 = buildItem("1", buildCalendar(2018, 1, 1, 4), OfflineItemFilter.VIDEO);
        OfflineItem item2 = buildItem("2", buildCalendar(2018, 1, 1, 5), OfflineItemFilter.VIDEO);
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1, item2));
        DateOrderedListMutator list = createMutatorWithoutJustNowProvider();

        Assert.assertEquals(3, mModel.size());
        assertSectionHeader(mModel.get(0), buildCalendar(2018, 1, 1, 0), false);
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 1, 5), item2);
        assertOfflineItem(mModel.get(2), buildCalendar(2018, 1, 1, 4), item1);
    }

    /**
     * Action                                      List
     * 1. Set(item1 @ 4:00 1/2/2018 Video,         [ DATE      @ 0:00 1/2/2018,
     *        item2 @ 5:00 1/1/2018 Video)
     *                                               item2     @ 4:00 1/2/2018,
     *                                               DATE      @ 0:00 1/1/2018,
     *                                               item1     @ 5:00 1/1/2018 ]
     */
    @Test
    public void testTwoItemsDifferentDaySameSection() {
        OfflineItem item1 = buildItem("1", buildCalendar(2018, 1, 2, 4), OfflineItemFilter.VIDEO);
        OfflineItem item2 = buildItem("2", buildCalendar(2018, 1, 1, 5), OfflineItemFilter.VIDEO);
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1, item2));
        DateOrderedListMutator list = createMutatorWithoutJustNowProvider();

        Assert.assertEquals(4, mModel.size());
        assertSectionHeader(mModel.get(0), buildCalendar(2018, 1, 2, 0), false);
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 2, 4), item1);
        assertSectionHeader(mModel.get(2), buildCalendar(2018, 1, 1, 0), true);
        assertOfflineItem(mModel.get(3), buildCalendar(2018, 1, 1, 5), item2);
    }

    /**
     * Action                                      List
     * 1. Set(item1 @ 4:00 1/2/2018 Video,         [ DATE      @ 0:00 1/2/2018,
     *        item2 @ 5:00 1/1/2018 Page )
     *                                               item2     @ 4:00 1/2/2018,
     *                                               DATE      @ 0:00 1/1/2018,
     *                                               item1     @ 5:00 1/1/2018 ]
     */
    @Test
    public void testTwoItemsDifferentDayDifferentSection() {
        OfflineItem item1 = buildItem("1", buildCalendar(2018, 1, 2, 4), OfflineItemFilter.VIDEO);
        OfflineItem item2 = buildItem("2", buildCalendar(2018, 1, 1, 5), OfflineItemFilter.PAGE);
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1, item2));
        DateOrderedListMutator list = createMutatorWithoutJustNowProvider();

        Assert.assertEquals(4, mModel.size());
        assertSectionHeader(mModel.get(0), buildCalendar(2018, 1, 2, 0), false);
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 2, 4), item1);
        assertSectionHeader(mModel.get(2), buildCalendar(2018, 1, 1, 0), true);
        assertOfflineItem(mModel.get(3), buildCalendar(2018, 1, 1, 5), item2);
    }

    /**
     * Action                               List
     * 1. Set(item1 @ 4:00 1/1/2018,        [ DATE   @ 0:00 1/2/2018,
     *        item2 @ 3:00 1/2/2018)
     *                                        item2  @ 3:00 1/2/2018,
     *                                        DATE   @ 0:00 1/1/2018,
     *                                        item1  @ 4:00 1/1/2018 ]
     */
    @Test
    public void testTwoItemsDifferentDayOutOfOrder() {
        OfflineItem item1 = buildItem("1", buildCalendar(2018, 1, 1, 4), OfflineItemFilter.VIDEO);
        OfflineItem item2 = buildItem("2", buildCalendar(2018, 1, 2, 3), OfflineItemFilter.VIDEO);
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1, item2));
        DateOrderedListMutator list = createMutatorWithoutJustNowProvider();

        Assert.assertEquals(4, mModel.size());
        assertSectionHeader(mModel.get(0), buildCalendar(2018, 1, 2, 0), false);
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 2, 3), item2);
        assertSectionHeader(mModel.get(2), buildCalendar(2018, 1, 1, 0), true);
        assertOfflineItem(mModel.get(3), buildCalendar(2018, 1, 1, 4), item1);
    }

    /**
     * Action                               List
     * 1. Set()                             [ ]
     *
     * 2. Add(item1 @ 4:00 1/1/2018)        [ DATE    @ 0:00 1/1/2018,
     *                                        item1  @ 4:00 1/1/2018 ]
     */
    @Test
    public void testAddItemToEmptyList() {
        when(mSource.getItems()).thenReturn(Collections.emptySet());
        DateOrderedListMutator list = createMutatorWithoutJustNowProvider();
        mModel.addObserver(mObserver);

        OfflineItem item1 = buildItem("1", buildCalendar(2018, 1, 1, 4), OfflineItemFilter.VIDEO);
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1));
        list.onItemsAdded(CollectionUtil.newArrayList(item1));

        Assert.assertEquals(2, mModel.size());
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 1, 4), item1);
    }

    /**
     * Action                               List
     * 1. Set(item1 @ 1:00 1/2/2018)        [ DATE    @ 0:00 1/2/2018,
     *                                        item1  @ 1:00 1/2/2018 ]
     * 2. Add(item2 @ 2:00 1/2/2018)        [ DATE    @ 0:00 1/2/2018,
     *                                        item2  @ 2:00 1/2/2018
     *                                        item1  @ 1:00 1/2/2018 ]
     * 3. Add(item3 @ 2:00 1/3/2018)        [ DATE    @ 0:00 1/3/2018,
     *                                        item3  @ 2:00 1/3/2018
     *                                        DATE    @ 0:00 1/2/2018,
     *                                        item2  @ 2:00 1/2/2018
     *                                        item1  @ 1:00 1/2/2018 ]
     */
    @Test
    public void testAddFirstItemToList() {
        OfflineItem item1 = buildItem("1", buildCalendar(2018, 1, 2, 1), OfflineItemFilter.VIDEO);
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1));
        DateOrderedListMutator list = createMutatorWithoutJustNowProvider();
        mModel.addObserver(mObserver);

        // Add an item on the same day that will be placed first.
        OfflineItem item2 = buildItem("2", buildCalendar(2018, 1, 2, 2), OfflineItemFilter.VIDEO);
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1, item2));
        list.onItemsAdded(CollectionUtil.newArrayList(item2));

        Assert.assertEquals(3, mModel.size());
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 2, 2), item2);
        assertOfflineItem(mModel.get(2), buildCalendar(2018, 1, 2, 1), item1);

        // Add an item on an earlier day that will be placed first.
        OfflineItem item3 = buildItem("3", buildCalendar(2018, 1, 3, 2), OfflineItemFilter.VIDEO);
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1, item2, item3));
        list.onItemsAdded(CollectionUtil.newArrayList(item3));

        Assert.assertEquals(5, mModel.size());
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 3, 2), item3);
        assertOfflineItem(mModel.get(3), buildCalendar(2018, 1, 2, 2), item2);
        assertOfflineItem(mModel.get(4), buildCalendar(2018, 1, 2, 1), item1);
    }

    /**
     * Action                               List
     * 1. Set(item1 @ 4:00 1/2/2018)        [ DATE    @ 0:00 1/2/2018,
     *                                        item1  @ 4:00 1/2/2018 ]
     *
     * 2. Add(item2 @ 3:00 1/2/2018)        [ DATE    @ 0:00 1/2/2018,
     *                                        item1  @ 4:00 1/2/2018
     *                                        item2  @ 3:00 1/2/2018 ]
     *
     * 3. Add(item3 @ 4:00 1/1/2018)        [ DATE    @ 0:00 1/2/2018,
     *                                        item1  @ 4:00 1/2/2018
     *                                        item2  @ 3:00 1/2/2018,
     *                                        DATE    @ 0:00 1/1/2018,
     *                                        item3  @ 4:00 1/1/2018
     */
    @Test
    public void testAddLastItemToList() {
        OfflineItem item1 = buildItem("1", buildCalendar(2018, 1, 2, 4), OfflineItemFilter.VIDEO);
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1));
        DateOrderedListMutator list = createMutatorWithoutJustNowProvider();
        mModel.addObserver(mObserver);

        // Add an item on the same day that will be placed last.
        OfflineItem item2 = buildItem("2", buildCalendar(2018, 1, 2, 3), OfflineItemFilter.VIDEO);
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1, item2));
        list.onItemsAdded(CollectionUtil.newArrayList(item2));

        Assert.assertEquals(3, mModel.size());
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 2, 4), item1);
        assertOfflineItem(mModel.get(2), buildCalendar(2018, 1, 2, 3), item2);

        // Add an item on a later day that will be placed last.
        OfflineItem item3 = buildItem("3", buildCalendar(2018, 1, 1, 4), OfflineItemFilter.VIDEO);
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1, item2, item3));
        list.onItemsAdded(CollectionUtil.newArrayList(item3));

        Assert.assertEquals(5, mModel.size());
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 2, 4), item1);
        assertOfflineItem(mModel.get(2), buildCalendar(2018, 1, 2, 3), item2);
        assertOfflineItem(mModel.get(4), buildCalendar(2018, 1, 1, 4), item3);
    }

    /**
     * Action                               List
     * 1. Set(item1 @ 2:00 1/2/2018)        [ DATE    @ 0:00 1/2/2018,
     *
     *                                        item1  @ 2:00 1/2/2018 ]
     *
     * 2. Remove(item1)                     [ ]
     */
    @Test
    public void testRemoveOnlyItemInList() {
        OfflineItem item1 = buildItem("1", buildCalendar(2018, 1, 2, 2), OfflineItemFilter.VIDEO);
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1));
        DateOrderedListMutator list = createMutatorWithoutJustNowProvider();
        mModel.addObserver(mObserver);

        when(mSource.getItems()).thenReturn(Collections.emptySet());
        list.onItemsRemoved(CollectionUtil.newArrayList(item1));

        Assert.assertEquals(0, mModel.size());
    }

    /**
     * Action                               List
     * 1. Set(item1 @ 3:00 1/2/2018,        [ DATE    @ 0:00 1/2/2018,
     *        item2 @ 2:00 1/2/2018)
     *                                        item1  @ 3:00 1/2/2018,
     *                                        item2  @ 2:00 1/2/2018 ]
     *
     * 2. Remove(item1)                     [ DATE    @ 0:00 1/2/2018,
     *                                        item2  @ 2:00 1/2/2018 ]
     */
    @Test
    public void testRemoveFirstItemInListSameDay() {
        OfflineItem item1 = buildItem("1", buildCalendar(2018, 1, 2, 3), OfflineItemFilter.VIDEO);
        OfflineItem item2 = buildItem("2", buildCalendar(2018, 1, 2, 2), OfflineItemFilter.VIDEO);
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1, item2));
        DateOrderedListMutator list = createMutatorWithoutJustNowProvider();
        mModel.addObserver(mObserver);

        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item2));
        list.onItemsRemoved(CollectionUtil.newArrayList(item1));

        Assert.assertEquals(2, mModel.size());
        assertSectionHeader(mModel.get(0), buildCalendar(2018, 1, 2, 0), false);
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 2, 2), item2);
    }

    /**
     * Action                               List
     * 1. Set(item1 @ 3:00 1/2/2018,        [ DATE    @ 0:00 1/2/2018,
     *        item2 @ 2:00 1/2/2018)
     *                                        item1  @ 3:00 1/2/2018,
     *                                        item2  @ 2:00 1/2/2018 ]
     *
     * 2. Remove(item2)                     [ DATE    @ 0:00 1/2/2018,
     *                                        item1  @ 3:00 1/2/2018 ]
     */
    @Test
    public void testRemoveLastItemInListSameDay() {
        OfflineItem item1 = buildItem("1", buildCalendar(2018, 1, 2, 3), OfflineItemFilter.VIDEO);
        OfflineItem item2 = buildItem("2", buildCalendar(2018, 1, 2, 2), OfflineItemFilter.VIDEO);
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1, item2));
        DateOrderedListMutator list = createMutatorWithoutJustNowProvider();
        mModel.addObserver(mObserver);

        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1));
        list.onItemsRemoved(CollectionUtil.newArrayList(item2));

        Assert.assertEquals(2, mModel.size());
        assertSectionHeader(mModel.get(0), buildCalendar(2018, 1, 2, 0), false);
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 2, 3), item1);
    }

    /**
     * Action                               List
     * 1. Set(item1 @ 3:00 1/3/2018,        [ DATE    @ 0:00 1/3/2018,
     *        item2 @ 2:00 1/2/2018)
     *                                        item1  @ 3:00 1/3/2018,
     *                                        DATE    @ 0:00 1/2/2018,
     *                                        item2  @ 2:00 1/2/2018 ]
     *
     * 2. Remove(item2)                     [ DATE    @ 0:00 1/3/2018,
     *                                        item1  @ 3:00 1/3/2018 ]
     */
    @Test
    public void testRemoveLastItemInListWithMultipleDays() {
        OfflineItem item1 = buildItem("1", buildCalendar(2018, 1, 3, 3), OfflineItemFilter.VIDEO);
        OfflineItem item2 = buildItem("2", buildCalendar(2018, 1, 2, 2), OfflineItemFilter.VIDEO);
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1, item2));
        DateOrderedListMutator list = createMutatorWithoutJustNowProvider();
        mModel.addObserver(mObserver);

        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1));
        list.onItemsRemoved(CollectionUtil.newArrayList(item2));

        Assert.assertEquals(2, mModel.size());
        assertSectionHeader(mModel.get(0), buildCalendar(2018, 1, 3, 0), false);
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 3, 3), item1);
    }

    /**
     * Action                               List
     * 1. Set()                             [ ]
     *
     * 2. Add(item1 @ 6:00  1/1/2018,       [ DATE    @ 0:00  1/2/2018,
     *        item2 @ 4:00  1/1/2018,
     *        item3 @ 10:00 1/2/2018,         item4  @ 12:00 1/2/2018,
     *        item4 @ 12:00 1/2/2018)         item3  @ 10:00 1/2/2018
     *                                        DATE    @ 0:00  1/1/2018,
     *
     *                                        item1  @ 6:00  1/1/2018,
     *                                        item2  @ 4:00  1/1/2018 ]
     */
    @Test
    public void testAddMultipleItems() {
        when(mSource.getItems()).thenReturn(Collections.emptySet());
        DateOrderedListMutator list = createMutatorWithoutJustNowProvider();
        mModel.addObserver(mObserver);

        OfflineItem item1 = buildItem("1", buildCalendar(2018, 1, 1, 6), OfflineItemFilter.VIDEO);
        OfflineItem item2 = buildItem("2", buildCalendar(2018, 1, 1, 4), OfflineItemFilter.VIDEO);
        OfflineItem item3 = buildItem("3", buildCalendar(2018, 1, 2, 10), OfflineItemFilter.VIDEO);
        OfflineItem item4 = buildItem("4", buildCalendar(2018, 1, 2, 12), OfflineItemFilter.VIDEO);

        when(mSource.getItems())
                .thenReturn(CollectionUtil.newArrayList(item1, item2, item3, item4));
        list.onItemsAdded(CollectionUtil.newArrayList(item1, item2, item3, item4));

        Assert.assertEquals(6, mModel.size());
        assertSectionHeader(mModel.get(0), buildCalendar(2018, 1, 2, 0), false);
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 2, 12), item4);
        assertOfflineItem(mModel.get(2), buildCalendar(2018, 1, 2, 10), item3);
        assertSectionHeader(mModel.get(3), buildCalendar(2018, 1, 1, 0), true);
        assertOfflineItem(mModel.get(4), buildCalendar(2018, 1, 1, 6), item1);
        assertOfflineItem(mModel.get(5), buildCalendar(2018, 1, 1, 4), item2);
    }

    /**
     * Action                               List
     * 1. Set()                             [ ]
     *
     * 2. Add(item3 @ 4:00 1/1/2018)        [ DATE    @ 0:00 1/1/2018,
     *                                        item3  @ 4:00 1/1/2018 ]
     *
     * 3. Add(item1 @ 4:00 1/1/2018)        [ DATE    @ 0:00 1/1/2018,
     *                                        item1  @ 4:00 1/1/2018,
     *                                        item3  @ 4:00 1/1/2018 ]
     *
     * 4. Add(item2 @ 4:00 1/1/2018)        [ DATE    @ 0:00 1/1/2018,
     *                                        item1  @ 4:00 1/1/2018,
     *                                        item2  @ 4:00 1/1/2018,
     *                                        item3  @ 4:00 1/1/2018 ]
     */
    @Test
    public void testAddMultipleItemsSameTimestamp() {
        when(mSource.getItems()).thenReturn(Collections.emptySet());
        DateOrderedListMutator list = createMutatorWithoutJustNowProvider();
        mModel.addObserver(mObserver);

        OfflineItem item3 = buildItem("3", buildCalendar(2018, 1, 1, 4), OfflineItemFilter.VIDEO);
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item3));
        list.onItemsAdded(CollectionUtil.newArrayList(item3));

        Assert.assertEquals(2, mModel.size());
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 1, 4), item3);

        OfflineItem item1 = buildItem("1", buildCalendar(2018, 1, 1, 4), OfflineItemFilter.VIDEO);
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item3, item1));
        list.onItemsAdded(CollectionUtil.newArrayList(item1));

        Assert.assertEquals(3, mModel.size());
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 1, 4), item1);
        assertOfflineItem(mModel.get(2), buildCalendar(2018, 1, 1, 4), item3);

        OfflineItem item2 = buildItem("2", buildCalendar(2018, 1, 1, 4), OfflineItemFilter.VIDEO);
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item3, item1, item2));
        list.onItemsAdded(CollectionUtil.newArrayList(item2));

        Assert.assertEquals(4, mModel.size());
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 1, 4), item1);
        assertOfflineItem(mModel.get(2), buildCalendar(2018, 1, 1, 4), item2);
        assertOfflineItem(mModel.get(3), buildCalendar(2018, 1, 1, 4), item3);
    }

    /**
     * Action                                          List
     * 1. Set(item1 @ 6:00 IN_PROGRESS 1/1/2018)       [ DATE    @ 0:00 1/1/2018,
     *
     *                                                   item1  @ 3:00 1/1/2018 IN_PROGRESS]
     *
     * 2. Update(item1 @ 6:00 COMPLETE 1/1/2018)
     *
     * 3. Add(item2 @ 4:00 IN_PROGRESS 1/1/2018)       [
     *                                                   DATE    @ 0:00  1/1/2018,
     *                                                   item1  @ 6:00  1/1/2018 COMPLETE,
     *                                                   item2  @ 4:00  1/1/2018 IN_PROGRESS]
     */
    @Test
    public void testFirstItemUpdatedAfterSecondItemAdded() {
        when(mSource.getItems()).thenReturn(Collections.emptySet());
        DateOrderedListMutator list = createMutatorWithoutJustNowProvider();
        mModel.addObserver(mObserver);

        OfflineItem item1 = buildItem("1", buildCalendar(2018, 1, 1, 6), OfflineItemFilter.VIDEO);

        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1));
        list.onItemsAdded(CollectionUtil.newArrayList(item1));

        Assert.assertEquals(2, mModel.size());
        assertSectionHeader(mModel.get(0), buildCalendar(2018, 1, 1, 0), false);
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 1, 6), item1);

        // Complete the download.
        OfflineItem update1 = buildItem("1", buildCalendar(2018, 1, 1, 6), OfflineItemFilter.VIDEO);
        update1.state = OfflineItemState.COMPLETE;
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(update1));
        list.onItemUpdated(item1, update1);

        // Add a new download.
        OfflineItem item2 = buildItem("2", buildCalendar(2018, 1, 1, 4), OfflineItemFilter.VIDEO);
        list.onItemsAdded(CollectionUtil.newArrayList(item2));

        Assert.assertEquals(3, mModel.size());
        assertSectionHeader(mModel.get(0), buildCalendar(2018, 1, 1, 0), false);
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 1, 6), update1);
        assertOfflineItem(mModel.get(2), buildCalendar(2018, 1, 1, 4), item2);
    }

    /**
     * Action                               List
     * 2. Set(item1 @ 6:00  1/1/2018,       [ DATE    @ 0:00  1/2/2018,
     *        item2 @ 4:00  1/1/2018,
     *        item3 @ 10:00 1/2/2018,         item4  @ 12:00 1/2/2018,
     *        item4 @ 12:00 1/2/2018)         item3  @ 10:00 1/2/2018
     *                                        DATE    @ 0:00  1/1/2018,
     *
     *                                        item1  @ 6:00  1/1/2018,
     *                                        item2  @ 4:00  1/1/2018 ]
     *
     * 2. Remove(item2,                     [ DATE    @ 0:00  1/1/2018,
     *           item3,
     *           item4)                       item1  @ 6:00  1/1/2018 ]
     */
    @Test
    public void testRemoveMultipleItems() {
        OfflineItem item1 = buildItem("1", buildCalendar(2018, 1, 1, 6), OfflineItemFilter.VIDEO);
        OfflineItem item2 = buildItem("2", buildCalendar(2018, 1, 1, 4), OfflineItemFilter.VIDEO);
        OfflineItem item3 = buildItem("3", buildCalendar(2018, 1, 2, 10), OfflineItemFilter.VIDEO);
        OfflineItem item4 = buildItem("4", buildCalendar(2018, 1, 2, 12), OfflineItemFilter.VIDEO);

        when(mSource.getItems())
                .thenReturn(CollectionUtil.newArrayList(item1, item2, item3, item4));
        DateOrderedListMutator list = createMutatorWithoutJustNowProvider();
        mModel.addObserver(mObserver);

        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1));
        list.onItemsRemoved(CollectionUtil.newArrayList(item2, item3, item4));

        Assert.assertEquals(2, mModel.size());
        assertSectionHeader(mModel.get(0), buildCalendar(2018, 1, 1, 0), false);
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 1, 6), item1);
    }

    /**
     * Action                               List
     * 1. Set(item1 @ 4:00 1/1/2018)        [ DATE      @ 0:00  1/1/2018,
     *
     *                                        item1     @ 4:00  1/1/2018 ]
     *
     * 2. Update (item1,
     *            newItem1 @ 4:00 1/1/2018) [ DATE      @ 0:00  1/1/2018,
     *
     *                                        newItem1  @ 4:00  1/1/2018 ]
     */
    @Test
    public void testItemUpdatedSameTimestamp() {
        OfflineItem item1 = buildItem("1", buildCalendar(2018, 1, 1, 4), OfflineItemFilter.VIDEO);

        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1));
        DateOrderedListMutator list = createMutatorWithoutJustNowProvider();
        mModel.addObserver(mObserver);

        // Update an item with the same timestamp.
        OfflineItem newItem1 =
                buildItem("1", buildCalendar(2018, 1, 1, 4), OfflineItemFilter.VIDEO);
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(newItem1));
        list.onItemUpdated(item1, newItem1);

        Assert.assertEquals(2, mModel.size());
        assertSectionHeader(mModel.get(0), buildCalendar(2018, 1, 1, 0), false);
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 1, 4), newItem1);
    }

    /**
     * Action                               List
     * 1. Set(item1 @ 5:00 1/1/2018,        [ DATE      @ 0:00  1/1/2018,
     *        item2 @ 4:00 1/1/2018)
     *                                        item1     @ 5:00  1/1/2018,
     *                                        item2     @ 4:00  1/1/2018
     * 2. Update (item1,
     *            newItem1 @ 3:00 1/1/2018) [ DATE      @ 0:00  1/1/2018,
     *
     *                                        item2     @ 4:00  1/1/2018,
     *                                        newItem1  @ 3:00  1/1/2018 ]
     */
    @Test
    public void testItemUpdatedSameDay() {
        OfflineItem item1 = buildItem("1", buildCalendar(2018, 1, 1, 5), OfflineItemFilter.VIDEO);
        OfflineItem item2 = buildItem("2", buildCalendar(2018, 1, 1, 4), OfflineItemFilter.VIDEO);

        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1, item2));
        DateOrderedListMutator list = createMutatorWithoutJustNowProvider();
        mModel.addObserver(mObserver);

        // Update an item with the same timestamp.
        OfflineItem newItem1 =
                buildItem("1", buildCalendar(2018, 1, 1, 3), OfflineItemFilter.VIDEO);
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(newItem1, item2));
        list.onItemUpdated(item1, newItem1);

        Assert.assertEquals(3, mModel.size());
        assertSectionHeader(mModel.get(0), buildCalendar(2018, 1, 1, 0), false);
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 1, 4), item2);
        assertOfflineItem(mModel.get(2), buildCalendar(2018, 1, 1, 3), newItem1);
    }

    /**
     * Action                                   List
     * 1. Set(item1 @ 5:00 1/1/2018,            [ DATE      @ 0:00  1/1/2018,
     *        item2 @ 4:00 1/1/2018)
     *                                            item1     @ 5:00  1/1/2018,
     *                                            item2     @ 4:00  1/1/2018
     * 2. Update (item1,
     *            newItem1 @ 3:00 1/1/2018 Image) [ DATE      @ 0:00  1/1/2018,
     *
     *                                              item2     @ 4:00  1/1/2018,
     *                                              newItem1  @ 3:00  1/1/2018 ]
     */
    @Test
    public void testItemUpdatedSameDayDifferentSection() {
        OfflineItem item1 = buildItem("1", buildCalendar(2018, 1, 1, 5), OfflineItemFilter.VIDEO);
        OfflineItem item2 = buildItem("2", buildCalendar(2018, 1, 1, 4), OfflineItemFilter.VIDEO);

        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1, item2));
        DateOrderedListMutator list = createMutatorWithoutJustNowProvider();
        mModel.addObserver(mObserver);

        // Update an item with the same timestamp.
        OfflineItem newItem1 =
                buildItem("1", buildCalendar(2018, 1, 1, 3), OfflineItemFilter.IMAGE);
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(newItem1, item2));
        list.onItemUpdated(item1, newItem1);

        Assert.assertEquals(3, mModel.size());
        assertSectionHeader(mModel.get(0), buildCalendar(2018, 1, 1, 0), false);
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 1, 4), item2);
        assertOfflineItem(mModel.get(2), buildCalendar(2018, 1, 1, 3), newItem1);
    }

    /**
     * Action                               List
     * 1. Set(item1 @ 4:00 1/1/2018)        [ DATE      @ 0:00  1/1/2018,
     *                                        item1     @ 4:00  1/1/2018 ]
     *
     * 2. Update (item1,
     *            newItem1 @ 6:00 1/2/2018) [ DATE      @ 0:00  1/2/2018,
     *                                        newItem1  @ 6:00  1/2/2018 ]
     */
    @Test
    public void testItemUpdatedDifferentDay() {
        OfflineItem item1 = buildItem("1", buildCalendar(2018, 1, 1, 4), OfflineItemFilter.VIDEO);

        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(item1));
        DateOrderedListMutator list = createMutatorWithoutJustNowProvider();
        mModel.addObserver(mObserver);

        // Update an item with the same timestamp.
        OfflineItem newItem1 =
                buildItem("1", buildCalendar(2018, 1, 2, 6), OfflineItemFilter.VIDEO);
        when(mSource.getItems()).thenReturn(CollectionUtil.newArrayList(newItem1));
        list.onItemUpdated(item1, newItem1);

        Assert.assertEquals(2, mModel.size());
        assertSectionHeader(mModel.get(0), buildCalendar(2018, 1, 2, 0), false);
        assertOfflineItem(mModel.get(1), buildCalendar(2018, 1, 2, 6), newItem1);
    }

    private static Calendar buildCalendar(int year, int month, int dayOfMonth, int hourOfDay) {
        Calendar calendar = CalendarFactory.get();
        calendar.set(year, month, dayOfMonth, hourOfDay, 0);
        return calendar;
    }

    private static OfflineItem buildItem(
            String id, Calendar calendar, @OfflineItemFilter int filter) {
        OfflineItem item = new OfflineItem();
        item.id.namespace = "test";
        item.id.id = id;
        item.creationTimeMs = calendar.getTimeInMillis();
        item.filter = filter;
        return item;
    }

    private DownloadManagerUiConfig createConfig() {
        return new DownloadManagerUiConfig.Builder()
                .setUseNewDownloadPath(true)
                .setSupportsGrouping(false)
                .build();
    }

    private JustNowProvider buildJustNowProvider(Date overrideNow) {
        JustNowProvider justNowProvider = new JustNowProvider(createConfig()) {
            @Override
            protected Date now() {
                return overrideNow;
            }
        };
        return justNowProvider;
    }

    private DateOrderedListMutator createMutatorWithoutJustNowProvider() {
        DownloadManagerUiConfig config = createConfig();
        JustNowProvider justNowProvider = new JustNowProvider(config) {
            @Override
            public boolean isJustNowItem(OfflineItem item) {
                return false;
            }
        };
        DateOrderedListMutator mutator =
                new DateOrderedListMutator(mSource, mModel, justNowProvider);
        new ListMutationController(config, justNowProvider, mutator, mModel);
        return mutator;
    }

    private DateOrderedListMutator createMutatorWithJustNowProvider() {
        JustNowProvider justNowProvider = new JustNowProvider(createConfig());
        return createMutatorWithJustNowProvider(justNowProvider);
    }

    private DateOrderedListMutator createMutatorWithJustNowProvider(
            JustNowProvider justNowProvider) {
        DateOrderedListMutator mutator =
                new DateOrderedListMutator(mSource, mModel, justNowProvider);
        new ListMutationController(createConfig(), justNowProvider, mutator, mModel);
        return mutator;
    }

    private static void assertDatesAreEqual(Date date, Calendar calendar) {
        Calendar calendar2 = CalendarFactory.get();
        calendar2.setTime(date);
        Assert.assertEquals(calendar.getTimeInMillis(), calendar2.getTimeInMillis());
    }

    private static void assertOfflineItem(
            ListItem item, Calendar calendar, OfflineItem offlineItem) {
        Assert.assertTrue(item instanceof OfflineItemListItem);
        assertDatesAreEqual(((OfflineItemListItem) item).date, calendar);
        Assert.assertEquals(OfflineItemListItem.generateStableId(offlineItem), item.stableId);
        Assert.assertEquals(offlineItem, ((OfflineItemListItem) item).item);
    }

    private static void assertSectionHeader(ListItem item, Calendar calendar, boolean showDivider) {
        Assert.assertTrue(item instanceof SectionHeaderListItem);
        SectionHeaderListItem sectionHeader = (SectionHeaderListItem) item;
        assertDatesAreEqual(sectionHeader.date, calendar);
        Assert.assertEquals(
                SectionHeaderListItem.generateStableId(calendar.getTimeInMillis()), item.stableId);
        Assert.assertEquals(sectionHeader.showTopDivider, showDivider);
    }

    private static void assertJustNowSection(ListItem item) {
        Assert.assertTrue(item instanceof SectionHeaderListItem);
        SectionHeaderListItem sectionHeader = (SectionHeaderListItem) item;
        Assert.assertTrue(sectionHeader.isJustNow);
        Assert.assertEquals(false, sectionHeader.showTopDivider);
        Assert.assertEquals(StableIds.JUST_NOW_SECTION, item.stableId);
    }
}
