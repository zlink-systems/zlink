package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import java.net.URI;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BiConsumer;
import java.util.logging.Level;
import java.util.logging.Logger;

final class DefaultZLinkStreamConnectorTraceTest {
    @TempDir Path outputDirectory;

    @Test
    void disabledTraceDoesNotRegisterCompletionCallback() throws Exception {
        runTraceDisabledProbe("callback");
    }

    @Test
    void disabledTraceDoesNotBuildMessage() throws Exception {
        runTraceDisabledProbe("message");
    }

    private void runTraceDisabledProbe(String check) throws Exception {
        Path output = outputDirectory.resolve("probe.log");
        var command =
                new ProcessBuilder(
                                Path.of(System.getProperty("java.home"), "bin", "java").toString(),
                                "-cp",
                                ConnectorTestClasspath.resolve(),
                                TraceProbe.class.getName(),
                                check)
                        .redirectErrorStream(true)
                        .redirectOutput(output.toFile());
        command.environment().put("ZLINK_JAVA_STREAM_TRACE", "0");
        Process process = command.start();
        try {
            assertTrue(process.waitFor(10, TimeUnit.SECONDS), "trace probe did not finish");
            assertEquals(0, process.exitValue(), Files.readString(output));
        } finally {
            if (process.isAlive()) {
                process.destroyForcibly().waitFor();
            }
        }
    }

    public static final class TraceProbe {
        private static final Logger LOGGER =
                Logger.getLogger(DefaultZLinkStreamConnector.class.getName());

        public static void main(String[] args) {
            LOGGER.setLevel(Level.FINE);
            switch (args[0]) {
                case "callback" -> checkCompletionCallback();
                case "message" -> checkMessageSupplier();
                default -> throw new IllegalArgumentException(args[0]);
            }
        }

        private static void checkCompletionCallback() {
            var connector =
                    new DefaultZLinkStreamConnector(
                            ZLinkStreamConnectorOptions.createDefault(
                                    URI.create("tcp://127.0.0.1:1")));
            var callbacks = new AtomicInteger();
            var publication =
                    new CompletableFuture<Void>() {
                        @Override
                        public CompletableFuture<Void> whenComplete(
                                BiConsumer<? super Void, ? super Throwable> action) {
                            callbacks.incrementAndGet();
                            return super.whenComplete(action);
                        }
                    };
            assertSame(publication, connector.traceWrite(null, publication));
            assertEquals(0, callbacks.get());
        }

        private static void checkMessageSupplier() {
            var messageBuilt = new AtomicBoolean();
            DefaultZLinkStreamConnector.trace(
                    () -> {
                        messageBuilt.set(true);
                        return "test trace";
                    });
            assertFalse(messageBuilt.get());
        }
    }
}
