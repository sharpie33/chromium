// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.device.nfc;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.mockito.ArgumentMatchers.any;
import static org.mockito.ArgumentMatchers.anyInt;
import static org.mockito.ArgumentMatchers.anyString;
import static org.mockito.ArgumentMatchers.isNull;
import static org.mockito.ArgumentMatchers.nullable;
import static org.mockito.Mockito.doNothing;
import static org.mockito.Mockito.doReturn;
import static org.mockito.Mockito.doThrow;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.times;
import static org.mockito.Mockito.verify;

import android.app.Activity;
import android.content.Context;
import android.content.pm.PackageManager;
import android.nfc.FormatException;
import android.nfc.NfcAdapter;
import android.nfc.NfcAdapter.ReaderCallback;
import android.nfc.NfcManager;
import android.os.Bundle;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.ArgumentCaptor;
import org.mockito.Captor;
import org.mockito.Mock;
import org.mockito.MockitoAnnotations;
import org.robolectric.annotation.Config;

import org.chromium.base.ApiCompatibilityUtils;
import org.chromium.base.Callback;
import org.chromium.base.ContextUtils;
import org.chromium.base.test.util.Feature;
import org.chromium.device.mojom.NdefError;
import org.chromium.device.mojom.NdefErrorType;
import org.chromium.device.mojom.NdefMessage;
import org.chromium.device.mojom.NdefRecord;
import org.chromium.device.mojom.NdefRecordTypeCategory;
import org.chromium.device.mojom.NdefScanOptions;
import org.chromium.device.mojom.NdefWriteOptions;
import org.chromium.device.mojom.Nfc.CancelAllWatchesResponse;
import org.chromium.device.mojom.Nfc.CancelPushResponse;
import org.chromium.device.mojom.Nfc.CancelWatchResponse;
import org.chromium.device.mojom.Nfc.PushResponse;
import org.chromium.device.mojom.Nfc.WatchResponse;
import org.chromium.device.mojom.NfcClient;
import org.chromium.testing.local.LocalRobolectricTestRunner;

import java.io.IOException;
import java.io.UnsupportedEncodingException;
import java.nio.ByteBuffer;
import java.nio.charset.Charset;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

/**
 * Unit tests for NfcImpl and NdefMessageUtils classes.
 */
@RunWith(LocalRobolectricTestRunner.class)
@Config(manifest = Config.NONE)
public class NFCTest {
    private TestNfcDelegate mDelegate;
    private int mNextWatchId;
    @Mock
    private Context mContext;
    @Mock
    private NfcManager mNfcManager;
    @Mock
    private NfcAdapter mNfcAdapter;
    @Mock
    private Activity mActivity;
    @Mock
    private NfcClient mNfcClient;
    @Mock
    private NfcTagHandler mNfcTagHandler;
    @Captor
    private ArgumentCaptor<NdefError> mErrorCaptor;
    @Captor
    private ArgumentCaptor<int[]> mOnWatchCallbackCaptor;

    // Constants used for the test.
    private static final String DUMMY_EXTERNAL_TYPE = "abc.com:xyz";
    private static final String DUMMY_RECORD_ID = "https://www.example.com/ids/1";
    private static final String TEST_TEXT = "test";
    private static final String TEST_URL = "https://google.com";
    private static final String TEST_JSON = "{\"key1\":\"value1\",\"key2\":2}";
    private static final String TEXT_MIME = "text/plain";
    private static final String JSON_MIME = "application/json";
    private static final String OCTET_STREAM_MIME = "application/octet-stream";
    private static final String ENCODING_UTF8 = "utf-8";
    private static final String ENCODING_UTF16 = "utf-16";
    private static final String LANG_EN_US = "en-US";

    /**
     * Class that is used test NfcImpl implementation
     */
    private static class TestNfcImpl extends NfcImpl {
        public TestNfcImpl(Context context, NfcDelegate delegate) {
            super(0, delegate);
        }

        public void processPendingOperationsForTesting(NfcTagHandler handler) {
            super.processPendingOperations(handler);
        }
    }

    private static class TestNfcDelegate implements NfcDelegate {
        Activity mActivity;
        Callback<Activity> mCallback;

        public TestNfcDelegate(Activity activity) {
            mActivity = activity;
        }
        @Override
        public void trackActivityForHost(int hostId, Callback<Activity> callback) {
            mCallback = callback;
        }

        public void invokeCallback() {
            mCallback.onResult(mActivity);
        }

        @Override
        public void stopTrackingActivityForHost(int hostId) {}
    }

    @Before
    public void setUp() {
        MockitoAnnotations.initMocks(this);
        mDelegate = new TestNfcDelegate(mActivity);
        doReturn(mNfcManager).when(mContext).getSystemService(Context.NFC_SERVICE);
        doReturn(mNfcAdapter).when(mNfcManager).getDefaultAdapter();
        doReturn(true).when(mNfcAdapter).isEnabled();
        doReturn(PackageManager.PERMISSION_GRANTED)
                .when(mContext)
                .checkPermission(anyString(), anyInt(), anyInt());
        doNothing()
                .when(mNfcAdapter)
                .enableReaderMode(any(Activity.class), any(ReaderCallback.class), anyInt(),
                        (Bundle) isNull());
        doNothing().when(mNfcAdapter).disableReaderMode(any(Activity.class));
        // Tag handler overrides used to mock connected tag.
        doReturn(true).when(mNfcTagHandler).isConnected();
        doReturn(false).when(mNfcTagHandler).isTagOutOfRange();
        try {
            doNothing().when(mNfcTagHandler).connect();
            doNothing().when(mNfcTagHandler).write(any(android.nfc.NdefMessage.class));
            doReturn(createNdefMessageWithRecordId(DUMMY_RECORD_ID)).when(mNfcTagHandler).read();
            doNothing().when(mNfcTagHandler).close();
        } catch (IOException | FormatException e) {
        }
        ContextUtils.initApplicationContextForTests(mContext);
    }

    /**
     * Test that error with type NOT_SUPPORTED is returned if NFC is not supported.
     */
    @Test
    @Feature({"NFCTest"})
    public void testNFCNotSupported() {
        doReturn(null).when(mNfcManager).getDefaultAdapter();
        TestNfcImpl nfc = new TestNfcImpl(mContext, mDelegate);
        mDelegate.invokeCallback();
        CancelAllWatchesResponse mockCallback = mock(CancelAllWatchesResponse.class);
        nfc.cancelAllWatches(mockCallback);
        verify(mockCallback).call(mErrorCaptor.capture());
        assertEquals(NdefErrorType.NOT_SUPPORTED, mErrorCaptor.getValue().errorType);
    }

    /**
     * Test that error with type SECURITY is returned if permission to use NFC is not granted.
     */
    @Test
    @Feature({"NFCTest"})
    public void testNFCIsNotPermitted() {
        doReturn(PackageManager.PERMISSION_DENIED)
                .when(mContext)
                .checkPermission(anyString(), anyInt(), anyInt());
        TestNfcImpl nfc = new TestNfcImpl(mContext, mDelegate);
        CancelAllWatchesResponse mockCallback = mock(CancelAllWatchesResponse.class);
        nfc.cancelAllWatches(mockCallback);
        verify(mockCallback).call(mErrorCaptor.capture());
        assertEquals(NdefErrorType.NOT_ALLOWED, mErrorCaptor.getValue().errorType);
    }

    /**
     * Test that method can be invoked successfully if NFC is supported and adapter is enabled.
     */
    @Test
    @Feature({"NFCTest"})
    public void testNFCIsSupported() {
        TestNfcImpl nfc = new TestNfcImpl(mContext, mDelegate);
        mDelegate.invokeCallback();
        WatchResponse mockCallback = mock(WatchResponse.class);
        nfc.watch(createNdefScanOptions(), mNextWatchId, mockCallback);
        verify(mockCallback).call(mErrorCaptor.capture());
        assertNull(mErrorCaptor.getValue());
    }

