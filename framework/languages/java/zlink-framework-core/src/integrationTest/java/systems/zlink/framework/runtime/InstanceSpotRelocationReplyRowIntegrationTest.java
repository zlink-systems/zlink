package systems.zlink.framework.runtime;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.locationprovider.ZLinkStoreScanPageResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationOutcome;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;

import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.logging.Handler;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

final class InstanceSpotRelocationReplyRowIntegrationTest
        extends InstanceSpotHostRelocationFixture {
    InstanceSpotRelocationReplyRowIntegrationTest() {
        super(true);
    }

    @Test
    void requestHeldDuringRestoreRepliesFromTheRelocatedInstance() throws Exception {
        Room.additionalHandlers = List.of(CapturedRequestHandler.class);
        Room.releaseRestore = new CompletableFuture<>();
        try (var source = start(SOURCE);
                var caller = start(CLIENT)) {
            var client = activateFrom(caller);
            try (var target = start(TARGET)) {
                awaitPeer(source, TARGET);
                awaitPeer(caller, TARGET);
                var move = relocate(source);
                var received = new CompletableFuture<Void>();
                var flow = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
                var listener =
                        new Handler() {
                            @Override
                            public void publish(LogRecord record) {
                                if (record.getMessage()
                                        .contains(
                                                "phase=received surface=spot kind=request packet=CapturedRequest spot=room ")) {
                                    received.complete(null);
                                }
                            }

                            @Override
                            public void flush() {}

                            @Override
                            public void close() {}
                        };
                CompletionStage<String> reply;
                flow.addHandler(listener);
                try {
                    await(Room.restoreEntered);
                    assertEquals(SOURCE, authority().allocation().descriptor().rid());
                    reply =
                            client.outbound()
                                    .requestToSpot(SPOT_ID, new CapturedRequest("node"))
                                    .instanceSpot(SPOT_TYPE)
                                    .inMesh(MESH)
                                    .timeout(DEADLINE)
                                    .submit(String.class);
                    await(received);
                    assertFalse(reply.toCompletableFuture().isDone());
                } finally {
                    flow.removeHandler(listener);
                    Room.releaseRestore.complete(null);
                }
                var result = await(move);
                assertEquals(
                        ZLinkFrameworkRelocationOutcome.RELOCATED,
                        result.outcome(),
                        result::toString);
                var rows =
                        assertInstanceOf(
                                        ZLinkStoreScanPageResult.class,
                                        await(
                                                store.scan(
                                                        new ZLinkStoreScanRequest(
                                                                "$zlink/relocation-reply/",
                                                                null,
                                                                10),
                                                        () -> false)))
                                .value()
                                .items();
                assertEquals(1, rows.size(), "the held request must contribute one commit row");
                assertEquals("target", await(reply));
            }
        }
    }

    public record CapturedRequest(String query) {}

    public static final class CapturedRequestHandler
            implements ZLinkSpotRequestHandler<Room, CapturedRequest, String> {
        @Override
        public CompletionStage<String> handle(Room spot, CapturedRequest request) {
            return CompletableFuture.completedFuture(
                    new String(spot.context().nodeRid().toBytes(), StandardCharsets.UTF_8));
        }
    }
}
