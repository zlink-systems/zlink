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
                IllegalArgumentException.class,
                () ->
                        replayer.prepareReplay(
                                "actor:actor-b",
                                new ZLinkSerialExecutionQueue.QueuedRecord(1, encoded),
                                new CompletableFuture<>(),
                                observed -> fail("mismatched record must not attach an observer")));

        assertThrows(
                CompletionException.class,
                () ->
                        replayer.replay(
                                        "actor:actor-b",
                                        new ZLinkSerialExecutionQueue.QueuedRecord(1, encoded))
                                .toCompletableFuture()
                                .join());
    }

    @org.junit.jupiter.params.ParameterizedTest
    @org.junit.jupiter.params.provider.MethodSource("replyBoundaries")
    void queuedActorHasOneReplyOwnerAcrossRejectHandlerRelayAndTransfer(ReplyBoundary boundary) {
        var replies = new ArrayList<String>();
        var original = new IllegalStateException("handler or admission failed");
        var relayFailure = new IllegalArgumentException("reply relay failed");
        var replayer =
                new ZLinkAcceptedJournalReplayer(
                        record -> CompletableFuture.completedFuture(List.of()),
                        record ->
                                boundary == ReplyBoundary.HANDLER_FAILURE
                                        ? CompletableFuture.failedFuture(original)
                                        : CompletableFuture.completedFuture(
                                                Optional.of(new byte[] {7})),
                        new ZLinkAcceptedJournalReplayer.ReplyRelay() {
                            public CompletionStage<Void> completeSpot(
                                    ZLinkSpotAcceptedJournal.Record record,
                                    long sequence,
                                    List<byte[]> reply,
                                    Throwable failure) {
                                throw new AssertionError();
                            }

                            public CompletionStage<Void> completeActor(
                                    ZLinkActorAcceptedJournal.Record record,
                                    long sequence,
                                    Optional<byte[]> reply,
                                    Throwable failure) {
                                if (failure != null) {
                                    assertSame(original, failure);
                                    replies.add("failure:" + sequence);
                                    return CompletableFuture.completedFuture(null);
                                }
                                replies.add("reply:" + sequence);
                                return boundary == ReplyBoundary.REPLY_RELAY_FAILURE
                                        ? CompletableFuture.failedFuture(relayFailure)
                                        : CompletableFuture.completedFuture(null);
                            }
                        });
        var queued =
                new ZLinkSerialExecutionQueue.QueuedRecord(
                        11,
                        ZLinkAcceptedJournalTestRecords.actor(
                                "actor-a", 41, "probe", Map.of(), new byte[] {3}));
        var terminal = new CompletableFuture<Void>();
        var observed = new ArrayList<CompletionStage<Void>>();
        var operation = replayer.prepareReplay("actor:actor-a", queued, terminal, observed::add);
        assertTrue(replies.isEmpty());
        if (boundary == ReplyBoundary.REJECT_BEFORE_START) terminal.completeExceptionally(original);
        else if (boundary == ReplyBoundary.TRANSFER_UNSTARTED) terminal.complete(null);
        else
            operation
                    .get()
                    .whenComplete(
                            (ignored, failure) -> {
                                if (failure == null) terminal.complete(null);
                                else terminal.completeExceptionally(failure);
                            });
        var result = observed.getFirst().toCompletableFuture();
        assertTrue(
                result.isDone(),
                "an unstarted transferred Entry must not wait for its dispatch result");
        if (boundary != ReplyBoundary.TRANSFER_UNSTARTED) {
            var failure = assertThrows(CompletionException.class, result::join);
            assertSame(
                    boundary == ReplyBoundary.REPLY_RELAY_FAILURE ? relayFailure : original,
                    failure.getCause());
            assertEquals(
                    List.of(
                            boundary == ReplyBoundary.REPLY_RELAY_FAILURE
                                    ? "reply:11"
                                    : "failure:11"),
                    replies,
                    "relay failure must not trigger another error reply");
        } else {
            result.join();
            assertTrue(replies.isEmpty(), "old transferred Entry has no reply ownership");
            var nextTerminal = new CompletableFuture<Void>();
            var nextOperation =
                    replayer.prepareReplay("actor:actor-a", queued, nextTerminal, observed::add);
            nextOperation.get().whenComplete((ignored, failure) -> nextTerminal.complete(null));
            assertTrue(observed.getLast().toCompletableFuture().isDone());
            observed.getLast().toCompletableFuture().join();
            assertEquals(List.of("reply:11"), replies, "only the next owner replies");
        }
    }

    private enum ReplyBoundary {
        REJECT_BEFORE_START,
        HANDLER_FAILURE,
        REPLY_RELAY_FAILURE,
        TRANSFER_UNSTARTED
    }

    private static java.util.stream.Stream<org.junit.jupiter.params.provider.Arguments>
            replyBoundaries() {
        return java.util.stream.IntStream.range(0, 20)
                .boxed()
                .flatMap(ignored -> java.util.Arrays.stream(ReplyBoundary.values()))
                .map(org.junit.jupiter.params.provider.Arguments::of);
    }
}
