/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.contract;

import org.junit.jupiter.api.Test;
import systems.zlink.MessageSlotFixture;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.errors.ZlinkConfigException;
import systems.zlink.internal.ContractAccess;
import java.util.Arrays;
import java.util.concurrent.Executors;

import static org.junit.jupiter.api.Assertions.*;

class MessageCopySlotContractTest {
    @Test
    void warmCopiesReuseHeadersAndCloseEveryPayload() throws Exception {
        MessageSlotFixture.runProbe(getClass());
    }

    public static void main(String[] args) throws Exception {
        MessageSlotFixture counts = new MessageSlotFixture();
        try (Message source = Message.from(new byte[1024])) {
            source.copy().close();
            int opens = counts.opens.get();
            int closes = counts.scopeCloses.get();
            int payloads = counts.payloadCloses.values().stream().mapToInt(v -> v.get()).sum();
            for (int i = 0; i < 1000; i++) source.copy().close();
            int payloadDelta = counts.payloadCloses.values().stream()
                .mapToInt(v -> v.get()).sum() - payloads;
            System.out.printf("warm copies=1000 header opens=%d closes=%d payload closes=%d refcount=%d%n",
                counts.opens.get() - opens, counts.scopeCloses.get() - closes,
                payloadDelta, source.refCount());
            assertEquals(1000, payloadDelta);
            assertEquals(1, source.refCount());
            assertEquals(0, counts.opens.get() - opens, "warm copy header Arena creation");
            assertEquals(0, counts.scopeCloses.get() - closes, "warm copy header Arena close");
        }
        lifetime();
        overflow(counts, false);
        overflow(counts, true);
        failures(counts);
    }

    private static long address(Message message) {
        return ContractAccess.nativeMessageAddress(ContractAccess.messageNativeHandle(message));
    }

    private static void lifetime() throws Exception {
        for (int size : new int[] {0, 9, 29, 30, 1024}) {
            for (boolean sourceFirst : new boolean[] {false, true}) {
                byte[] expected = new byte[size];
                Arrays.fill(expected, (byte) 83);
                try (Message source = Message.from(expected);
                     Message target = new Message()) {
                    ContractAccess.messageSetMore(source, true);
                    try (Message copy = source.copy()) {
                        assertNotEquals(address(source), address(copy));
                        assertTrue(copy.more());
                        var view = copy.dataBuffer();
                        assertArrayEquals(expected, copy.data());
                        if (sourceFirst) {
                            source.close();
                            byte[] actual = new byte[size];
                            view.get(actual);
                            assertArrayEquals(expected, actual);
                            copy.move(target);
                            assertEquals(0, copy.size());
                            assertFalse(copy.more());
                            assertTrue(target.more());
                            assertArrayEquals(expected, target.data());
                            ContractAccess.messageResetReusable(copy);
                            assertEquals(0, copy.size());
                            try (var executor = Executors.newSingleThreadExecutor()) {
                                executor.submit(copy::close).get();
                            }
                        } else {
                            try (var executor = Executors.newVirtualThreadPerTaskExecutor()) {
                                executor.submit(copy::close).get();
                            }
                            assertArrayEquals(expected, source.data());
                        }
                    }
                }
            }
            try (Message source = Message.from(new byte[size]); Message copy = source.copy()) {
                ContractAccess.messageResetReusable(copy);
                assertEquals(0, copy.size());
                assertArrayEquals(new byte[size], source.data());
            }
        }
        System.out.println("copy lifetime sizes=0,9,29,30,1024 both close orders, views, more, move, reset, cross-thread PASS");
    }

    private static void overflow(MessageSlotFixture counts, boolean virtual) throws Exception {
        try (Message source = Message.from(new byte[1024])) {
            Message[] copies = new Message[4200];
            for (int i = 0; i < copies.length; i++) copies[i] = source.copy();
            assertEquals(4201, source.refCount());
            int scopes = counts.scopeCloses.get();
            int payloads = counts.payloadCloses.values().stream().mapToInt(v -> v.get()).sum();
            try (var executor = virtual ? Executors.newVirtualThreadPerTaskExecutor()
                    : Executors.newSingleThreadExecutor()) {
                executor.submit(() -> {
                    for (Message copy : copies) {
                        assertEquals(1024, copy.dataBuffer().remaining());
                        copy.close();
                    }
                }).get();
            }
            assertEquals(104, counts.scopeCloses.get() - scopes);
            assertEquals(4200, counts.payloadCloses.values().stream()
                .mapToInt(v -> v.get()).sum() - payloads);
            assertEquals(1, source.refCount());
            System.out.printf("overflow copies=4200 virtual=%s Arena closes=104 payload closes=4200 refcount=1 PASS%n", virtual);
        }
    }

    private static void failures(MessageSlotFixture counts) {
        try (Message source = Message.from(new byte[1024])) {
            for (String operation : new String[] {"init", "copy"}) {
                counts.failOperation = operation;
                ZlinkConfigException failure = assertThrows(ZlinkConfigException.class, source::copy);
                assertEquals(counts.failureErrno, failure.getNativeErrno());
                assertEquals(0, failure.getSuppressed().length);
                int closes = counts.closes(counts.failedHeader);
                assertEquals(counts.closesAtFailure + (operation.equals("copy") ? 1 : 0), closes);
                try (Message reused = new Message()) {
                    assertEquals(counts.failedHeader, address(reused), "failed copy must return its header");
                    assertEquals(closes, counts.closes(address(reused)));
                }
                assertEquals(1, source.refCount());
            }
            for (boolean nativeResult : new boolean[] {false, true}) {
                counts.failOperation = "copy";
                RuntimeException cleanup = new IllegalStateException("scripted payload close failure");
                counts.closeFailure = nativeResult ? null : cleanup;
                counts.rejectClose = nativeResult;
                ZlinkConfigException failure = assertThrows(ZlinkConfigException.class, source::copy);
                counts.closeFailure = null;
                counts.rejectClose = false;
                assertEquals(counts.failureErrno, failure.getNativeErrno());
                assertEquals(1, failure.getSuppressed().length);
                if (nativeResult) {
                    assertInstanceOf(ZlinkConfigException.class, failure.getSuppressed()[0]);
                } else {
                    assertSame(cleanup, failure.getSuppressed()[0]);
                }
                assertEquals(counts.closesAtFailure, counts.closes(counts.failedHeader));
                try (Message next = new Message()) {
                    assertNotEquals(counts.failedHeader, address(next), "failed native close must retain its header");
                }
                // Release the quarantined initialized header after checking non-reuse.
                counts.releaseQuarantinedHeader();
                assertEquals(1, source.refCount());
            }
        }
        System.out.println("init/copy failure cleanup, suppressed close failure and header quarantine PASS");
    }
}
