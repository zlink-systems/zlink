package systems.zlink.e2e.runtimemonitoring.service.support;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpServer;
import java.net.InetSocketAddress;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import org.springframework.beans.factory.ObjectProvider;
import org.springframework.context.ConfigurableApplicationContext;
import org.springframework.context.SmartLifecycle;
import systems.zlink.e2e.runtimemonitoring.shared.Contracts;
import systems.zlink.framework.channels.ZLinkChannelRuntimeOptions;
import systems.zlink.framework.channels.ZLinkRouteMeshRuntimeOptions;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.ZLinkMeshNodeSnapshot;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.framework.spots.ZLinkSpotPublisherClient;

public final class EvidenceHttpServer implements SmartLifecycle {
    private final EvidenceState state;
    private final ObjectMapper json;
    private final ZLinkChannelRuntimeOptions runtimeOptions;
    private final ZLinkRouteMeshRuntimeOptions meshRuntimeOptions;
    private final ZLinkRouteClient routeClient;
    private final ObjectProvider<ZLinkRouteMeshRuntime> meshRuntime;
    private final ObjectProvider<ZLinkFrameworkLifecycle> runtimeQuery;
    private final ObserverIsolationProbe observerIsolation;
    private final ObjectProvider<ZLinkSpotManager> spots;
    private final ObjectProvider<ZLinkSpotPublisherClient> publisher;
    private final ConfigurableApplicationContext applicationContext;
    private final String endpoint;
    private HttpServer server;
    private boolean running;

    public EvidenceHttpServer(
        EvidenceState state,
        ObjectMapper json,
        ZLinkChannelRuntimeOptions runtimeOptions,
        ZLinkRouteMeshRuntimeOptions meshRuntimeOptions,
        ZLinkRouteClient routeClient,
        ObjectProvider<ZLinkRouteMeshRuntime> meshRuntime,
        ObjectProvider<ZLinkFrameworkLifecycle> runtimeQuery,
        ObserverIsolationProbe observerIsolation,
        ObjectProvider<ZLinkSpotManager> spots,
        ObjectProvider<ZLinkSpotPublisherClient> publisher,
        ConfigurableApplicationContext applicationContext,
        String endpoint) {
        this.state = state;
        this.json = json;
        this.runtimeOptions = runtimeOptions;
        this.meshRuntimeOptions = meshRuntimeOptions;
        this.routeClient = routeClient;
        this.meshRuntime = meshRuntime;
        this.runtimeQuery = runtimeQuery;
        this.observerIsolation = observerIsolation;
        this.spots = spots;
        this.publisher = publisher;
        this.applicationContext = applicationContext;
        this.endpoint = endpoint;
    }

    @Override
    public void start() {
        if (endpoint == null || endpoint.isBlank()) {
            return;
        }
        try {
            URI uri = URI.create(endpoint);
            server = HttpServer.create(new InetSocketAddress(uri.getHost(), uri.getPort()), 0);
            server.createContext("/health", exchange -> write(exchange, "ok\n"));
            server.createContext("/evidence", exchange -> write(
                exchange,
                json.writeValueAsString(state.snapshot())));
            server.createContext("/runtime/snapshot", exchange -> write(
                exchange,
                json.writeValueAsString(runtimeSnapshot())));
            server.createContext("/runtime/observer/start", exchange -> write(
                exchange,
                json.writeValueAsString(observerIsolation.start(meshRuntime.getObject()))));
            server.createContext("/runtime/observer/status", exchange -> write(
                exchange,
                json.writeValueAsString(observerIsolation.status())));
            server.createContext("/runtime/observer/release", exchange -> write(
                exchange,
                json.writeValueAsString(observerIsolation.release())));
            server.createContext("/runtime/weight/zero", exchange -> {
                setMeshWeight(0);
                write(exchange, json.writeValueAsString(new AdminResult("weight-updated", 0)));
            });
            server.createContext("/runtime/weight/restore", exchange -> {
                setMeshWeight(100);
                write(exchange, json.writeValueAsString(new AdminResult("weight-updated", 100)));
            });
            server.createContext("/runtime/request", exchange -> {
                try {
                    Contracts.WorkReq request =
                        json.readValue(exchange.getRequestBody(), Contracts.WorkReq.class);
                    Contracts.WorkRes response = routeClient.requestToChannel(
                            Contracts.SPOT_CHANNEL,
                            request)
                        .timeout(java.time.Duration.ofSeconds(5))
                        .submit(Contracts.WorkRes.class)
                        .toCompletableFuture()
                        .join();
                    write(exchange, json.writeValueAsString(response));
                } catch (Throwable error) {
                    Throwable cause = error.getCause() == null ? error : error.getCause();
                    cause.printStackTrace(System.err);
                    String failure = cause instanceof ZLinkFrameworkException frameworkError
                        ? frameworkError.kind().name() + ": " + frameworkError.getMessage()
                        : cause.getClass().getName() + ": " + cause.getMessage();
                    write(exchange, 500, failure);
                }
            });
            server.createContext("/admin/drain", exchange -> {
                setWeight(0, "drain");
                write(exchange, json.writeValueAsString(new AdminResult("drained", 0)));
            });
            server.createContext("/admin/restore", exchange -> {
                setWeight(100, "restore");
                write(exchange, json.writeValueAsString(new AdminResult("restored", 100)));
            });
            server.createContext("/admin/crash", exchange -> {
                write(exchange, json.writeValueAsString(new AdminResult("crashing", -1)));
                Thread crash = new Thread(
                    () -> Runtime.getRuntime().halt(137),
                    "runtime-monitoring-crash");
                crash.setDaemon(false);
                crash.start();
            });
            server.createContext("/runtime/unknown-mesh", exchange -> {
                try {
                    requireMeshRuntime().snapshot("runtime-monitoring-unknown-mesh");
                    write(exchange, 500, "unknown mesh unexpectedly resolved");
                } catch (RuntimeException error) {
                    write(exchange, 400, error.getClass().getName() + ": " + error.getMessage());
                }
            });
            server.createContext("/admin/create-subject-spot", exchange -> {
                try {
                    ZLinkSpotManager manager = spots.getIfAvailable();
                    if (manager == null) {
                        throw new IllegalStateException("spot manager is not configured");
                    }
                    manager.getOrCreate(
                            "monitoring-subject-trigger",
                            Contracts.TRIGGERED_MONITORING_SPOT_TYPE)
                        .request(ZLinkMessage.of("monitoring-subject-trigger"))
                        .submit()
                        .toCompletableFuture()
                        .join();
                    write(exchange, json.writeValueAsString(new AdminResult("subject-created", -1)));
                } catch (RuntimeException error) {
                    Throwable cause = error.getCause() == null ? error : error.getCause();
                    write(exchange, 500, cause.getClass().getName() + ": " + cause.getMessage());
                }
            });
            server.createContext("/runtime/publish/", exchange -> {
                String topic = exchange.getRequestURI().getPath()
                    .substring("/runtime/publish/".length());
                ZLinkSpotPublisherClient publishClient = publisher.getIfAvailable();
                if (publishClient == null) {
                    write(exchange, 400, "spot publisher is not configured");
                    return;
                }
                publishClient.publish(
                        Contracts.SPOT_MESH,
                        Contracts.SPOT_CHANNEL,
                        topic,
                        new Contracts.SpotSubjectProbe(topic))
                    .submit()
                    .toCompletableFuture()
                    .join();
                write(exchange, json.writeValueAsString(new AdminResult("published", -1)));
            });
            server.createContext("/shutdown", exchange -> {
                write(exchange, json.writeValueAsString(new AdminResult("stopping", -1)));
                Thread shutdown = new Thread(applicationContext::close, "runtime-monitoring-shutdown");
                shutdown.setDaemon(false);
                shutdown.start();
            });
            server.start();
            running = true;
        } catch (Exception error) {
            throw new IllegalStateException("failed to start evidence endpoint " + endpoint, error);
        }
    }

