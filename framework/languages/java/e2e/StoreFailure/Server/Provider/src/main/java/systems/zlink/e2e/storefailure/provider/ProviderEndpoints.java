package systems.zlink.e2e.storefailure.provider;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpServer;
import java.time.Duration;
import org.springframework.context.SmartLifecycle;
import systems.zlink.e2e.storefailure.shared.Contracts;
import systems.zlink.e2e.storefailure.shared.HttpSupport;

public final class ProviderEndpoints implements SmartLifecycle {
    private final ProviderOptions options;
    private final ProviderEvidenceStore evidence;
    private final ObjectMapper json;
    private HttpServer server;
    private boolean running;

    public ProviderEndpoints(
        ProviderOptions options,
        ProviderEvidenceStore evidence,
        ObjectMapper json) {
        this.options = options;
        this.evidence = evidence;
        this.json = json;
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
            server.createContext("/shutdown", exchange -> {
                HttpSupport.writeJson(exchange, json, java.util.Map.of("status", "stopping"));
                HttpSupport.shutdownAsync();
            });
            server.start();
            running = true;
        } catch (Exception error) {
            throw new IllegalStateException("failed to start provider endpoints " + options.httpEndpoint(), error);
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
