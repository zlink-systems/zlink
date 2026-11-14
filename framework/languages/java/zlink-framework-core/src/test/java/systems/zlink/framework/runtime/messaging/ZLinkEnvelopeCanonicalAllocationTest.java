package systems.zlink.framework.runtime.messaging;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.testutil.PerCallAllocation;

import java.nio.ByteBuffer;
import java.util.Map;
import java.util.function.Supplier;

final class ZLinkEnvelopeCanonicalAllocationTest {
    @Test
    void nativeRequestAndReplyStringsUseOneSmallTemporaryBuffer() {
        for (int kind :
                new int[] {ZLinkChannelEnvelope.KIND_REQUEST, ZLinkChannelEnvelope.KIND_RESPONSE}) {
            var header =
                    new ZLinkChannelEnvelope.Header(
                            kind,
                            "allocation-channel",
                            "AllocationMessage",
                            "application/x-protobuf",
                            "00112233445566778899aabbccddeeff",
                            null,
                            null,
                            null,
                            null,
                            null,
                            Map.of(),
                            null,
                            null);
            byte[] encoded = ZLinkChannelEnvelope.encodeHeaderBytes(header);
            ByteBuffer heap = ByteBuffer.wrap(encoded);
            ByteBuffer storage = ByteBuffer.allocateDirect(encoded.length + 11);
            storage.position(7).put(encoded).flip().position(7);
            ByteBuffer nativeInput = storage.asReadOnlyBuffer();
            assertEquals(header, ZLinkChannelEnvelope.decodeHeader(nativeInput, false));
            assertEquals(nativeInput.limit(), nativeInput.position());
            Supplier<Object> alternating =
                    new Supplier<>() {
                        private boolean useNative;

                        @Override
                        public Object get() {
                            ByteBuffer input = useNative ? nativeInput : heap;
                            input.position(useNative ? 7 : 0);
                            useNative = !useNative;
                            return ZLinkChannelEnvelope.decodeHeader(input, false);
                        }
                    };
            int calls = 2_000;
            var allocation = PerCallAllocation.minAlternatingWindow(alternating, calls, 20_000);
            long extra = allocation.measuredBytes() - allocation.baselineBytes();
            // Native input may need one 64-byte scratch array (80 bytes including its header).
            // It must not allocate a character array for every decoded field.
            assertTrue(
                    extra <= 96L * calls,
                    "kind="
                            + kind
                            + ": native decode allocated "
                            + (extra / calls)
                            + " extra bytes per header; expected at most 96");
        }
    }

    @Test
    void laterNativeFieldsAndMessagesCannotMutateDecodedStrings() {
        var expected =
                new ZLinkChannelEnvelope.Header(
                        ZLinkChannelEnvelope.KIND_RESPONSE,
                        "short",
                        "longer-message-name",
                        "application/x-protobuf",
                        "00112233445566778899aabbccddeeff",
                        null,
                        "topic-" + "x".repeat(300),
                        null,
                        null,
                        null,
                        Map.of("tenant", "north"),
                        null,
                        null);
        byte[] encoded = ZLinkChannelEnvelope.encodeHeaderBytes(expected);
        ByteBuffer storage = ByteBuffer.allocateDirect(encoded.length);
        storage.put(encoded).flip();
        var decoded = ZLinkChannelEnvelope.decodeHeader(storage.asReadOnlyBuffer(), false);
        assertEquals(expected, decoded);
        storage.clear();
        while (storage.hasRemaining()) storage.put((byte) '!');
        assertEquals(expected, decoded);
    }
}
