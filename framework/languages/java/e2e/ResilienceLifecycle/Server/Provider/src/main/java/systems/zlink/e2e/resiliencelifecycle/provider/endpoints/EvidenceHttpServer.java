package systems.zlink.e2e.resiliencelifecycle.provider.endpoints;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpServer;
import java.net.InetSocketAddress;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import org.springframework.context.ConfigurableApplicationContext;
import org.springframework.context.SmartLifecycle;
import systems.zlink.e2e.resiliencelifecycle.provider.infrastructure.ScenarioState;
import systems.zlink.e2e.resiliencelifecycle.shared.Contracts;
import systems.zlink.framework.channels.ZLinkChannelRuntimeOptions;
import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle;

public final class EvidenceHttpServer implements SmartLifecycle {
    private final ScenarioState state;
    private final ObjectMapper json;
    private final String endpoint;
    private final ZLinkChannelRuntimeOptions runtimeOptions;
    private final ConfigurableApplicationContext applicationContext;
    private final ZLinkFrameworkLifecycle drain;
    private HttpServer server;
    private java.util.concurrent.ExecutorService executor;
    private boolean running;

    public EvidenceHttpServer(
        ScenarioState state,
        ObjectMapper json,
        String endpoint,
        ZLinkChannelRuntimeOptions runtimeOptions,
        ConfigurableApplicationContext applicationContext,
        ZLinkFrameworkLifecycle drain) {
        this.state = state;
        this.json = json;
        this.endpoint = endpoint;
        this.runtimeOptions = runtimeOptions;
        this.applicationContext = applicationContext;
        this.drain = drain;
    }

    @Override
    public void start() {
        if (endpoint == null || endpoint.isBlank()) {
            return;
        }
        try {
            URI uri = URI.create(endpoint);
            server = HttpServer.create(new InetSocketAddress(uri.getHost(), uri.getPort()), 0);
            executor = java.util.concurrent.Executors.newVirtualThreadPerTaskExecutor();
            server.setExecutor(executor);
            server.createContext("/health", exchange -> write(exchange, 200, "ok\n"));
            server.createContext("/evidence", exchange -> {
                byte[] body = json.writeValueAsBytes(state.snapshot());
                exchange.getResponseHeaders().add("Content-Type", "application/json");
                exchange.sendResponseHeaders(200, body.length);
                exchange.getResponseBody().write(body);
                exchange.close();
            });
            server.createContext("/admin/drain", exchange -> setWeight(exchange, 0, "drained"));
            server.createContext("/admin/restore", exchange -> setWeight(exchange, 100, "restored"));
            server.createContext("/admin/weight", exchange -> write(
                exchange,
                200,
                "{\"weight\":" + state.weight() + "}\n"));
            server.createContext("/admin/release-slow", exchange -> {
                state.releaseSlow();
                write(exchange, 200, "{\"status\":\"released\"}\n");
            });
            server.createContext("/admin/fault-on", exchange -> setGrayFailure(exchange, true));
            server.createContext("/admin/fault-off", exchange -> setGrayFailure(exchange, false));
            server.createContext("/admin/fault/observer-throws", exchange -> setObserverThrows(exchange, true));
            server.createContext("/admin/fault/none", exchange -> setObserverThrows(exchange, false));
            server.createContext("/admin/shutdown", exchange -> {
                state.record("AdminShutdown", state.providerRid());
                var result = drain.shutdown(java.time.Duration.ofSeconds(30))
                    .toCompletableFuture().join();
                String terminal = result.outcome() == systems.zlink.framework.runtime.host
                    .ZLinkFrameworkTerminationOutcome.STOPPED ? "Drained" : "ForceStopped";
                write(exchange, 200, "{\"result\":\"" + terminal + "\"}\n");
                Thread shutdown = new Thread(applicationContext::close, "java-rl-admin-shutdown");
                shutdown.setDaemon(false);
                shutdown.start();
            });
            server.start();
            running = true;
        } catch (Exception error) {
            throw new IllegalStateException("failed to start evidence endpoint " + endpoint, error);
        }
    }

    private void setGrayFailure(
        com.sun.net.httpserver.HttpExchange exchange,
        boolean enabled) throws java.io.IOException {
        state.grayFailure(enabled);
        state.record("GrayFailureMode", String.valueOf(enabled));
        write(exchange, 200, "{\"grayFailure\":" + enabled + "}\n");
    }

    private void setObserverThrows(
        com.sun.net.httpserver.HttpExchange exchange,
        boolean enabled) throws java.io.IOException {
        state.observerThrows(enabled);
        state.record("ObserverFaultMode", String.valueOf(enabled));
        write(exchange, 200, "{\"observerThrows\":" + enabled + "}\n");
    }

    private void setWeight(
        com.sun.net.httpserver.HttpExchange exchange,
        int weight,
        String status) throws java.io.IOException {
        runtimeOptions.clientServerChannel(Contracts.CHANNEL)
            .configureServerSocket()
            .weight(weight);
        state.weight(weight);
        state.record("AdminWeight", String.valueOf(weight));
        write(exchange, 200, "{\"status\":\"" + status + "\",\"weight\":" + weight + "}\n");
    }

    private static void write(
        com.sun.net.httpserver.HttpExchange exchange,
        int status,
        String value) throws java.io.IOException {
        byte[] body = value.getBytes(StandardCharsets.UTF_8);
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
