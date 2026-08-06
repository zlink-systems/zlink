package systems.zlink.e2e.instancespot.client;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpServer;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import org.springframework.context.SmartLifecycle;
import systems.zlink.e2e.instancespot.shared.Contracts;
import systems.zlink.e2e.instancespot.shared.HttpSupport;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.spots.SpotRef;
import systems.zlink.framework.spots.ZLinkSpotManager;

public final class ClientEndpoints implements SmartLifecycle {
    private final ClientOptions options;
    private final ZLinkRouteClient routes;
    private final ZLinkSpotManager spots;
    private final ObjectMapper json;
    private HttpServer server;
    private ExecutorService executor;
    private boolean running;

    public ClientEndpoints(
        ClientOptions options,
        ZLinkRouteClient routes,
        ZLinkSpotManager spots,
        ObjectMapper json) {
        this.options = options;
        this.routes = routes;
        this.spots = spots;
        this.json = json;
    }

    @Override
    public void start() {
        try {
            server = HttpSupport.createServer(options.httpEndpoint());
            executor = Executors.newFixedThreadPool(8);
            server.setExecutor(executor);
            server.createContext("/health", exchange -> HttpSupport.writeJson(
                exchange,
                json,
                java.util.Map.of("status", "ready", "rid", options.rid())));
            server.createContext("/lookup", exchange -> {
                var request = HttpSupport.readJson(
                    exchange, json, java.util.Map.class);
                String spotId = String.valueOf(request.get("spotId"));
                HttpSupport.writeJson(exchange, json, lookup(spotId));
            });
            server.createContext("/request", exchange -> {
                Contracts.InstanceRequest request = HttpSupport.readJson(
                    exchange, json, Contracts.InstanceRequest.class);
                HttpSupport.writeJson(exchange, json, request(request));
            });
            server.createContext("/send", exchange -> {
                Contracts.InstanceSend request = HttpSupport.readJson(
                    exchange, json, Contracts.InstanceSend.class);
                HttpSupport.writeJson(exchange, json, send(request));
            });
            server.createContext("/close", exchange -> {
                Contracts.CloseRequest request = HttpSupport.readJson(
                    exchange, json, Contracts.CloseRequest.class);
                HttpSupport.writeJson(exchange, json, sendClose(request));
            });
            server.createContext("/concurrent", exchange -> {
                Contracts.ConcurrentRequest request = HttpSupport.readJson(
                    exchange, json, Contracts.ConcurrentRequest.class);
                HttpSupport.writeJson(exchange, json, concurrent(request));
            });
            server.createContext("/shutdown", exchange -> {
                HttpSupport.writeJson(
                    exchange, json, java.util.Map.of("status", "stopping"));
                HttpSupport.shutdownAsync();
            });
            server.start();
            running = true;
        } catch (Exception error) {
            throw new IllegalStateException(
                "failed to start Instance Spot client endpoints "
                    + options.httpEndpoint(), error);
        }
    }

    private Contracts.RequestOutcome request(Contracts.InstanceRequest request) {
        try {
            Contracts.InstanceReply reply = routes
                .requestToSpot(request.spotId(), request)
                .instanceSpot(Contracts.STABLE_TYPE)
                .inMesh(Contracts.MESH)
                .timeout(Duration.ofMillis(request.timeoutMilliseconds()))
                .submit(Contracts.InstanceReply.class)
                .toCompletableFuture()
                .join();
            return new Contracts.RequestOutcome(true, reply, "", "");
        } catch (RuntimeException error) {
            Throwable cause = unwrap(error);
            return new Contracts.RequestOutcome(
                false,
                null,
                errorKind(cause),
                cause.getMessage() == null ? "" : cause.getMessage());
        }
    }

    private Contracts.LookupOutcome lookup(String spotId) {
        try {
            var result = spots.find(spotId).toCompletableFuture().join();
            if (result.isEmpty()) {
                return new Contracts.LookupOutcome(
                    false, spotId, 0, "", "", "", "");
            }
            SpotRef ref = result.get();
            return new Contracts.LookupOutcome(
                true,
                ref.spotId(),
                ref.objectGeneration(),
                ref.meshName(),
                ref.nodeRid().toString(),
                "",
                "");
        } catch (RuntimeException error) {
            Throwable cause = unwrap(error);
            return new Contracts.LookupOutcome(
                false,
                spotId,
                0,
                "",
                "",
                errorKind(cause),
                cause.getMessage() == null ? "" : cause.getMessage());
        }
    }

    private Contracts.SendOutcome send(Contracts.InstanceSend request) {
        try {
            routes
                .sendToSpot(request.spotId(), request)
                .instanceSpot(Contracts.STABLE_TYPE)
                .inMesh(Contracts.MESH)
                .submit()
                .toCompletableFuture()
                .join();
            return new Contracts.SendOutcome(true, "", "");
        } catch (RuntimeException error) {
            Throwable cause = unwrap(error);
            return new Contracts.SendOutcome(
                false,
                errorKind(cause),
                cause.getMessage() == null ? "" : cause.getMessage());
        }
    }

    private Contracts.SendOutcome sendClose(Contracts.CloseRequest close) {
        try {
            routes
                .sendToSpot(close.spotId(), close)
                .instanceSpot()
                .inMesh(Contracts.MESH)
                .submit()
                .toCompletableFuture()
                .join();
            return new Contracts.SendOutcome(true, "", "");
        } catch (RuntimeException error) {
            Throwable cause = unwrap(error);
            return new Contracts.SendOutcome(
                false,
                errorKind(cause),
                cause.getMessage() == null ? "" : cause.getMessage());
        }
    }

    private Contracts.ConcurrentOutcome concurrent(
        Contracts.ConcurrentRequest request) {
        List<CompletableFuture<Contracts.RequestOutcome>> futures = new ArrayList<>();
        for (int index = 0; index < request.count(); index++) {
            int operationIndex = index;
            futures.add(CompletableFuture.supplyAsync(() -> request(
                new Contracts.InstanceRequest(
                    request.spotId(),
                    request.operationPrefix() + "-" + operationIndex,
                    "payload-" + operationIndex,
                    request.timeoutMilliseconds())), executor));
        }
        CompletableFuture.allOf(futures.toArray(CompletableFuture[]::new)).join();
        return new Contracts.ConcurrentOutcome(
            futures.stream().map(CompletableFuture::join).toList());
    }

    private static String errorKind(Throwable error) {
        if (error instanceof ZLinkFrameworkException frameworkError) {
            return frameworkError.kind().name();
        }
        return error.getClass().getSimpleName();
    }

    private static Throwable unwrap(Throwable error) {
        Throwable current = error;
        while ((current instanceof CompletionException
                || current instanceof ExecutionException)
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
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
