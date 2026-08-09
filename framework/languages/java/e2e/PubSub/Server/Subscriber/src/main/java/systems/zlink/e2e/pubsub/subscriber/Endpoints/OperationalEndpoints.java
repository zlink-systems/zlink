package Endpoints;

import com.sun.net.httpserver.HttpExchange;
import systems.zlink.e2e.pubsub.subscriber.Configuration;
import systems.zlink.e2e.pubsub.subscriber.Endpoints;
import systems.zlink.e2e.pubsub.subscriber.Infrastructure;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpServer;
import java.net.InetSocketAddress;
import java.net.URI;
import java.net.URLDecoder;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Map;
import org.springframework.context.SmartLifecycle;
import systems.zlink.e2e.pubsub.subscriber.Configuration.SubscriberOptions;
import systems.zlink.e2e.pubsub.subscriber.Infrastructure.EvidenceStore;
import systems.zlink.e2e.pubsub.subscriber.Infrastructure.FanoutObserverController;
import systems.zlink.e2e.pubsub.subscriber.Infrastructure.SubscriberConnections;
import systems.zlink.e2e.pubsub.shared.Contracts;
import systems.zlink.framework.monitoring.ZLinkFanoutRuntime;
import systems.zlink.framework.monitoring.ZLinkPeerState;

public final class OperationalEndpoints implements SmartLifecycle {
    private final SubscriberOptions options;
    private final EvidenceStore evidence;
    private final ObjectMapper json;
    private final ZLinkFanoutRuntime fanoutRuntime;
    private final SubscriberConnections connections;
    private final FanoutObserverController observers;
    private HttpServer server;
    private boolean running;

    public OperationalEndpoints(
        SubscriberOptions options,
        EvidenceStore evidence,
        ObjectMapper json,
        ZLinkFanoutRuntime fanoutRuntime,
        SubscriberConnections connections,
        FanoutObserverController observers) {
        this.options = options;
        this.evidence = evidence;
        this.json = json;
        this.fanoutRuntime = fanoutRuntime;
        this.connections = connections;
        this.observers = observers;
    }

