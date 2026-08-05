package systems.zlink.e2e.resiliencelifecycle.client.Support;

import java.io.IOException;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.URI;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.charset.StandardCharsets;
import java.nio.file.attribute.PosixFilePermission;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import systems.zlink.httpclient.RawHttpResponse;
import systems.zlink.httpclient.ZLinkHttpClient;

public final class ResilienceProcessManager implements AutoCloseable {
    private static final Duration READY_TIMEOUT = Duration.ofSeconds(60);
    private static final Duration SIGNAL_TIMEOUT = Duration.ofSeconds(30);

    private final ClientOptions options;
    private final List<ManagedProcess> processes = new ArrayList<>();

    public ResilienceProcessManager(ClientOptions options) {
        this.options = options;
    }

    public ManagedProcess startProvider(String name, String rid, String apiEndpoint, String httpEndpoint) {
        String config = """
            e2e.provider-rid=%s
            e2e.api-endpoint=%s
            e2e.http-endpoint=%s
            e2e.redis-location-endpoint=%s
            e2e.location-key-prefix=%s
            e2e.log-dir=%s
            """.formatted(rid, apiEndpoint, httpEndpoint, options.redisLocationEndpoint(),
                options.locationKeyPrefix(), options.logDir());
        ManagedProcess process = start(name, providerBin(), config);
        waitTcp(name + "-api", apiEndpoint, true);
        waitTcp(name + "-http", httpEndpoint, true);
        return process;
    }

    public ManagedProcess startConsumer(
        String name,
        String httpEndpoint,
        String logDir) {
        String config = """
            e2e.http-endpoint=%s
            e2e.redis-location-endpoint=%s
            e2e.location-key-prefix=%s
            e2e.log-dir=%s
            """.formatted(httpEndpoint, options.redisLocationEndpoint(),
                options.locationKeyPrefix(), logDir);
        ManagedProcess process = start(name, consumerBin(), config);
        waitTcp(name + "-http", httpEndpoint, true);
        return process;
    }

    public String reserveHttpEndpoint() {
        try (ServerSocket socket = new ServerSocket(0)) {
            socket.setReuseAddress(true);
            return "http://127.0.0.1:" + socket.getLocalPort();
        } catch (IOException error) {
            throw new IllegalStateException("failed to reserve HTTP endpoint", error);
        }
    }

    public Path controlDir() {
        return Path.of(options.controlDir());
    }

    public void prepareControlDir() {
        try {
            Files.createDirectories(controlDir());
        } catch (IOException error) {
            throw new IllegalStateException("failed to create control dir", error);
        }
    }

    public void waitSignal(String name) {
        waitSignal(name, null);
    }

    public void waitSignal(String name, CompletableFuture<?> operation) {
        Path path = controlDir().resolve(name);
        long deadline = System.nanoTime() + SIGNAL_TIMEOUT.toNanos();
        while (System.nanoTime() < deadline) {
            if (Files.exists(path)) {
                return;
            }
            if (operation != null && operation.isDone()) {
                operation.join();
                throw new IllegalStateException("operation completed before signal " + name);
            }
            sleep(100);
        }
        throw new IllegalStateException("timed out waiting for " + path);
    }

    public void touchSignal(String name) {
        try {
            Files.createFile(controlDir().resolve(name));
        } catch (java.nio.file.FileAlreadyExistsException ignored) {
        } catch (IOException error) {
            throw new IllegalStateException("failed to create control signal " + name, error);
        }
    }

    public void waitEndpointDown(String name, String endpoint) {
        waitTcp(name, endpoint, false);
    }

    public void sleep(long millis) {
        sleepMillis(millis);
    }

    public void pauseStore() {
        runStoreCommand("pause", options.storePauseCommand());
    }

    public void unpauseStore() {
        runStoreCommand("resume", options.storeResumeCommand());
    }

    @Override
    public void close() {
        for (int i = processes.size() - 1; i >= 0; i--) {
            processes.get(i).close();
        }
        processes.clear();
    }

