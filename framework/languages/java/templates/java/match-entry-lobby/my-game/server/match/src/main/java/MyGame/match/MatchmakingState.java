package MyGame.match;

import MyGame.*;

import java.util.*;

public final class MatchmakingState {
    public Map<String, List<QueueRequest>> waiting = new LinkedHashMap<>();
    public long roomSequence;

    public void enqueue(QueueRequest request) {
        if (request.mode().isBlank()) throw new IllegalArgumentException("Mode required");
        if (waiting.values().stream()
                .flatMap(List::stream)
                .anyMatch(p -> p.playerId().equals(request.playerId()))) return;
        waiting.computeIfAbsent(request.mode(), ignored -> new ArrayList<>()).add(request);
    }

    public List<QueueRequest> pair() {
        return waiting.values().stream()
                .filter(q -> q.size() >= Rules.PLAYERS_PER_ROOM)
                .findFirst()
                .map(q -> List.copyOf(q.subList(0, Rules.PLAYERS_PER_ROOM)))
                .orElse(List.of());
    }

    public String allocateRoom() {
        return Names.ROOM_PREFIX + (roomSequence = Math.incrementExact(roomSequence));
    }

    public void remove(List<QueueRequest> pair) {
        var queue = waiting.get(pair.getFirst().mode());
        queue.subList(0, Rules.PLAYERS_PER_ROOM).clear();
        if (queue.isEmpty()) waiting.remove(pair.getFirst().mode());
    }
}