    /**
     * Test conversion from NdefMessage to mojo NdefMessage.
     */
    @Test
    @Feature({"NFCTest"})
    public void testNdefToMojoConversion() throws UnsupportedEncodingException {
        // Test EMPTY record conversion.
        android.nfc.NdefMessage emptyNdefMessage = new android.nfc.NdefMessage(
                new android.nfc.NdefRecord(android.nfc.NdefRecord.TNF_EMPTY, null, null, null));
        NdefMessage emptyMojoNdefMessage = NdefMessageUtils.toNdefMessage(emptyNdefMessage);
        assertEquals(1, emptyMojoNdefMessage.data.length);
        assertEquals(NdefRecordTypeCategory.STANDARDIZED, emptyMojoNdefMessage.data[0].category);
        assertEquals(NdefMessageUtils.RECORD_TYPE_EMPTY, emptyMojoNdefMessage.data[0].recordType);
        assertEquals(null, emptyMojoNdefMessage.data[0].mediaType);
        assertEquals(null, emptyMojoNdefMessage.data[0].id);
        assertNull(emptyMojoNdefMessage.data[0].encoding);
        assertNull(emptyMojoNdefMessage.data[0].lang);
        assertEquals(0, emptyMojoNdefMessage.data[0].data.length);

        // Test url record conversion.
        android.nfc.NdefMessage urlNdefMessage =
                new android.nfc.NdefMessage(NdefMessageUtils.createPlatformUrlRecord(
                        ApiCompatibilityUtils.getBytesUtf8(TEST_URL), DUMMY_RECORD_ID,
                        false /* isAbsUrl */));
        NdefMessage urlMojoNdefMessage = NdefMessageUtils.toNdefMessage(urlNdefMessage);
        assertEquals(1, urlMojoNdefMessage.data.length);
        assertEquals(NdefRecordTypeCategory.STANDARDIZED, urlMojoNdefMessage.data[0].category);
        assertEquals(NdefMessageUtils.RECORD_TYPE_URL, urlMojoNdefMessage.data[0].recordType);
        assertEquals(null, urlMojoNdefMessage.data[0].mediaType);
        assertEquals(DUMMY_RECORD_ID, urlMojoNdefMessage.data[0].id);
        assertNull(urlMojoNdefMessage.data[0].encoding);
        assertNull(urlMojoNdefMessage.data[0].lang);
        assertEquals(TEST_URL, new String(urlMojoNdefMessage.data[0].data));

        // Test absolute-url record conversion.
        android.nfc.NdefMessage absUrlNdefMessage =
                new android.nfc.NdefMessage(NdefMessageUtils.createPlatformUrlRecord(
                        ApiCompatibilityUtils.getBytesUtf8(TEST_URL), DUMMY_RECORD_ID,
                        true /* isAbsUrl */));
        NdefMessage absUrlMojoNdefMessage = NdefMessageUtils.toNdefMessage(absUrlNdefMessage);
        assertEquals(1, absUrlMojoNdefMessage.data.length);
        assertEquals(NdefRecordTypeCategory.STANDARDIZED, absUrlMojoNdefMessage.data[0].category);
        assertEquals(NdefMessageUtils.RECORD_TYPE_ABSOLUTE_URL,
                absUrlMojoNdefMessage.data[0].recordType);
        assertEquals(null, absUrlMojoNdefMessage.data[0].mediaType);
        assertEquals(DUMMY_RECORD_ID, absUrlMojoNdefMessage.data[0].id);
        assertEquals(TEST_URL, new String(absUrlMojoNdefMessage.data[0].data));

        // Test text record conversion for UTF-8 content.
        android.nfc.NdefMessage utf8TextNdefMessage = new android.nfc.NdefMessage(
                NdefMessageUtils.createPlatformTextRecord(DUMMY_RECORD_ID, LANG_EN_US,
                        ENCODING_UTF8, ApiCompatibilityUtils.getBytesUtf8(TEST_TEXT)));
        NdefMessage utf8TextMojoNdefMessage = NdefMessageUtils.toNdefMessage(utf8TextNdefMessage);
        assertEquals(1, utf8TextMojoNdefMessage.data.length);
        assertEquals(NdefRecordTypeCategory.STANDARDIZED, utf8TextMojoNdefMessage.data[0].category);
        assertEquals(NdefMessageUtils.RECORD_TYPE_TEXT, utf8TextMojoNdefMessage.data[0].recordType);
        assertEquals(null, utf8TextMojoNdefMessage.data[0].mediaType);
        assertEquals(DUMMY_RECORD_ID, utf8TextMojoNdefMessage.data[0].id);
        assertEquals(ENCODING_UTF8, utf8TextMojoNdefMessage.data[0].encoding);
        assertEquals(LANG_EN_US, utf8TextMojoNdefMessage.data[0].lang);
        assertEquals(TEST_TEXT, new String(utf8TextMojoNdefMessage.data[0].data, "UTF-8"));

        // Test text record conversion for UTF-16 content.
        byte[] textBytes = TEST_TEXT.getBytes(StandardCharsets.UTF_16BE);
        byte[] languageCodeBytes = LANG_EN_US.getBytes(StandardCharsets.US_ASCII);
        android.nfc.NdefMessage utf16TextNdefMessage =
                new android.nfc.NdefMessage(NdefMessageUtils.createPlatformTextRecord(
                        DUMMY_RECORD_ID, LANG_EN_US, ENCODING_UTF16, textBytes));
        NdefMessage utf16TextMojoNdefMessage = NdefMessageUtils.toNdefMessage(utf16TextNdefMessage);
        assertEquals(1, utf16TextMojoNdefMessage.data.length);
        assertEquals(
                NdefRecordTypeCategory.STANDARDIZED, utf16TextMojoNdefMessage.data[0].category);
        assertEquals(
                NdefMessageUtils.RECORD_TYPE_TEXT, utf16TextMojoNdefMessage.data[0].recordType);
        assertEquals(null, utf16TextMojoNdefMessage.data[0].mediaType);
        assertEquals(DUMMY_RECORD_ID, utf16TextMojoNdefMessage.data[0].id);
        assertEquals(ENCODING_UTF16, utf16TextMojoNdefMessage.data[0].encoding);
        assertEquals(LANG_EN_US, utf16TextMojoNdefMessage.data[0].lang);
        assertEquals(TEST_TEXT, new String(utf16TextMojoNdefMessage.data[0].data, "UTF-16"));

        // Test mime record conversion with "text/plain" mime type.
        android.nfc.NdefMessage mimeNdefMessage =
                new android.nfc.NdefMessage(NdefMessageUtils.createPlatformMimeRecord(
                        TEXT_MIME, DUMMY_RECORD_ID, ApiCompatibilityUtils.getBytesUtf8(TEST_TEXT)));
        NdefMessage mimeMojoNdefMessage = NdefMessageUtils.toNdefMessage(mimeNdefMessage);
        assertEquals(1, mimeMojoNdefMessage.data.length);
        assertEquals(NdefRecordTypeCategory.STANDARDIZED, mimeMojoNdefMessage.data[0].category);
        assertEquals(NdefMessageUtils.RECORD_TYPE_MIME, mimeMojoNdefMessage.data[0].recordType);
        assertEquals(TEXT_MIME, mimeMojoNdefMessage.data[0].mediaType);
        assertEquals(DUMMY_RECORD_ID, mimeMojoNdefMessage.data[0].id);
        assertNull(mimeMojoNdefMessage.data[0].encoding);
        assertNull(mimeMojoNdefMessage.data[0].lang);
        assertEquals(TEST_TEXT, new String(mimeMojoNdefMessage.data[0].data));

        // Test mime record conversion with "application/json" mime type.
        android.nfc.NdefMessage jsonNdefMessage =
                new android.nfc.NdefMessage(NdefMessageUtils.createPlatformMimeRecord(
                        JSON_MIME, DUMMY_RECORD_ID, ApiCompatibilityUtils.getBytesUtf8(TEST_JSON)));
        NdefMessage jsonMojoNdefMessage = NdefMessageUtils.toNdefMessage(jsonNdefMessage);
        assertEquals(1, jsonMojoNdefMessage.data.length);
        assertEquals(NdefRecordTypeCategory.STANDARDIZED, jsonMojoNdefMessage.data[0].category);
        assertEquals(NdefMessageUtils.RECORD_TYPE_MIME, jsonMojoNdefMessage.data[0].recordType);
        assertEquals(JSON_MIME, jsonMojoNdefMessage.data[0].mediaType);
        assertEquals(DUMMY_RECORD_ID, jsonMojoNdefMessage.data[0].id);
        assertNull(jsonMojoNdefMessage.data[0].encoding);
        assertNull(jsonMojoNdefMessage.data[0].lang);
        assertEquals(TEST_JSON, new String(jsonMojoNdefMessage.data[0].data));

        // Test unknown record conversion.
        android.nfc.NdefMessage unknownNdefMessage = new android.nfc.NdefMessage(
                new android.nfc.NdefRecord(android.nfc.NdefRecord.TNF_UNKNOWN, null,
                        ApiCompatibilityUtils.getBytesUtf8(DUMMY_RECORD_ID),
                        ApiCompatibilityUtils.getBytesUtf8(TEST_TEXT)));
        NdefMessage unknownMojoNdefMessage = NdefMessageUtils.toNdefMessage(unknownNdefMessage);
        assertEquals(1, unknownMojoNdefMessage.data.length);
        assertEquals(NdefRecordTypeCategory.STANDARDIZED, unknownMojoNdefMessage.data[0].category);
        assertEquals(
                NdefMessageUtils.RECORD_TYPE_UNKNOWN, unknownMojoNdefMessage.data[0].recordType);
        assertEquals(DUMMY_RECORD_ID, unknownMojoNdefMessage.data[0].id);
        assertNull(unknownMojoNdefMessage.data[0].encoding);
        assertNull(unknownMojoNdefMessage.data[0].lang);
        assertEquals(TEST_TEXT, new String(unknownMojoNdefMessage.data[0].data));

        // Test external record conversion.
        android.nfc.NdefMessage extNdefMessage = new android.nfc.NdefMessage(
                NdefMessageUtils.createPlatformExternalRecord(DUMMY_EXTERNAL_TYPE, DUMMY_RECORD_ID,
                        ApiCompatibilityUtils.getBytesUtf8(TEST_TEXT), null /* payloadMessage */));
        NdefMessage extMojoNdefMessage = NdefMessageUtils.toNdefMessage(extNdefMessage);
        assertEquals(1, extMojoNdefMessage.data.length);
        assertEquals(NdefRecordTypeCategory.EXTERNAL, extMojoNdefMessage.data[0].category);
        assertEquals(DUMMY_EXTERNAL_TYPE, extMojoNdefMessage.data[0].recordType);
        assertEquals(null, extMojoNdefMessage.data[0].mediaType);
        assertEquals(DUMMY_RECORD_ID, extMojoNdefMessage.data[0].id);
        assertNull(extMojoNdefMessage.data[0].encoding);
        assertNull(extMojoNdefMessage.data[0].lang);
        assertEquals(TEST_TEXT, new String(extMojoNdefMessage.data[0].data));

        // Test conversion for external records with the payload being a ndef message.
        android.nfc.NdefMessage payloadMessage = new android.nfc.NdefMessage(
                android.nfc.NdefRecord.createTextRecord(LANG_EN_US, TEST_TEXT));
        byte[] payloadBytes = payloadMessage.toByteArray();
        // Put |payloadBytes| as payload of an external record.
        android.nfc.NdefMessage extNdefMessage1 = new android.nfc.NdefMessage(
                NdefMessageUtils.createPlatformExternalRecord(DUMMY_EXTERNAL_TYPE, DUMMY_RECORD_ID,
                        payloadBytes, null /* payloadMessage */));
        NdefMessage extMojoNdefMessage1 = NdefMessageUtils.toNdefMessage(extNdefMessage1);
        assertEquals(1, extMojoNdefMessage1.data.length);
        assertEquals(NdefRecordTypeCategory.EXTERNAL, extMojoNdefMessage1.data[0].category);
        assertEquals(DUMMY_EXTERNAL_TYPE, extMojoNdefMessage1.data[0].recordType);
        assertEquals(null, extMojoNdefMessage1.data[0].mediaType);
        assertEquals(DUMMY_RECORD_ID, extMojoNdefMessage1.data[0].id);
        // The embedded ndef message should have content corresponding with the original
        // |payloadMessage|.
        NdefMessage payloadMojoMessage = extMojoNdefMessage1.data[0].payloadMessage;
        assertEquals(1, payloadMojoMessage.data.length);
        assertEquals(NdefRecordTypeCategory.STANDARDIZED, payloadMojoMessage.data[0].category);
        assertEquals(NdefMessageUtils.RECORD_TYPE_TEXT, payloadMojoMessage.data[0].recordType);
        assertEquals(null, payloadMojoMessage.data[0].mediaType);
        assertEquals(TEST_TEXT, new String(payloadMojoMessage.data[0].data));

        // Test local record conversion.
        android.nfc.NdefMessage localNdefMessage = new android.nfc.NdefMessage(
                NdefMessageUtils.createPlatformLocalRecord("xyz", DUMMY_RECORD_ID,
                        ApiCompatibilityUtils.getBytesUtf8(TEST_TEXT), null /* payloadMessage */));
        NdefMessage localMojoNdefMessage = NdefMessageUtils.toNdefMessage(localNdefMessage);
        assertEquals(1, localMojoNdefMessage.data.length);
        assertEquals(NdefRecordTypeCategory.LOCAL, localMojoNdefMessage.data[0].category);
        // Is already prefixed with ':'.
        assertEquals(":xyz", localMojoNdefMessage.data[0].recordType);
        assertEquals(null, localMojoNdefMessage.data[0].mediaType);
        assertEquals(DUMMY_RECORD_ID, localMojoNdefMessage.data[0].id);
        assertNull(localMojoNdefMessage.data[0].encoding);
        assertNull(localMojoNdefMessage.data[0].lang);
        assertEquals(TEST_TEXT, new String(localMojoNdefMessage.data[0].data));

        // Test conversion for local records with the payload being a ndef message.
        payloadMessage = new android.nfc.NdefMessage(
                android.nfc.NdefRecord.createTextRecord(LANG_EN_US, TEST_TEXT));
        payloadBytes = payloadMessage.toByteArray();
        // Put |payloadBytes| as payload of a local record.
        android.nfc.NdefMessage localNdefMessage1 =
                new android.nfc.NdefMessage(NdefMessageUtils.createPlatformLocalRecord(
                        "xyz", DUMMY_RECORD_ID, payloadBytes, null /* payloadMessage */));
        NdefMessage localMojoNdefMessage1 = NdefMessageUtils.toNdefMessage(localNdefMessage1);
        assertEquals(1, localMojoNdefMessage1.data.length);
        assertEquals(NdefRecordTypeCategory.LOCAL, localMojoNdefMessage1.data[0].category);
        // Is already prefixed with ':'.
        assertEquals(":xyz", localMojoNdefMessage1.data[0].recordType);
        assertEquals(null, localMojoNdefMessage1.data[0].mediaType);
        assertEquals(DUMMY_RECORD_ID, localMojoNdefMessage1.data[0].id);
        // The embedded ndef message should have content corresponding with the original
        // |payloadMessage|.
        payloadMojoMessage = localMojoNdefMessage1.data[0].payloadMessage;
        assertEquals(1, payloadMojoMessage.data.length);
        assertEquals(NdefRecordTypeCategory.STANDARDIZED, payloadMojoMessage.data[0].category);
        assertEquals(NdefMessageUtils.RECORD_TYPE_TEXT, payloadMojoMessage.data[0].recordType);
        assertEquals(null, payloadMojoMessage.data[0].mediaType);
        assertEquals(TEST_TEXT, new String(payloadMojoMessage.data[0].data));
    }

