package MyGame;

public enum Scenario {
    REGION,
    MOVEMENT,
    DUNGEON,
    RECONNECT,
    UPDATE;

    public String id() {
        return name().toLowerCase(java.util.Locale.ROOT);
    }

    public static Scenario parse(String id) {
        return java.util.Arrays.stream(values())
                .filter(s -> s.id().equals(id))
                .findFirst()
                .orElseThrow(() -> new IllegalArgumentException("Unknown scenario: " + id));
    }
}
