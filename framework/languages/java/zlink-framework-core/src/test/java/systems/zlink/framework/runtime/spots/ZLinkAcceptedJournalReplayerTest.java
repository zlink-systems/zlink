package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertSame;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

import java.util.ArrayList;
import java.util.EnumSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;

final class ZLinkAcceptedJournalReplayerTest {
    @ParameterizedTest
    @CsvSource({"spot,false", "actor,false", "actor,true"})
    void failureReachesTheExistingReplyOwnerAndPreservesTheQueuedTerminal(
            String lane, boolean relayFails) {
        var original = new IllegalStateException("adopted handler failed");
        var relayFailure = new IllegalStateException("reply relay failed");
        var spotHandler = new CompletableFuture<List<byte[]>>();
        var actorHandler = new CompletableFuture<Optional<byte[]>>();
        var relay = new CompletableFuture<Void>();
        var observed = new java.util.concurrent.atomic.AtomicReference<Throwable>();
        var replayer =
                new ZLinkAcceptedJournalReplayer(
                        record -> spotHandler,
                        record -> actorHandler,
                        new ZLinkAcceptedJournalReplayer.ReplyRelay() {
                            @Override
                            public CompletionStage<Void> completeSpot(
                                    ZLinkSpotAcceptedJournal.Record request,
                                    long sequence,
                                    List<byte[]> reply,
                                    Throwable failure) {
                                observed.set(failure);
                                return relay;
                            }

                            @Override
                            public CompletionStage<Void> completeActor(
                                    ZLinkActorAcceptedJournal.Record request,
                                    long sequence,
                                    Optional<byte[]> reply,
                                    Throwable failure) {
                                observed.set(failure);
                                return relay;
                            }
                        });
        byte[] record =
                lane.equals("spot")
                        ? ZLinkAcceptedJournalTestRecords.spot(
                                "source", "room-a", 41, "query", Map.of(), new byte[0])
                        : ZLinkAcceptedJournalTestRecords.actor(
                                "actor-a", 41, "query", Map.of(), new byte[0]);
        var terminal =
                replayer.replayFrozen(lane.equals("spot") ? "spot" : "actor:actor-a", record)
                        .toCompletableFuture();
        assertFalse(terminal.isDone());
        if (lane.equals("spot")) spotHandler.completeExceptionally(original);
        else actorHandler.completeExceptionally(original);
        assertSame(original, observed.get());
        assertFalse(
                terminal.isDone(), "the original terminal must retain its reply-owner completion");
        if (relayFails) relay.completeExceptionally(relayFailure);
        else relay.complete(null);
        assertSame(original, assertThrows(CompletionException.class, terminal::join).getCause());
        assertArrayEquals(
                relayFails ? new Throwable[] {relayFailure} : new Throwable[0],
                original.getSuppressed());
    }

    @Test
    void decodesSpotRequestAndRelaysReplyAfterDispatch() {
        List<String> order = new ArrayList<>();
        byte[] encoded;
        try (var received =
                new ZLinkBackendReceived(
                        systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult
                                .OK,
                        Optional.of(RoutingId.from("source")),
                        Optional.of("room-a"),
                        Optional.of(17L),
                        new byte[0],
                        ZLinkAcceptedJournalTestRecords.spot(
                                "room-a", "room-b", 17, "probe", Map.of(), new byte[] {1}),
                        List.of(Message.from(new byte[] {1})),
                        null,
                        () -> {})) {
            encoded = ZLinkSpotAcceptedJournal.encode(received);
        }
        var replayer =
                new ZLinkAcceptedJournalReplayer(
                        request -> {
                            order.add("dispatch");
                            return CompletableFuture.completedFuture(List.of(new byte[] {9}));
                        },
                        request -> CompletableFuture.completedFuture(Optional.empty()),
                        new ZLinkAcceptedJournalReplayer.ReplyRelay() {
                            @Override
                            public CompletionStage<Void> completeSpot(
                                    ZLinkSpotAcceptedJournal.Record request,
                                    long acceptedSequence,
                                    List<byte[]> reply,
                                    Throwable failure) {
                                order.add("reply:" + request.requestSequence().orElseThrow());
                                assertEquals(1, acceptedSequence);
                                assertArrayEquals(new byte[] {9}, reply.get(0));
                                return CompletableFuture.completedFuture(null);
                            }

                            @Override
                            public CompletionStage<Void> completeActor(
                                    ZLinkActorAcceptedJournal.Record request,
                                    long acceptedSequence,
                                    Optional<byte[]> reply,
                                    Throwable failure) {
                                return CompletableFuture.failedFuture(
                                        new AssertionError("unexpected Actor reply"));
                            }
                        });

        replayer.replay("spot", new ZLinkSerialExecutionQueue.QueuedRecord(1, encoded))
                .toCompletableFuture()
                .join();

        assertEquals(List.of("dispatch", "reply:17"), order);
    }

    @Test
    void actorLaneMustMatchEncodedActorAndPreservesHeader() {
        var header =
                new ZLinkStreamHeader(
                        ZLinkStreamMessageKind.REQUEST,
                        ZLinkStreamCodec.RAW,
                        EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                        Optional.of(41L),
                        "probe",
                        Map.of());
        byte[] encoded;
        try (Message payload = Message.from(new byte[] {3})) {
            encoded =
                    ZLinkActorAcceptedJournal.encode(
                            "actor-a",
                            header,
                            payload,
                            ZLinkAcceptedJournalTestRecords.actor(
                                    "actor-a", 41, "probe", Map.of(), new byte[] {3}));
        }
        var replayer =
                new ZLinkAcceptedJournalReplayer(
                        request -> CompletableFuture.completedFuture(List.of()),
                        request -> {
                            assertEquals("actor-a", request.actorId());
                            assertEquals(41L, request.header().requestSequence().orElseThrow());
                            return CompletableFuture.completedFuture(Optional.of(new byte[] {7}));
                        },
                        new ZLinkAcceptedJournalReplayer.ReplyRelay() {
                            @Override
                            public CompletionStage<Void> completeSpot(
                                    ZLinkSpotAcceptedJournal.Record request,
                                    long acceptedSequence,
                                    List<byte[]> reply,
                                    Throwable failure) {
                                return CompletableFuture.failedFuture(
                                        new AssertionError("unexpected Spot reply"));
                            }

                            @Override
                            public CompletionStage<Void> completeActor(
                                    ZLinkActorAcceptedJournal.Record request,
                                    long acceptedSequence,
                                    Optional<byte[]> reply,
                                    Throwable failure) {
                                assertEquals(1, acceptedSequence);
                                assertArrayEquals(new byte[] {7}, reply.orElseThrow());
                                return CompletableFuture.completedFuture(null);
                            }
                        });

        var bareLane =
                assertThrows(
                        CompletionException.class,
                        () ->
                                replayer.replayFrozen("actor-a", encoded)
                                        .toCompletableFuture()
                                        .join());
        assertInstanceOf(IllegalArgumentException.class, bareLane.getCause());
        assertEquals("unknown accepted journal lane: actor-a", bareLane.getCause().getMessage());

        replayer.replay("actor:actor-a", new ZLinkSerialExecutionQueue.QueuedRecord(1, encoded))
                .toCompletableFuture()
                .join();

        assertThrows(
                CompletionException.class,
                () ->
                        replayer.replay(
                                        "actor:actor-b",
                                        new ZLinkSerialExecutionQueue.QueuedRecord(1, encoded))
                                .toCompletableFuture()
                                .join());
    }
}