    /**
     * Test conversion from mojo NdefMessage to android NdefMessage.
     */
    @Test
    @Feature({"NFCTest"})
    public void testMojoToNdefConversion() throws InvalidNdefMessageException, FormatException {
        // Test url record conversion.
        NdefRecord urlMojoNdefRecord = new NdefRecord();
        urlMojoNdefRecord.category = NdefRecordTypeCategory.STANDARDIZED;
        urlMojoNdefRecord.recordType = NdefMessageUtils.RECORD_TYPE_URL;
        urlMojoNdefRecord.id = DUMMY_RECORD_ID;
        urlMojoNdefRecord.data = ApiCompatibilityUtils.getBytesUtf8(TEST_URL);
        NdefMessage urlMojoNdefMessage = createMojoNdefMessage(urlMojoNdefRecord);
        android.nfc.NdefMessage urlNdefMessage = NdefMessageUtils.toNdefMessage(urlMojoNdefMessage);
        assertEquals(1, urlNdefMessage.getRecords().length);
        assertEquals(
                android.nfc.NdefRecord.TNF_WELL_KNOWN, urlNdefMessage.getRecords()[0].getTnf());
        assertEquals(new String(android.nfc.NdefRecord.RTD_URI),
                new String(urlNdefMessage.getRecords()[0].getType()));
        assertEquals(DUMMY_RECORD_ID, new String(urlNdefMessage.getRecords()[0].getId()));
        assertEquals(TEST_URL, urlNdefMessage.getRecords()[0].toUri().toString());

        // Test absolute-url record conversion.
        NdefRecord absUrlMojoNdefRecord = new NdefRecord();
        absUrlMojoNdefRecord.category = NdefRecordTypeCategory.STANDARDIZED;
        absUrlMojoNdefRecord.recordType = NdefMessageUtils.RECORD_TYPE_ABSOLUTE_URL;
        absUrlMojoNdefRecord.id = DUMMY_RECORD_ID;
        absUrlMojoNdefRecord.data = ApiCompatibilityUtils.getBytesUtf8(TEST_URL);
        NdefMessage absUrlMojoNdefMessage = createMojoNdefMessage(absUrlMojoNdefRecord);
        android.nfc.NdefMessage absUrlNdefMessage =
                NdefMessageUtils.toNdefMessage(absUrlMojoNdefMessage);
        assertEquals(1, absUrlNdefMessage.getRecords().length);
        assertEquals(android.nfc.NdefRecord.TNF_ABSOLUTE_URI,
                absUrlNdefMessage.getRecords()[0].getTnf());
        assertEquals(DUMMY_RECORD_ID, new String(absUrlNdefMessage.getRecords()[0].getId()));
        assertEquals(TEST_URL, absUrlNdefMessage.getRecords()[0].toUri().toString());

        // Test text record conversion for UTF-8 content.
        NdefRecord utf8TextMojoNdefRecord = new NdefRecord();
        utf8TextMojoNdefRecord.category = NdefRecordTypeCategory.STANDARDIZED;
        utf8TextMojoNdefRecord.recordType = NdefMessageUtils.RECORD_TYPE_TEXT;
        utf8TextMojoNdefRecord.id = DUMMY_RECORD_ID;
        utf8TextMojoNdefRecord.encoding = ENCODING_UTF8;
        utf8TextMojoNdefRecord.lang = LANG_EN_US;
        utf8TextMojoNdefRecord.data = ApiCompatibilityUtils.getBytesUtf8(TEST_TEXT);
        NdefMessage utf8TextMojoNdefMessage = createMojoNdefMessage(utf8TextMojoNdefRecord);
        android.nfc.NdefMessage utf8TextNdefMessage =
                NdefMessageUtils.toNdefMessage(utf8TextMojoNdefMessage);
        assertEquals(1, utf8TextNdefMessage.getRecords().length);
        assertEquals(android.nfc.NdefRecord.TNF_WELL_KNOWN,
                utf8TextNdefMessage.getRecords()[0].getTnf());
        assertEquals(DUMMY_RECORD_ID, new String(utf8TextNdefMessage.getRecords()[0].getId()));
        {
            byte[] languageCodeBytes = LANG_EN_US.getBytes(StandardCharsets.US_ASCII);
            ByteBuffer expectedPayload = ByteBuffer.allocate(
                    1 + languageCodeBytes.length + utf8TextMojoNdefRecord.data.length);
            byte status = (byte) languageCodeBytes.length;
            expectedPayload.put(status);
            expectedPayload.put(languageCodeBytes);
            expectedPayload.put(utf8TextMojoNdefRecord.data);
            assertArrayEquals(
                    expectedPayload.array(), utf8TextNdefMessage.getRecords()[0].getPayload());
        }

        // Test text record conversion for UTF-16 content.
        NdefRecord utf16TextMojoNdefRecord = new NdefRecord();
        utf16TextMojoNdefRecord.category = NdefRecordTypeCategory.STANDARDIZED;
        utf16TextMojoNdefRecord.recordType = NdefMessageUtils.RECORD_TYPE_TEXT;
        utf16TextMojoNdefRecord.id = DUMMY_RECORD_ID;
        utf16TextMojoNdefRecord.encoding = ENCODING_UTF16;
        utf16TextMojoNdefRecord.lang = LANG_EN_US;
        utf16TextMojoNdefRecord.data = TEST_TEXT.getBytes(Charset.forName("UTF-16"));
        NdefMessage utf16TextMojoNdefMessage = createMojoNdefMessage(utf16TextMojoNdefRecord);
        android.nfc.NdefMessage utf16TextNdefMessage =
                NdefMessageUtils.toNdefMessage(utf16TextMojoNdefMessage);
        assertEquals(1, utf16TextNdefMessage.getRecords().length);
        assertEquals(android.nfc.NdefRecord.TNF_WELL_KNOWN,
                utf16TextNdefMessage.getRecords()[0].getTnf());
        assertEquals(DUMMY_RECORD_ID, new String(utf16TextNdefMessage.getRecords()[0].getId()));
        {
            byte[] languageCodeBytes = LANG_EN_US.getBytes(StandardCharsets.US_ASCII);
            ByteBuffer expectedPayload = ByteBuffer.allocate(
                    1 + languageCodeBytes.length + utf16TextMojoNdefRecord.data.length);
            byte status = (byte) languageCodeBytes.length;
            status |= (byte) (1 << 7);
            expectedPayload.put(status);
            expectedPayload.put(languageCodeBytes);
            expectedPayload.put(utf16TextMojoNdefRecord.data);
            assertArrayEquals(
                    expectedPayload.array(), utf16TextNdefMessage.getRecords()[0].getPayload());
        }

        // Test mime record conversion with "text/plain" mime type.
        NdefRecord mimeMojoNdefRecord = new NdefRecord();
        mimeMojoNdefRecord.category = NdefRecordTypeCategory.STANDARDIZED;
        mimeMojoNdefRecord.recordType = NdefMessageUtils.RECORD_TYPE_MIME;
        mimeMojoNdefRecord.mediaType = TEXT_MIME;
        mimeMojoNdefRecord.id = DUMMY_RECORD_ID;
        mimeMojoNdefRecord.data = ApiCompatibilityUtils.getBytesUtf8(TEST_TEXT);
        NdefMessage mimeMojoNdefMessage = createMojoNdefMessage(mimeMojoNdefRecord);
        android.nfc.NdefMessage mimeNdefMessage =
                NdefMessageUtils.toNdefMessage(mimeMojoNdefMessage);
        assertEquals(1, mimeNdefMessage.getRecords().length);
        assertEquals(
                android.nfc.NdefRecord.TNF_MIME_MEDIA, mimeNdefMessage.getRecords()[0].getTnf());
        assertEquals(TEXT_MIME, mimeNdefMessage.getRecords()[0].toMimeType());
        assertEquals(DUMMY_RECORD_ID, new String(mimeNdefMessage.getRecords()[0].getId()));
        assertEquals(TEST_TEXT, new String(mimeNdefMessage.getRecords()[0].getPayload()));

        // Test mime record conversion with "application/json" mime type.
        NdefRecord jsonMojoNdefRecord = new NdefRecord();
        jsonMojoNdefRecord.category = NdefRecordTypeCategory.STANDARDIZED;
        jsonMojoNdefRecord.recordType = NdefMessageUtils.RECORD_TYPE_MIME;
        jsonMojoNdefRecord.mediaType = JSON_MIME;
        jsonMojoNdefRecord.id = DUMMY_RECORD_ID;
        jsonMojoNdefRecord.data = ApiCompatibilityUtils.getBytesUtf8(TEST_JSON);
        NdefMessage jsonMojoNdefMessage = createMojoNdefMessage(jsonMojoNdefRecord);
        android.nfc.NdefMessage jsonNdefMessage =
                NdefMessageUtils.toNdefMessage(jsonMojoNdefMessage);
        assertEquals(1, jsonNdefMessage.getRecords().length);
        assertEquals(
                android.nfc.NdefRecord.TNF_MIME_MEDIA, jsonNdefMessage.getRecords()[0].getTnf());
        assertEquals(JSON_MIME, jsonNdefMessage.getRecords()[0].toMimeType());
        assertEquals(DUMMY_RECORD_ID, new String(jsonNdefMessage.getRecords()[0].getId()));
        assertEquals(TEST_JSON, new String(jsonNdefMessage.getRecords()[0].getPayload()));

        // Test unknown record conversion.
        NdefRecord unknownMojoNdefRecord = new NdefRecord();
        unknownMojoNdefRecord.category = NdefRecordTypeCategory.STANDARDIZED;
        unknownMojoNdefRecord.recordType = NdefMessageUtils.RECORD_TYPE_UNKNOWN;
        unknownMojoNdefRecord.id = DUMMY_RECORD_ID;
        unknownMojoNdefRecord.data = ApiCompatibilityUtils.getBytesUtf8(TEST_TEXT);
        NdefMessage unknownMojoNdefMessage = createMojoNdefMessage(unknownMojoNdefRecord);
        android.nfc.NdefMessage unknownNdefMessage =
                NdefMessageUtils.toNdefMessage(unknownMojoNdefMessage);
        assertEquals(1, unknownNdefMessage.getRecords().length);
        assertEquals(
                android.nfc.NdefRecord.TNF_UNKNOWN, unknownNdefMessage.getRecords()[0].getTnf());
        assertEquals(DUMMY_RECORD_ID, new String(unknownNdefMessage.getRecords()[0].getId()));
        assertEquals(TEST_TEXT, new String(unknownNdefMessage.getRecords()[0].getPayload()));

        // Test external record conversion.
        NdefRecord extMojoNdefRecord = new NdefRecord();
        extMojoNdefRecord.category = NdefRecordTypeCategory.EXTERNAL;
        extMojoNdefRecord.recordType = DUMMY_EXTERNAL_TYPE;
        extMojoNdefRecord.id = DUMMY_RECORD_ID;
        extMojoNdefRecord.data = ApiCompatibilityUtils.getBytesUtf8(TEST_TEXT);
        NdefMessage extMojoNdefMessage = createMojoNdefMessage(extMojoNdefRecord);
        android.nfc.NdefMessage extNdefMessage = NdefMessageUtils.toNdefMessage(extMojoNdefMessage);
        assertEquals(1, extNdefMessage.getRecords().length);
        assertEquals(
                android.nfc.NdefRecord.TNF_EXTERNAL_TYPE, extNdefMessage.getRecords()[0].getTnf());
        assertEquals(DUMMY_EXTERNAL_TYPE, new String(extNdefMessage.getRecords()[0].getType()));
        assertEquals(DUMMY_RECORD_ID, new String(extNdefMessage.getRecords()[0].getId()));
        assertEquals(TEST_TEXT, new String(extNdefMessage.getRecords()[0].getPayload()));

        // Test conversion for external records with the payload being a ndef message.
        NdefRecord payloadMojoRecord = new NdefRecord();
        payloadMojoRecord.category = NdefRecordTypeCategory.STANDARDIZED;
        payloadMojoRecord.recordType = NdefMessageUtils.RECORD_TYPE_URL;
        payloadMojoRecord.id = DUMMY_RECORD_ID;
        payloadMojoRecord.data = ApiCompatibilityUtils.getBytesUtf8(TEST_URL);
        // Prepare an external record that embeds |payloadMojoRecord| in its payload.
        NdefRecord extMojoNdefRecord1 = new NdefRecord();
        extMojoNdefRecord1.category = NdefRecordTypeCategory.EXTERNAL;
        extMojoNdefRecord1.recordType = DUMMY_EXTERNAL_TYPE;
        extMojoNdefRecord1.id = DUMMY_RECORD_ID;
        // device.mojom.NDEFRecord.data is not allowed to be null, instead, empty byte array is just
        // what's passed from Blink.
        extMojoNdefRecord1.data = new byte[0];
        extMojoNdefRecord1.payloadMessage = createMojoNdefMessage(payloadMojoRecord);
        // Do the conversion.
        android.nfc.NdefMessage extNdefMessage1 =
                NdefMessageUtils.toNdefMessage(createMojoNdefMessage(extMojoNdefRecord1));
        assertEquals(1, extNdefMessage1.getRecords().length);
        assertEquals(
                android.nfc.NdefRecord.TNF_EXTERNAL_TYPE, extNdefMessage1.getRecords()[0].getTnf());
        assertEquals(DUMMY_EXTERNAL_TYPE, new String(extNdefMessage1.getRecords()[0].getType()));
        assertEquals(DUMMY_RECORD_ID, new String(extNdefMessage1.getRecords()[0].getId()));
        // The payload raw bytes should be able to construct an ndef message containing an ndef
        // record that has content corresponding with the original |payloadMojoRecord|.
        android.nfc.NdefMessage payloadMessage =
                new android.nfc.NdefMessage(extNdefMessage1.getRecords()[0].getPayload());
        assertNotNull(payloadMessage);
        assertEquals(1, payloadMessage.getRecords().length);
        assertEquals(
                android.nfc.NdefRecord.TNF_WELL_KNOWN, payloadMessage.getRecords()[0].getTnf());
        assertEquals(new String(android.nfc.NdefRecord.RTD_URI),
                new String(payloadMessage.getRecords()[0].getType()));
        assertEquals(DUMMY_RECORD_ID, new String(payloadMessage.getRecords()[0].getId()));
        assertEquals(TEST_URL, payloadMessage.getRecords()[0].toUri().toString());

        // Test local record conversion.
        NdefRecord localMojoNdefRecord = new NdefRecord();
        localMojoNdefRecord.category = NdefRecordTypeCategory.LOCAL;
        localMojoNdefRecord.recordType = ":xyz";
        localMojoNdefRecord.id = DUMMY_RECORD_ID;
        localMojoNdefRecord.data = ApiCompatibilityUtils.getBytesUtf8(TEST_TEXT);
        NdefMessage localMojoNdefMessage = createMojoNdefMessage(localMojoNdefRecord);
        android.nfc.NdefMessage localNdefMessage =
                NdefMessageUtils.toNdefMessage(localMojoNdefMessage);
        assertEquals(1, localNdefMessage.getRecords().length);
        assertEquals(
                android.nfc.NdefRecord.TNF_WELL_KNOWN, localNdefMessage.getRecords()[0].getTnf());
        // The ':' prefix is already omitted.
        assertEquals("xyz", new String(localNdefMessage.getRecords()[0].getType()));
        assertEquals(DUMMY_RECORD_ID, new String(localNdefMessage.getRecords()[0].getId()));
        assertEquals(TEST_TEXT, new String(localNdefMessage.getRecords()[0].getPayload()));

        // Test conversion for local records with the payload being a ndef message.
        //
        // Prepare a local record that embeds |payloadMojoRecord| in its payload.
        NdefRecord localMojoNdefRecord1 = new NdefRecord();
        localMojoNdefRecord1.category = NdefRecordTypeCategory.LOCAL;
        localMojoNdefRecord1.recordType = ":xyz";
        localMojoNdefRecord1.id = DUMMY_RECORD_ID;
        // device.mojom.NDEFRecord.data is not allowed to be null, instead, empty byte array is just
        // what's passed from Blink.
        localMojoNdefRecord1.data = new byte[0];
        localMojoNdefRecord1.payloadMessage = createMojoNdefMessage(payloadMojoRecord);
        // Do the conversion.
        android.nfc.NdefMessage localNdefMessage1 =
                NdefMessageUtils.toNdefMessage(createMojoNdefMessage(localMojoNdefRecord1));
        assertEquals(1, localNdefMessage1.getRecords().length);
        assertEquals(
                android.nfc.NdefRecord.TNF_WELL_KNOWN, localNdefMessage1.getRecords()[0].getTnf());
        // The ':' prefix is already omitted.
        assertEquals("xyz", new String(localNdefMessage1.getRecords()[0].getType()));
        assertEquals(DUMMY_RECORD_ID, new String(localNdefMessage1.getRecords()[0].getId()));
        // The payload raw bytes should be able to construct an ndef message containing an ndef
        // record that has content corresponding with the original |payloadMojoRecord|.
        payloadMessage =
                new android.nfc.NdefMessage(localNdefMessage1.getRecords()[0].getPayload());
        assertNotNull(payloadMessage);
        assertEquals(1, payloadMessage.getRecords().length);
        assertEquals(
                android.nfc.NdefRecord.TNF_WELL_KNOWN, payloadMessage.getRecords()[0].getTnf());
        assertEquals(new String(android.nfc.NdefRecord.RTD_URI),
                new String(payloadMessage.getRecords()[0].getType()));
        assertEquals(DUMMY_RECORD_ID, new String(payloadMessage.getRecords()[0].getId()));
        assertEquals(TEST_URL, payloadMessage.getRecords()[0].toUri().toString());

        // Test EMPTY record conversion.
        NdefRecord emptyMojoNdefRecord = new NdefRecord();
        emptyMojoNdefRecord.category = NdefRecordTypeCategory.STANDARDIZED;
        emptyMojoNdefRecord.recordType = NdefMessageUtils.RECORD_TYPE_EMPTY;
        NdefMessage emptyMojoNdefMessage = createMojoNdefMessage(emptyMojoNdefRecord);
        android.nfc.NdefMessage emptyNdefMessage =
                NdefMessageUtils.toNdefMessage(emptyMojoNdefMessage);
        assertEquals(1, emptyNdefMessage.getRecords().length);
        assertEquals(android.nfc.NdefRecord.TNF_EMPTY, emptyNdefMessage.getRecords()[0].getTnf());
    }

