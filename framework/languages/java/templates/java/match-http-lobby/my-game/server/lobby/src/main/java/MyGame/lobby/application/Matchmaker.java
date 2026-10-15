package MyGame.lobby.application;

import MyGame.*;

import java.util.*;
import java.util.concurrent.*;

public final class Matchmaker implements AutoCloseable {
    private final RoomProvisioner rooms;
    private final Map<String, CompletableFuture<MatchResponse>> waiting = new HashMap<>();
    private final Map<String, CompletableFuture<MatchResponse>> participants = new HashMap<>();
    private long sequence;
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
        CompletableFuture<MatchResponse> partner;
        String roomId = null;
        synchronized (this) {
            if (closed) throw new IllegalStateException("Lobby is closed");
            if (participants.containsKey(request.playerId()))
                throw new IllegalArgumentException("Player already waiting");
            if (waiting.containsKey(request.mode())) {
                sequence = Math.incrementExact(sequence);
                roomId = Names.ROOM_PREFIX + sequence;
            }
            participants.put(request.playerId(), result);
            partner = waiting.remove(request.mode());
            if (partner == null) waiting.put(request.mode(), result);
        }
        result.whenComplete(
                (value, error) -> {
                    synchronized (this) {
                        participants.remove(request.playerId(), result);
                        waiting.remove(request.mode(), result);
                    }
                });
        if (partner != null) {
            var first = partner;
            try {
                rooms.prepare(roomId)
                        .whenComplete(
                                (value, error) -> {
                                    if (error == null) {
                                        first.complete(value);
                                        result.complete(value);
                                    } else {
                                        first.completeExceptionally(error);
                                        result.completeExceptionally(error);
                                    }
                                });
            } catch (Exception error) {
                first.completeExceptionally(error);
                result.completeExceptionally(error);
            }
        }
        return result;
    }

    public synchronized boolean cancelWaiting(
            MatchRequest request, CompletableFuture<MatchResponse> result) {
        if (waiting.get(request.mode()) != result) return false;
        result.cancel(false);
        return true;
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
