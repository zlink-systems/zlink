package systems.zlink.e2e.storefailure.client.support;

public final class ScenarioAssert {
    private ScenarioAssert() {
    }

    public static void that(boolean condition, String message) {
        if (!condition) {
            throw new IllegalStateException(message);
        }
    }
}
