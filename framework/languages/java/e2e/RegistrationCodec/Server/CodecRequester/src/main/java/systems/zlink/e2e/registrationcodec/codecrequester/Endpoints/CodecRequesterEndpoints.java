package systems.zlink.e2e.registrationcodec.codecrequester.Endpoints;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.google.protobuf.StringValue;
import com.sun.net.httpserver.HttpServer;
import java.net.InetSocketAddress;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import org.springframework.context.SmartLifecycle;
import systems.zlink.e2e.registrationcodec.codecrequester.Configuration.CodecRequesterOptions;
import systems.zlink.e2e.registrationcodec.shared.Contracts;
import systems.zlink.framework.channels.ZLinkClient;
import systems.zlink.framework.errors.ZLinkFrameworkException;

public final class CodecRequesterEndpoints implements SmartLifecycle {
    private final CodecRequesterOptions options;
    private final ObjectMapper json;
    private final ZLinkClient client;
    private HttpServer server;
    private boolean running;

    public CodecRequesterEndpoints(CodecRequesterOptions options, ObjectMapper json, ZLinkClient client) {
        this.options = options;
        this.json = json;
        this.client = client;
    }

    @Override
    public void start() {
        try {
            URI uri = URI.create(options.httpEndpoint());
            server = HttpServer.create(new InetSocketAddress(uri.getHost(), uri.getPort()), 0);
            server.createContext("/health", exchange -> {
                byte[] body = "ok\n".getBytes(StandardCharsets.UTF_8);
                exchange.sendResponseHeaders(200, body.length);
                exchange.getResponseBody().write(body);
                exchange.close();
            });
            server.createContext("/codec/protobuf/request", exchange -> writeJson(exchange, protobufRequest()));
            server.createContext("/codec/json/request", exchange -> writeJson(exchange, jsonRequest()));
            server.start();
            running = true;
        } catch (Exception error) {
            throw new IllegalStateException("failed to start codec requester endpoint " + options.httpEndpoint(), error);
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

    private CompletionStage<Contracts.CodecMismatchProbeRes> protobufRequest() {
        return client.requestToChannel(
                    Contracts.CHANNEL,
                    StringValue.of("rc-b5"))
                .timeout(Duration.ofSeconds(2))
                .submit(StringValue.class)
            .handle((reply, error) -> error == null
                ? new Contracts.CodecMismatchProbeRes(false, null, reply.getValue())
                : new Contracts.CodecMismatchProbeRes(
                    true,
                    protocolErrorName(error),
                    null));
    }

    private static String protocolErrorName(Throwable error) {
        Throwable cause = error instanceof CompletionException && error.getCause() != null
            ? error.getCause()
            : error;
        return cause instanceof ZLinkFrameworkException frameworkError
            ? frameworkError.kind().name()
            : cause.getClass().getSimpleName();
    }

    private CompletionStage<Contracts.EchoRes> jsonRequest() {
        return client.requestToChannel(
                Contracts.CHANNEL,
                new Contracts.JsonEchoReq("rc-b5-json"))
            .timeout(Duration.ofSeconds(5))
            .submit(Contracts.EchoRes.class);
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
        if (!"POST".equals(exchange.getRequestMethod())) {
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