    /**
     * Test external record conversion with invalid custom type.
     */
    @Test(expected = InvalidNdefMessageException.class)
    @Feature({"NFCTest"})
    public void testInvalidExternalRecordType() throws InvalidNdefMessageException {
        NdefRecord extMojoNdefRecord = new NdefRecord();
        extMojoNdefRecord.category = NdefRecordTypeCategory.EXTERNAL;
        extMojoNdefRecord.data = ApiCompatibilityUtils.getBytesUtf8(TEST_TEXT);
        {
            // Must have a ':'.
            extMojoNdefRecord.recordType = "abc.com";
            NdefMessage extMojoNdefMessage = createMojoNdefMessage(extMojoNdefRecord);
            android.nfc.NdefMessage extNdefMessage =
                    NdefMessageUtils.toNdefMessage(extMojoNdefMessage);
            assertNull(extNdefMessage);
        }
        {
            // '-' is allowed in the domain part.
            extMojoNdefRecord.recordType = "abc-123.com:xyz";
            NdefMessage extMojoNdefMessage = createMojoNdefMessage(extMojoNdefRecord);
            android.nfc.NdefMessage extNdefMessage =
                    NdefMessageUtils.toNdefMessage(extMojoNdefMessage);
            assertNotNull(extNdefMessage);
            assertEquals(1, extNdefMessage.getRecords().length);
            assertEquals(android.nfc.NdefRecord.TNF_EXTERNAL_TYPE,
                    extNdefMessage.getRecords()[0].getTnf());
            assertEquals("abc-1.com:xyz", new String(extNdefMessage.getRecords()[0].getType()));
            assertEquals(DUMMY_RECORD_ID, new String(extNdefMessage.getRecords()[0].getId()));
            assertEquals(TEST_TEXT, new String(extNdefMessage.getRecords()[0].getPayload()));
        }
        {
            // '-' is not allowed in the type part.
            extMojoNdefRecord.recordType = "abc.com:xyz-123";
            NdefMessage extMojoNdefMessage = createMojoNdefMessage(extMojoNdefRecord);
            android.nfc.NdefMessage extNdefMessage =
                    NdefMessageUtils.toNdefMessage(extMojoNdefMessage);
            assertNull(extNdefMessage);
        }
        {
            // As the 2 cases above have proved that '-' is allowed in the domain part but not
            // allowed in the type part, from this case we can say that the first occurrence of
            // ':' is used to separate the domain part and the type part, i.e. "xyz-123:uvw" is
            // separated as the type part and is treated as invalid due to the existence of '-'.
            extMojoNdefRecord.recordType = "abc.com:xyz-123:uvw";
            NdefMessage extMojoNdefMessage = createMojoNdefMessage(extMojoNdefRecord);
            android.nfc.NdefMessage extNdefMessage =
                    NdefMessageUtils.toNdefMessage(extMojoNdefMessage);
            assertNull(extNdefMessage);
        }
        {
            // |recordType| is a string mixed with ASCII/non-ASCII, FAIL.
            extMojoNdefRecord.recordType = "example.com:hellö";
            android.nfc.NdefMessage extNdefMessage_nonASCII =
                    NdefMessageUtils.toNdefMessage(createMojoNdefMessage(extMojoNdefRecord));
            assertNull(extNdefMessage_nonASCII);

            char[] chars = new char[251];
            Arrays.fill(chars, 'a');
            String domain = new String(chars);

            // |recordType|'s length is 255, OK.
            extMojoNdefRecord.recordType = domain + ":xyz";
            android.nfc.NdefMessage extNdefMessage_255 =
                    NdefMessageUtils.toNdefMessage(createMojoNdefMessage(extMojoNdefRecord));
            assertNotNull(extNdefMessage_255);

            // Exceeding the maximum length 255, FAIL.
            extMojoNdefRecord.recordType = domain + ":xyze";
            android.nfc.NdefMessage extNdefMessage_256 =
                    NdefMessageUtils.toNdefMessage(createMojoNdefMessage(extMojoNdefRecord));
            assertNull(extNdefMessage_256);
        }
        {
            // '/' is not allowed in the type part.
            extMojoNdefRecord.recordType = "abc.com:xyz/";
            NdefMessage extMojoNdefMessage = createMojoNdefMessage(extMojoNdefRecord);
            android.nfc.NdefMessage extNdefMessage =
                    NdefMessageUtils.toNdefMessage(extMojoNdefMessage);
            assertNull(extNdefMessage);
        }
    }

