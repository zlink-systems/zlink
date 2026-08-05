package systems.zlink.e2e.storefailure.shared;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;
import java.io.IOException;
import java.net.HttpURLConnection;
import java.net.InetSocketAddress;
import java.net.URI;
import java.nio.charset.StandardCharsets;

public final class HttpSupport {
    private HttpSupport() {
    }

    public static HttpServer createServer(String endpoint) throws IOException {
        URI uri = URI.create(endpoint);
        return HttpServer.create(new InetSocketAddress(uri.getHost(), uri.getPort()), 0);
    }

    public static <T> T readJson(
        HttpExchange exchange,
        ObjectMapper json,
        Class<T> type) throws IOException {
        return json.readValue(exchange.getRequestBody(), type);
    }

    public static void writeJson(
        HttpExchange exchange,
        ObjectMapper json,
        Object value) throws IOException {
        byte[] body = json.writeValueAsBytes(value);
        exchange.getResponseHeaders().add("Content-Type", "application/json");
        exchange.sendResponseHeaders(HttpURLConnection.HTTP_OK, body.length);
        exchange.getResponseBody().write(body);
        exchange.close();
    }

    public static void writeText(
        HttpExchange exchange,
        String value) throws IOException {
        byte[] body = value.getBytes(StandardCharsets.UTF_8);
        exchange.sendResponseHeaders(HttpURLConnection.HTTP_OK, body.length);
        exchange.getResponseBody().write(body);
        exchange.close();
    }

    public static void shutdownAsync() {
        Thread shutdown = new Thread(() -> {
            try {
                Thread.sleep(100);
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
            }
            System.exit(0);
        }, "zlink-e2e-shutdown");
        shutdown.setDaemon(false);
        shutdown.start();
    }

}
