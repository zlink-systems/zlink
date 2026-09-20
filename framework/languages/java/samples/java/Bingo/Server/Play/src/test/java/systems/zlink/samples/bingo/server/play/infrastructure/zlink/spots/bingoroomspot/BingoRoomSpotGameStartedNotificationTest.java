package systems.zlink.samples.bingo.server.play.infrastructure.zlink.spots.bingoroomspot;

import static org.junit.jupiter.api.Assertions.assertAll;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorJoinCall;
import systems.zlink.framework.actors.ZLinkBoundSession;
import systems.zlink.framework.actors.ZLinkBoundSessionSendCall;
import systems.zlink.framework.channels.ZLinkPublishCall;
import systems.zlink.framework.channels.ZLinkRequestCall;
import systems.zlink.framework.channels.ZLinkSendCall;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotOutbound;
import systems.zlink.framework.spots.ZLinkSpotRelocationReadyCall;
import systems.zlink.framework.spots.ZLinkSpotRequestCall;
import systems.zlink.framework.spots.ZLinkSpotSendCall;
import systems.zlink.framework.spots.ZLinkTimer;
import systems.zlink.framework.spots.ZLinkTimerOptions;
import systems.zlink.samples.bingo.server.play.infrastructure.zlink.actors.PlayerActor;
import systems.zlink.samples.bingo.shared.contracts.BingoMessages;
import systems.zlink.samples.bingo.shared.contracts.Messages;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

final class BingoRoomSpotGameStartedNotificationTest {
    @Test
    void secondJoinPushesGameStartedToBothPlayers() {
        String roomId = "bingo-room-regression";
        BingoRoomSpot room = new BingoRoomSpot(new TestSpotContext(roomId), null);
        CapturedBoundSession playerOneSession = new CapturedBoundSession();
        CapturedBoundSession playerTwoSession = new CapturedBoundSession();
        PlayerActor playerOne =
                new PlayerActor("player-1", new TestActorContext("player-1", playerOneSession));
        PlayerActor playerTwo =
                new PlayerActor("player-2", new TestActorContext("player-2", playerTwoSession));

        room.join(
                playerOne,
                BingoMessages.bingoRoomJoinReq(roomId, "player-1", "Player One", false),
                0,
                0);

        assertAll(
                () ->
                        assertEquals(
                                0,
                                playerOneSession
                                        .messagesOf(Messages.PlayerJoinedNotify.class)
                                        .size()),
                () ->
                        assertEquals(
                                0,
                                playerTwoSession
                                        .messagesOf(Messages.PlayerJoinedNotify.class)
                                        .size()));

        room.join(
                playerTwo,
                BingoMessages.bingoRoomJoinReq(roomId, "player-2", "Player Two", false),
                0,
                0);

        List<Messages.BingoGameStartedNotify> playerOneStarted =
                playerOneSession.messagesOf(Messages.BingoGameStartedNotify.class);
        List<Messages.BingoGameStartedNotify> playerTwoStarted =
                playerTwoSession.messagesOf(Messages.BingoGameStartedNotify.class);
        assertAll(
                () ->
                        assertEquals(
                                1,
                                playerOneSession
                                        .messagesOf(Messages.PlayerJoinedNotify.class)
                                        .size()),
                () ->
                        assertEquals(
                                0,
                                playerTwoSession
                                        .messagesOf(Messages.PlayerJoinedNotify.class)
                                        .size()),
                () -> assertEquals(1, playerOneStarted.size()),
                () -> assertEquals(1, playerTwoStarted.size()));

        Messages.BingoRoomState playerOneState = playerOneStarted.getFirst().getState();
        Messages.BingoRoomState playerTwoState = playerTwoStarted.getFirst().getState();
        assertAll(
                () -> assertEquals(roomId, playerOneState.getRoomId()),
                () -> assertEquals("Running", playerOneState.getStatus()),
                () -> assertEquals(2, playerOneState.getPlayersCount()),
                () -> assertEquals(playerOneState, playerTwoState));
    }

