package systems.zlink.e2e.channelegress.role;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;
import java.net.InetSocketAddress;
import java.net.URI;
import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import org.springframework.context.SmartLifecycle;
import systems.zlink.e2e.channelegress.shared.Contracts;
import systems.zlink.e2e.channelegress.shared.EvidenceState;
import systems.zlink.framework.channels.ZLinkClient;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.monitoring.ZLinkClientServerRuntime;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle;

public final class ChannelEgressHttpServer implements SmartLifecycle {
    private final RoleOptions options;
    private final EvidenceState evidence;
    private final ObjectMapper json;
    private final ZLinkClient client;
    private final ZLinkRouteClient routes;
    private final ZLinkRouteMeshRuntime routeRuntime;
    private final ZLinkClientServerRuntime clientServerRuntime;
    private final ZLinkFrameworkLifecycle lifecycle;
    private HttpServer server;
    private ExecutorService executor;
    private boolean running;

    public ChannelEgressHttpServer(
        RoleOptions options,
        EvidenceState evidence,
        ObjectMapper json,
        ZLinkClient client,
        ZLinkRouteClient routes,
        ZLinkRouteMeshRuntime routeRuntime,
        ZLinkClientServerRuntime clientServerRuntime,
        ZLinkFrameworkLifecycle lifecycle) {
        this.options = options;
        this.evidence = evidence;
        this.json = json;
        this.client = client;
        this.routes = routes;
        this.routeRuntime = routeRuntime;
        this.clientServerRuntime = clientServerRuntime;
        this.lifecycle = lifecycle;
    }

    @Override
    public void start() {
        try {
            URI endpoint = URI.create(options.httpEndpoint());
            server = HttpServer.create(
                new InetSocketAddress(endpoint.getHost(), endpoint.getPort()), 0);
            executor = Executors.newVirtualThreadPerTaskExecutor();
            server.setExecutor(executor);
            server.createContext("/health", exchange ->
                HttpSupport.writeJson(exchange, json, Map.of(
                    "status", "ready",
                    "role", options.role(),
                    "rid", options.rid())));
            server.createContext("/evidence", exchange ->
                HttpSupport.writeJson(exchange, json, evidence.snapshot()));
            server.createContext("/evidence/wait", exchange -> {
                try {
                    waitEvidence(exchange);
                } catch (Exception error) {
                    HttpSupport.writeJson(exchange, json, 500, Map.of("error", error.toString()));
                }
            });
            server.createContext("/request", exchange -> {
                try {
                    request(exchange);
                } catch (Exception error) {
                    HttpSupport.writeJson(exchange, json, 500, Map.of("error", error.toString()));
                }
            });
            server.createContext("/send", exchange -> {
                try {
                    send(exchange);
                } catch (Exception error) {
                    HttpSupport.writeJson(exchange, json, 500, Map.of("error", error.toString()));
                }
            });
            server.createContext("/status/route", exchange ->
                HttpSupport.writeJson(exchange, json, routeStatus(Contracts.GAME_MESH)));
            server.createContext("/status/audit", exchange ->
                HttpSupport.writeJson(exchange, json, routeStatus(Contracts.AUDIT_MESH)));
            server.createContext("/status/workflow", exchange ->
                HttpSupport.writeJson(exchange, json, workflowStatus()));
            server.createContext("/control/hold", exchange -> {
                evidence.hold();
                HttpSupport.writeJson(exchange, json, Map.of("status", "held"));
            });
            server.createContext("/control/release", exchange -> {
                evidence.release();
                HttpSupport.writeJson(exchange, json, Map.of("status", "released"));
            });
            server.createContext("/shutdown", exchange -> {
                HttpSupport.writeJson(exchange, json, Map.of("status", "stopping"));
                lifecycle.shutdown(Duration.ofSeconds(10));
            });
            server.start();
            running = true;
        } catch (Exception error) {
            throw new IllegalStateException(
                "failed to start channel-egress HTTP endpoint " + options.httpEndpoint(), error);
        }
    }

    private void waitEvidence(HttpExchange exchange) throws Exception {
        Contracts.EvidenceWaitReq request = HttpSupport.readJson(
            exchange, json, Contracts.EvidenceWaitReq.class);
        long deadline = System.nanoTime()
            + Math.max(1, request.timeoutMilliseconds()) * 1_000_000L;
        while (System.nanoTime() < deadline && !evidence.contains(request.contains())) {
            Thread.sleep(25);
        }
        if (!evidence.contains(request.contains())) {
            HttpSupport.writeJson(exchange, json, 408, evidence.snapshot());
            return;
        }
        HttpSupport.writeJson(exchange, json, evidence.snapshot());
    }

