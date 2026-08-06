package systems.zlink.e2e.registrymessaging.client.Support;

import java.io.IOException;
import java.net.ServerSocket;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.nio.file.attribute.PosixFilePermission;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.TimeUnit;
import java.util.function.Predicate;
import systems.zlink.httpclient.ZLinkHttpClient;

public final class DynamicClusterLauncher implements AutoCloseable {
    private static final Duration LOCAL_READINESS_TIMEOUT = Duration.ofSeconds(3);
    private static final Duration ROUTE_SETTLE_TIMEOUT = Duration.ofSeconds(30);
    private final List<DynamicProcess> processes = new ArrayList<>();
    private final Path logDir;
    private final String buildDir;
    private final Path configDir;
    private final ClientOptions options;

    private DynamicClusterLauncher(ClientOptions options) {
        this.options = options;
        this.logDir = Path.of(options.logDir());
        this.buildDir = options.buildDir();
        this.configDir = Path.of(options.configDir());
    }

    public static DynamicClusterLauncher start(ClientOptions options) {
        DynamicClusterLauncher launcher = new DynamicClusterLauncher(options);
        try {
            return launcher;
        } catch (RuntimeException error) {
            launcher.close();
            throw error;
        }
    }

    public DynamicProvider startProvider(String name, String rid) {
        return startProvider(name, rid, rid, "");
    }

    public DynamicProvider startApiProvider(String name, String rid) {
        String httpUrl = pickHttpUrl();
        String apiEndpoint = pickEndpoint();
        DynamicProcess process = startProcess(
            name,
            providerBinary(),
            providerConfig(rid, rid, "", false, apiEndpoint, "", "", "", httpUrl),
            httpUrl);
        process.waitReady();
        return new DynamicProvider(process, httpUrl, apiEndpoint, rid);
    }

    public DynamicProvider startProvider(String name, String rid, String instanceId, String weight) {
        String httpUrl = pickHttpUrl();
        String apiEndpoint = pickEndpoint();
        DynamicProcess process = startProcess(
            name,
            providerBinary(),
            providerConfig(
                rid, instanceId, weight, false, apiEndpoint, "", "", "", httpUrl),
            httpUrl);
        process.waitReady();
        return new DynamicProvider(process, httpUrl, apiEndpoint, rid);
    }

    public DynamicProvider startObjectProvider(String name, String rid, String objectMeshName) {
        String httpUrl = pickHttpUrl();
        String apiEndpoint = pickEndpoint();
        String routeEndpoint = pickEndpoint();
        String objectEndpoint = pickEndpoint();
        DynamicProcess process = startProcess(
            name,
            providerBinary(),
            providerConfig(
                rid, rid, "", true, apiEndpoint, routeEndpoint, objectMeshName, objectEndpoint, httpUrl),
            httpUrl);
        process.waitReady();
        return new DynamicProvider(process, httpUrl, objectEndpoint, rid);
    }

    public DynamicWorkflow startWorkflow(String name, String rid, String objectMeshName) {
        String httpUrl = pickHttpUrl();
        String apiEndpoint = pickEndpoint();
        String routeEndpoint = pickEndpoint();
        String workflowEndpoint = pickEndpoint();
        String objectEndpoint = pickEndpoint();
        DynamicProcess process = startProcess(
            name,
            workflowBinary(),
            workflowConfig(
                rid, rid, apiEndpoint, routeEndpoint, workflowEndpoint,
                objectMeshName, objectEndpoint, httpUrl),
            httpUrl);
        process.waitReady();
        return new DynamicWorkflow(process, httpUrl, objectEndpoint, rid);
    }

    public DynamicConsumer startConsumer(String name) {
        String httpUrl = pickHttpUrl();
        DynamicProcess process = startProcess(
            name,
            consumerBinary(),
            consumerConfig(name, httpUrl),
            httpUrl);
        process.waitReady();
        return new DynamicConsumer(process, httpUrl);
    }

    public void waitPeerCount(ZLinkHttpClient consumer, int count) {
        waitPeers(consumer, peers -> peers.length == count, "peer count " + count);
    }

    public String stop(DynamicProvider provider) {
        String result = provider.process().drainAndStop();
        processes.remove(provider.process());
        return result;
    }

    @Override
    public void close() {
        for (int index = processes.size() - 1; index >= 0; index--) {
            processes.get(index).stop();
        }
        processes.clear();
    }

