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
import org.springframework.beans.factory.ObjectProvider;
import systems.zlink.e2e.channelegress.shared.Contracts;
import systems.zlink.e2e.channelegress.shared.EvidenceState;
import systems.zlink.framework.channels.ZLinkClient;
import systems.zlink.framework.channels.ZLinkChannelRuntimeOptions;
import systems.zlink.framework.channels.ZLinkFanoutClient;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.monitoring.ZLinkClientServerRuntime;
import systems.zlink.framework.monitoring.ZLinkFanoutRuntime;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.locations.ZLinkLocationTopologyFilter;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;

public final class ChannelEgressHttpServer implements SmartLifecycle {
    private final RoleOptions options;
    private final EvidenceState evidence;
    private final ObjectMapper json;
    private final ZLinkClient client;
    private final ZLinkFanoutClient fanout;
    private final ZLinkRouteClient routes;
    private final ZLinkChannelRuntimeOptions runtimeOptions;
    private final ZLinkSpotManager spots;
    private final ZLinkActorManager actors;
    private final ZLinkRouteMeshRuntime routeRuntime;
    private final ZLinkClientServerRuntime clientServerRuntime;
    private final ZLinkFanoutRuntime fanoutRuntime;
    private final ZLinkFrameworkLifecycle lifecycle;
    private final ObjectProvider<ZLinkFrameworkRuntime> runtime;
    private HttpServer server;
    private ExecutorService executor;
    private boolean running;

