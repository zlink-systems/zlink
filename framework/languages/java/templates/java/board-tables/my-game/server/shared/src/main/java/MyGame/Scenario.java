package MyGame;

public enum Scenario {
    TABLE("table"),
    RECONNECT("reconnect"),
    MATCH("match"),
    MATCH_CANCEL("match-cancel"),
    // Drain last so all selected scenarios' rooms count toward the relocation capacity.
    UPDATE("update");
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
