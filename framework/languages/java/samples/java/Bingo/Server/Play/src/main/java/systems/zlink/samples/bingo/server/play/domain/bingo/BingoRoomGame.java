package systems.zlink.samples.bingo.server.play.domain.bingo;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.Queue;
import systems.zlink.samples.bingo.shared.contracts.BingoMessages;
import systems.zlink.samples.bingo.shared.contracts.Messages;

public final class BingoRoomGame {
    private static final String WaitingForPlayers = "WaitingForPlayers";
    private static final String Running = "Running";
    private static final String Finished = "Finished";

    private final String roomId;
    private final BingoRoomModels.BingoRoomSettings settings;
    private final List<BingoRoomModels.RoomPlayer> players = new ArrayList<>();
    private final Queue<Integer> drawDeck = new ArrayDeque<>();
    private final List<Integer> drawnNumbers = new ArrayList<>();
    private final List<String> winners = new ArrayList<>();
    private String status = WaitingForPlayers;

    public BingoRoomGame(String roomId, BingoRoomModels.BingoRoomSettings settings) {
        this.roomId = roomId;
        this.settings = settings;
        for (int number = 1; number <= settings.maxDrawNumber(); number++) {
            drawDeck.add(number);
        }
    }

    public static BingoRoomGame restore(
        String roomId,
        BingoRoomModels.BingoRoomSettings settings,
        Messages.BingoRoomState state) {
        BingoRoomGame game = new BingoRoomGame(roomId, settings);
        game.status = state.getStatus();
        game.drawnNumbers.addAll(state.getDrawnNumbersList());
        game.drawDeck.removeAll(state.getDrawnNumbersList());
        game.winners.addAll(state.getWinnersList());
        for (Messages.BingoPlayerState player : state.getPlayersList()) {
            BingoCard card = player.getCardCount() == 0
                ? null
                : BingoCard.restore(player.getCardList(), player.getMarksList());
            game.players.add(new BingoRoomModels.RoomPlayer(
                player.getActorId(),
                player.getDisplayName(),
                player.getSeat(),
                card,
                player.getWins(),
                player.getLosses()));
        }
        return game;
    }

    public Change join(String actorId, String displayName, int wins, int losses) {
        BingoRoomModels.RoomPlayer existing = player(actorId);
        if (existing != null) {
            return new Change(snapshot(), List.of(), false);
        }
        if (!status.equals(WaitingForPlayers) || players.size() >= settings.requiredPlayers()) {
            throw new IllegalStateException("Room " + roomId + " cannot accept more players.");
        }
        BingoRoomModels.RoomPlayer joined =
            new BingoRoomModels.RoomPlayer(actorId, displayName, players.size(), null, wins, losses);
        players.add(joined);
        Messages.BingoRoomState joinedState = snapshot();
        ArrayList<BingoRoomModels.RoomEvent> events = new ArrayList<>(playerJoinedEvents(joined, joinedState));
        boolean started = false;
        if (players.size() == settings.requiredPlayers()) {
            status = Running;
            started = true;
            Messages.BingoRoomState startedState = snapshot();
            events.addAll(eventsForAll(BingoRoomModels.EventKind.GAME_STARTED, startedState));
            return new Change(startedState, events, true);
        }
        return new Change(joinedState, events, false);
    }

    public Messages.BingoRoomState previewJoin(String actorId, String displayName) {
        BingoRoomModels.RoomPlayer existing = player(actorId);
        if (existing != null) {
            return snapshot();
        }
        if (!status.equals(WaitingForPlayers) || players.size() >= settings.requiredPlayers()) {
            throw new IllegalStateException("Room " + roomId + " cannot accept more players.");
        }
        ArrayList<BingoRoomModels.RoomPlayer> previewPlayers = new ArrayList<>(players);
        previewPlayers.add(new BingoRoomModels.RoomPlayer(
            actorId,
            displayName,
            previewPlayers.size(),
            null,
            0,
            0));
        String previewStatus = previewPlayers.size() == settings.requiredPlayers()
            ? Running
            : status;
        return snapshot(previewPlayers, previewStatus);
    }

