package Support;

import systems.zlink.e2e.registrymessaging.client.Support;
public final class ScenarioWait {
    private ScenarioWait() {
    }

    public static void sleep(long millis) {
        try {
            Thread.sleep(millis);
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("interrupted", error);
        }
    }
}
