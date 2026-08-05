package systems.zlink.e2e.kotlin.toactormessaging.shared;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.util.function.Function;

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

    public static <T> T postJson(String endpoint, Object body, Class<T> replyType) {
        try {
            java.net.http.HttpClient client = java.net.http.HttpClient.newHttpClient();
            java.net.http.HttpRequest request = java.net.http.HttpRequest.newBuilder(URI.create(endpoint))
                .header("content-type", "application/json")
                .POST(java.net.http.HttpRequest.BodyPublishers.ofByteArray(JSON.writeValueAsBytes(body)))
                .build();
            java.net.http.HttpResponse<byte[]> response =
                client.send(request, java.net.http.HttpResponse.BodyHandlers.ofByteArray());
            if (response.statusCode() / 100 != 2) {
                throw new IllegalStateException("HTTP " + response.statusCode() + " from " + endpoint);
            }
            return JSON.readValue(response.body(), replyType);
        } catch (IOException ex) {
            throw new IllegalStateException("HTTP call failed " + endpoint, ex);
        } catch (InterruptedException ex) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("HTTP call interrupted " + endpoint, ex);
        }
    }

    public static <T> T getJson(String endpoint, Class<T> replyType) {
        try {
            java.net.http.HttpClient client = java.net.http.HttpClient.newHttpClient();
            java.net.http.HttpRequest request = java.net.http.HttpRequest.newBuilder(URI.create(endpoint))
                .GET()
                .build();
            java.net.http.HttpResponse<byte[]> response =
                client.send(request, java.net.http.HttpResponse.BodyHandlers.ofByteArray());
            if (response.statusCode() / 100 != 2) {
                throw new IllegalStateException("HTTP " + response.statusCode() + " from " + endpoint);
            }
            return JSON.readValue(response.body(), replyType);
        } catch (IOException ex) {
            throw new IllegalStateException("HTTP call failed " + endpoint, ex);
        } catch (InterruptedException ex) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("HTTP call interrupted " + endpoint, ex);
        }
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
