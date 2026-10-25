package MyGame.lobby.application;

import MyGame.*;

import java.util.*;
import java.util.concurrent.*;

public final class Matchmaker implements AutoCloseable {
    private record Waiting(String playerId, CompletableFuture<MatchResponse> completion) {}

    private final RoomProvisioner rooms;
    private final Map<String, Waiting> waiting = new HashMap<>();
    private final Map<String, CompletableFuture<MatchResponse>> participants = new HashMap<>();
    private boolean closed;

    public Matchmaker(RoomProvisioner rooms) {
        this.rooms = rooms;
    }

    public CompletionStage<MatchResponse> match(MatchRequest request) {
        if (request == null
                || request.playerId() == null
                || request.playerId().isBlank()
                || request.mode() == null
                || request.mode().isBlank())
            throw new IllegalArgumentException("Player and mode are required");
        var result = new CompletableFuture<MatchResponse>();
        Waiting partner;
        String roomId = null;
        synchronized (this) {
            if (closed) throw new IllegalStateException("Lobby is closed");
            if (participants.containsKey(request.playerId()))
                throw new IllegalArgumentException("Player already waiting");
            participants.put(request.playerId(), result);
            partner = waiting.remove(request.mode());
            if (partner == null) {
                waiting.put(request.mode(), new Waiting(request.playerId(), result));
            } else {
                roomId = Names.ROOM_PREFIX + UUID.randomUUID().toString().replace("-", "");
            }
        }
        result.whenComplete(
                (value, error) -> {
                    synchronized (this) {
                        participants.remove(request.playerId(), result);
                        var pending = waiting.get(request.mode());
                        if (pending != null && pending.completion() == result)
                            waiting.remove(request.mode());
                    }
                });
        if (partner != null) {
            var members = List.of(partner.playerId(), request.playerId());
            try {
                rooms.prepare(roomId, members)
                        .whenComplete(
                                (assignment, error) -> {
                                    if (error == null) {
                                        partner.completion().complete(assignment);
                                        result.complete(assignment);
                                    } else {
                                        partner.completion().completeExceptionally(error);
                                        result.completeExceptionally(error);
                                    }
                                });
            } catch (Exception error) {
                partner.completion().completeExceptionally(error);
                result.completeExceptionally(error);
            }
        }
        return result;
    }

    public synchronized boolean cancelWaiting(
            MatchRequest request, CompletableFuture<MatchResponse> result) {
        var pending = request == null ? null : waiting.get(request.mode());
        if (pending == null || pending.completion() != result) return false;
        waiting.remove(request.mode());
        return result.cancel(false);
    }

    public void close() {
        List<CompletableFuture<MatchResponse>> pending;
        synchronized (this) {
            closed = true;
            pending = List.copyOf(participants.values());
        }
        for (var request : pending)
            request.completeExceptionally(new IllegalStateException("Lobby shutdown"));
    }
}