    /**
     * Test local type record conversion with invalid local type.
     */
    @Test(expected = InvalidNdefMessageException.class)
    @Feature({"NFCTest"})
    public void testInvalidLocalRecordType() throws InvalidNdefMessageException {
        NdefRecord localMojoNdefRecord = new NdefRecord();
        localMojoNdefRecord.category = NdefRecordTypeCategory.LOCAL;
        localMojoNdefRecord.data = ApiCompatibilityUtils.getBytesUtf8(TEST_TEXT);
        {
            // Must start with ':'.
            localMojoNdefRecord.recordType = "dummyLocalTypeNotStartingwith:";
            localMojoNdefRecord.data = ApiCompatibilityUtils.getBytesUtf8(TEST_TEXT);
            NdefMessage localMojoNdefMessage = createMojoNdefMessage(localMojoNdefRecord);
            android.nfc.NdefMessage localNdefMessage =
                    NdefMessageUtils.toNdefMessage(localMojoNdefMessage);
            assertNull(localNdefMessage);
        }
        {
            // |recordType| is a string mixed with ASCII/non-ASCII, FAIL.
            localMojoNdefRecord.recordType = ":hellö";
            android.nfc.NdefMessage localNdefMessage_nonASCII =
                    NdefMessageUtils.toNdefMessage(createMojoNdefMessage(localMojoNdefRecord));
            assertNull(localNdefMessage_nonASCII);

            char[] chars = new char[255];
            Arrays.fill(chars, 'a');
            String chars_255 = new String(chars);

            // The length of the real local type is 255, OK.
            localMojoNdefRecord.recordType = ":" + chars_255;
            android.nfc.NdefMessage localNdefMessage_255 =
                    NdefMessageUtils.toNdefMessage(createMojoNdefMessage(localMojoNdefRecord));
            assertNotNull(localNdefMessage_255);

            // Exceeding the maximum length 255, FAIL.
            localMojoNdefRecord.recordType = ":a" + chars_255;
            android.nfc.NdefMessage localNdefMessage_256 =
                    NdefMessageUtils.toNdefMessage(createMojoNdefMessage(localMojoNdefRecord));
            assertNull(localNdefMessage_256);
        }
    }