    // Contract §7.2: the join continuation resumed after Yield must re-validate the pending
    // join identity, `canAcceptPlayer()` and the game state, leaving the actor on any mismatch
    // instead of joining it with a stale record lookup result.
    @Test
    void joinResumptionRevalidatesRoomStateAndLeavesActorOnMismatch() {
        String roomId = "bingo-room-mismatch";
        RecordingSpotContext context = new RecordingSpotContext(roomId);
        BingoRoomSpot room = new BingoRoomSpot(context, null);
        CapturedBoundSession session = new CapturedBoundSession();
        PlayerActor actor = new PlayerActor("player-1", new TestActorContext("player-1", session));

        room.onActorJoin(
                "player-1",
                ZLinkMessage.of(
                        BingoMessages.bingoRoomJoinReq(roomId, "player-1", "Player One", false)));

        CompletionStage<Void> joined = room.onJoinedActor(actor);

        // A second admission overwrites the pending join identity while the first join is
        // still suspended on the Yield — this is the race §7.2 requires re-checking for.
        room.onActorJoin(
                "player-1",
                ZLinkMessage.of(
                        BingoMessages.bingoRoomJoinReq(
                                roomId, "player-1", "Player One Retry", false)));

        context.outbound.recordFuture.complete(BingoMessages.getPlayerRecordRes("player-1", 3, 1));
        joined.toCompletableFuture().join();

        assertAll(
                () -> assertEquals(List.of(actor), context.leftActors),
                () -> assertEquals(0, session.messagesOf(Messages.PlayerJoinedNotify.class).size()),
                () ->
                        assertEquals(
                                0,
                                session.messagesOf(Messages.BingoGameStartedNotify.class).size()));
    }

    @Test
    void joinResumptionLeavesActorWhenRoomCanNoLongerAcceptPlayers() {
        String roomId = "bingo-room-finished";
        RecordingSpotContext context = new RecordingSpotContext(roomId);
        BingoRoomSpot room = new BingoRoomSpot(context, null);
        CapturedBoundSession lateSession = new CapturedBoundSession();
        PlayerActor lateActor =
                new PlayerActor("player-3", new TestActorContext("player-3", lateSession));

        // player-3 is admitted while the room is still empty, then suspends on the record
        // lookup Yield.
        room.onActorJoin(
                "player-3",
                ZLinkMessage.of(
                        BingoMessages.bingoRoomJoinReq(roomId, "player-3", "Player Three", false)));
        CompletionStage<Void> joined = room.onJoinedActor(lateActor);

        // While player-3 is suspended, two other players join synchronously and fill the
        // two-player room, so it can no longer accept a player by the time the Yield resumes.
        CapturedBoundSession playerOneSession = new CapturedBoundSession();
        CapturedBoundSession playerTwoSession = new CapturedBoundSession();
        room.join(
                new PlayerActor("player-1", new TestActorContext("player-1", playerOneSession)),
                BingoMessages.bingoRoomJoinReq(roomId, "player-1", "Player One", false),
                0,
                0);
        room.join(
                new PlayerActor("player-2", new TestActorContext("player-2", playerTwoSession)),
                BingoMessages.bingoRoomJoinReq(roomId, "player-2", "Player Two", false),
                0,
                0);

        context.outbound.recordFuture.complete(BingoMessages.getPlayerRecordRes("player-3", 0, 0));
        joined.toCompletableFuture().join();

        assertAll(
                () -> assertEquals(List.of(lateActor), context.leftActors),
                () ->
                        assertTrue(
                                lateSession
                                        .messagesOf(Messages.PlayerJoinedNotify.class)
                                        .isEmpty()));
    }

    private static final class CapturedBoundSession implements ZLinkBoundSession {
        private final List<Object> messages = new ArrayList<>();

        @Override
        public ZLinkBoundSessionSendCall send(Object message) {
            messages.add(message);
            return new ZLinkBoundSessionSendCall() {
                @Override
                public ZLinkBoundSessionSendCall metadata(String key, String value) {
                    return this;
                }

                @Override
                public CompletionStage<Void> submit() {
                    return CompletableFuture.completedFuture(null);
                }
            };
        }

