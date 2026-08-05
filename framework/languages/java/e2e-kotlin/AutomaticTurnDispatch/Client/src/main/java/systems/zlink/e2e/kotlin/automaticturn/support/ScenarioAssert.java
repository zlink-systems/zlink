package systems.zlink.e2e.kotlin.automaticturn.support;

import java.util.List;

public final class ScenarioAssert {
    private ScenarioAssert() {
    }

    public static void that(boolean condition, String message) {
        if (!condition) {
            throw new IllegalStateException(message);
        }
    }

    public static void lifecycle(CheckedRunnable action) {
        try {
            action.run();
        } catch (Exception error) {
            throw new IllegalStateException("stream lifecycle operation failed", error);
        }
    }

    public static void containsMarkersInOrder(
        List<String> evidence,
        String... markers) {
        int cursor = 0;
        for (String entry : evidence) {
            if (cursor < markers.length && entry.startsWith(markers[cursor] + "|")) {
                cursor++;
            }
        }
        if (cursor != markers.length) {
            throw new IllegalStateException(
                "evidence marker order mismatch. expected="
                    + List.of(markers)
                    + ", actual="
                    + evidence);
        }
    }

    @FunctionalInterface
    public interface CheckedRunnable {
        void run() throws Exception;
    }
}