    public Change submitCard(String actorId, List<Integer> submittedCard) {
        int index = playerIndex(actorId);
        if (!status.equals(Running)) {
            throw new IllegalStateException("bingo card can be submitted only after the room starts");
        }
        BingoRoomModels.RoomPlayer current = players.get(index);
        if (current.card() != null) {
            throw new IllegalStateException("bingo card was already submitted");
        }
        players.set(index, new BingoRoomModels.RoomPlayer(
            current.actorId(),
            current.displayName(),
            current.seat(),
            BingoCard.from(submittedCard),
            current.wins(),
            current.losses()));
        return new Change(snapshot(), List.of(), allCardsSubmitted());
    }

    public Change drawNext() {
        if (!status.equals(Running) || !allCardsSubmitted() || drawDeck.isEmpty()) {
            return new Change(snapshot(), List.of(), allCardsSubmitted());
        }

        int number = drawDeck.remove();
        drawnNumbers.add(number);
        ArrayList<String> newlyCompleted = new ArrayList<>();
        for (BingoRoomModels.RoomPlayer player : players) {
            int before = player.card().completedLines();
            player.card().markDrawnNumber(number);
            if (player.card().completedLines() > before) {
                newlyCompleted.add(player.actorId());
            }
        }

        if (!newlyCompleted.isEmpty()) {
            status = Finished;
            winners.addAll(newlyCompleted);
        }

        Messages.BingoRoomState state = snapshot();
        ArrayList<BingoRoomModels.RoomEvent> events = new ArrayList<>(numberDrawnEvents(state, number));
        events.addAll(eventsForAll(
            status.equals(Finished)
                ? BingoRoomModels.EventKind.GAME_ENDED
                : BingoRoomModels.EventKind.STATE,
            state));
        return new Change(state, events, false);
    }

    public Messages.BingoRoomState snapshot() {
        return snapshot(players, status);
    }

    private Messages.BingoRoomState snapshot(
        List<BingoRoomModels.RoomPlayer> snapshotPlayers,
        String snapshotStatus) {
        String hostActorId = snapshotPlayers.isEmpty() ? "" : snapshotPlayers.getFirst().actorId();
        Integer lastDrawn = drawnNumbers.isEmpty() ? null : drawnNumbers.getLast();
        return BingoMessages.bingoRoomState(
            roomId,
            snapshotStatus,
            hostActorId,
            false,
            drawnNumbers.size(),
            lastDrawn,
            List.copyOf(drawnNumbers),
            snapshotPlayers.stream().map(player -> player.toState(hostActorId)).toList(),
            List.copyOf(winners));
    }

    private boolean allCardsSubmitted() {
        return players.size() == settings.requiredPlayers()
            && players.stream().allMatch(player -> player.card() != null);
    }

    private BingoRoomModels.RoomPlayer player(String actorId) {
        return players.stream()
            .filter(player -> player.actorId().equals(actorId))
            .findFirst()
            .orElse(null);
    }

    private int playerIndex(String actorId) {
        for (int i = 0; i < players.size(); i++) {
            if (players.get(i).actorId().equals(actorId)) {
                return i;
            }
        }
        throw new IllegalStateException("player has not joined the bingo room");
    }

    private List<BingoRoomModels.RoomEvent> playerJoinedEvents(
        BingoRoomModels.RoomPlayer joined,
        Messages.BingoRoomState state) {
        return players.stream()
            .filter(player -> !player.actorId().equals(joined.actorId()))
            .map(player -> new BingoRoomModels.RoomEvent(
                BingoRoomModels.EventKind.PLAYER_JOINED,
                player.actorId(),
                state,
                joined.actorId(),
                joined.displayName(),
                joined.seat(),
                joined.seat() == 0,
                0))
            .toList();
    }

    private List<BingoRoomModels.RoomEvent> numberDrawnEvents(
        Messages.BingoRoomState state,
        int number) {
        return players.stream()
            .map(player -> new BingoRoomModels.RoomEvent(
                BingoRoomModels.EventKind.NUMBER_DRAWN,
                player.actorId(),
                state,
                null,
                null,
                -1,
                false,
                number))
            .toList();
    }

    private List<BingoRoomModels.RoomEvent> eventsForAll(
        BingoRoomModels.EventKind kind,
        Messages.BingoRoomState state) {
        return players.stream()
            .map(player -> new BingoRoomModels.RoomEvent(
                kind,
                player.actorId(),
                state,
                null,
                null,
                -1,
                false,
                0))
            .toList();
    }

    public record Change(
        Messages.BingoRoomState state,
        List<BingoRoomModels.RoomEvent> events,
        boolean drawReady) {
    }
}
