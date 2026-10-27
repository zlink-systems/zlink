package systems.zlink.framework.runtime.messaging;

import static org.junit.jupiter.api.Assertions.*;

import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.internal.messaging.ZLinkEncodedPayloadAccess;

import java.lang.management.ManagementFactory;

final class ZLinkPayloadCopiesTest {
    private static final int PAYLOAD_SIZE = 4 * 1024 * 1024;
    private final ZLinkMessageSerializer serializer = new ZLinkJsonMessageSerializer();

    @Test
    void publicFactoryAndAccessorKeepArraysIsolated() {
        byte[] source = {1, 2, 3};
        var payload = ZLinkEncodedPayload.from(source);
        source[0] = 9;
        byte[] first = payload.bytes();
        assertArrayEquals(new byte[] {1, 2, 3}, first);
        first[1] = 9;
        assertArrayEquals(new byte[] {1, 2, 3}, payload.bytes());
        assertNotSame(first, payload.bytes());
    }

    @Test
    void internalOwnershipBridgeRetainsTheSameArray() {
        byte[] owned = new byte[PAYLOAD_SIZE];
        var payload = ZLinkEncodedPayloadAccess.fromOwned(owned);
        assertSame(owned, ZLinkEncodedPayloadAccess.borrow(payload));
        assertSame(owned, ZLinkEncodedPayloadAccess.borrow(payload));
        assertNotSame(owned, payload.bytes());
        assertThrows(NullPointerException.class, () -> ZLinkEncodedPayloadAccess.fromOwned(null));
        assertThrows(NullPointerException.class, () -> ZLinkEncodedPayloadAccess.borrow(null));
        assertThrows(
                IllegalStateException.class,
                () ->
                        ZLinkEncodedPayloadAccess.registerAccess(
                                new ZLinkEncodedPayloadAccess.PayloadAccess() {
                                    @Override
                                    public ZLinkEncodedPayload fromOwned(byte[] bytes) {
                                        throw new AssertionError(
                                                "Replacement access must not be used");
                                    }

                                    @Override
                                    public byte[] borrow(ZLinkEncodedPayload value) {
                                        throw new AssertionError(
                                                "Replacement access must not be used");
                                    }
                                }));
        assertSame(owned, ZLinkEncodedPayloadAccess.borrow(payload));
    }

    @Test
    void jsonSerializerAdoptsTheEncoderResult() {
        byte[] encoded = {1, 2, 3};
        var mapper =
                new ObjectMapper() {
                    @Override
                    public byte[] writeValueAsBytes(Object value) {
                        return encoded;
                    }
                };
        var selected = new ZLinkJsonMessageSerializer(mapper);
        assertSame(encoded, ZLinkEncodedPayloadAccess.borrow(selected.serialize(new Object())));
    }

    @Test
    void serializersKeepApplicationArraysIsolated() {
        for (var selected :
                new ZLinkMessageSerializer[] {serializer, new ZLinkStringMessageSerializer()}) {
            byte[] source = {1, 2, 3};
            var payload = selected.serialize(source);
            source[0] = 9;
            assertArrayEquals(new byte[] {1, 2, 3}, payload.bytes());
            byte[] decoded = selected.deserialize(payload, byte[].class);
            decoded[1] = 9;
            assertArrayEquals(new byte[] {1, 2, 3}, payload.bytes());
        }
    }

    @Test
    void fromEncodedDoesNotCopyPayloadArray() {
        var small = ZLinkEncodedPayload.from(new byte[] {1});
        for (int index = 0; index < 2_000; index++) {
            assertFalse(ZLinkMessage.fromEncoded(small, serializer).isEmpty());
        }
        var payload = ZLinkEncodedPayload.from(new byte[PAYLOAD_SIZE]);
        var allocations = allocations();
        long thread = Thread.currentThread().threadId();
        long before = allocations.getThreadAllocatedBytes(thread);
        var message = ZLinkMessage.fromEncoded(payload, serializer);
        long allocated = allocations.getThreadAllocatedBytes(thread) - before;
        System.out.println("fromEncoded allocated bytes: " + allocated);
        assertFalse(message.isEmpty());
        assertSame(payload, message.toEncodedPayload(serializer));
        assertTrue(allocated < PAYLOAD_SIZE, "fromEncoded allocated " + allocated + " bytes");
        assertTrue(
                ZLinkMessage.fromEncoded(ZLinkEncodedPayload.from(new byte[0]), serializer)
                        .isEmpty());
    }

    @Test
    void internalSendDoesNotCopyPayloadArray() {
        var small = ZLinkEncodedPayload.from(new byte[] {1});
        for (int index = 0; index < 2_000; index++) {
            try (Message ignored = ZLinkMessagePayloads.message(small)) {}
        }
        var payload = ZLinkEncodedPayload.from(new byte[PAYLOAD_SIZE]);
        var message = ZLinkMessage.fromEncoded(payload, serializer);
        var allocations = allocations();
        long thread = Thread.currentThread().threadId();
        long before = allocations.getThreadAllocatedBytes(thread);
        try (Message sent = ZLinkMessagePayloads.message(message, serializer)) {
            long allocated = allocations.getThreadAllocatedBytes(thread) - before;
            System.out.println("internal send allocated bytes: " + allocated);
            assertEquals(PAYLOAD_SIZE, sent.size());
            assertTrue(allocated < PAYLOAD_SIZE, "internal send allocated " + allocated + " bytes");
        }
    }

    private static com.sun.management.ThreadMXBean allocations() {
        var bean = (com.sun.management.ThreadMXBean) ManagementFactory.getThreadMXBean();
        assertTrue(bean.isThreadAllocatedMemorySupported());
        bean.setThreadAllocatedMemoryEnabled(true);
        return bean;
    }
}
