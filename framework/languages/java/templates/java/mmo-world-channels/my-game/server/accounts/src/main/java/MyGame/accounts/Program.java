package MyGame.accounts;

import MyGame.*;

import com.sun.net.httpserver.HttpServer;

import java.io.*;
import java.net.*;

public final class Program {
    public static void main(String[] args) throws Exception {
        if (args.length != 1) throw new IllegalArgumentException("Supply accounts configuration");
        var config = Json.MAPPER.readValue(new File(args[0]), NodeConfig.class);
        var endpoint = URI.create(config.httpEndpoint());
        var server =
                HttpServer.create(new InetSocketAddress(endpoint.getHost(), endpoint.getPort()), 0);
        var worlds = Json.MAPPER.writeValueAsBytes(config.worlds());
        server.createContext(
                Names.WORLD_LIST_PATH,
                exchange -> {
                    try (exchange) {
                        if (!exchange.getRequestMethod().equals("GET")
                                || !exchange.getRequestURI()
                                        .getPath()
                                        .equals(Names.WORLD_LIST_PATH)) {
                            exchange.sendResponseHeaders(404, -1);
                            return;
                        }
                        exchange.getResponseHeaders().set("Content-Type", "application/json");
                        exchange.sendResponseHeaders(200, worlds.length);
                        exchange.getResponseBody().write(worlds);
                    }
                });
        server.start();
        try {
            System.out.println(
                    Control.READY
                            + Json.MAPPER.writeValueAsString(
                                    java.util.Map.of(
                                            "id",
                                            config.id(),
                                            "httpEndpoint",
                                            "http://"
                                                    + endpoint.getHost()
                                                    + ":"
                                                    + server.getAddress().getPort())));
            var input = new BufferedReader(new InputStreamReader(System.in));
            for (String command; (command = input.readLine()) != null; ) {
                if (!command.equals(Control.SHUTDOWN))
                    throw new IllegalArgumentException("Accounts supports shutdown only");
                server.stop(0);
                System.out.println(Control.STOPPED);
                return;
            }
        } finally {
            server.stop(0);
        }
    }
}