    private DynamicProcess startProcess(
        String name,
        String binary,
        String config,
        String httpUrl) {
        try {
            Files.createDirectories(logDir);
            Path configPath = writeConfig(name, config);
            Files.writeString(
                logDir.resolve(name + ".launch.properties"),
                config,
                StandardOpenOption.CREATE,
                StandardOpenOption.TRUNCATE_EXISTING,
                StandardOpenOption.WRITE);
            ProcessBuilder builder = new ProcessBuilder(binary, "--config", configPath.toString());
            Path stdoutLog = logDir.resolve(name + ".stdout.log");
            builder.redirectOutput(stdoutLog.toFile());
            builder.redirectError(logDir.resolve(name + ".stderr.log").toFile());
            DynamicProcess process = new DynamicProcess(builder.start(), httpUrl, configPath, stdoutLog);
            processes.add(process);
            return process;
        } catch (IOException error) {
            throw new IllegalStateException("failed to start " + name, error);
        }
    }

    private String providerConfig(
        String rid, String instanceId, String weight, boolean apiClient, String apiEndpoint,
        String routeEndpoint, String objectMeshName, String objectEndpoint, String httpUrl) {
        return """
            e2e.provider-rid=%s
            e2e.provider-instance=%s
            e2e.api-weight=%s
            e2e.api-client=%s
            e2e.api-endpoint=%s
            e2e.route-endpoint=%s
            e2e.route-peers=
            e2e.workflow-endpoint=
            e2e.object-mesh-name=%s
            e2e.object-endpoint=%s
            e2e.http-port=%s
            server.port=${e2e.http-port}
            e2e.redis-location-endpoint=%s
            e2e.location-key-prefix=%s
            e2e.log-dir=%s
            """.formatted(rid, instanceId, weight, apiClient, apiEndpoint,
                routeEndpoint, objectMeshName, objectEndpoint, portOf(httpUrl), options.redisLocationEndpoint(),
                options.locationKeyPrefix(), logDir);
    }

    private String workflowConfig(
        String rid, String instanceId, String apiEndpoint, String routeEndpoint,
        String workflowEndpoint, String objectMeshName, String objectEndpoint, String httpUrl) {
        return """
            e2e.provider-rid=%s
            e2e.provider-instance=%s
            e2e.api-weight=
            e2e.api-endpoint=%s
            e2e.route-endpoint=%s
            e2e.workflow-endpoint=%s
            e2e.object-mesh-name=%s
            e2e.object-endpoint=%s
            e2e.http-port=%s
            server.port=${e2e.http-port}
            e2e.redis-location-endpoint=%s
            e2e.location-key-prefix=%s
            e2e.log-dir=%s
            """.formatted(rid, instanceId, apiEndpoint, routeEndpoint, workflowEndpoint,
                objectMeshName, objectEndpoint, portOf(httpUrl), options.redisLocationEndpoint(),
                options.locationKeyPrefix(), logDir);
    }

    private String consumerConfig(String name, String httpUrl) {
        return """
            e2e.consumer-name=%s
            e2e.consumer-mode=discovery
            e2e.provider-endpoints=
            e2e.http-port=%s
            server.port=${e2e.http-port}
            e2e.redis-location-endpoint=%s
            e2e.location-key-prefix=%s
            e2e.log-dir=%s
            """.formatted(name, portOf(httpUrl), options.redisLocationEndpoint(),
                options.locationKeyPrefix(), logDir);
    }

    private Path writeConfig(String name, String contents) throws IOException {
        Files.createDirectories(configDir);
        Path path = configDir.resolve(name + "-" + System.nanoTime() + ".properties");
        Files.writeString(path, contents, StandardCharsets.UTF_8, StandardOpenOption.CREATE_NEW);
        Files.setPosixFilePermissions(path, Set.of(
            PosixFilePermission.OWNER_READ, PosixFilePermission.OWNER_WRITE));
        return path;
    }

    private String providerBinary() {
        return buildDir + "/Server-Provider/install/registry-messaging-provider/bin/registry-messaging-provider";
    }

    private String consumerBinary() {
        return buildDir + "/Server-Consumer/install/registry-messaging-consumer/bin/registry-messaging-consumer";
    }

    private String workflowBinary() {
        return buildDir + "/Server-Workflow/install/registry-messaging-workflow/bin/registry-messaging-workflow";
    }