    public ChannelEgressHttpServer(
        RoleOptions options,
        EvidenceState evidence,
        ObjectMapper json,
        ZLinkClient client,
        ZLinkFanoutClient fanout,
        ZLinkRouteClient routes,
        ZLinkChannelRuntimeOptions runtimeOptions,
        ZLinkSpotManager spots,
        ZLinkActorManager actors,
        ZLinkRouteMeshRuntime routeRuntime,
        ZLinkClientServerRuntime clientServerRuntime,
        ZLinkFanoutRuntime fanoutRuntime,
        ZLinkFrameworkLifecycle lifecycle,
        ObjectProvider<ZLinkFrameworkRuntime> runtime) {
        this.options = options;
        this.evidence = evidence;
        this.json = json;
        this.client = client;
        this.fanout = fanout;
        this.routes = routes;
        this.runtimeOptions = runtimeOptions;
        this.spots = spots;
        this.actors = actors;
        this.routeRuntime = routeRuntime;
        this.clientServerRuntime = clientServerRuntime;
        this.fanoutRuntime = fanoutRuntime;
        this.lifecycle = lifecycle;
        this.runtime = runtime;
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
            server.createContext("/status/fanout", exchange ->
                HttpSupport.writeJson(exchange, json, fanoutStatus()));
            server.createContext("/status/locations", exchange ->
                HttpSupport.writeJson(exchange, json, locationTopology()));
            server.createContext("/status/listeners", exchange ->
                HttpSupport.writeJson(exchange, json, listenerStatuses()));
            server.createContext("/fanout/publish", exchange -> {
                try {
                    fanoutPublish(exchange);
                } catch (Exception error) {
                    HttpSupport.writeJson(exchange, json, 500, Map.of("error", error.toString()));
                }
            });
            server.createContext("/objects/spots", exchange -> {
                try {
                    spots(exchange);
                } catch (Exception error) {
                    HttpSupport.writeJson(exchange, json, 500, Map.of("error", error.toString()));
                }
            });
            server.createContext("/objects/actors", exchange -> {
                try {
                    actors(exchange);
                } catch (Exception error) {
                    HttpSupport.writeJson(exchange, json, 500, Map.of("error", error.toString()));
                }
            });
            server.createContext("/objects/state-address", exchange -> {
                try {
                    stateAddress(exchange);
                } catch (Exception error) {
                    HttpSupport.writeJson(exchange, json, 500, Map.of("error", error.toString()));
                }
            });
            server.createContext("/control/weight", exchange -> {
                try {
                    updateWeight(exchange);
                } catch (Exception error) {
                    HttpSupport.writeJson(exchange, json, 500, Map.of("error", error.toString()));
                }
            });
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
                lifecycle.shutdown(Duration.ofSeconds(30));
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

    private void spots(HttpExchange exchange) throws Exception {
        if (spots == null) {
            throw new IllegalStateException("this role has no Object Server or Client role");
        }
        String[] path = exchange.getRequestURI().getPath().split("/");
        if (path.length == 3) {
            Contracts.SpotCreateReq request = HttpSupport.readJson(
                exchange, json, Contracts.SpotCreateReq.class);
            var result = spots.getOrCreate(request.spotId(), Contracts.INSTANCE_SPOT_TYPE)
                .inMesh(Contracts.GAME_MESH)
                .request(ZLinkMessage.of(request))
                .timeout(Duration.ofSeconds(5))
                .submit()
                .toCompletableFuture()
                .join();
            HttpSupport.writeJson(exchange, json, new Contracts.SpotCreateRes(
                result.spot().spotId(), result.spot().nodeRid().toString()));
            return;
        }
        if (path.length == 5 && "workflow".equals(path[4])) {
            Contracts.SpotWorkflowReq request = HttpSupport.readJson(
                exchange, json, Contracts.SpotWorkflowReq.class);
            Contracts.SpotWorkflowRes reply = routes.requestToSpot(path[3], request)
                .timeout(Duration.ofSeconds(8))
                .submit(Contracts.SpotWorkflowRes.class)
                .toCompletableFuture()
                .join();
            HttpSupport.writeJson(exchange, json, reply);
            return;
        }
        throw new IllegalArgumentException("spot path must create or invoke workflow");
    }

    private void actors(HttpExchange exchange) throws Exception {
        if (actors == null) {
            throw new IllegalStateException("this role has no Object Server role");
        }
        if (!"/objects/actors".equals(exchange.getRequestURI().getPath())) {
            throw new IllegalArgumentException("actor path must be /objects/actors");
        }
        Contracts.ActorCreateReq request = HttpSupport.readJson(
            exchange, json, Contracts.ActorCreateReq.class);
        ZLinkActorCreateResult result = actors.create(request.actorId(), Contracts.ACTOR_TYPE)
            .inMesh(Contracts.GAME_MESH)
            .request(ZLinkMessage.of(request))
            .timeout(Duration.ofSeconds(5))
            .submit()
            .toCompletableFuture()
            .join();
        ActorRef actor;
        if (result instanceof ZLinkActorCreateResult.Created created) {
            actor = created.actor();
        } else if (result instanceof ZLinkActorCreateResult.Existing existing) {
            actor = existing.actor();
        } else {
            throw new IllegalStateException("actor creation was rejected");
        }
        HttpSupport.writeJson(exchange, json, new Contracts.ActorCreateRes(
            actor.actorId(), actor.nodeRid().toString()));
    }

    private void stateAddress(HttpExchange exchange) throws Exception {
        Contracts.StateAddressReq request = HttpSupport.readJson(
            exchange, json, Contracts.StateAddressReq.class);
        Contracts.StateAddressRes reply = client.requestToChannel(
                Contracts.WORKFLOW_CHANNEL, request)
            .timeout(Duration.ofSeconds(5))
            .submit(Contracts.StateAddressRes.class)
            .toCompletableFuture()
            .join();
        HttpSupport.writeJson(exchange, json, reply);
    }

    private void updateWeight(HttpExchange exchange) throws Exception {
        String[] path = exchange.getRequestURI().getPath().split("/");
        if (path.length != 4) {
            throw new IllegalArgumentException("weight path must be /control/weight/{value}");
        }
        int weight = Integer.parseInt(path[3]);
        runtimeOptions.clientServerChannel(Contracts.WORKFLOW_CHANNEL)
            .configureServerSocket()
            .weight(weight);
        evidence.add("weight", "value=" + weight);
        HttpSupport.writeJson(exchange, json, Map.of("weight", weight));
    }

    private Contracts.ChannelProbeRes requestChannel(Contracts.InvokeReq request) {
        if (Contracts.WORKFLOW_CHANNEL.equals(request.channel())) {
            return client.requestToChannel(
                    request.channel(),
                    new Contracts.ChannelProbeReq(request.id(), request.mode()))
                .timeout("hold".equals(request.mode())
                    ? Duration.ofSeconds(30)
                    : Duration.ofSeconds(5))
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

    private void fanoutPublish(HttpExchange exchange) throws Exception {
        Contracts.FanoutProbe request = HttpSupport.readJson(
            exchange, json, Contracts.FanoutProbe.class);
        fanout.publish(Contracts.FANOUT_CHANNEL, request)
            .submit()
            .toCompletableFuture()
            .join();
        HttpSupport.writeJson(exchange, json, Map.of("succeeded", true));
    }

    private List<Contracts.ListenerStatus> listenerStatuses() {
        List<Contracts.ListenerStatus> statuses = new java.util.ArrayList<>();
        if (options.gameServerNames().length > 0) {
            statuses.add(listenerStatus(
                ZLinkListenerKind.ROUTE_MESH, Contracts.GAME_MESH));
        }
        if (options.workflowServer()) {
            statuses.add(listenerStatus(
                ZLinkListenerKind.CLIENT_SERVER, Contracts.WORKFLOW_CHANNEL));
        }
        if (options.fanoutPublisher()) {
            statuses.add(listenerStatus(
                ZLinkListenerKind.FANOUT, Contracts.FANOUT_CHANNEL));
        }
        if (options.streamServer()) {
            statuses.add(listenerStatus(
                ZLinkListenerKind.STREAM, Contracts.STREAM_NODE));
        }
        return List.copyOf(statuses);
    }

    private Contracts.ListenerStatus listenerStatus(
        ZLinkListenerKind kind,
        String name) {
        var status = runtime.getObject().listenerStatus(kind, name);
        return new Contracts.ListenerStatus(
            kind.name().equals("ROUTE_MESH") ? "RouteMesh"
                : kind.name().equals("CLIENT_SERVER") ? "ClientServer"
                    : kind.name().equals("FANOUT") ? "Fanout" : "STREAM",
            name,
            true,
            status.endpoint(),
            "public ZLinkFrameworkRuntime.listenerStatus");
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

    private Map<String, Object> fanoutStatus() {
        var snapshot = fanoutRuntime.snapshot(Contracts.FANOUT_CHANNEL);
        return Map.of(
            "state", snapshot.state().name(),
            "isReady", snapshot.isReady(),
            "readyPublisherCount", snapshot.readyPublisherCount(),
            "publishers", snapshot.publishers().stream().map(publisher -> Map.of(
                "rid", publisher.nodeRid().toString(),
                "state", publisher.state().name())).toList());
    }

    private List<Map<String, Object>> locationTopology() {
        return lifecycle.monitoringLocationRuntimeQuery()
            .listTopology(ZLinkLocationTopologyFilter.all(), new ZLinkPageRequest(1_000, null))
            .toCompletableFuture()
            .join()
            .items()
            .stream()
            .map(entry -> Map.<String, Object>of(
                "meshName", entry.meshName(),
                "rid", entry.nodeRid().toString(),
                "endpoint", entry.endpoint(),
                "state", entry.state().name()))
            .toList();
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
