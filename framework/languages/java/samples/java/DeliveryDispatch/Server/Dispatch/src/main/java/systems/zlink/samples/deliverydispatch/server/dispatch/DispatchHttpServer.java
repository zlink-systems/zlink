package systems.zlink.samples.deliverydispatch.server.dispatch;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;

import systems.zlink.framework.channels.ZLinkClient;
import systems.zlink.samples.deliverydispatch.server.configuration.SampleNames;
import systems.zlink.samples.deliverydispatch.shared.contracts.Messages;

import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.util.Map;

public final class DispatchHttpServer implements AutoCloseable {
    private final HttpServer server;
    private final ObjectMapper json;
    private final ZLinkClient channels;
    private final DispatchWorkQueue queue;

    public DispatchHttpServer(
            ObjectMapper json, ZLinkClient channels, DispatchWorkQueue queue, String endpoint)
            throws IOException {
        this.json = json;
        this.channels = channels;
        this.queue = queue;
        URI uri = URI.create(endpoint);
        this.server = HttpServer.create(new InetSocketAddress(uri.getHost(), uri.getPort()), 0);
        server.createContext("/deliveries", this::handleCreateDelivery);
        server.createContext("/self-check/assert", this::handleServerAssertion);
        server.start();
    }

    // --8<-- [start:doc-dd-http-create]
    private void handleCreateDelivery(HttpExchange exchange) throws IOException {
        if (!"POST".equals(exchange.getRequestMethod())) {
            write(exchange, 405, "");
            return;
        }
        Messages.CreateDeliveryReq request =
                json.readValue(exchange.getRequestBody(), Messages.CreateDeliveryReq.class);
        Messages.AssignDeliveryMsg assign =
                new Messages.AssignDeliveryMsg(
                        request.deliveryId(),
                        request.customerId(),
                        request.pickupAddress(),
                        request.dropoffAddress());
        channels.sendToChannel(SampleNames.DispatchChannel, assign).submit_sync();
        write(
                exchange,
                200,
                json.writeValueAsString(new Messages.CreateDeliveryRes(request.deliveryId())));
    }

    // --8<-- [end:doc-dd-http-create]

    private void handleServerAssertion(HttpExchange exchange) throws IOException {
        if (!"POST".equals(exchange.getRequestMethod())) {
            write(exchange, 405, "");
            return;
        }
        Messages.ServerAssertionReq request =
                json.readValue(exchange.getRequestBody(), Messages.ServerAssertionReq.class);
        queue.assertServerEvidence(request)
                .whenComplete(
                        (response, error) -> {
                            try {
                                if (error != null) {
                                    write(
                                            exchange,
                                            500,
                                            json.writeValueAsString(
                                                    Map.of("error", error.getMessage())));
                                } else {
                                    write(
                                            exchange,
                                            response.passed() ? 200 : 500,
                                            json.writeValueAsString(response));
                                }
                            } catch (IOException writeError) {
                                exchange.close();
                            }
                        });
    }

    private static void write(HttpExchange exchange, int status, String body) throws IOException {
        byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
        exchange.getResponseHeaders().add("content-type", "application/json");
        exchange.sendResponseHeaders(status, bytes.length);
        exchange.getResponseBody().write(bytes);
        exchange.close();
    }

    @Override
    public void close() {
        server.stop(0);
    }
}