    /**
     * Test that invalid NdefMessage is rejected with INVALID_MESSAGE error code.
     */
    @Test
    @Feature({"NFCTest"})
    public void testInvalidNdefMessage() {
        TestNfcImpl nfc = new TestNfcImpl(mContext, mDelegate);
        mDelegate.invokeCallback();
        PushResponse mockCallback = mock(PushResponse.class);
        nfc.push(new NdefMessage(), createNdefWriteOptions(), mockCallback);
        nfc.processPendingOperationsForTesting(mNfcTagHandler);
        verify(mockCallback).call(mErrorCaptor.capture());
        assertEquals(NdefErrorType.INVALID_MESSAGE, mErrorCaptor.getValue().errorType);
    }

    /**
     * Test that Nfc.suspendNfcOperations() and Nfc.resumeNfcOperations() work correctly.
     */
    @Test
    @Feature({"NFCTest"})
    public void testResumeSuspend() {
        TestNfcImpl nfc = new TestNfcImpl(mContext, mDelegate);
        // No activity / client or active pending operations
        nfc.suspendNfcOperations();
        nfc.resumeNfcOperations();

        mDelegate.invokeCallback();
        nfc.setClient(mNfcClient);
        WatchResponse mockCallback = mock(WatchResponse.class);
        nfc.watch(createNdefScanOptions(), mNextWatchId, mockCallback);
        nfc.suspendNfcOperations();
        verify(mNfcAdapter, times(1)).disableReaderMode(mActivity);
        nfc.resumeNfcOperations();
        // 1. Enable after watch is called, 2. after resumeNfcOperations is called.
        verify(mNfcAdapter, times(2))
                .enableReaderMode(any(Activity.class), any(ReaderCallback.class), anyInt(),
                        (Bundle) isNull());

        nfc.processPendingOperationsForTesting(mNfcTagHandler);
        // Check that watch request was completed successfully.
        verify(mockCallback).call(mErrorCaptor.capture());
        assertNull(mErrorCaptor.getValue());

        // Check that client was notified and watch with correct id was triggered.
        verify(mNfcClient, times(1))
                .onWatch(mOnWatchCallbackCaptor.capture(), nullable(String.class),
                        any(NdefMessage.class));
        assertEquals(mNextWatchId, mOnWatchCallbackCaptor.getValue()[0]);
    }

    /**
     * Test that Nfc.push() successful when NFC tag is connected.
     */
    @Test
    @Feature({"NFCTest"})
    public void testPush() {
        TestNfcImpl nfc = new TestNfcImpl(mContext, mDelegate);
        mDelegate.invokeCallback();
        PushResponse mockCallback = mock(PushResponse.class);
        nfc.push(createMojoNdefMessage(), createNdefWriteOptions(), mockCallback);
        nfc.processPendingOperationsForTesting(mNfcTagHandler);
        verify(mockCallback).call(mErrorCaptor.capture());
        assertNull(mErrorCaptor.getValue());
    }

    /**
     * Test that Nfc.cancelPush() cancels pending push opration and completes successfully.
     */
    @Test
    @Feature({"NFCTest"})
    public void testCancelPush() {
        TestNfcImpl nfc = new TestNfcImpl(mContext, mDelegate);
        mDelegate.invokeCallback();
        PushResponse mockPushCallback = mock(PushResponse.class);
        CancelPushResponse mockCancelPushCallback = mock(CancelPushResponse.class);
        nfc.push(createMojoNdefMessage(), createNdefWriteOptions(), mockPushCallback);
        nfc.cancelPush(mockCancelPushCallback);

        // Check that push request was cancelled with OPERATION_CANCELLED.
        verify(mockPushCallback).call(mErrorCaptor.capture());
        assertEquals(NdefErrorType.OPERATION_CANCELLED, mErrorCaptor.getValue().errorType);

        // Check that cancel request was successfuly completed.
        verify(mockCancelPushCallback).call(mErrorCaptor.capture());
        assertNull(mErrorCaptor.getValue());
    }

    /**
     * Test that Nfc.watch() works correctly and client is notified.
     */
    @Test
    @Feature({"NFCTest"})
    public void testWatch() {
        TestNfcImpl nfc = new TestNfcImpl(mContext, mDelegate);
        mDelegate.invokeCallback();
        nfc.setClient(mNfcClient);
        int watchId1 = mNextWatchId++;
        WatchResponse mockWatchCallback1 = mock(WatchResponse.class);
        nfc.watch(createNdefScanOptions(), watchId1, mockWatchCallback1);

        // Check that watch requests were completed successfully.
        verify(mockWatchCallback1).call(mErrorCaptor.capture());
        assertNull(mErrorCaptor.getValue());

        int watchId2 = mNextWatchId++;
        WatchResponse mockWatchCallback2 = mock(WatchResponse.class);
        nfc.watch(createNdefScanOptions(), watchId2, mockWatchCallback2);
        verify(mockWatchCallback2).call(mErrorCaptor.capture());
        assertNull(mErrorCaptor.getValue());

        // Mocks 'NFC tag found' event.
        nfc.processPendingOperationsForTesting(mNfcTagHandler);

        // Check that client was notified and correct watch ids were provided.
        verify(mNfcClient, times(1))
                .onWatch(mOnWatchCallbackCaptor.capture(), nullable(String.class),
                        any(NdefMessage.class));
        assertEquals(watchId1, mOnWatchCallbackCaptor.getValue()[0]);
        assertEquals(watchId2, mOnWatchCallbackCaptor.getValue()[1]);
    }

    /**
     * Test that Nfc.watch() notifies client when tag is not formatted.
     */
    @Test
    @Feature({"NFCTest"})
    public void testWatchNotFormattedTag() throws IOException, FormatException {
        TestNfcImpl nfc = new TestNfcImpl(mContext, mDelegate);
        mDelegate.invokeCallback();
        nfc.setClient(mNfcClient);
        int watchId = mNextWatchId++;
        WatchResponse mockWatchCallback = mock(WatchResponse.class);
        nfc.watch(createNdefScanOptions(), watchId, mockWatchCallback);
        verify(mockWatchCallback).call(mErrorCaptor.capture());
        assertNull(mErrorCaptor.getValue());

        // Returning null means tag is not formatted.
        doReturn(null).when(mNfcTagHandler).read();
        nfc.processPendingOperationsForTesting(mNfcTagHandler);

        // Check that client was notified and correct watch id was provided.
        verify(mNfcClient, times(1))
                .onWatch(mOnWatchCallbackCaptor.capture(), nullable(String.class),
                        any(NdefMessage.class));
        assertEquals(watchId, mOnWatchCallbackCaptor.getValue()[0]);
    }

    /**
     * Test that Nfc.watch() matching function works correctly.
     */
    @Test
    @Feature({"NFCTest"})
    public void testWatchMatching() {
        TestNfcImpl nfc = new TestNfcImpl(mContext, mDelegate);
        mDelegate.invokeCallback();
        nfc.setClient(mNfcClient);

        // Should match by record id (exact match).
        NdefScanOptions options1 = createNdefScanOptions();
        options1.id = DUMMY_RECORD_ID;
        int watchId1 = mNextWatchId++;
        WatchResponse mockWatchCallback1 = mock(WatchResponse.class);
        nfc.watch(options1, watchId1, mockWatchCallback1);
        verify(mockWatchCallback1).call(mErrorCaptor.capture());
        assertNull(mErrorCaptor.getValue());

        // Should match by media type.
        NdefScanOptions options2 = createNdefScanOptions();
        int watchId2 = mNextWatchId++;
        WatchResponse mockWatchCallback2 = mock(WatchResponse.class);
        nfc.watch(options2, watchId2, mockWatchCallback2);
        verify(mockWatchCallback2).call(mErrorCaptor.capture());
        assertNull(mErrorCaptor.getValue());

        // Should match by record type.
        NdefScanOptions options3 = createNdefScanOptions();
        options3.recordType = NdefMessageUtils.RECORD_TYPE_URL;
        int watchId3 = mNextWatchId++;
        WatchResponse mockWatchCallback3 = mock(WatchResponse.class);
        nfc.watch(options3, watchId3, mockWatchCallback3);
        verify(mockWatchCallback3).call(mErrorCaptor.capture());
        assertNull(mErrorCaptor.getValue());

        // Should not match
        NdefScanOptions options4 = createNdefScanOptions();
        options4.id = "random_record_id";
        int watchId4 = mNextWatchId++;
        WatchResponse mockWatchCallback4 = mock(WatchResponse.class);
        nfc.watch(options4, watchId4, mockWatchCallback4);
        verify(mockWatchCallback4).call(mErrorCaptor.capture());
        assertNull(mErrorCaptor.getValue());

        // Should not match because the record type must match case-sensitive.
        NdefScanOptions options5 = createNdefScanOptions();
        options5.recordType = "Url";
        int watchId5 = mNextWatchId++;
        WatchResponse mockWatchCallback5 = mock(WatchResponse.class);
        nfc.watch(options5, watchId5, mockWatchCallback5);
        verify(mockWatchCallback5).call(mErrorCaptor.capture());
        assertNull(mErrorCaptor.getValue());

        nfc.processPendingOperationsForTesting(mNfcTagHandler);

        // Check that client was notified and watch with correct id was triggered.
        verify(mNfcClient, times(1))
                .onWatch(mOnWatchCallbackCaptor.capture(), nullable(String.class),
                        any(NdefMessage.class));
        assertEquals(3, mOnWatchCallbackCaptor.getValue().length);
        assertEquals(watchId1, mOnWatchCallbackCaptor.getValue()[0]);
        assertEquals(watchId2, mOnWatchCallbackCaptor.getValue()[1]);
        assertEquals(watchId3, mOnWatchCallbackCaptor.getValue()[2]);
    }

