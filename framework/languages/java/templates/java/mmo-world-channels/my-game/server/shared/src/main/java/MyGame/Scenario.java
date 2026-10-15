package MyGame;

public enum Scenario {
    WORLDS,
    CHANNEL,
    RECONNECT,
    UPDATE;

    public String id() {
        return name().toLowerCase(java.util.Locale.ROOT);
    }

    public static Scenario parse(String id) {
        for (var s : values()) if (s.id().equals(id)) return s;
        throw new IllegalArgumentException("Unknown scenario: " + id);
    }
}
