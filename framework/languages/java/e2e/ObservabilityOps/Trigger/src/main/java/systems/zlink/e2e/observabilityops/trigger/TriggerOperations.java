package systems.zlink.e2e.observabilityops.trigger;

import java.time.Duration;
import java.net.URI;
import java.util.Map;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.stream.connector.ZLinkStreamCodec;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions;
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory;
import systems.zlink.stream.connector.ZLinkStreamEncodedPayload;
import com.fasterxml.jackson.databind.ObjectMapper;
import io.micrometer.core.instrument.Metrics;
import io.micrometer.core.instrument.simple.SimpleMeterRegistry;
import java.nio.file.Path;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import systems.zlink.httpclient.RawHttpResponse;
import systems.zlink.httpclient.ZLinkHttpClient;

/** Sends a real unknown packet so the Session runtime emits received and error flow events. */
public final class TriggerOperations {
    private TriggerOperations() { }

    public static void main(String[] args) throws Exception {
        if (args.length == 3 && "--metrics-b1".equals(args[0])) {
            runMetricsB1(args[1], Path.of(args[2]));
            return;
        }
        if (args.length == 3 && "--reader-free-b4".equals(args[0])) {
            runReaderFreeB4(args[1], Path.of(args[2]));
            return;
        }
        if (args.length == 4 && "--drain-watch".equals(args[0])) {
            runDrainWatch(args[1], args[2], Path.of(args[3]));
            return;
        }
        if (args.length != 1) throw new IllegalArgumentException("Usage: observability-ops-trigger <endpoint>");
        runMissingHandler(args[0]);
    }