    private ManagedProcess start(String name, Path bin, String config) {
        try {
            Path configPath = writeConfig(name, config);
            ProcessBuilder builder = new ProcessBuilder(bin.toString(), "--config", configPath.toString());
            builder.redirectOutput(Path.of(options.logDir(), name + ".stdout.log").toFile());
            builder.redirectError(Path.of(options.logDir(), name + ".stderr.log").toFile());
            ManagedProcess process = new ManagedProcess(name, builder.start(), configPath);
            processes.add(process);
            return process;
        } catch (IOException error) {
            throw new IllegalStateException("failed to start " + name + " at " + bin, error);
        }
    }

    private Path writeConfig(String name, String contents) throws IOException {
        Path directory = Path.of(options.configDir());
        Files.createDirectories(directory);
        Path path = directory.resolve(name + ".properties");
        Files.writeString(path, contents, StandardCharsets.UTF_8);
        Files.setPosixFilePermissions(path, Set.of(
            PosixFilePermission.OWNER_READ, PosixFilePermission.OWNER_WRITE));
        return path;
    }

    private void runStoreCommand(String action, String commandPath) {
        if (commandPath == null || commandPath.isBlank()) {
            throw new IllegalStateException("RL-C4 requires a store " + action + " command from the runner.");
        }
        runCommand("store " + action, commandPath);
    }

    private void runCommand(String description, String... command) {
        try {
            Process process = new ProcessBuilder(command).start();
            boolean exited = process.waitFor(15, TimeUnit.SECONDS);
            if (!exited || process.exitValue() != 0) {
                throw new IllegalStateException(description + " failed");
            }
        } catch (IOException error) {
            throw new IllegalStateException("failed to run " + description, error);
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("interrupted while running " + description, error);
        }
    }

    private void waitTcp(String name, String endpoint, boolean expectedUp) {
        long deadline = System.nanoTime() + READY_TIMEOUT.toNanos();
        while (System.nanoTime() < deadline) {
            boolean up = canConnect(endpoint);
            if (up == expectedUp) {
                return;
            }
            sleep(100);
        }
        throw new IllegalStateException("timed out waiting for " + name + " up=" + expectedUp + " at " + endpoint);
    }

    private boolean canConnect(String endpoint) {
        if (endpoint.startsWith("tcp://")) {
            try {
                URI uri = URI.create(endpoint);
                try (Socket socket = new Socket(uri.getHost(), uri.getPort())) {
                    return true;
                }
            } catch (Exception ignored) {
                return false;
            }
        }
        try (ZLinkHttpClient http = ZLinkHttpClient.create(endpoint).build()) {
            RawHttpResponse response = http.get("/health")
                .timeout(Duration.ofMillis(300))
                .submitRaw()
                .toCompletableFuture()
                .join();
            return response.status() >= 200 && response.status() < 300;
        } catch (RuntimeException ignored) {
            return false;
        }
    }

    private Path providerBin() {
        return Path.of(options.buildDir(), "Server-Provider", "install",
            "resilience-lifecycle-provider", "bin", "resilience-lifecycle-provider");
    }

    private Path consumerBin() {
        return Path.of(options.buildDir(), "Server-Consumer", "install",
            "resilience-lifecycle-consumer", "bin", "resilience-lifecycle-consumer");
    }

    private static void sleepMillis(long millis) {
        try {
            Thread.sleep(millis);
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("interrupted", error);
        }
    }

    public final class ManagedProcess implements AutoCloseable {
        private final String name;
        private final Process process;
        private final Path configPath;
        private boolean closed;

        private ManagedProcess(String name, Process process, Path configPath) {
            this.name = name;
            this.process = process;
            this.configPath = configPath;
        }

        public void killForcibly() {
            if (closed) {
                return;
            }
            closed = true;
            process.destroyForcibly();
            try {
                process.waitFor(5, TimeUnit.SECONDS);
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
                throw new IllegalStateException("interrupted while killing " + name, error);
            } finally {
                deleteConfig();
            }
        }

        @Override
        public void close() {
            if (closed) {
                return;
            }
            closed = true;
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