    private void request(HttpExchange exchange) throws Exception {
        Contracts.InvokeReq request = HttpSupport.readJson(exchange, json, Contracts.InvokeReq.class);
        long started = System.nanoTime();
        try {
            Contracts.ChannelProbeRes reply = requestChannel(request);
            HttpSupport.writeJson(exchange, json, new Contracts.InvokeRes(
                true,
                null,
                reply,
                elapsedMillis(started)));
        } catch (Throwable error) {
            evidence.add("request-error", describe(error));
            HttpSupport.writeJson(exchange, json, new Contracts.InvokeRes(
                false,
                publicErrorKind(error),
                null,
                elapsedMillis(started)));
        }
    }

    private void send(HttpExchange exchange) throws Exception {
        Contracts.InvokeReq request = HttpSupport.readJson(exchange, json, Contracts.InvokeReq.class);
        long started = System.nanoTime();
        try {
            sendChannel(request);
            HttpSupport.writeJson(exchange, json, new Contracts.SendRes(
                true, null, elapsedMillis(started)));
        } catch (Throwable error) {
            evidence.add("send-error", describe(error));
            HttpSupport.writeJson(exchange, json, new Contracts.SendRes(
                false, publicErrorKind(error), elapsedMillis(started)));
        }
    }

    private Contracts.ChannelProbeRes requestChannel(Contracts.InvokeReq request) {
        if (Contracts.WORKFLOW_CHANNEL.equals(request.channel())) {
            return client.requestToChannel(
                    request.channel(),
                    new Contracts.ChannelProbeReq(request.id(), request.mode()))
                .timeout(Duration.ofSeconds(5))
                .submit(Contracts.ChannelProbeRes.class)
                .toCompletableFuture()
                .join();
        }
        return routes.requestToChannel(
                request.channel(),
                new Contracts.ChannelProbeReq(request.id(), request.mode()))
            .timeout(Duration.ofSeconds(5))
            .submit(Contracts.ChannelProbeRes.class)
            .toCompletableFuture()
            .join();
    }

    private void sendChannel(Contracts.InvokeReq request) {
        if (Contracts.WORKFLOW_CHANNEL.equals(request.channel())) {
            client.sendToChannel(
                    request.channel(),
                    new Contracts.ChannelProbeMsg(request.id()))
                .submit()
                .toCompletableFuture()
                .join();
            return;
        }
        routes.sendToChannel(
                request.channel(),
                new Contracts.ChannelProbeMsg(request.id()))
            .submit()
            .toCompletableFuture()
            .join();
    }

    private Map<String, Object> routeStatus(String meshName) {
        var snapshot = routeRuntime.snapshot(meshName);
        return Map.of(
            "state", snapshot.state().name(),
            "isReady", snapshot.isReady(),
            "readyPeerCount", snapshot.readyPeerCount(),
            "peers", snapshot.peers().stream().map(peer -> Map.of(
                "rid", peer.nodeRid().toString(),
                "state", peer.state().name())).toList(),
            "channels", snapshot.channels().stream().map(channel -> Map.of(
                "channelName", channel.channelName(),
                "isReady", channel.isReady(),
                "readyTargetCount", channel.readyTargetCount())).toList());
    }

    private Map<String, Object> workflowStatus() {
        var snapshot = clientServerRuntime.snapshot(Contracts.WORKFLOW_CHANNEL);
        return Map.of(
            "state", snapshot.state().name(),
            "isReady", snapshot.isReady(),
            "readyTargetCount", snapshot.readyTargetCount(),
            "localRole", snapshot.localRole().name(),
            "targets", snapshot.targets().stream().map(target -> Map.of(
                "rid", target.nodeRid().toString(),
                "weight", target.weight(),
                "state", target.state().name())).toList());
    }

    private static long elapsedMillis(long started) {
        return Math.max(0, (System.nanoTime() - started) / 1_000_000L);
    }

    private static String publicErrorKind(Throwable error) {
        Throwable current = error;
        while (current instanceof CompletionException && current.getCause() != null) {
            current = current.getCause();
        }
        if (current instanceof ZLinkFrameworkException frameworkError) {
            return frameworkError.kind().name();
        }
        return current.getClass().getSimpleName();
    }

    private static String describe(Throwable error) {
        Throwable current = error;
        while (current instanceof CompletionException && current.getCause() != null) {
            current = current.getCause();
        }
        String message = current.getMessage();
        return current.getClass().getSimpleName()
            + (message == null || message.isBlank() ? "" : ":" + message);
    }

    @Override
    public void stop() {
        if (server != null) {
            server.stop(0);
            server = null;
        }
        if (executor != null) {
            executor.shutdownNow();
            executor = null;
        }
        running = false;
    }

    @Override
    public boolean isRunning() {
        return running;
    }
}