    public static void runMissingHandler(String endpoint) throws Exception {
        ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
            ZLinkStreamConnectorOptions.createDefault(URI.create(endpoint)));
        try {
            connector.connect().submit().toCompletableFuture().get();
            try {
                connector.request(new ZLinkStreamEncodedPayload(
                        "ObservabilityMissingPacket",
                        Message.from(new byte[] {(byte) 0xff, 0x00}),
                        Map.of(),
                        ZLinkStreamCodec.RAW))
                    .timeout(Duration.ofSeconds(5))
                    .submit()
                    .toCompletableFuture().join();
                throw new IllegalStateException("missing packet unexpectedly succeeded");
            } catch (java.util.concurrent.CompletionException expected) {
                System.out.println("OBS-A2 missing-handler reply=" + expected.getCause().getClass().getSimpleName());
            }
        } finally {
            connector.close().submit().toCompletableFuture().join();
        }
    }

    public static void runMetricsB1(String endpoint, Path output) throws Exception {
        SimpleMeterRegistry registry = new SimpleMeterRegistry();
        Metrics.addRegistry(registry);
        ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
            unlimitedReconnectOptions(endpoint));
        try {
            connector.connect().submit().toCompletableFuture().join();
            probeLifecycle(connector);
            for (int expectedCycle = 1; expectedCycle <= 3; expectedCycle++) {
                forceReconnect(connector, expectedCycle);
                connector.connect().submit().toCompletableFuture().get(30, TimeUnit.SECONDS);
                probeLifecycle(connector);
            }
            double reconnects = registry.get("zlink.stream.reconnects").counter().count();
            new ObjectMapper().writeValue(output.toFile(), List.of(Map.of(
                "name", "zlink.stream.reconnects",
                "kind", "counter",
                "unit", "{event}",
                "value", reconnects,
                "count", Math.round(reconnects),
                "tags", Map.of())));
        } finally {
            connector.close().submit().toCompletableFuture().join();
            Metrics.removeRegistry(registry);
            registry.close();
        }
    }

    private static void forceReconnect(ZLinkStreamConnector connector, int cycle) {
        try {
            connector.request(new ForceReconnectReq(cycle))
                .timeout(Duration.ofSeconds(5))
                .submit(Void.class)
                .toCompletableFuture().join();
            throw new IllegalStateException("force reconnect request unexpectedly received a reply");
        } catch (java.util.concurrent.CompletionException expected) {
            // Closing the server-side session fails the pending request and starts automatic reconnect.
        }
    }

    private static ZLinkStreamConnectorOptions unlimitedReconnectOptions(String endpoint) {
        ZLinkStreamConnectorOptions defaults =
            ZLinkStreamConnectorOptions.createDefault(URI.create(endpoint));
        return new ZLinkStreamConnectorOptions(
            defaults.endpoint(),
            defaults.dispatchMode(),
            defaults.requestTimeout(),
            defaults.waitTimeout(),
            ZLinkStreamConnectorOptions.UNLIMITED_RECONNECT_ATTEMPTS,
            defaults.connectTimeout(),
            defaults.maxSendPayloadSize(),
            defaults.maxReceivePayloadSize(),
            defaults.maxReceivedMessages(),
            defaults.heartbeatEnabled(),
            defaults.heartbeatInterval(),
            defaults.heartbeatTimeout(),
            defaults.reconnectEnabled(),
            defaults.reconnectInitialDelay(),
            defaults.reconnectMaxDelay(),
            defaults.reconnectBackoffFactor(),
            defaults.skipServerCertificateValidation(),
            defaults.compression(),
            defaults.compressionCodec(),
            defaults.nameResolver(),
            defaults.typedCodec());
    }

    private record ForceReconnectReq(int cycle) { }

    private static void probeLifecycle(ZLinkStreamConnector connector) {
        try {
            connector.request(new ZLinkStreamEncodedPayload(
                    "MetricsLifecycleProbe",
                    Message.from(new byte[] {1}),
                    Map.of(),
                    ZLinkStreamCodec.RAW))
                .timeout(Duration.ofSeconds(5))
                .submit().toCompletableFuture().join();
        } catch (java.util.concurrent.CompletionException expected) {
            // The missing-handler error proves the application session processed the frame.
        }
    }

    public static void runReaderFreeB4(String endpoint, Path output) throws Exception {
        ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
            ZLinkStreamConnectorOptions.createDefault(URI.create(endpoint)));
        long trafficEvents = 0;
        boolean messagingAccurate = false;
        try {
            connector.connect().submit().toCompletableFuture().join();
            for (int i = 0; i < 8192; i++) {
                connector.send(new ZLinkStreamEncodedPayload(
                    "ReaderFreeTraffic",
                    Message.from(new byte[] {(byte) (i & 0xff)}),
                    Map.of(),
                    ZLinkStreamCodec.RAW)).submit();
                trafficEvents++;
            }
            try {
                connector.request(new ZLinkStreamEncodedPayload(
                        "ReaderFreeProbe",
                        Message.from(new byte[] {1}),
                        Map.of(),
                        ZLinkStreamCodec.RAW))
                    .timeout(Duration.ofSeconds(20))
                    .submit().toCompletableFuture().join();
            } catch (java.util.concurrent.CompletionException expected) {
                messagingAccurate = true;
            }
            new ObjectMapper().writeValue(output.toFile(), Map.of(
                "trafficEvents", trafficEvents,
                "messagingAccurate", messagingAccurate));
        } finally {
            connector.close().submit().toCompletableFuture().join();
        }
    }

    public static void runDrainWatch(String endpoint, String drainUrl, Path output) throws Exception {
        ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
            ZLinkStreamConnectorOptions.createDefault(URI.create(endpoint)));
        CompletableFuture<String> disconnected = new CompletableFuture<>();
        try (AutoCloseable ignored = connector.onDisconnected(event -> {
            disconnected.complete(event.closeReason().name().toLowerCase());
            return CompletableFuture.completedFuture(null);
        })) {
            connector.connect().submit().toCompletableFuture().join();
            probeLifecycle(connector);
            URI target = URI.create(drainUrl);
            String baseUrl = target.getScheme() + "://" + target.getRawAuthority();
            String path = target.getRawPath();
            if (target.getRawQuery() != null) {
                path += "?" + target.getRawQuery();
            }
            RawHttpResponse response = ZLinkHttpClient.create(baseUrl)
                .get(path)
                .submitRaw()
                .toCompletableFuture()
                .join();
            if (response.status() < 200 || response.status() >= 300) {
                throw new IllegalStateException(
                    "drain request returned " + response.status() + ": " + response.body());
            }
            String closeReason = disconnected.get(15, TimeUnit.SECONDS);
            new ObjectMapper().writeValue(output.toFile(), Map.of("closeReason", closeReason));
        } finally {
            if (connector.state() != systems.zlink.stream.connector.ZLinkStreamConnectionState.CLOSED) {
                connector.close().submit().toCompletableFuture().join();
            }
        }
    }
}
