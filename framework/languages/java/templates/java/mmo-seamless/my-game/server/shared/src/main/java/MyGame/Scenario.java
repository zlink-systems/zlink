package MyGame;

public enum Scenario {
    CROSS("cross"),
    BORDER("border"),
    RECONNECT("reconnect"),
    UPDATE("update");
    private final String id;

    Scenario(String id) {
        this.id = id;
    }

    public String id() {
        return id;
    }

    public static Scenario parse(String id) {
        for (var value : values()) if (value.id.equals(id)) return value;
        throw new IllegalArgumentException("Unknown scenario: " + id);
    }
}