    @Override
    public void start() {
        if (options.httpEndpoint() == null || options.httpEndpoint().isBlank()) {
            return;
        }
        try {
            URI uri = URI.create(options.httpEndpoint());
            server = HttpServer.create(new InetSocketAddress(uri.getHost(), uri.getPort()), 0);
            server.createContext("/health", exchange -> {
                byte[] body = "ok\n".getBytes(StandardCharsets.UTF_8);
                exchange.sendResponseHeaders(200, body.length);
                exchange.getResponseBody().write(body);
                exchange.close();
            });
            server.createContext("/evidence", exchange -> {
                byte[] body = json.writeValueAsBytes(evidence.snapshot());
                exchange.getResponseHeaders().add("Content-Type", "application/json");
                exchange.sendResponseHeaders(200, body.length);
                exchange.getResponseBody().write(body);
                exchange.close();
            });
            // Config 3의 publisher 상태 전환을 public monitoring snapshot으로
            // 관측한다. 내부 Location Store의 descriptor를 process 밖으로 노출하지 않는다.
            server.createContext("/locations/publishers", exchange -> {
                var publishers = fanoutRuntime.snapshot(Contracts.EVENT_CHANNEL).publishers().stream()
                    .filter(publisher -> publisher.state() == ZLinkPeerState.READY)
                    .map(publisher -> publisher.nodeRid().toString())
                    .toList();
                byte[] body = json.writeValueAsBytes(publishers);
                exchange.getResponseHeaders().add("Content-Type", "application/json");
                exchange.sendResponseHeaders(200, body.length);
                exchange.getResponseBody().write(body);
                exchange.close();
            });
            server.createContext("/status", exchange -> {
                var status = fanoutRuntime.snapshot(Contracts.EVENT_CHANNEL);
                writeJson(exchange, Map.of(
                    "channelName", status.channelName(),
                    "state", status.state().name(),
                    "isReady", status.isReady(),
                    "readyPublisherCount", status.readyPublisherCount(),
                    "publishers", status.publishers().stream().map(publisher -> Map.of(
                        "nodeRid", publisher.nodeRid().toString(),
                        "state", publisher.state().name())).toList(),
                    "sequence", status.sequence(),
                    "observedAt", status.observedAt().toString()));
            });
            server.createContext("/connections", exchange -> {
                try {
                    Map<String, String> values = query(exchange.getRequestURI());
                    switch (values.getOrDefault("operation", "list")) {
                        case "connect" -> connections.connect(required(values, "endpoint"));
                        case "disconnect" -> connections.disconnect(required(values, "endpoint"));
                        case "list" -> { }
                        default -> throw new IllegalArgumentException("unsupported connection operation");
                    }
                    writeJson(exchange, Map.of("connections", connections.list()));
                } catch (Exception error) {
                    writeText(exchange, 400, error.getMessage() + "\n");
                }
            });
            server.createContext("/observer/start", exchange -> {
                try {
                    Map<String, String> values = query(exchange.getRequestURI());
                    observers.start(
                        values.getOrDefault("name", "normal"),
                        intQuery(values, "capacity", 1),
                        Boolean.parseBoolean(values.getOrDefault("slow", "false")));
                    writeJson(exchange, Map.of("status", "started"));
                } catch (Exception error) {
                    writeText(exchange, 400, error.getMessage() + "\n");
                }
            });
            server.createContext("/observer/release", exchange -> {
                observers.release(query(exchange.getRequestURI()).getOrDefault("name", "slow"));
                writeJson(exchange, Map.of("status", "released"));
            });
            server.createContext("/observer/cancel", exchange -> {
                observers.cancel(query(exchange.getRequestURI()).getOrDefault("name", "slow"));
                writeJson(exchange, Map.of("status", "cancelled"));
            });
            server.createContext("/observer/wait", exchange -> {
                try {
                    Map<String, String> values = query(exchange.getRequestURI());
                    observers.waitFor(
                        values.getOrDefault("name", "normal"),
                        longQuery(values, "timeoutMs", 30_000L));
                    writeJson(exchange, Map.of("status", "observed"));
                } catch (Exception error) {
                    writeText(exchange, 504, error.getMessage() + "\n");
                }
            });
            server.createContext("/observer/evidence", exchange ->
                writeJson(exchange, observers.snapshot()));
            server.createContext("/evidence/wait", exchange -> {
                try {
                    Map<String, String> query = query(exchange.getRequestURI());
                    long timeoutMillis = longQuery(query, "timeoutMillis", 15_000L);
                    var snapshot = switch (query.getOrDefault("kind", "event")) {
                        case "any-event" -> evidence.waitFor(
                            entry -> "EventMsg".equals(entry.marker())
                                && query.getOrDefault("scenario", "").equals(entry.scenario()),
                            timeoutMillis);
                        case "event" -> evidence.waitFor(
                            entry -> "EventMsg".equals(entry.marker())
                                && query.getOrDefault("scenario", "").equals(entry.scenario())
                                && entry.sequence() == intQuery(query, "sequence", Integer.MIN_VALUE),
                            timeoutMillis);
                        case "sequence-at-least" -> evidence.waitFor(
                            entry -> "EventMsg".equals(entry.marker())
                                && query.getOrDefault("scenario", "").equals(entry.scenario())
                                && entry.sequence() >= intQuery(query, "minSequence", Integer.MAX_VALUE),
                            timeoutMillis);
                        case "dispatch-error" -> evidence.waitFor(
                            entry -> "DispatchError".equals(entry.marker())
                                && entry.value().contains("HANDLER_MISSING")
                                && entry.value().contains("DROP")
                                && entry.value().contains(query.getOrDefault("packetName", "")),
                            timeoutMillis);
                        case "contiguous-run" -> evidence.waitForContiguousRun(
                            query.getOrDefault("scenario", ""),
                            intQuery(query, "minLength", 1),
                            timeoutMillis);
                        default -> throw new IllegalArgumentException("unknown evidence wait kind");
                    };
                    byte[] body = json.writeValueAsBytes(snapshot);
                    exchange.getResponseHeaders().add("Content-Type", "application/json");
                    exchange.sendResponseHeaders(200, body.length);
                    exchange.getResponseBody().write(body);
                } catch (Exception error) {
                    byte[] body = (error.getMessage() + "\n").getBytes(StandardCharsets.UTF_8);
                    exchange.sendResponseHeaders(408, body.length);
                    exchange.getResponseBody().write(body);
                } finally {
                    exchange.close();
                }
            });
            server.start();
            running = true;
        } catch (Exception error) {
            throw new IllegalStateException("failed to start evidence endpoint " + options.httpEndpoint(), error);
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

    private static Map<String, String> query(URI uri) {
        Map<String, String> result = new HashMap<>();
        String raw = uri.getRawQuery();
        if (raw == null || raw.isBlank()) {
            return result;
        }
        for (String pair : raw.split("&")) {
            int separator = pair.indexOf('=');
            if (separator <= 0) {
                continue;
            }
            String key = decode(pair.substring(0, separator));
            String value = decode(pair.substring(separator + 1));
            result.put(key, value);
        }
        return result;
    }

    private static String decode(String value) {
        return URLDecoder.decode(value, StandardCharsets.UTF_8);
    }

    private static int intQuery(Map<String, String> query, String name, int fallback) {
        String value = query.get(name);
        return value == null || value.isBlank() ? fallback : Integer.parseInt(value);
    }

    private static long longQuery(Map<String, String> query, String name, long fallback) {
        String value = query.get(name);
        return value == null || value.isBlank() ? fallback : Long.parseLong(value);
    }

    private static String required(Map<String, String> values, String name) {
        String value = values.get(name);
        if (value == null || value.isBlank()) {
            throw new IllegalArgumentException(name + " is required");
        }
        return value;
    }

    private void writeJson(HttpExchange exchange, Object value) {
        try {
            byte[] body = json.writeValueAsBytes(value);
            exchange.getResponseHeaders().add("Content-Type", "application/json");
            exchange.sendResponseHeaders(200, body.length);
            exchange.getResponseBody().write(body);
            exchange.close();
        } catch (Exception error) {
            throw new IllegalStateException("failed to write subscriber json response", error);
        }
    }

    private static void writeText(
        HttpExchange exchange,
        int status,
        String value) {
        try {
            byte[] body = value.getBytes(StandardCharsets.UTF_8);
            exchange.sendResponseHeaders(status, body.length);
            exchange.getResponseBody().write(body);
            exchange.close();
        } catch (Exception error) {
            throw new IllegalStateException("failed to write subscriber response", error);
        }
    }
}
