package systems.zlink.framework.runtime.messaging;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotSame;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.service.ZLinkFrameworkMultipartView;
import systems.zlink.framework.testutil.PerCallAllocation;

import java.nio.ByteBuffer;
import java.nio.ReadOnlyBufferException;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.function.Supplier;

final class ZLinkEnvelopeHeaderCacheHotPathTest {
    private static final String FLOW_ID = "018f2f1d-5d52-7b70-8f08-13fecf6f6abc";

    @Test
    void repeatableDecodeUsesExactByteCacheAndConsumesTheBuffer() {
        byte[] bytes =
                ZLinkChannelEnvelope.encodeHeaderBytes(
                        simpleHeader(ZLinkChannelEnvelope.KIND_COMMAND, "cache-hit", "Update"));
        ByteBuffer firstBuffer = ByteBuffer.wrap(bytes);
        ZLinkChannelEnvelope.Header first = ZLinkChannelEnvelope.decodeHeader(firstBuffer, false);
        ByteBuffer secondBuffer = ByteBuffer.wrap(bytes);
        ZLinkChannelEnvelope.Header second = ZLinkChannelEnvelope.decodeHeader(secondBuffer, true);

        assertSame(first, second);
        assertEquals(firstBuffer.limit(), firstBuffer.position());
        assertEquals(secondBuffer.limit(), secondBuffer.position());
    }

    @Test
    void dynamicHeadersDoNotEnterTheDecodedCache() {
        assertNotCached(
                new ZLinkChannelEnvelope.Header(
                        ZLinkChannelEnvelope.KIND_REQUEST,
                        "cache-dynamic-correlation",
                        "Request",
                        ZLinkChannelEnvelope.DEFAULT_CONTENT_TYPE,
                        "correlation-1",
                        null,
                        null,
                        null,
                        null,
                        null,
                        Map.of(),
                        null,
                        null),
                false);
        assertNotCached(
                new ZLinkChannelEnvelope.Header(
                        ZLinkChannelEnvelope.KIND_COMMAND,
                        "cache-dynamic-deadline",
                        "Update",
                        ZLinkChannelEnvelope.DEFAULT_CONTENT_TYPE,
                        null,
                        "2030-01-02T03:04:05Z",
                        null,
                        null,
                        null,
                        null,
                        Map.of(),
                        null,
                        null),
                false);
        assertNotCached(
                new ZLinkChannelEnvelope.Header(
                        ZLinkChannelEnvelope.KIND_COMMAND,
                        "cache-dynamic-flow",
                        "Update",
                        ZLinkChannelEnvelope.DEFAULT_CONTENT_TYPE,
                        null,
                        null,
                        null,
                        null,
                        null,
                        null,
                        Map.of(),
                        FLOW_ID,
                        ZLinkFlowOrigin.APPLICATION),
                false);
        assertNotCached(
                new ZLinkChannelEnvelope.Header(
                        ZLinkChannelEnvelope.KIND_COMMAND,
                        "cache-dynamic-metadata",
                        "Update",
                        ZLinkChannelEnvelope.DEFAULT_CONTENT_TYPE,
                        null,
                        null,
                        null,
                        null,
                        null,
                        null,
                        Map.of("tenant", "blue"),
                        null,
                        null),
                false);
    }

    @Test
    void headersOver1024BytesBypassTheDecodedCache() {
        byte[] simple =
                ZLinkChannelEnvelope.encodeHeaderBytes(
                        simpleHeader(ZLinkChannelEnvelope.KIND_COMMAND, "cache-large", "Update"));
        String withExtension =
                new String(simple, StandardCharsets.UTF_8).replaceFirst("}$", "")
                        + ",\"extension\":\""
                        + "x".repeat(1100)
                        + "\"}";
        byte[] bytes = withExtension.getBytes(StandardCharsets.UTF_8);
        assertTrue(bytes.length > 1024);

        ZLinkChannelEnvelope.Header first =
                ZLinkChannelEnvelope.decodeHeader(ByteBuffer.wrap(bytes), false);
        ZLinkChannelEnvelope.Header second =
                ZLinkChannelEnvelope.decodeHeader(ByteBuffer.wrap(bytes), false);

        assertNotSame(first, second);
    }

