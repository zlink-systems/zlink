package MyGame.lobby.infrastructure;

import MyGame.*;
import MyGame.lobby.application.Matchmaker;

import com.sun.net.httpserver.*;

import java.io.*;
import java.net.*;
import java.util.concurrent.*;

public final class HttpLobby implements AutoCloseable, Hosting.HttpEndpoint {
    private final HttpServer server;
    private final ExecutorService executor = Executors.newVirtualThreadPerTaskExecutor();
    private final Matchmaker matches;

    public HttpLobby(NodeConfig config, Matchmaker matches) throws IOException {
        this.matches = matches;
        var uri = URI.create(config.httpEndpoint());
        server = HttpServer.create(new InetSocketAddress(uri.getHost(), uri.getPort()), 0);
        server.setExecutor(executor);
        server.createContext(Names.MATCH_PATH, this::handle);
        server.start();
    }

    public String endpoint() {
        return "http://"
                + server.getAddress().getHostString()
                + ":"
                + server.getAddress().getPort();
    }

    private void handle(HttpExchange exchange) throws IOException {
        try (exchange) {
            if (!exchange.getRequestMethod().equals(HttpProtocol.POST)
                    || !exchange.getRequestURI().getPath().equals(Names.MATCH_PATH)) {
                exchange.sendResponseHeaders(HttpProtocol.NOT_FOUND, -1);
                return;
            }
            MatchRequest request;
            try {
                request = Json.MAPPER.readValue(exchange.getRequestBody(), MatchRequest.class);
            } catch (IOException error) {
                exchange.sendResponseHeaders(HttpProtocol.BAD_REQUEST, -1);
                return;
            }
            try {
                var assignment = matches.match(request).toCompletableFuture().join();
                byte[] body = Json.MAPPER.writeValueAsBytes(assignment);
                exchange.getResponseHeaders().set(HttpProtocol.CONTENT_TYPE, HttpProtocol.JSON);
                exchange.sendResponseHeaders(HttpProtocol.OK, body.length);
                exchange.getResponseBody().write(body);
            } catch (IllegalArgumentException error) {
                exchange.sendResponseHeaders(HttpProtocol.BAD_REQUEST, -1);
            } catch (CompletionException error) {
                error.getCause().printStackTrace(System.err);
                exchange.sendResponseHeaders(HttpProtocol.UNAVAILABLE, -1);
            }
        }
    }

    public void close() {
        matches.close();
        server.stop(0);
        executor.close();
    }
}
