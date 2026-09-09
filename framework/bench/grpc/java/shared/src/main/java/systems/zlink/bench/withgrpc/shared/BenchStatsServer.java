/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.shared;

import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.Executors;

/**
 * The stats endpoint the client polls.
 *
 * <p>It must answer WHILE the server is still draining, because the settle
 * contract (spec section 3 / FB-008) is "poll until the received count stops moving",
 * not "sleep a fixed time". It runs on its own executor so that a blocking
 * receive loop on another thread cannot stall it.
 */
public final class BenchStatsServer {
    private BenchStatsServer() {
    }

    public static HttpServer start(String url, BenchServerMetrics metrics, String infoJson) {
        URI parsed = URI.create(url);
        try {
            HttpServer server = HttpServer.create(
                new InetSocketAddress(parsed.getHost(), parsed.getPort()), 64);
            server.setExecutor(Executors.newFixedThreadPool(2, runnable -> {
                Thread thread = new Thread(runnable, "bench-stats");
                thread.setDaemon(true);
                return thread;
            }));
            server.createContext("/ready", exchange -> respond(exchange, 200, "ready"));
            server.createContext("/bench/reset", exchange -> {
                metrics.reset();
                respond(exchange, 200, "{}");
            });
            server.createContext("/bench/stats", exchange ->
                respond(exchange, 200, metrics.snapshot().toJson()));
            server.createContext("/bench/info", exchange -> respond(exchange, 200, infoJson));
            server.start();
            return server;
        } catch (IOException error) {
            throw new IllegalStateException("failed to start stats server on " + url, error);
        }
    }

    private static void respond(HttpExchange exchange, int status, String body)
        throws IOException {
        byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
        exchange.getResponseHeaders().add("content-type", "application/json");
        exchange.sendResponseHeaders(status, bytes.length);
        try (var stream = exchange.getResponseBody()) {
            stream.write(bytes);
        }
    }
}
