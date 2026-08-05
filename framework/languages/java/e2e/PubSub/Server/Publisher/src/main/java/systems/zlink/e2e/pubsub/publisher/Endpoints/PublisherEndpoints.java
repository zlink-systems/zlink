package systems.zlink.e2e.pubsub.publisher.Endpoints;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;
import java.net.InetSocketAddress;
import java.net.URI;
import java.net.URLDecoder;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Map;
import java.time.Duration;
import org.springframework.context.SmartLifecycle;
import systems.zlink.e2e.pubsub.publisher.Configuration.PublisherOptions;
import systems.zlink.e2e.pubsub.publisher.Infrastructure.EvidenceStore;
import systems.zlink.e2e.pubsub.shared.Contracts;
import systems.zlink.framework.channels.ZLinkFanoutClient;
import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle;

public final class PublisherEndpoints implements SmartLifecycle {
    private final PublisherOptions options;
    private final ZLinkFanoutClient fanout;
    private final EvidenceStore evidence;
    private final ObjectMapper json;
    private final ZLinkFrameworkLifecycle drain;
    private HttpServer server;
    private boolean running;

    public PublisherEndpoints(
        PublisherOptions options,
        ZLinkFanoutClient fanout,
        EvidenceStore evidence,
        ObjectMapper json,
        ZLinkFrameworkLifecycle drain) {
        this.options = options;
        this.fanout = fanout;
        this.evidence = evidence;
        this.json = json;
        this.drain = drain;
    }

    @Override
    public void start() {
        try {
            URI uri = URI.create(options.httpEndpoint());
            server = HttpServer.create(new InetSocketAddress(uri.getHost(), uri.getPort()), 0);
            server.createContext("/health", exchange -> writeText(exchange, 200, "ok\n"));
            server.createContext("/evidence", exchange -> writeJson(exchange, evidence.snapshot()));
            server.createContext("/publish/event", exchange ->
                publish(exchange, Contracts.EVENT_PACKET));
            server.createContext("/publish/missing", exchange ->
                publish(exchange, Contracts.MISSING_PACKET));
            server.createContext("/admin/drain", exchange -> {
                var result = drain.shutdown(Duration.ofSeconds(30)).toCompletableFuture().join();
                writeText(exchange, 200,
                    result.outcome() == systems.zlink.framework.runtime.host
                        .ZLinkFrameworkTerminationOutcome.STOPPED
                        ? "Drained\n" : "ForceStopped\n");
            });
            server.start();
            running = true;
        } catch (Exception error) {
            throw new IllegalStateException("failed to start publisher endpoint " + options.httpEndpoint(), error);
        }
    }

    private void publish(HttpExchange exchange, String packetName) {
        try {
            Map<String, String> query = parseQuery(exchange.getRequestURI().getRawQuery());
            String topic = query.getOrDefault("topic", "all");
            String scenario = query.getOrDefault("scenario", "");
            int sequence = Integer.parseInt(query.getOrDefault("sequence", "0"));
            String value = query.getOrDefault("value", "");
            Object event = Contracts.MISSING_PACKET.equals(packetName)
                ? new Contracts.MissingEventMsg(scenario, sequence, value)
                : new Contracts.EventMsg(scenario, sequence, value);
            fanout.publish(Contracts.EVENT_CHANNEL, topic, event)
                .submit()
                .toCompletableFuture()
                .join();
            evidence.record(packetName + "|" + topic + "|" + scenario + "|" + sequence);
            writeText(exchange, 200, "published\n");
        } catch (Exception error) {
            writeText(exchange, 500, error.toString() + "\n");
        }
    }

    private static Map<String, String> parseQuery(String rawQuery) {
        Map<String, String> values = new HashMap<>();
        if (rawQuery == null || rawQuery.isBlank()) {
            return values;
        }
        for (String part : rawQuery.split("&")) {
            int separator = part.indexOf('=');
            if (separator < 0) {
                values.put(decode(part), "");
            } else {
                values.put(
                    decode(part.substring(0, separator)),
                    decode(part.substring(separator + 1)));
            }
        }
        return values;
    }

    private static String decode(String value) {
        return URLDecoder.decode(value, StandardCharsets.UTF_8);
    }

    private void writeJson(HttpExchange exchange, Object body) {
        try {
            byte[] bytes = json.writeValueAsBytes(body);
            exchange.getResponseHeaders().add("Content-Type", "application/json");
            exchange.sendResponseHeaders(200, bytes.length);
            exchange.getResponseBody().write(bytes);
            exchange.close();
        } catch (Exception error) {
            throw new IllegalStateException("failed to write publisher json response", error);
        }
    }

    private static void writeText(HttpExchange exchange, int status, String body) {
        try {
            byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
            exchange.sendResponseHeaders(status, bytes.length);
            exchange.getResponseBody().write(bytes);
            exchange.close();
        } catch (Exception error) {
            throw new IllegalStateException("failed to write publisher response", error);
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
