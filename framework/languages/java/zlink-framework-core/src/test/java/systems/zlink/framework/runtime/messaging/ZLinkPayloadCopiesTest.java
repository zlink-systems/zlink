package systems.zlink.framework.runtime.messaging;

import static org.junit.jupiter.api.Assertions.*;

import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.internal.messaging.ZLinkEncodedPayloadAccess;
import systems.zlink.framework.testutil.PerCallAllocation;

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
        ZLinkMessage[] measuredMessage = new ZLinkMessage[1];
        long allocated =
                PerCallAllocation.minWindow(
                        () -> {
                            measuredMessage[0] = ZLinkMessage.fromEncoded(payload, serializer);
                            return measuredMessage[0];
                        },
                        1,
                        2_000);
        var message = measuredMessage[0];
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
        int[] sentSize = new int[1];
        long allocated =
                PerCallAllocation.minWindow(
                        () -> {
                            try (Message sent = ZLinkMessagePayloads.message(message, serializer)) {
                                sentSize[0] = sent.size();
                                return sent;
                            }
                        },
                        1,
                        2_000);
        System.out.println("internal send allocated bytes: " + allocated);
        assertEquals(PAYLOAD_SIZE, sentSize[0]);
        assertTrue(allocated < PAYLOAD_SIZE, "internal send allocated " + allocated + " bytes");
    }
}