    private void setWeight(int weight, String action) {
        runtimeOptions
            .clientServerChannel(Contracts.CHANNEL)
            .configureServerSocket()
            .weight(weight);
        state.record("admin", state.rid(), action, "weight=" + weight);
    }

    private void setMeshWeight(int weight) {
        meshRuntimeOptions
            .channel(Contracts.SPOT_MESH, Contracts.SPOT_CHANNEL)
            .weight(weight);
    }

    private ZLinkRouteMeshRuntime requireMeshRuntime() {
        ZLinkRouteMeshRuntime runtime = meshRuntime.getIfAvailable();
        if (runtime == null) {
            throw new IllegalStateException("RouteMesh runtime is not configured");
        }
        return runtime;
    }

    private Contracts.RuntimeSnapshot runtimeSnapshot() {
        ZLinkRouteMeshRuntime runtime = requireMeshRuntime();
        ZLinkMeshNodeSnapshot snapshot = runtime.snapshot(Contracts.SPOT_MESH);
        var host = runtimeQuery.getObject().status();
        return new Contracts.RuntimeSnapshot(
            snapshot.meshName(),
            snapshot.state().name(),
            snapshot.isReady(),
            snapshot.readyPeerCount(),
            snapshot.sequence(),
            snapshot.observedAt().toString(),
            snapshot.peers().stream().map(peer -> new Contracts.RuntimePeer(
                peer.nodeRid().toHex(),
                peer.state().name(),
                peer.unavailableReason().map(Enum::name).orElse(""))).toList(),
            snapshot.channels().stream().map(channel -> new Contracts.RuntimeChannel(
                channel.channelName(),
                channel.isReady(),
                channel.readyTargetCount())).toList(),
            snapshot.placement().isAvailable(),
            snapshot.placement().activeActorCount(),
            snapshot.placement().activeSpotCount(),
            snapshot.placement().unavailableReason().map(Enum::name).orElse(""),
            host.state().name());
    }

    private record AdminResult(String status, int weight) {
    }

    private static void write(
        com.sun.net.httpserver.HttpExchange exchange,
        String value) throws java.io.IOException {
        write(exchange, 200, value);
    }

    private static void write(
        com.sun.net.httpserver.HttpExchange exchange,
        int status,
        String value) throws java.io.IOException {
        byte[] body = value.getBytes(StandardCharsets.UTF_8);
        exchange.getResponseHeaders().add("Content-Type", "application/json");
        exchange.sendResponseHeaders(status, body.length);
        exchange.getResponseBody().write(body);
        exchange.close();
    }

    @Override
    public void stop() {
        if (server != null) {
            server.stop(0);
            server = null;
        }
        running = false;
    }

    @Override
    public boolean isRunning() {
        return running;
    }
}
