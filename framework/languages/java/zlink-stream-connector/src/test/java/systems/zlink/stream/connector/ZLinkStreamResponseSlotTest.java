package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import java.net.URI;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.Map;
import java.util.concurrent.CompletableFuture;

final class ZLinkStreamResponseSlotTest {
    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void unregisteredResponseSlotIsRejectedBeforePendingCompletion(boolean hasPending) {
        var config =
                ZLinkStreamConnectorConfiguration.from(
                        ZLinkStreamConnectorOptions.createDefault(URI.create("tcp://127.0.0.1:1")));
        var queue = new ZLinkStreamDispatchQueue();
        var actors = new ZLinkStreamActorRegistry(null, config, queue, ignored -> {});
        var pending = new ZLinkStreamPendingRequests();
        var result = new CompletableFuture<ZLinkStreamEncodedPayload>();
        if (hasPending)
            pending.add(
                    1,
                    "Reply",
                    result,
                    (payload, complete) -> complete.getAsBoolean(),
                    (failure, complete) -> complete.getAsBoolean());
        var dispatcher =
                new ZLinkStreamReceiveDispatcher(
                        config,
                        Map.of(),
                        queue,
                        pending,
                        new ZLinkStreamConnectorPayloadCodec(config),
                        ignored -> {},
                        ignored -> CompletableFuture.completedFuture(null),
                        ignored -> {},
                        actors);
        var header =
                new ZLinkStreamWireProtocol.Header(
                        ZLinkStreamWireProtocol.KIND_RESPONSE,
                        ZLinkStreamWireProtocol.CODEC_RAW,
                        ZLinkStreamWireProtocol.FLAG_HAS_REQUEST_SEQ
                                | ZLinkStreamWireProtocol.FLAG_HAS_ACTOR_SLOT,
                        1L,
                        "",
                        Map.of(),
                        null,
                        null,
                        0,
                        7);
        byte[] encoded = ZLinkStreamWireProtocol.encodeHeader(header);
        assertThrows(
                IllegalArgumentException.class, () -> dispatcher.dispatch(encoded, new byte[0]));
        assertFalse(result.isDone());
        byte[] id = "player".getBytes(StandardCharsets.UTF_8);
        actors.bound(
                ByteBuffer.allocate(4 + id.length)
                        .put((byte) 1)
                        .putShort((short) 7)
                        .put((byte) id.length)
                        .put(id)
                        .array());
        assertDoesNotThrow(() -> dispatcher.dispatch(encoded, new byte[0]));
        assertEquals(hasPending, result.isDone());
        if (hasPending) result.join().payload().close();
    }
}
