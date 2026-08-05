package systems.zlink.samples.bingo.server.play.domain.bingo;

import java.util.List;
import systems.zlink.samples.bingo.shared.contracts.BingoMessages;
import systems.zlink.samples.bingo.shared.contracts.Messages;

public final class BingoRoomModels {
    private BingoRoomModels() {
    }

    public enum EventKind {
        PLAYER_JOINED,
        GAME_STARTED,
        NUMBER_DRAWN,
        STATE,
        GAME_ENDED
    }

    public record RoomEvent(
        EventKind kind,
        String recipientActorId,
        Messages.BingoRoomState state,
        String joinedActorId,
        String joinedDisplayName,
        int seat,
        boolean host,
        int drawnNumber) {
    }

    public record RoomPlayer(
        String actorId,
        String displayName,
        int seat,
        BingoCard card,
        int wins,
        int losses) {
        public Messages.BingoPlayerState toState(String hostActorId) {
            return BingoMessages.bingoPlayerState(
                actorId,
                displayName,
                seat,
                actorId.equals(hostActorId),
                card == null ? List.of() : card.numbersSnapshot(),
                card == null ? List.of() : card.marksSnapshot(),
                card == null ? 0 : card.completedLines(),
                wins,
                losses);
        }
    }

    public record BingoRoomSettings(
        String roomName,
        String mode,
        int requiredPlayers,
        int maxDrawNumber,
        long drawPeriodMillis,
        String purpose,
        String observedRoomId) {
        public static final String GamePurpose = "Game";
        public static final String ObserverPurpose = "Observer";

        public static BingoRoomSettings create(String mode, int roomSeq, long drawPeriodMillis) {
            if (!"two-player".equals(mode)) {
                throw new IllegalStateException("Unsupported bingo mode. mode=" + mode);
            }
            return new BingoRoomSettings(
                "Bingo Room %03d".formatted(roomSeq),
                mode,
                2,
                15,
                drawPeriodMillis,
                GamePurpose,
                null);
        }

        public static BingoRoomSettings createObserver(
            String observedRoomId,
            String observerActorId,
            long drawPeriodMillis) {
            if (observedRoomId == null || observedRoomId.isBlank()) {
                throw new IllegalStateException("Observed room id is required.");
            }
            return new BingoRoomSettings(
                "Bingo Observer " + observerActorId,
                "two-player",
                0,
                15,
                drawPeriodMillis,
                ObserverPurpose,
                observedRoomId);
        }

        public boolean observerMode() {
            return ObserverPurpose.equals(purpose);
        }
    }
}