    @Test
    void malformedHeaderKeepsTheSameErrorOnRepeatedDecode() {
        byte[] malformed = "{\"formatMarker\":242,\"kind\":3}".getBytes(StandardCharsets.UTF_8);

        ZLinkFrameworkException first =
                assertThrows(
                        ZLinkFrameworkException.class,
                        () -> ZLinkChannelEnvelope.decodeHeader(ByteBuffer.wrap(malformed), false));
        ZLinkFrameworkException second =
                assertThrows(
                        ZLinkFrameworkException.class,
                        () -> ZLinkChannelEnvelope.decodeHeader(ByteBuffer.wrap(malformed), false));

        assertEquals(first.getClass(), second.getClass());
        assertEquals(first.kind(), second.kind());
        assertEquals(first.getMessage(), second.getMessage());
    }

    @Test
    void simpleEncodingMatchesCanonicalBytesAndExposesAnOwnedArray() {
        ZLinkChannelEnvelope.Header command =
                simpleHeader(ZLinkChannelEnvelope.KIND_COMMAND, "orders", "PlaceOrder");
        String commandGolden =
                "{\"formatMarker\":242,\"flowId\":null,\"flowOrigin\":null,\"kind\":3,"
                        + "\"channelName\":\"orders\",\"messageName\":\"PlaceOrder\","
                        + "\"contentType\":\"application/json\",\"correlationId\":null,"
                        + "\"deadline\":null,\"topic\":null,\"errorCode\":null,"
                        + "\"errorMessage\":null,\"source\":null,\"metadata\":{}}";
        assertGolden(command, commandGolden);

        ZLinkChannelEnvelope.Header publish =
                new ZLinkChannelEnvelope.Header(
                        ZLinkChannelEnvelope.KIND_PUBLISH,
                        "events",
                        "OrderPlaced",
                        "application/x-event",
                        null,
                        null,
                        null,
                        null,
                        null,
                        null,
                        Map.of(),
                        null,
                        null);
        String publishGolden =
                "{\"formatMarker\":242,\"flowId\":null,\"flowOrigin\":null,\"kind\":4,"
                        + "\"channelName\":\"events\",\"messageName\":\"OrderPlaced\","
                        + "\"contentType\":\"application/x-event\",\"correlationId\":null,"
                        + "\"deadline\":null,\"topic\":null,\"errorCode\":null,"
                        + "\"errorMessage\":null,\"source\":null,\"metadata\":{}}";
        assertGolden(publish, publishGolden);

        byte[] exposed = ZLinkChannelEnvelope.encodeHeaderBytes(command);
        exposed[0] = '!';
        assertArrayEquals(
                commandGolden.getBytes(StandardCharsets.UTF_8),
                ZLinkChannelEnvelope.encodeHeaderBytes(command));
        try (Message encoded = ZLinkChannelEnvelope.encodeHeader(command)) {
            assertArrayEquals(
                    commandGolden.getBytes(StandardCharsets.UTF_8), encoded.toByteArray());
        }
    }

    @Test
    void cachesStayWithinTheDecodedAndSimpleHeaderBounds() throws Exception {
        for (int index = 0; index < 70; index++) {
            byte[] bytes =
                    ZLinkChannelEnvelope.encodeHeaderBytes(
                            simpleHeader(
                                    ZLinkChannelEnvelope.KIND_COMMAND,
                                    "decode-bound-" + index,
                                    "Update"));
            ZLinkChannelEnvelope.decodeHeader(ByteBuffer.wrap(bytes), false);
        }
        assertEquals(64, ZLinkChannelEnvelope.decodedHeaderCacheSize());

        for (int index = 0; index < 4100; index++) {
            try (Message ignored =
                    ZLinkChannelEnvelope.encodeHeader(
                            simpleHeader(
                                    ZLinkChannelEnvelope.KIND_COMMAND,
                                    "encode-bound-" + index,
                                    "Update"))) {}
        }
        assertEquals(4096, ZLinkChannelEnvelope.simpleHeaderCacheSize());
    }

