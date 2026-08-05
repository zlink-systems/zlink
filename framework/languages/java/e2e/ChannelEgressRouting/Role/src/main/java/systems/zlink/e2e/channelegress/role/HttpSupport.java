package systems.zlink.e2e.channelegress.role;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpExchange;
import java.io.IOException;
import java.net.HttpURLConnection;
import java.nio.charset.StandardCharsets;

final class HttpSupport {
    private HttpSupport() {
    }

    static <T> T readJson(HttpExchange exchange, ObjectMapper json, Class<T> type)
        throws IOException {
        return json.readValue(exchange.getRequestBody(), type);
    }

    static void writeJson(HttpExchange exchange, ObjectMapper json, Object value)
        throws IOException {
        writeJson(exchange, json, HttpURLConnection.HTTP_OK, value);
    }

    static void writeJson(
        HttpExchange exchange,
        ObjectMapper json,
        int status,
        Object value) throws IOException {
        byte[] body = json.writeValueAsBytes(value);
        exchange.getResponseHeaders().add("Content-Type", "application/json");
        exchange.sendResponseHeaders(status, body.length);
        try (var output = exchange.getResponseBody()) {
            output.write(body);
        }
    }

    static void writeText(HttpExchange exchange, int status, String value)
        throws IOException {
        byte[] body = value.getBytes(StandardCharsets.UTF_8);
        exchange.sendResponseHeaders(status, body.length);
        try (var output = exchange.getResponseBody()) {
            output.write(body);
        }
    }
}
