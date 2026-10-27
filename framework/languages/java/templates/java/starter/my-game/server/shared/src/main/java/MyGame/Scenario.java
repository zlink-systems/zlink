package MyGame;

public enum Scenario {
    CONNECT("connect");
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
