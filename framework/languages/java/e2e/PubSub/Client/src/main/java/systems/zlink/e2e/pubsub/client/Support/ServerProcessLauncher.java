package systems.zlink.e2e.pubsub.client.Support;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.attribute.PosixFilePermission;
import java.time.Duration;
import java.util.Set;
import java.util.concurrent.TimeUnit;
import com.fasterxml.jackson.databind.ObjectMapper;

public final class ServerProcessLauncher {
    private static final Duration START_TIMEOUT = Duration.ofSeconds(3);
    // A discovered fanout connection becomes ready after a liveness beacon.
    // The publisher emits beacons every five seconds and expires a publisher
    // after fifteen seconds, so topology convergence must allow one delayed
    // beacon interval plus Redis and process scheduling time.
    private static final Duration ROUTE_SETTLE_TIMEOUT = Duration.ofSeconds(20);

    private final ClientOptions options;
    private final PubSubHttpClient http;

    ServerProcessLauncher(ClientOptions options, PubSubHttpClient http) {
        this.options = options;
        this.http = http;
    }

    public ManagedProcess startSubscriber(String rid, String topics, String httpEndpoint) {
        String config = """
            e2e.rid=%s
            e2e.topics=%s
            e2e.http-endpoint=%s
            e2e.redis-location-endpoint=%s
            e2e.location-key-prefix=%s
            e2e.log-dir=%s
            e2e.delay.delay-millis=0
            """.formatted(rid, topics, httpEndpoint, options.redisLocationEndpoint(),
                options.locationKeyPrefix(), options.logDir());
        ManagedProcess process = start(rid, subscriberBin(), config);
        waitHealthy(rid, httpEndpoint);
        return process;
    }

    public ManagedProcess startPublisher(String name) {
        String config = """
            e2e.http-endpoint=%s
            e2e.publisher-endpoint=%s
            e2e.redis-location-endpoint=%s
            e2e.location-key-prefix=%s
            e2e.log-dir=%s
            """.formatted(options.publisherHttp(), options.publisherEndpoint(),
                options.redisLocationEndpoint(), options.locationKeyPrefix(), options.logDir());
        ManagedProcess process = start(name, publisherBin(), config);
        waitHealthy(name, options.publisherHttp());
        return process;
    }

    public void waitStopped(String name, String endpoint) {
        long deadline = System.nanoTime() + START_TIMEOUT.toNanos();
        while (System.nanoTime() < deadline) {
            if (!isHealthy(endpoint)) {
                return;
            }
            ScenarioAssert.sleep(100);
        }
        throw new IllegalStateException("timed out waiting for " + name + " to stop at " + endpoint);
    }

    public String drainPublisher(ManagedProcess publisher) {
        String result = http.post(options.publisherHttp() + "/admin/drain").trim();
        publisher.close();
        return result;
    }

    public void waitPublisherRow(boolean present) {
        long deadline = System.nanoTime() + ROUTE_SETTLE_TIMEOUT.toNanos();
        ObjectMapper json = new ObjectMapper();
        String[] latest = new String[0];
        while (System.nanoTime() < deadline) {
            try {
                latest = json.readValue(
                    http.get(options.sub1Http() + "/locations/publishers"),
                    String[].class);
                boolean found = latest.length > 0;
                if (found == present) {
                    return;
                }
            } catch (Exception error) {
                // The subscriber endpoint may still be converging.
            }
            ScenarioAssert.sleep(100);
        }
        throw new IllegalStateException(
            "timed out waiting for publisher row present=" + present
                + ": " + java.util.Arrays.toString(latest));
    }

    private ManagedProcess start(String name, Path bin, String config) {
        try {
            Path configPath = writeConfig(name, config);
            ProcessBuilder builder = new ProcessBuilder(
                bin.toString(), "--config", configPath.toString());
            builder.redirectOutput(Path.of(options.logDir(), name + ".stdout.log").toFile());
            builder.redirectError(Path.of(options.logDir(), name + ".stderr.log").toFile());
            return new ManagedProcess(name, builder.start(), configPath);
        } catch (IOException error) {
            throw new IllegalStateException("failed to start " + name + " at " + bin, error);
        }
    }

    private Path writeConfig(String name, String contents) throws IOException {
        Path directory = Path.of(options.configDir());
        Files.createDirectories(directory);
        Path path = directory.resolve(name + "-dynamic.properties");
        Files.writeString(path, contents, StandardCharsets.UTF_8);
        Files.setPosixFilePermissions(path, Set.of(
            PosixFilePermission.OWNER_READ,
            PosixFilePermission.OWNER_WRITE));
        return path;
    }

    private void waitHealthy(String name, String endpoint) {
        long deadline = System.nanoTime() + START_TIMEOUT.toNanos();
        while (System.nanoTime() < deadline) {
            if (isHealthy(endpoint)) {
                return;
            }
            ScenarioAssert.sleep(100);
        }
        throw new IllegalStateException("timed out waiting for " + name + " health at " + endpoint);
    }

    private boolean isHealthy(String endpoint) {
        return http.isHealthy(endpoint, Duration.ofMillis(300));
    }

    private Path publisherBin() {
        return Path.of(options.buildDir(), "Server-Publisher", "install",
            "pub-sub-publisher", "bin", "pub-sub-publisher");
    }

    private Path subscriberBin() {
        return Path.of(options.buildDir(), "Server-Subscriber", "install",
            "pub-sub-subscriber", "bin", "pub-sub-subscriber");
    }

    public static final class ManagedProcess implements AutoCloseable {
        private final String name;
        private final Process process;
        private final Path configPath;

        private ManagedProcess(String name, Process process, Path configPath) {
            this.name = name;
            this.process = process;
            this.configPath = configPath;
        }

        @Override
        public void close() {
            if (!process.isAlive()) {
                deleteConfig();
                return;
            }
            process.destroy();
            try {
                if (!process.waitFor(5, TimeUnit.SECONDS)) {
                    process.destroyForcibly();
                    process.waitFor(5, TimeUnit.SECONDS);
                }
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
                process.destroyForcibly();
                throw new IllegalStateException("interrupted while stopping " + name, error);
            } finally {
                deleteConfig();
            }
        }

        private void deleteConfig() {
            try {
                Files.deleteIfExists(configPath);
            } catch (IOException error) {
                throw new IllegalStateException("failed to delete config for " + name, error);
            }
        }
    }
}
