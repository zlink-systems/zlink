package systems.zlink.testing;

import org.junit.platform.engine.TestExecutionResult;
import org.junit.platform.launcher.TestExecutionListener;
import org.junit.platform.launcher.TestIdentifier;

import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.time.Instant;

/** Records events in the test JVM so worker termination cannot synthesize an end event. */
public final class TestEventsListener implements TestExecutionListener {
    @Override
    public void executionStarted(TestIdentifier test) {
        record("STARTED", test, "");
    }

    @Override
    public void executionFinished(TestIdentifier test, TestExecutionResult result) {
        record("ENDED", test, result.getStatus().name());
    }

    @Override
    public void executionSkipped(TestIdentifier test, String reason) {
        record("ENDED", test, "SKIPPED");
    }

    private static void record(String event, TestIdentifier test, String result) {
        String line =
                Instant.now()
                        + "\t"
                        + event
                        + "\t"
                        + test.getUniqueId()
                        + "\t"
                        + test.getDisplayName()
                        + "\t"
                        + result
                        + "\n";
        try {
            Files.write(
                    Path.of(System.getProperty("zlink.test.events.file")),
                    line.getBytes(StandardCharsets.UTF_8),
                    StandardOpenOption.CREATE,
                    StandardOpenOption.APPEND);
        } catch (IOException exception) {
            throw new UncheckedIOException(exception);
        }
    }
}