    private static void waitPeers(
        ZLinkHttpClient consumer,
        Predicate<Map<String, Object>[]> predicate,
        String description) {
        long deadline = System.nanoTime() + ROUTE_SETTLE_TIMEOUT.toNanos();
        Map<String, Object>[] latest = new Map[0];
        while (System.nanoTime() < deadline) {
            latest = consumer.get("/locations/peers").submit(Map[].class).toCompletableFuture().join().body();
            if (predicate.test(latest)) {
                return;
            }
            DynamicProcess.sleep(100);
        }
        throw new IllegalStateException("timed out waiting for " + description + ": " + java.util.Arrays.toString(latest));
    }

    private static String pickEndpoint() {
        return "tcp://127.0.0.1:" + pickPort();
    }

    private static String pickHttpUrl() {
        return "http://127.0.0.1:" + pickPort();
    }

    private static int pickPort() {
        try (ServerSocket socket = new ServerSocket(0)) {
            socket.setReuseAddress(true);
            return socket.getLocalPort();
        } catch (IOException error) {
            throw new IllegalStateException("failed to reserve port", error);
        }
    }

    private static String portOf(String endpoint) {
        int index = endpoint.lastIndexOf(':');
        return endpoint.substring(index + 1);
    }

    public record DynamicProvider(
        DynamicProcess process,
        String httpUrl,
        String channelEndpoint,
        String routingId) {
    }

    public record DynamicConsumer(DynamicProcess process, String httpUrl) {
    }

    public record DynamicWorkflow(
        DynamicProcess process,
        String httpUrl,
        String objectEndpoint,
        String routingId) {
    }

    public static final class DynamicProcess {
        private final Process process;
        private final String httpUrl;
        private final ZLinkHttpClient healthClient;
        private final Path configPath;
        private final Path stdoutLog;
        private boolean stopped;

        DynamicProcess(Process process, String httpUrl, Path configPath, Path stdoutLog) {
            this.process = process;
            this.httpUrl = httpUrl;
            this.configPath = configPath;
            this.stdoutLog = stdoutLog;
            this.healthClient = ZLinkHttpClient.create(httpUrl)
                .timeout(Duration.ofMillis(300))
                .build();
        }

        void waitReady() {
            long deadline = System.nanoTime() + LOCAL_READINESS_TIMEOUT.toNanos();
            while (System.nanoTime() < deadline) {
                if (!process.isAlive()) {
                    throw new IllegalStateException("process exited before readiness: " + process.exitValue());
                }
                try {
                    var response = healthClient.get("/health").submitRaw().toCompletableFuture().join();
                    if (response.status() >= 200 && response.status() < 300
                        && frameworkIsReady()) {
                        return;
                    }
                } catch (RuntimeException error) {
                    // The process may accept TCP before its HTTP handler is ready.
                }
                sleep(100);
            }
            throw new IllegalStateException("timed out waiting for " + httpUrl);
        }

        private boolean frameworkIsReady() {
            try {
                return Files.exists(stdoutLog)
                    && Files.readString(stdoutLog).contains("ZLINK_FRAMEWORK_READY");
            } catch (IOException error) {
                return false;
            }
        }

        void stop() {
            if (stopped) {
                return;
            }
            stopped = true;
            healthClient.close();
            process.destroy();
            try {
                if (!process.waitFor(5, TimeUnit.SECONDS)) {
                    process.destroyForcibly();
                    process.waitFor(5, TimeUnit.SECONDS);
                }
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
                process.destroyForcibly();
            } finally {
                try {
                    Files.deleteIfExists(configPath);
                } catch (IOException ignored) {
                }
            }
        }

        public void crash() {
            if (stopped) {
                return;
            }
            stopped = true;
            healthClient.close();
            process.destroyForcibly();
            try {
                if (!process.waitFor(10, TimeUnit.SECONDS)) {
                    throw new IllegalStateException("crashed process did not terminate: " + httpUrl);
                }
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
                throw new IllegalStateException("interrupted while crashing " + httpUrl, error);
            } finally {
                try {
                    Files.deleteIfExists(configPath);
                } catch (IOException ignored) {
                }
            }
        }

        String drainAndStop() {
            try (ZLinkHttpClient drainClient = ZLinkHttpClient.create(httpUrl)
                .timeout(Duration.ofSeconds(35))
                .build()) {
                @SuppressWarnings("unchecked")
                Map<String, Object> response = drainClient.post("/admin/drain")
                    .submit(Map.class).toCompletableFuture().join().body();
                String result = String.valueOf(response.get("result"));
                stop();
                return result;
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
}
