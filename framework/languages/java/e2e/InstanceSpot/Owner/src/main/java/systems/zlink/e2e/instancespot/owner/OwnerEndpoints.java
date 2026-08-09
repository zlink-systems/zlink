package systems.zlink.e2e.instancespot.owner;
import java.util.Map;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpServer;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import org.springframework.context.SmartLifecycle;
import systems.zlink.e2e.instancespot.shared.Contracts;
import systems.zlink.e2e.instancespot.shared.HttpSupport;

public final class OwnerEndpoints implements SmartLifecycle {
    private final OwnerOptions options;
    private final EvidenceStore evidence;
    private final GateController gates;
    private final ObjectMapper json;
    private HttpServer server;
    private ExecutorService executor;
    private boolean running;

    public OwnerEndpoints(
        OwnerOptions options,
        EvidenceStore evidence,
        GateController gates,
        ObjectMapper json) {
        this.options = options;
        this.evidence = evidence;
        this.gates = gates;
        this.json = json;
    }

    @Override
    public void start() {
        try {
            server = HttpSupport.createServer(options.httpEndpoint());
            executor = Executors.newFixedThreadPool(4);
            server.setExecutor(executor);
            server.createContext("/health", exchange -> HttpSupport.writeJson(
                exchange,
                json,
                Map.of(
                    "status", "ready",
                    "rid", options.rid(),
                    "lifecycleId", options.lifecycleId())));
            server.createContext("/evidence", exchange -> HttpSupport.writeJson(
                exchange, json, evidence.snapshot()));
            server.createContext("/evidence/wait", exchange -> {
                Contracts.EvidenceWaitRequest request = HttpSupport.readJson(
                    exchange, json, Contracts.EvidenceWaitRequest.class);
                HttpSupport.writeJson(exchange, json, evidence.waitFor(request));
            });
            server.createContext("/gate", exchange -> {
                Contracts.GateRequest request = HttpSupport.readJson(
                    exchange, json, Contracts.GateRequest.class);
                gates.set(request.gateId(), request.open());
                HttpSupport.writeJson(exchange, json, request);
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
                "failed to start Instance Spot owner endpoints "
                    + options.httpEndpoint(), error);
        }
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
