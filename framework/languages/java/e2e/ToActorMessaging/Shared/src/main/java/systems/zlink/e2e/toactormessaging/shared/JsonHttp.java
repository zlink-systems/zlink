package systems.zlink.e2e.toactormessaging.shared;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.util.function.Function;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;

public final class JsonHttp implements AutoCloseable {
    private static final ObjectMapper JSON = new ObjectMapper().findAndRegisterModules();
    private final HttpServer server;

    public JsonHttp(String endpoint) {
        try {
            URI uri = URI.create(endpoint);
            this.server = HttpServer.create(new InetSocketAddress(uri.getHost(), uri.getPort()), 0);
        } catch (IOException ex) {
            throw new IllegalStateException("failed to open HTTP server " + endpoint, ex);
        }
    }

    public <T> void post(String path, Class<T> requestType, Function<T, Object> handler) {
        server.createContext(path, exchange -> {
            if (!"POST".equals(exchange.getRequestMethod())) {
                send(exchange, 405, "method not allowed");
                return;
            }
            T request = JSON.readValue(exchange.getRequestBody(), requestType);
            sendJson(exchange, handler.apply(request));
        });
    }

    public <T> void postAsync(
        String path,
        Class<T> requestType,
        Function<T, CompletionStage<?>> handler) {
        server.createContext(path, exchange -> {
            if (!"POST".equals(exchange.getRequestMethod())) {
                send(exchange, 405, "method not allowed");
                return;
            }
            T request = JSON.readValue(exchange.getRequestBody(), requestType);
            handler.apply(request).whenComplete((reply, failure) -> {
                try {
                    if (failure == null) {
                        sendJson(exchange, reply);
                        return;
                    }
                    Throwable cause = failure instanceof CompletionException && failure.getCause() != null
                        ? failure.getCause()
                        : failure;
                    send(exchange, 500, cause.toString());
                } catch (IOException ignored) {
                    exchange.close();
                }
            });
        });
    }

    public void get(String path, java.util.function.Supplier<Object> handler) {
        server.createContext(path, exchange -> sendJson(exchange, handler.get()));
    }

    public void start() {
        server.start();
    }

    @Override
    public void close() {
        server.stop(0);
    }

    private static void sendJson(HttpExchange exchange, Object body) throws IOException {
        byte[] bytes = JSON.writeValueAsBytes(body);
        exchange.getResponseHeaders().set("content-type", "application/json");
        exchange.sendResponseHeaders(200, bytes.length);
        exchange.getResponseBody().write(bytes);
        exchange.close();
    }

    private static void send(HttpExchange exchange, int status, String body) throws IOException {
        byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
        exchange.sendResponseHeaders(status, bytes.length);
        exchange.getResponseBody().write(bytes);
        exchange.close();
    }
}
