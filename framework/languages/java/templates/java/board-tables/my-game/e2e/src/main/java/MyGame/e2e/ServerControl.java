package MyGame.e2e;

import MyGame.Control;
import MyGame.Json;

import com.sun.net.httpserver.HttpServer;

import java.io.IOException;
import java.net.*;
import java.net.http.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.time.Duration;
import java.util.UUID;

interface ServerControl {
    void drain(String id) throws Exception;

    void replace(String id) throws Exception;

    String CONTROL_HEADER = "X-MyGame-Control";

    record State(URI controlEndpoint, ServerHost.Endpoints endpoints, UUID controlToken) {}

    record Connection(Path root, ServerHost.Endpoints endpoints, ServerControl control) {
        String endpoint() {
            return endpoints.session();
        }
    }

    static Path statePath(Path root) {
        return root.resolve(".logs/server.json");
    }

    static Connection attach(Path root) throws Exception {
        Structure.require(
                ServerHost.isRunning(root), "Start the server with run-server before attaching.");
        var state = Json.MAPPER.readValue(statePath(root).toFile(), State.class);
        var endpoint = state.controlEndpoint();
        Structure.require(
                "http".equals(endpoint.getScheme())
                        && ServerHost.LOOPBACK.equals(endpoint.getHost())
                        && endpoint.getPort() > 0,
                "Invalid server control endpoint");
        var client = HttpClient.newBuilder().connectTimeout(Control.HOST_DEADLINE).build();
        return new Connection(
                root,
                state.endpoints(),
                new ServerControl() {
                    private void call(String operation, String id, Duration timeout)
                            throws Exception {
                        var request =
                                HttpRequest.newBuilder(endpoint.resolve(operation + "/" + id))
                                        .header(CONTROL_HEADER, state.controlToken().toString())
                                        .timeout(timeout)
                                        .POST(HttpRequest.BodyPublishers.noBody())
                                        .build();
                        var response = client.send(request, HttpResponse.BodyHandlers.ofString());
                        Structure.require(
                                response.statusCode() == 204,
                                operation
                                        + " "
                                        + id
                                        + " failed ("
                                        + response.statusCode()
                                        + "): "
                                        + response.body());
                    }

                    public void drain(String id) throws Exception {
                        call("drain", id, Control.SAFE_DEADLINE.plus(Control.HOST_DEADLINE));
                    }

                    public void replace(String id) throws Exception {
                        call("replace", id, Duration.ofSeconds(2 * Control.PROCESS_SECONDS));
                    }
                });
    }

    static HttpServer publish(Path root, ServerHost host, ServerHost.Endpoints endpoints)
            throws Exception {
        var server = HttpServer.create(new InetSocketAddress(ServerHost.LOOPBACK, 0), 0);
        UUID token = UUID.randomUUID();
        server.createContext(
                "/",
                exchange -> {
                    try (exchange) {
                        var path = exchange.getRequestURI().getPath().split("/", -1);
                        if (!exchange.getRequestMethod().equals("POST")
                                || !token.toString()
                                        .equals(
                                                exchange.getRequestHeaders()
                                                        .getFirst(CONTROL_HEADER))
                                || path.length != 3
                                || !(path[1].equals("drain") || path[1].equals("replace"))) {
                            exchange.sendResponseHeaders(404, -1);
                            return;
                        }
                        try {
                            if (path[1].equals("drain")) host.drain(path[2]);
                            else host.replace(path[2]);
                            exchange.sendResponseHeaders(204, -1);
                        } catch (Exception failure) {
                            failure.printStackTrace(System.err);
                            var body = failure.toString().getBytes(StandardCharsets.UTF_8);
                            exchange.sendResponseHeaders(500, body.length);
                            exchange.getResponseBody().write(body);
                        }
                    }
                });
        server.start();
        var state = statePath(root);
        var temporary = state.resolveSibling("server-" + UUID.randomUUID() + ".json");
        try {
            var uri =
                    URI.create(
                            "http://"
                                    + ServerHost.LOOPBACK
                                    + ":"
                                    + server.getAddress().getPort()
                                    + "/");
            Json.MAPPER.writeValue(temporary.toFile(), new State(uri, endpoints, token));
            Files.move(temporary, state, StandardCopyOption.ATOMIC_MOVE);
            return server;
        } catch (Exception failure) {
            server.stop(0);
            try {
                Files.deleteIfExists(temporary);
            } catch (IOException cleanup) {
                failure.addSuppressed(cleanup);
            }
            throw failure;
        }
    }
}
