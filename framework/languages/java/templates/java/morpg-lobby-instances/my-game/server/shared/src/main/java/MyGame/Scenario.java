package MyGame;

public enum Scenario {
    PARTY("party"),
    RECONNECT("reconnect"),
    UPDATE("update"),
    DUNGEON_LEAVE("dungeon-leave");
    private final String id;

    Scenario(String id) {
        this.id = id;
    }

    public String id() {
        return id;
    }

    public static Scenario parse(String id) {
        for (var scenario : values()) if (scenario.id.equals(id)) return scenario;
        throw new IllegalArgumentException("Unknown scenario: " + id);
    }
}