        @Override
        public CompletionStage<Void> disconnect() {
            return CompletableFuture.completedFuture(null);
        }

        <T> List<T> messagesOf(Class<T> messageType) {
            return messages.stream()
                    .filter(messageType::isInstance)
                    .map(messageType::cast)
                    .toList();
        }
    }

    private record TestActorContext(String actorId, CapturedBoundSession boundSession)
            implements ZLinkActorContext {
        @Override
        public long objectGeneration() {
            return 1;
        }

        @Override
        public String meshName() {
            return "bingo.play";
        }

        @Override
        public Optional<String> spotId() {
            return Optional.empty();
        }

        @Override
        public ZLinkActorJoinCall joinSpot(String spotId) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkActorJoinCall joinSpot(String spotId, Object request) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkActorJoinCall joinEntrySpot() {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkActorJoinCall joinEntrySpot(Object request) {
            throw new UnsupportedOperationException();
        }
    }

    private record TestSpotContext(String spotId) implements ZLinkSpotContext {
        @Override
        public long objectGeneration() {
            return 1;
        }

        @Override
        public RoutingId nodeRid() {
            return null;
        }

        @Override
        public ZLinkSpotOutbound outbound() {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkSpotRelocationReadyCall relocationReady() {
            throw new UnsupportedOperationException();
        }

        @Override
        public CompletionStage<Void> leaveActor(ZLinkActor actor) {
            throw new UnsupportedOperationException();
        }

        @Override
        public CompletionStage<Boolean> close() {
            throw new UnsupportedOperationException();
        }

        @Override
        public CompletionStage<ZLinkTimer> addTimer(
                String name, Duration period, Class<?> handlerType, ZLinkTimerOptions options) {
            throw new UnsupportedOperationException();
        }
    }

    /**
     * Spot context whose outbound record lookup is a controllable Yield and whose {@code
     * leaveActor} calls are recorded, so tests can assert the §7.2 re-check.
     */
    private static final class RecordingSpotContext implements ZLinkSpotContext {
        private final String spotId;
        final FakeOutbound outbound = new FakeOutbound();
        final List<ZLinkActor> leftActors = new ArrayList<>();

        RecordingSpotContext(String spotId) {
            this.spotId = spotId;
        }

        @Override
        public String spotId() {
            return spotId;
        }

        @Override
        public long objectGeneration() {
            return 1;
        }

        @Override
        public RoutingId nodeRid() {
            return null;
        }

        @Override
        public ZLinkSpotOutbound outbound() {
            return outbound;
        }

        @Override
        public ZLinkSpotRelocationReadyCall relocationReady() {
            throw new UnsupportedOperationException();
        }

        @Override
        public CompletionStage<Void> leaveActor(ZLinkActor actor) {
            leftActors.add(actor);
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Boolean> close() {
            throw new UnsupportedOperationException();
        }

        @Override
        public CompletionStage<ZLinkTimer> addTimer(
                String name, Duration period, Class<?> handlerType, ZLinkTimerOptions options) {
            throw new UnsupportedOperationException();
        }
    }

    private static final class FakeOutbound implements ZLinkSpotOutbound {
        final CompletableFuture<Messages.GetPlayerRecordRes> recordFuture =
                new CompletableFuture<>();

        @Override
        public ZLinkSpotSendCall sendToSpot(String spotId, Object message) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkSpotRequestCall requestToSpot(String spotId, Object request) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkPublishCall publish(String channelName, String topic, Object message) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkSendCall sendToChannel(String channelName, Object message) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkRequestCall requestToChannel(String channelName, Object request) {
            return new ZLinkRequestCall() {
                @Override
                public ZLinkRequestCall timeout(Duration timeout) {
                    return this;
                }

                @Override
                public <TReply> CompletionStage<TReply> submit(Class<TReply> replyType) {
                    throw new UnsupportedOperationException();
                }

                @Override
                @SuppressWarnings("unchecked")
                public <TReply> CompletionStage<TReply> yield(Class<TReply> replyType) {
                    return (CompletionStage<TReply>) (CompletionStage<?>) recordFuture;
                }
            };
        }
    }
}