    /**
     * Test that Nfc.watch() matching function compares 2 external types in case-insensitive manner.
     */
    @Test
    @Feature({"NFCTest"})
    public void testWatchMatchingExternalType() {
        TestNfcImpl nfc = new TestNfcImpl(mContext, mDelegate);
        mDelegate.invokeCallback();
        nfc.setClient(mNfcClient);

        // Prepare the external type record.
        android.nfc.NdefMessage extNdefMessage = new android.nfc.NdefMessage(
                NdefMessageUtils.createPlatformExternalRecord(DUMMY_EXTERNAL_TYPE, DUMMY_RECORD_ID,
                        ApiCompatibilityUtils.getBytesUtf8(TEST_TEXT), null /* payloadMessage */));
        try {
            doReturn(extNdefMessage).when(mNfcTagHandler).read();
        } catch (IOException | FormatException e) {
        }

        // Should match, the record type is exactly equal.
        NdefScanOptions options1 = createNdefScanOptions();
        options1.recordType = DUMMY_EXTERNAL_TYPE;
        int watchId1 = mNextWatchId++;
        WatchResponse mockWatchCallback1 = mock(WatchResponse.class);
        nfc.watch(options1, watchId1, mockWatchCallback1);
        verify(mockWatchCallback1).call(mErrorCaptor.capture());
        assertNull(mErrorCaptor.getValue());

        // Should match, the record type is equal in case-insensitive manner.
        NdefScanOptions options2 = createNdefScanOptions();
        options2.recordType = "aBc.com:xyZ";
        int watchId2 = mNextWatchId++;
        WatchResponse mockWatchCallback2 = mock(WatchResponse.class);
        nfc.watch(options2, watchId2, mockWatchCallback2);
        verify(mockWatchCallback2).call(mErrorCaptor.capture());
        assertNull(mErrorCaptor.getValue());

        // Should not match, the record type is NOT equal even in case-insensitive manner.
        NdefScanOptions options3 = createNdefScanOptions();
        options3.recordType = "abcd.com:xyz";
        int watchId3 = mNextWatchId++;
        WatchResponse mockWatchCallback3 = mock(WatchResponse.class);
        nfc.watch(options3, watchId3, mockWatchCallback3);
        verify(mockWatchCallback3).call(mErrorCaptor.capture());
        assertNull(mErrorCaptor.getValue());

        nfc.processPendingOperationsForTesting(mNfcTagHandler);

        // Check that client was notified and watch with correct id was triggered.
        verify(mNfcClient, times(1))
                .onWatch(mOnWatchCallbackCaptor.capture(), nullable(String.class),
                        any(NdefMessage.class));
        assertEquals(2, mOnWatchCallbackCaptor.getValue().length);
        assertEquals(watchId1, mOnWatchCallbackCaptor.getValue()[0]);
        assertEquals(watchId2, mOnWatchCallbackCaptor.getValue()[1]);
    }

    /**
     * Test that Nfc.watch() can be cancelled with Nfc.cancelWatch().
     */
    @Test
    @Feature({"NFCTest"})
    public void testCancelWatch() {
        TestNfcImpl nfc = new TestNfcImpl(mContext, mDelegate);
        mDelegate.invokeCallback();
        WatchResponse mockWatchCallback = mock(WatchResponse.class);
        nfc.watch(createNdefScanOptions(), mNextWatchId, mockWatchCallback);

        verify(mockWatchCallback).call(mErrorCaptor.capture());
        assertNull(mErrorCaptor.getValue());

        CancelWatchResponse mockCancelWatchCallback = mock(CancelWatchResponse.class);
        nfc.cancelWatch(mNextWatchId, mockCancelWatchCallback);

        // Check that cancel request was successfuly completed.
        verify(mockCancelWatchCallback).call(mErrorCaptor.capture());
        assertNull(mErrorCaptor.getValue());

        // Check that watch is not triggered when NFC tag is in proximity.
        nfc.processPendingOperationsForTesting(mNfcTagHandler);
        verify(mNfcClient, times(0))
                .onWatch(any(int[].class), nullable(String.class), any(NdefMessage.class));
    }

    /**
     * Test that Nfc.cancelAllWatches() cancels all pending watch operations.
     */
    @Test
    @Feature({"NFCTest"})
    public void testCancelAllWatches() {
        TestNfcImpl nfc = new TestNfcImpl(mContext, mDelegate);
        mDelegate.invokeCallback();
        WatchResponse mockWatchCallback1 = mock(WatchResponse.class);
        WatchResponse mockWatchCallback2 = mock(WatchResponse.class);
        nfc.watch(createNdefScanOptions(), mNextWatchId++, mockWatchCallback1);
        verify(mockWatchCallback1).call(mErrorCaptor.capture());
        assertNull(mErrorCaptor.getValue());

        nfc.watch(createNdefScanOptions(), mNextWatchId++, mockWatchCallback2);
        verify(mockWatchCallback2).call(mErrorCaptor.capture());
        assertNull(mErrorCaptor.getValue());

        CancelAllWatchesResponse mockCallback = mock(CancelAllWatchesResponse.class);
        nfc.cancelAllWatches(mockCallback);

        // Check that cancel request was successfuly completed.
        verify(mockCallback).call(mErrorCaptor.capture());
        assertNull(mErrorCaptor.getValue());
    }

    /**
     * Test that Nfc.cancelWatch() with invalid id is failing with NOT_FOUND error.
     */
    @Test
    @Feature({"NFCTest"})
    public void testCancelWatchInvalidId() {
        TestNfcImpl nfc = new TestNfcImpl(mContext, mDelegate);
        mDelegate.invokeCallback();
        WatchResponse mockWatchCallback = mock(WatchResponse.class);
        nfc.watch(createNdefScanOptions(), mNextWatchId, mockWatchCallback);

        verify(mockWatchCallback).call(mErrorCaptor.capture());
        assertNull(mErrorCaptor.getValue());

        CancelWatchResponse mockCancelWatchCallback = mock(CancelWatchResponse.class);
        nfc.cancelWatch(mNextWatchId + 1, mockCancelWatchCallback);

        verify(mockCancelWatchCallback).call(mErrorCaptor.capture());
        assertEquals(NdefErrorType.NOT_FOUND, mErrorCaptor.getValue().errorType);
    }

    /**
     * Test that Nfc.cancelAllWatches() is failing with NOT_FOUND error if there are no active
     * watch opeartions.
     */
    @Test
    @Feature({"NFCTest"})
    public void testCancelAllWatchesWithNoWathcers() {
        TestNfcImpl nfc = new TestNfcImpl(mContext, mDelegate);
        mDelegate.invokeCallback();
        CancelAllWatchesResponse mockCallback = mock(CancelAllWatchesResponse.class);
        nfc.cancelAllWatches(mockCallback);

        verify(mockCallback).call(mErrorCaptor.capture());
        assertEquals(NdefErrorType.NOT_FOUND, mErrorCaptor.getValue().errorType);
    }

    /**
     * Test that when the tag in proximity is found to be not NDEF compatible, an error event will
     * be dispatched to the client and the pending push operation will also be ended with an error.
     */
    @Test
    @Feature({"NFCTest"})
    public void testNonNdefCompatibleTagFound() {
        TestNfcImpl nfc = new TestNfcImpl(mContext, mDelegate);
        mDelegate.invokeCallback();
        nfc.setClient(mNfcClient);
        // Prepare at least one watcher, otherwise the error won't be notified.
        WatchResponse mockWatchCallback = mock(WatchResponse.class);
        nfc.watch(createNdefScanOptions(), mNextWatchId, mockWatchCallback);
        // Start a push.
        PushResponse mockCallback = mock(PushResponse.class);
        nfc.push(createMojoNdefMessage(), createNdefWriteOptions(), mockCallback);

        // Pass null tag handler to simulate that the tag is not NDEF compatible.
        nfc.processPendingOperationsForTesting(null);

        // An error is notified.
        verify(mNfcClient, times(1)).onError(mErrorCaptor.capture());
        assertNotNull(mErrorCaptor.getValue());
        assertEquals(NdefErrorType.NOT_SUPPORTED, mErrorCaptor.getValue().errorType);
        // No watch.
        verify(mNfcClient, times(0))
                .onWatch(mOnWatchCallbackCaptor.capture(), nullable(String.class),
                        any(NdefMessage.class));

        // The pending push failed with the correct error.
        verify(mockCallback).call(mErrorCaptor.capture());
        assertNotNull(mErrorCaptor.getValue());
        assertEquals(NdefErrorType.NOT_SUPPORTED, mErrorCaptor.getValue().errorType);
    }

    /**
     * Test that when the tag in proximity is found to be not NDEF compatible, an error event will
     * not be dispatched to the client if there is no watcher present.
     */
    @Test
    @Feature({"NFCTest"})
    public void testNonNdefCompatibleTagFoundWithoutWatcher() {
        TestNfcImpl nfc = new TestNfcImpl(mContext, mDelegate);
        mDelegate.invokeCallback();
        nfc.setClient(mNfcClient);

        // Pass null tag handler to simulate that the tag is not NDEF compatible.
        nfc.processPendingOperationsForTesting(null);

        // An error is NOT notified.
        verify(mNfcClient, times(0)).onError(mErrorCaptor.capture());
        // No watch.
        verify(mNfcClient, times(0))
                .onWatch(mOnWatchCallbackCaptor.capture(), nullable(String.class),
                        any(NdefMessage.class));
    }

    /**
     * Test that when tag is disconnected during read operation, IllegalStateException is handled.
     */
    @Test
    @Feature({"NFCTest"})
    public void testTagDisconnectedDuringRead() throws IOException, FormatException {
        TestNfcImpl nfc = new TestNfcImpl(mContext, mDelegate);
        mDelegate.invokeCallback();
        nfc.setClient(mNfcClient);
        WatchResponse mockWatchCallback = mock(WatchResponse.class);
        nfc.watch(createNdefScanOptions(), mNextWatchId, mockWatchCallback);

        // Force read operation to fail
        doThrow(IllegalStateException.class).when(mNfcTagHandler).read();

        // Mocks 'NFC tag found' event.
        nfc.processPendingOperationsForTesting(mNfcTagHandler);

        // Check that the watch was not triggered but an error was dispatched to the client.
        verify(mNfcClient, times(0))
                .onWatch(mOnWatchCallbackCaptor.capture(), nullable(String.class),
                        any(NdefMessage.class));
        verify(mNfcClient, times(1)).onError(mErrorCaptor.capture());
        assertNotNull(mErrorCaptor.getValue());
        assertEquals(NdefErrorType.IO_ERROR, mErrorCaptor.getValue().errorType);
    }

