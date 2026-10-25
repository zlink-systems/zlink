package MyGame.lobby.application;

import MyGame.*;

import java.util.*;
import java.util.concurrent.*;

public final class Matchmaker implements AutoCloseable {
    private record Pending(String playerId, String mode, CompletableFuture<MatchResponse> result) {}

    private final RoomProvisioner rooms;
    private final Map<String, Pending> waiting = new HashMap<>();
    private final Map<String, Pending> participants = new HashMap<>();
    private boolean closed;

    public Matchmaker(RoomProvisioner rooms) {
        this.rooms = rooms;
    }

    public CompletionStage<MatchResponse> match(MatchRequest request) {
        if (request == null || request.playerId() == null || request.playerId().isBlank())
            throw new IllegalArgumentException("Player is required");
        String mode = request.mode() == null ? Names.DEFAULT_MODE : request.mode();
        if (mode.isBlank()) throw new IllegalArgumentException("Mode is required");

        var result = new CompletableFuture<MatchResponse>();
        var current = new Pending(request.playerId(), mode, result);
        Pending partner;
        String roomId;
        synchronized (this) {
            if (closed) throw new IllegalStateException("Lobby is closed");
            if (participants.containsKey(current.playerId()))
                throw new IllegalArgumentException("Player already waiting");
            participants.put(current.playerId(), current);
            partner = waiting.remove(mode);
            if (partner == null) {
                waiting.put(mode, current);
                roomId = null;
            }
            else roomId = Names.ROOM_PREFIX + UUID.randomUUID().toString().replace("-", "");
        }
        result.whenComplete(
                (value, error) -> {
                    synchronized (this) {
                        participants.remove(current.playerId(), current);
                        waiting.remove(current.mode(), current);
                    }
                });

        if (partner != null) {
            List<String> members = List.of(partner.playerId(), current.playerId());
            try {
                rooms.prepare(roomId, members)
                        .whenComplete(
                                (assignment, error) -> {
                                    if (error != null) {
                                        partner.result().completeExceptionally(error);
                                        result.completeExceptionally(error);
                                    } else if (assignment == null
                                            || !roomId.equals(assignment.roomId())) {
                                        var failure =
                                                new IllegalStateException(
                                                        "Prepared Room identity mismatch");
                                        partner.result().completeExceptionally(failure);
                                        result.completeExceptionally(failure);
                                    } else {
                                        partner.result().complete(assignment);
                                        result.complete(assignment);
                                    }
                                });
            } catch (Exception error) {
                partner.result().completeExceptionally(error);
                result.completeExceptionally(error);
            }
        }
        return result;
    }

    public synchronized boolean cancelWaiting(
            MatchRequest request, CompletableFuture<MatchResponse> result) {
        if (request == null || request.playerId() == null || result == null) return false;
        var pending = participants.get(request.playerId());
        if (pending == null || pending.result() != result || !waiting.remove(pending.mode(), pending))
            return false;
        result.cancel(false);
        return true;
    }

    public void close() {
        List<CompletableFuture<MatchResponse>> pending;
        synchronized (this) {
            closed = true;
            pending = participants.values().stream().map(Pending::result).toList();
        }
        for (var request : pending)
            request.completeExceptionally(new IllegalStateException("Lobby shutdown"));
    }
}
