package systems.zlink.e2e.registrationcodec.client.Support;

import java.time.Duration;
import java.util.List;
import systems.zlink.e2e.registrationcodec.shared.Contracts;

public final class ScenarioAssert {
    private ScenarioAssert() {
    }

    public static void waitForEvidence(
        Evidence evidence,
        String marker,
        String packetName,
        String value) {
        long deadline = System.nanoTime() + Duration.ofSeconds(10).toNanos();
        while (System.nanoTime() < deadline) {
            if (evidence.snapshot().entries().stream().anyMatch(entry ->
                marker.equals(entry.marker())
                    && packetName.equals(entry.packetName())
                    && value.equals(entry.value()))) {
                return;
            }
            sleep(100);
        }
        throw new IllegalStateException(
            "timed out waiting for evidence " + marker + "/" + packetName + "/" + value);
    }

    public static void waitForEvidenceValueSuffix(
        Evidence evidence,
        String marker,
        String packetName,
        String suffix) {
        long deadline = System.nanoTime() + Duration.ofSeconds(10).toNanos();
        while (System.nanoTime() < deadline) {
            if (evidence.snapshot().entries().stream().anyMatch(entry ->
                marker.equals(entry.marker())
                    && packetName.equals(entry.packetName())
                    && entry.value().endsWith(suffix))) {
                return;
            }
            sleep(100);
        }
        throw new IllegalStateException(
            "timed out waiting for evidence " + marker + "/" + packetName + "/*" + suffix);
    }

    public static void waitForFilterOrder(Evidence evidence, String value) {
        long deadline = System.nanoTime() + Duration.ofSeconds(10).toNanos();
        while (System.nanoTime() < deadline) {
            List<String> entries = evidence.snapshot().entries().stream()
                .filter(entry -> "Filter".equals(entry.marker()))
                .filter(entry -> "EchoManual".equals(entry.packetName()))
                .map(Contracts.EvidenceEntry::value)
                .toList();
            List<String> expected = List.of(
                "first-before",
                "second-before",
                "second-after",
                "first-after");
            if (entries.size() >= expected.size()
                && entries.subList(entries.size() - expected.size(), entries.size())
                    .equals(expected)) {
                return;
            }
            sleep(100);
        }
        throw new IllegalStateException("timed out waiting for RC-A5 filter order");
    }

    public static void ensure(boolean condition, String message) {
        if (!condition) {
            throw new IllegalStateException(message);
        }
    }

    private static void sleep(long millis) {
        try {
            Thread.sleep(millis);
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("interrupted", error);
        }
    }
}