    @Test
    void warmHeaderPathsMatchMessageAllocationAndDoNotWaitForMutationLane() throws Exception {
        ZLinkChannelEnvelope.Header header =
                simpleHeader(ZLinkChannelEnvelope.KIND_COMMAND, "cache-allocation", "Update");
        byte[] bytes = ZLinkChannelEnvelope.encodeHeaderBytes(header);
        ZLinkChannelEnvelope.Header decoded =
                ZLinkChannelEnvelope.decodeHeader(ByteBuffer.wrap(bytes), false);
        for (int index = 0; index < 128; index++) {
            try (Message ignored = ZLinkChannelEnvelope.encodeHeader(header)) {
                ZLinkChannelEnvelope.decodeHeader(ByteBuffer.wrap(bytes), false);
            }
        }

        int calls = 1024;
        HeaderMessageAllocationProbe allocationProbe =
                new HeaderMessageAllocationProbe(header, bytes);
        allocationProbe.alternate();
        PerCallAllocation.AllocationComparison allocationComparison =
                PerCallAllocation.minAlternatingWindow(allocationProbe, calls, calls);
        allocationProbe.useDirectMessage();
        long directMessageBytes = allocationComparison.baselineBytes();
        long encodedHeaderBytes = allocationComparison.measuredBytes();
        ByteBuffer reusable = ByteBuffer.wrap(bytes);
        long decodedHeaderBytes =
                PerCallAllocation.minWindow(
                        () -> {
                            reusable.position(0);
                            return ZLinkChannelEnvelope.decodeHeader(reusable, false);
                        },
                        calls,
                        calls);
        System.out.printf(
                "warm encode=%d B/call, owned message=%d B/call, warm decode=%d B/call%n",
                encodedHeaderBytes / calls, directMessageBytes / calls, decodedHeaderBytes / calls);
        assertTrue(
                encodedHeaderBytes <= directMessageBytes + calls * 16L,
                "warm header encoding allocated more than the owned Message baseline");
        assertTrue(decodedHeaderBytes <= calls * 16L, "warm decode allocated per header hit");

        ZLinkStateLane lane = ZLinkChannelEnvelope.headerCacheLane();
        CompletableFuture<Void> entered = new CompletableFuture<>();
        CompletableFuture<Void> release = new CompletableFuture<>();
        var blocked =
                lane.runAsync(
                        () -> {
                            entered.complete(null);
                            release.join();
                            return null;
                        });
        entered.get(3, TimeUnit.SECONDS);
        ByteBuffer hitBuffer = ByteBuffer.wrap(bytes);
        CompletableFuture<Void> lookup =
                CompletableFuture.runAsync(
                        () -> {
                            try (Message message = ZLinkChannelEnvelope.encodeHeader(header)) {
                                assertArrayEquals(bytes, message.toByteArray());
                            }
                            assertSame(
                                    decoded, ZLinkChannelEnvelope.decodeHeader(hitBuffer, false));

                            ZLinkChannelEnvelope.Header coldHeader =
                                    simpleHeader(
                                            ZLinkChannelEnvelope.KIND_COMMAND,
                                            "cache-lane-miss",
                                            "Update");
                            byte[] coldBytes = ZLinkChannelEnvelope.encodeHeaderBytes(coldHeader);
                            try (Message message = ZLinkChannelEnvelope.encodeHeader(coldHeader)) {
                                assertArrayEquals(coldBytes, message.toByteArray());
                            }
                            ZLinkChannelEnvelope.Header firstMiss =
                                    ZLinkChannelEnvelope.decodeHeader(
                                            ByteBuffer.wrap(coldBytes), false);
                            ZLinkChannelEnvelope.Header secondMiss =
                                    ZLinkChannelEnvelope.decodeHeader(
                                            ByteBuffer.wrap(coldBytes), false);
                            assertNotSame(firstMiss, secondMiss);
                        });
        try {
            lookup.get(3, TimeUnit.SECONDS);
        } finally {
            release.complete(null);
            blocked.toCompletableFuture().join();
            if (!lookup.isDone()) lookup.get(3, TimeUnit.SECONDS);
        }
    }