    /**
     * Test that when tag is disconnected during write operation, IllegalStateException is handled.
     */
    @Test
    @Feature({"NFCTest"})
    public void testTagDisconnectedDuringWrite() throws IOException, FormatException {
        TestNfcImpl nfc = new TestNfcImpl(mContext, mDelegate);
        mDelegate.invokeCallback();
        PushResponse mockCallback = mock(PushResponse.class);

        // Force write operation to fail
        doThrow(IllegalStateException.class)
                .when(mNfcTagHandler)
                .write(any(android.nfc.NdefMessage.class));
        nfc.push(createMojoNdefMessage(), createNdefWriteOptions(), mockCallback);
        nfc.processPendingOperationsForTesting(mNfcTagHandler);
        verify(mockCallback).call(mErrorCaptor.capture());

        // Test that correct error is returned.
        assertNotNull(mErrorCaptor.getValue());
        assertEquals(NdefErrorType.IO_ERROR, mErrorCaptor.getValue().errorType);
    }

    /**
     * Test that multiple Nfc.push() invocations do not disable reader mode.
     */
    @Test
    @Feature({"NFCTest"})
    public void testPushMultipleInvocations() {
        TestNfcImpl nfc = new TestNfcImpl(mContext, mDelegate);
        mDelegate.invokeCallback();

        PushResponse mockCallback1 = mock(PushResponse.class);
        PushResponse mockCallback2 = mock(PushResponse.class);
        nfc.push(createMojoNdefMessage(), createNdefWriteOptions(), mockCallback1);
        nfc.push(createMojoNdefMessage(), createNdefWriteOptions(), mockCallback2);

        verify(mNfcAdapter, times(1))
                .enableReaderMode(any(Activity.class), any(ReaderCallback.class), anyInt(),
                        (Bundle) isNull());
        verify(mNfcAdapter, times(0)).disableReaderMode(mActivity);

        verify(mockCallback1).call(mErrorCaptor.capture());
        assertNotNull(mErrorCaptor.getValue());
        assertEquals(NdefErrorType.OPERATION_CANCELLED, mErrorCaptor.getValue().errorType);
    }

    /**
     * Test that reader mode is disabled and push operation is cancelled with correct error code.
     */
    @Test
    @Feature({"NFCTest"})
    public void testPushInvocationWithCancel() {
        TestNfcImpl nfc = new TestNfcImpl(mContext, mDelegate);
        mDelegate.invokeCallback();
        PushResponse mockCallback = mock(PushResponse.class);

        nfc.push(createMojoNdefMessage(), createNdefWriteOptions(), mockCallback);

        verify(mNfcAdapter, times(1))
                .enableReaderMode(any(Activity.class), any(ReaderCallback.class), anyInt(),
                        (Bundle) isNull());

        CancelPushResponse mockCancelPushCallback = mock(CancelPushResponse.class);
        nfc.cancelPush(mockCancelPushCallback);

        // Reader mode is disabled.
        verify(mNfcAdapter, times(1)).disableReaderMode(mActivity);

        // Test that correct error is returned.
        verify(mockCallback).call(mErrorCaptor.capture());
        assertNotNull(mErrorCaptor.getValue());
        assertEquals(NdefErrorType.OPERATION_CANCELLED, mErrorCaptor.getValue().errorType);
    }

    /**
     * Test that reader mode is disabled and two push operations are cancelled with correct
     * error code.
     */
    @Test
    @Feature({"NFCTest"})
    public void testTwoPushInvocationsWithCancel() {
        TestNfcImpl nfc = new TestNfcImpl(mContext, mDelegate);
        mDelegate.invokeCallback();

        PushResponse mockCallback1 = mock(PushResponse.class);
        PushResponse mockCallback2 = mock(PushResponse.class);
        nfc.push(createMojoNdefMessage(), createNdefWriteOptions(), mockCallback1);
        nfc.push(createMojoNdefMessage(), createNdefWriteOptions(), mockCallback2);

        verify(mNfcAdapter, times(1))
                .enableReaderMode(any(Activity.class), any(ReaderCallback.class), anyInt(),
                        (Bundle) isNull());

        // The second push should cancel the first push.
        verify(mockCallback1).call(mErrorCaptor.capture());
        assertNotNull(mErrorCaptor.getValue());
        assertEquals(NdefErrorType.OPERATION_CANCELLED, mErrorCaptor.getValue().errorType);

        // Cancel the second push.
        CancelPushResponse mockCancelPushCallback = mock(CancelPushResponse.class);
        nfc.cancelPush(mockCancelPushCallback);

        // Reader mode is disabled after cancelPush is invoked.
        verify(mNfcAdapter, times(1)).disableReaderMode(mActivity);

        // Test that correct error is returned.
        verify(mockCallback2).call(mErrorCaptor.capture());
        assertNotNull(mErrorCaptor.getValue());
        assertEquals(NdefErrorType.OPERATION_CANCELLED, mErrorCaptor.getValue().errorType);
    }

    /**
     * Test that reader mode is not disabled when there is an active watch operation and push
     * operation operation is cancelled.
     */
    @Test
    @Feature({"NFCTest"})
    public void testCancelledPushDontDisableReaderMode() {
        TestNfcImpl nfc = new TestNfcImpl(mContext, mDelegate);
        mDelegate.invokeCallback();
        WatchResponse mockWatchCallback = mock(WatchResponse.class);
        nfc.watch(createNdefScanOptions(), mNextWatchId, mockWatchCallback);

        PushResponse mockPushCallback = mock(PushResponse.class);
        nfc.push(createMojoNdefMessage(), createNdefWriteOptions(), mockPushCallback);

        verify(mNfcAdapter, times(1))
                .enableReaderMode(any(Activity.class), any(ReaderCallback.class), anyInt(),
                        (Bundle) isNull());

        CancelPushResponse mockCancelPushCallback = mock(CancelPushResponse.class);
        nfc.cancelPush(mockCancelPushCallback);

        // Push was cancelled with OPERATION_CANCELLED.
        verify(mockPushCallback).call(mErrorCaptor.capture());
        assertNotNull(mErrorCaptor.getValue());
        assertEquals(NdefErrorType.OPERATION_CANCELLED, mErrorCaptor.getValue().errorType);

        verify(mNfcAdapter, times(0)).disableReaderMode(mActivity);

        CancelAllWatchesResponse mockCancelCallback = mock(CancelAllWatchesResponse.class);
        nfc.cancelAllWatches(mockCancelCallback);

        // Check that cancel request was successfuly completed.
        verify(mockCancelCallback).call(mErrorCaptor.capture());
        assertNull(mErrorCaptor.getValue());

        // Reader mode is disabled when there are no pending push / watch operations.
        verify(mNfcAdapter, times(1)).disableReaderMode(mActivity);
    }

    /**
     * Test that Nfc.push() succeeds for NFC messages with EMPTY records.
     */
    @Test
    @Feature({"NFCTest"})
    public void testPushWithEmptyRecord() {
        TestNfcImpl nfc = new TestNfcImpl(mContext, mDelegate);
        mDelegate.invokeCallback();
        PushResponse mockCallback = mock(PushResponse.class);

        // Create message with empty record.
        NdefRecord emptyNdefRecord = new NdefRecord();
        emptyNdefRecord.category = NdefRecordTypeCategory.STANDARDIZED;
        emptyNdefRecord.recordType = NdefMessageUtils.RECORD_TYPE_EMPTY;
        NdefMessage ndefMessage = createMojoNdefMessage(emptyNdefRecord);

        nfc.push(ndefMessage, createNdefWriteOptions(), mockCallback);
        nfc.processPendingOperationsForTesting(mNfcTagHandler);
        verify(mockCallback).call(mErrorCaptor.capture());
        assertNull(mErrorCaptor.getValue());
    }

    /**
     * Creates NdefWriteOptions with default values.
     */
    private NdefWriteOptions createNdefWriteOptions() {
        NdefWriteOptions pushOptions = new NdefWriteOptions();
        pushOptions.ignoreRead = false;
        pushOptions.overwrite = true;
        return pushOptions;
    }

    private NdefScanOptions createNdefScanOptions() {
        NdefScanOptions options = new NdefScanOptions();
        return options;
    }

    private NdefMessage createMojoNdefMessage() {
        NdefMessage message = new NdefMessage();
        message.data = new NdefRecord[1];

        NdefRecord nfcRecord = new NdefRecord();
        nfcRecord.category = NdefRecordTypeCategory.STANDARDIZED;
        nfcRecord.recordType = NdefMessageUtils.RECORD_TYPE_TEXT;
        nfcRecord.encoding = ENCODING_UTF8;
        nfcRecord.lang = LANG_EN_US;
        nfcRecord.data = ApiCompatibilityUtils.getBytesUtf8(TEST_TEXT);
        message.data[0] = nfcRecord;
        return message;
    }

    private NdefMessage createMojoNdefMessage(NdefRecord record) {
        NdefMessage message = new NdefMessage();
        message.data = new NdefRecord[1];
        message.data[0] = record;
        return message;
    }

    private android.nfc.NdefMessage createNdefMessageWithRecordId(String id)
            throws UnsupportedEncodingException {
        return new android.nfc.NdefMessage(NdefMessageUtils.createPlatformUrlRecord(
                ApiCompatibilityUtils.getBytesUtf8(TEST_URL), id, false /* isAbsUrl */));
    }
}
