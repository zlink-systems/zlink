package systems.zlink.e2e.instancespot.client;
import java.util.Map;

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
                Map.of("status", "ready", "rid", options.rid())));
            server.createContext("/lookup", exchange -> {
                var request = HttpSupport.readJson(
                    exchange, json, Map.class);
                String spotId = String.valueOf(request.get("spotId"));
                HttpSupport.writeJson(exchange, json, lookup(spotId));
            });
            server.createContext("/request", exchange -> {
                Contracts.InstanceReq request = HttpSupport.readJson(
                    exchange, json, Contracts.InstanceReq.class);
                HttpSupport.writeJson(exchange, json, request(request));
            });
            server.createContext("/send", exchange -> {
                Contracts.InstanceMsg request = HttpSupport.readJson(
                    exchange, json, Contracts.InstanceMsg.class);
                HttpSupport.writeJson(exchange, json, send(request));
            });
            server.createContext("/close", exchange -> {
                Contracts.CloseMsg request = HttpSupport.readJson(
                    exchange, json, Contracts.CloseMsg.class);
                HttpSupport.writeJson(exchange, json, sendClose(request));
            });
            server.createContext("/concurrent", exchange -> {
                Contracts.ConcurrentReq request = HttpSupport.readJson(
                    exchange, json, Contracts.ConcurrentReq.class);
                HttpSupport.writeJson(exchange, json, concurrent(request));
            });
            server.createContext("/shutdown", exchange -> {
                HttpSupport.writeJson(
                    exchange, json, Map.of("status", "stopping"));
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

    private Contracts.InstanceCallRes request(Contracts.InstanceReq request) {
        try {
            Contracts.InstanceRes reply = routes
                .requestToSpot(request.spotId(), request)
                .instanceSpot(Contracts.STABLE_TYPE)
                .inMesh(Contracts.MESH)
                .timeout(Duration.ofMillis(request.timeoutMilliseconds()))
                .submit(Contracts.InstanceRes.class)
                .toCompletableFuture()
                .join();
            return new Contracts.InstanceCallRes(true, reply, "", "");
        } catch (RuntimeException error) {
            Throwable cause = unwrap(error);
            return new Contracts.InstanceCallRes(
                false,
                null,
                errorKind(cause),
                cause.getMessage() == null ? "" : cause.getMessage());
        }
    }

    private Contracts.LookupRes lookup(String spotId) {
        try {
            var result = spots.find(spotId).toCompletableFuture().join();
            if (result.isEmpty()) {
                return new Contracts.LookupRes(
                    false, spotId, 0, "", "", "", "");
            }
            SpotRef ref = result.get();
            return new Contracts.LookupRes(
                true,
                ref.spotId(),
                ref.objectGeneration(),
                ref.meshName(),
                ref.nodeRid().toString(),
                "",
                "");
        } catch (RuntimeException error) {
            Throwable cause = unwrap(error);
            return new Contracts.LookupRes(
                false,
                spotId,
                0,
                "",
                "",
                errorKind(cause),
                cause.getMessage() == null ? "" : cause.getMessage());
        }
    }

    private Contracts.SendSubmitRes send(Contracts.InstanceMsg request) {
        try {
            routes
                .sendToSpot(request.spotId(), request)
                .instanceSpot(Contracts.STABLE_TYPE)
                .inMesh(Contracts.MESH)
                .submit()
                .toCompletableFuture()
                .join();
            return new Contracts.SendSubmitRes(true, "", "");
        } catch (RuntimeException error) {
            Throwable cause = unwrap(error);
            return new Contracts.SendSubmitRes(
                false,
                errorKind(cause),
                cause.getMessage() == null ? "" : cause.getMessage());
        }
    }

    private Contracts.SendSubmitRes sendClose(Contracts.CloseMsg close) {
        try {
            routes
                .sendToSpot(close.spotId(), close)
                .instanceSpot()
                .inMesh(Contracts.MESH)
                .submit()
                .toCompletableFuture()
                .join();
            return new Contracts.SendSubmitRes(true, "", "");
        } catch (RuntimeException error) {
            Throwable cause = unwrap(error);
            return new Contracts.SendSubmitRes(
                false,
                errorKind(cause),
                cause.getMessage() == null ? "" : cause.getMessage());
        }
    }

    private Contracts.ConcurrentRes concurrent(
        Contracts.ConcurrentReq request) {
        List<CompletableFuture<Contracts.InstanceCallRes>> futures = new ArrayList<>();
        for (int index = 0; index < request.count(); index++) {
            int operationIndex = index;
            futures.add(CompletableFuture.supplyAsync(() -> request(
                new Contracts.InstanceReq(
                    request.spotId(),
                    request.operationPrefix() + "-" + operationIndex,
                    "payload-" + operationIndex,
                    request.timeoutMilliseconds())), executor));
        }
        CompletableFuture.allOf(futures.toArray(CompletableFuture[]::new)).join();
        return new Contracts.ConcurrentRes(
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