    @Test
    void sharedEncodedHeaderBytesAreExposedReadOnly() {
        ZLinkChannelEnvelope.Header command =
                simpleHeader(ZLinkChannelEnvelope.KIND_COMMAND, "orders", "ReadOnlyHeader");
        try (Message first = Message.from(new byte[] {1});
                Message second = Message.from(new byte[] {2})) {
            List<Message> firstParts = ZLinkChannelEnvelope.encode(command, first);
            List<Message> secondParts = ZLinkChannelEnvelope.encode(command, second);
            ByteBuffer header = ZLinkFrameworkMultipartView.buffer(firstParts, 0);
            assertTrue(header.isReadOnly());
            assertThrows(ReadOnlyBufferException.class, () -> header.put(0, (byte) '#'));
            assertEquals(
                    ZLinkFrameworkMultipartView.buffer(firstParts, 0),
                    ZLinkFrameworkMultipartView.buffer(secondParts, 0));
        }
    }

    private static void assertNotCached(ZLinkChannelEnvelope.Header header, boolean captureFlow) {
        byte[] bytes = ZLinkChannelEnvelope.encodeHeaderBytes(header);
        ZLinkChannelEnvelope.Header first =
                ZLinkChannelEnvelope.decodeHeader(ByteBuffer.wrap(bytes), captureFlow);
        ZLinkChannelEnvelope.Header second =
                ZLinkChannelEnvelope.decodeHeader(ByteBuffer.wrap(bytes), captureFlow);
        assertNotSame(first, second, header.messageName());
    }

    private static void assertGolden(ZLinkChannelEnvelope.Header header, String golden) {
        byte[] expected = golden.getBytes(StandardCharsets.UTF_8);
        assertArrayEquals(expected, ZLinkChannelEnvelope.encodeHeaderBytes(header));
        assertArrayEquals(expected, ZLinkChannelEnvelope.encodeHeaderBytes(header));
    }

    private static final class HeaderMessageAllocationProbe implements Supplier<Message> {
        private final ZLinkChannelEnvelope.Header header;
        private final byte[] bytes;
        private boolean encodeHeader;
        private boolean alternate;

        HeaderMessageAllocationProbe(ZLinkChannelEnvelope.Header header, byte[] bytes) {
            this.header = header;
            this.bytes = bytes;
        }

        void alternate() {
            alternate = true;
            encodeHeader = false;
        }

        void useDirectMessage() {
            alternate = false;
            encodeHeader = false;
        }

        @Override
        public Message get() {
            boolean encode = encodeHeader;
            if (alternate) encodeHeader = !encodeHeader;
            if (encode) {
                try (Message message = ZLinkChannelEnvelope.encodeHeader(header)) {
                    return message;
                }
            }
            try (Message message = Message.from(bytes)) {
                return message;
            }
        }
    }

    private static ZLinkChannelEnvelope.Header simpleHeader(int kind, String channel, String name) {
        return new ZLinkChannelEnvelope.Header(
                kind,
                channel,
                name,
                ZLinkChannelEnvelope.DEFAULT_CONTENT_TYPE,
                null,
                null,
                null,
                null,
                null,
                null,
                Map.of(),
                null,
                null);
    }
}
