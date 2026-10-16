package MyGame.region.domain;

import MyGame.PlayerPosition;

import java.util.*;

public final class Positions {
    private Map<String, Position> players = new LinkedHashMap<>();

    public boolean admit(String id, int capacity) {
        if (!players.containsKey(id) && players.size() >= capacity) return false;
        players.putIfAbsent(id, new Position(0, 0));
        return true;
    }

    public void leave(String id) {
        players.remove(id);
    }

    public void move(String id, int x, long sequence) {
        var current = Objects.requireNonNull(players.get(id), "Player is not in Region");
        if (sequence <= current.sequence())
            throw new IllegalArgumentException("Move sequence must increase");
        players.put(id, new Position(x, sequence));
    }

    public List<PlayerPosition> view() {
        return players.entrySet().stream()
                .sorted(Map.Entry.comparingByKey())
                .map(e -> new PlayerPosition(e.getKey(), e.getValue().x()))
                .toList();
    }

    public Set<String> playerIds() {
        return Collections.unmodifiableSet(players.keySet());
    }

    public Map<String, Position> snapshot() {
        return new LinkedHashMap<>(players);
    }

    public void restore(Map<String, Position> snapshot) {
        players = new LinkedHashMap<>(Objects.requireNonNull(snapshot));
    }
}
