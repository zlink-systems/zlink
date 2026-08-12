package systems.zlink.samples.bingo.server.play.infrastructure.zlink.spots.bingoroomspot;

import static org.junit.jupiter.api.Assertions.assertAll;
import static org.junit.jupiter.api.Assertions.assertEquals;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorJoinCall;
import systems.zlink.framework.actors.ZLinkBoundSession;
import systems.zlink.framework.actors.ZLinkBoundSessionSendCall;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotOutbound;
import systems.zlink.framework.spots.ZLinkSpotRelocationReadyCall;
import systems.zlink.framework.spots.ZLinkTimer;
import systems.zlink.framework.spots.ZLinkTimerOptions;
import systems.zlink.samples.bingo.server.play.infrastructure.zlink.actors.PlayerActor;
import systems.zlink.samples.bingo.shared.contracts.BingoMessages;
import systems.zlink.samples.bingo.shared.contracts.Messages;

final class BingoRoomSpotGameStartedNotificationTest {
    @Test
    void secondJoinPushesGameStartedToBothPlayers() {
        String roomId = "bingo-room-regression";
        BingoRoomSpot room = new BingoRoomSpot(new TestSpotContext(roomId), null);
        RecordingBoundSession playerOneSession = new RecordingBoundSession();
        RecordingBoundSession playerTwoSession = new RecordingBoundSession();
        PlayerActor playerOne = new PlayerActor(
            "player-1", new TestActorContext("player-1", playerOneSession));
        PlayerActor playerTwo = new PlayerActor(
            "player-2", new TestActorContext("player-2", playerTwoSession));

        room.join(
            playerOne,
            BingoMessages.bingoRoomJoinReq(roomId, "player-1", "Player One", false),
            0,
            0);

        assertAll(
            () -> assertEquals(0, playerOneSession.messagesOf(Messages.PlayerJoinedNotify.class).size()),
            () -> assertEquals(0, playerTwoSession.messagesOf(Messages.PlayerJoinedNotify.class).size()));

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
            () -> assertEquals(1, playerOneSession.messagesOf(Messages.PlayerJoinedNotify.class).size()),
            () -> assertEquals(0, playerTwoSession.messagesOf(Messages.PlayerJoinedNotify.class).size()),
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

    private static final class RecordingBoundSession implements ZLinkBoundSession {
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

    private record TestActorContext(
        String actorId,
        RecordingBoundSession boundSession) implements ZLinkActorContext {
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
            String name,
            Duration period,
            Class<?> handlerType,
            ZLinkTimerOptions options) {
            throw new UnsupportedOperationException();
        }
    }
}
