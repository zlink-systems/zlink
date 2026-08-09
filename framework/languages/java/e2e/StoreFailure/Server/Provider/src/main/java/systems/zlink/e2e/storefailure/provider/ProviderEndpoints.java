package systems.zlink.e2e.storefailure.provider;
import java.util.Map;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpServer;
import java.time.Duration;
import org.springframework.beans.factory.ObjectProvider;
import org.springframework.context.SmartLifecycle;
import systems.zlink.e2e.storefailure.shared.Contracts;
import systems.zlink.e2e.storefailure.shared.HttpSupport;
import systems.zlink.framework.channels.ZLinkFanoutClient;

public final class ProviderEndpoints implements SmartLifecycle {
    private final ProviderOptions options;
    private final ProviderEvidenceStore evidence;
    private final ObjectMapper json;
    private final ObjectProvider<ZLinkFanoutClient> fanout;
    private HttpServer server;
    private boolean running;

    public ProviderEndpoints(
        ProviderOptions options,
        ProviderEvidenceStore evidence,
        ObjectMapper json,
        ObjectProvider<ZLinkFanoutClient> fanout) {
        this.options = options;
        this.evidence = evidence;
        this.json = json;
        this.fanout = fanout;
    }

    @Override
    public void start() {
        if (options.httpEndpoint() == null || options.httpEndpoint().isBlank()) {
            return;
        }
        try {
            server = HttpSupport.createServer(options.httpEndpoint());
            server.createContext("/health", exchange -> HttpSupport.writeText(exchange, "ok\n"));
            server.createContext("/evidence", exchange ->
                HttpSupport.writeJson(exchange, json, evidence.snapshot()));
            server.createContext("/evidence/wait", exchange -> {
                Contracts.EvidenceWaitReq request =
                    HttpSupport.readJson(exchange, json, Contracts.EvidenceWaitReq.class);
                HttpSupport.writeJson(exchange, json, evidence.waitFor(
                    request.contains(),
                    Duration.ofMillis(Math.max(1, request.timeoutMilliseconds()))));
            });
            server.createContext("/c4/probe", exchange -> {
                Contracts.C4ProbeReq request =
                    HttpSupport.readJson(exchange, json, Contracts.C4ProbeReq.class);
                HttpSupport.writeJson(exchange, json, c4Probe(request));
            });
            server.createContext("/shutdown", exchange -> {
                HttpSupport.writeJson(exchange, json, Map.of("status", "stopping"));
                HttpSupport.shutdownAsync();
            });
            server.start();
            running = true;
        } catch (Exception error) {
            throw new IllegalStateException("failed to start provider endpoints " + options.httpEndpoint(), error);
        }
    }

    private Map<String, Object> c4Probe(Contracts.C4ProbeReq request) {
        if (!options.c4Roles()) {
            throw new IllegalStateException("C4 roles are not configured");
        }
        String marker = request.marker();
        try {
            fanout.getObject()
                .publish(
                    Contracts.C4_FANOUT_CHANNEL,
                    Contracts.C4_FANOUT_TOPIC,
                    new Contracts.C4FanoutEvent(
                        evidence.rid(), evidence.lifecycleId(), marker))
                .submit()
                .toCompletableFuture()
                .join();
            evidence.record("c4-fanout-published", marker);
            return Map.of(
                "fanout", "published",
                "marker", marker);
        } catch (RuntimeException error) {
            throw new IllegalStateException("C4 role probe failed", error);
        }
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
