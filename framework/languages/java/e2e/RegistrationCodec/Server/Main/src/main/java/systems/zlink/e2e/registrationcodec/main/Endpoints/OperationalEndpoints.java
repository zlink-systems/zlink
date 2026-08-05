package systems.zlink.e2e.registrationcodec.main.Endpoints;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpServer;
import com.google.protobuf.StringValue;
import java.net.InetSocketAddress;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import systems.zlink.e2e.registrationcodec.shared.Contracts;
import systems.zlink.framework.channels.ZLinkClient;
import org.springframework.context.SmartLifecycle;
import systems.zlink.e2e.registrationcodec.main.Infrastructure.EvidenceStore;

public final class OperationalEndpoints implements SmartLifecycle {
    private final EvidenceStore state;
    private final ObjectMapper json;
    private final ZLinkClient client;
    private final String endpoint;
    private HttpServer server;
    private boolean running;

    public OperationalEndpoints(EvidenceStore state, ObjectMapper json, ZLinkClient client, String endpoint) {
        this.state = state;
        this.json = json;
        this.client = client;
        this.endpoint = endpoint;
    }

    @Override
    public void start() {
        if (endpoint == null || endpoint.isBlank()) {
            return;
        }
        try {
            URI uri = URI.create(endpoint);
            server = HttpServer.create(new InetSocketAddress(uri.getHost(), uri.getPort()), 0);
            server.createContext("/health", exchange -> {
                byte[] body = "ok\n".getBytes(StandardCharsets.UTF_8);
                exchange.sendResponseHeaders(200, body.length);
                exchange.getResponseBody().write(body);
                exchange.close();
            });
            server.createContext("/evidence", exchange -> {
                byte[] body = json.writeValueAsBytes(state.snapshot());
                exchange.getResponseHeaders().add("Content-Type", "application/json");
                exchange.sendResponseHeaders(200, body.length);
                exchange.getResponseBody().write(body);
                exchange.close();
            });
            server.createContext("/registration/auto", exchange -> writeJson(exchange, registrationAuto()));
            server.createContext("/registration/attribute", exchange -> writeJson(exchange, registrationAttribute()));
            server.createContext("/registration/manual", exchange -> writeJson(exchange, registrationManual()));
            server.createContext("/registration/di-filter-order", exchange -> writeJson(exchange, registrationDi()));
            server.createContext("/registration/filter-order", exchange -> writeJson(exchange, registrationFilterOrder()));
            server.createContext("/codec/roundtrip", exchange -> writeJson(exchange, codecRoundtrip()));
            server.start();
            running = true;
        } catch (Exception error) {
            throw new IllegalStateException("failed to start evidence endpoint " + endpoint, error);
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

    private CompletionStage<Contracts.EchoRes> registrationAuto() {
        return client.requestToChannel(
                Contracts.CHANNEL,
                new Contracts.EchoAutoReq("auto-request"))
            .timeout(Duration.ofSeconds(5))
            .submit(Contracts.EchoRes.class)
            .thenApply(reply -> {
                client.sendToChannel(Contracts.CHANNEL, new Contracts.EchoAutoMsg("auto-send")).submit();
                return reply;
            });
    }

    private CompletionStage<Contracts.EchoRes> registrationAttribute() {
        return client.requestToChannel(
                Contracts.CHANNEL,
                new Contracts.EchoAttrReq("attr-request"))
            .timeout(Duration.ofSeconds(5))
            .submit(Contracts.EchoRes.class)
            .thenApply(reply -> {
                client.sendToChannel(Contracts.CHANNEL, new Contracts.EchoAttrMsg("attr-send")).submit();
                return reply;
            });
    }

    private CompletionStage<Contracts.EchoRes> registrationManual() {
        return client.requestToChannel(
                Contracts.CHANNEL,
                new Contracts.EchoManualReq("manual-request"))
            .timeout(Duration.ofSeconds(5))
            .submit(Contracts.EchoRes.class)
            .thenApply(reply -> {
                client.sendToChannel(Contracts.CHANNEL, new Contracts.EchoManualMsg("manual-send")).submit();
                return reply;
            });
    }

    private CompletionStage<List<Contracts.DiLifecycleRes>> registrationDi() {
        List<Contracts.DiLifecycleRes> replies = new ArrayList<>();
        CompletionStage<Void> sequence = CompletableFuture.completedFuture(null);
        for (int index = 0; index < 3; index++) {
            int requestIndex = index;
            sequence = sequence.thenCompose(ignored -> client.requestToChannel(
                    Contracts.CHANNEL,
                    new Contracts.DiLifecycleReq("di-" + requestIndex))
                .timeout(Duration.ofSeconds(5))
                .submit(Contracts.DiLifecycleRes.class)
                .thenAccept(replies::add));
        }
        return sequence.thenApply(ignored -> List.copyOf(replies));
    }

    private CompletionStage<Contracts.EchoRes> registrationFilterOrder() {
        return client.requestToChannel(
                Contracts.CHANNEL,
                new Contracts.EchoManualReq("filter-order-request"))
            .timeout(Duration.ofSeconds(5))
            .submit(Contracts.EchoRes.class);
    }

    private CompletionStage<Contracts.CodecScenarioRes> codecRoundtrip() {
        return client.requestToChannel(
                Contracts.CHANNEL,
                new Contracts.JsonEchoReq("json-request"))
            .timeout(Duration.ofSeconds(5))
            .submit(Contracts.EchoRes.class)
            .thenCompose(jsonReply -> {
                client.sendToChannel(Contracts.CHANNEL, new Contracts.JsonEchoMsg("json-send")).submit();
                return client.requestToChannel(Contracts.CHANNEL, StringValue.of("protobuf-request"))
                    .timeout(Duration.ofSeconds(5))
                    .submit(StringValue.class)
                    .thenApply(protobufReply -> new Object[] {jsonReply, protobufReply});
            })
            .thenCompose(replies -> {
                client.sendToChannel(Contracts.CHANNEL, StringValue.of("protobuf-send")).submit();
                return client.requestToChannel(
                        Contracts.CHANNEL,
                        new Contracts.PackedEchoReq("msgpack-request"))
                    .timeout(Duration.ofSeconds(5))
                    .submit(Contracts.PackedEchoRes.class)
                    .thenApply(packedReply -> new Contracts.CodecScenarioRes(
                        (Contracts.EchoRes) replies[0],
                        ((StringValue) replies[1]).getValue(),
                        packedReply.value()));
            })
            .thenApply(reply -> {
                client.sendToChannel(Contracts.CHANNEL, new Contracts.PackedEchoMsg("msgpack-send")).submit();
                return reply;
            });
    }

    private void writeJson(
        com.sun.net.httpserver.HttpExchange exchange,
        CompletionStage<?> response) {
        response.whenComplete((value, error) -> {
            try {
                if (error == null) {
                    writeJson(exchange, value);
                    return;
                }
                Throwable cause = error instanceof CompletionException && error.getCause() != null
                    ? error.getCause()
                    : error;
                writeError(exchange, cause);
            } catch (java.io.IOException writeFailure) {
                exchange.close();
            }
        });
    }

    private void writeJson(com.sun.net.httpserver.HttpExchange exchange, Object value) throws java.io.IOException {
        if (!"POST".equals(exchange.getRequestMethod()) && !"/health".equals(exchange.getRequestURI().getPath())) {
            byte[] body = "method not allowed\n".getBytes(StandardCharsets.UTF_8);
            exchange.sendResponseHeaders(405, body.length);
            exchange.getResponseBody().write(body);
            exchange.close();
            return;
        }
        try {
            byte[] body = json.writeValueAsBytes(value);
            exchange.getResponseHeaders().add("Content-Type", "application/json");
            exchange.sendResponseHeaders(200, body.length);
            exchange.getResponseBody().write(body);
        } catch (RuntimeException error) {
            writeError(exchange, error);
        } finally {
            exchange.close();
        }
    }

    private static void writeError(
        com.sun.net.httpserver.HttpExchange exchange,
        Throwable error) throws java.io.IOException {
        byte[] body = error.toString().getBytes(StandardCharsets.UTF_8);
        exchange.sendResponseHeaders(500, body.length);
        exchange.getResponseBody().write(body);
    }
}
