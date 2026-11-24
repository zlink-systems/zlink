package examples;

import org.jspecify.annotations.Nullable;

import systems.zlink.stream.connector.ZLinkStreamConnectionState;
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory;
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions;
import systems.zlink.stream.connector.ZLinkStreamDispatchMode;

import java.net.URI;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;

public final class ConnectorExample {
    private ConnectorExample() {}

    public static void main(String[] args) throws Exception {
        var endpoint =
                System.getenv().getOrDefault("CONNECTOR_EXAMPLE_ENDPOINT", "ws://127.0.0.1:7301");
        var timeout = Duration.ofSeconds(5);
        var connector =
                ZLinkStreamConnectorFactory.create(
                        new ZLinkStreamConnectorOptions(
                                URI.create(endpoint),
                                ZLinkStreamDispatchMode.IMMEDIATE,
                                timeout,
                                3));

        try {
            connector.connect().submit().toCompletableFuture().join();
            System.out.println("Connected");

            var pushed = new CompletableFuture<Notice>();
            try (var subscription =
                    connector.on(
                            Notice.class,
                            message -> {
                                pushed.complete(message.payload());
                                return CompletableFuture.completedFuture(null);
                            })) {
                connector.send(new Publish()).submit().toCompletableFuture().join();
                var notice = pushed.orTimeout(timeout.toMillis(), TimeUnit.MILLISECONDS).join();
                if (!notice.text().equals("Hello from the server")) {
                    throw new IllegalStateException("Unexpected Notice");
                }
                System.out.println("Push: " + notice.text());
            }

            var reconnected = new CompletableFuture<Void>();
            try (var subscription =
                    connector.onConnectionStateChanged(
                            state -> {
                                if (state == ZLinkStreamConnectionState.CONNECTED) {
                                    reconnected.complete(null);
                                }
                                return CompletableFuture.completedFuture(null);
                            })) {
                connector.send(new Reconnect()).submit().toCompletableFuture().join();
                reconnected.orTimeout(timeout.toMillis(), TimeUnit.MILLISECONDS).join();
            }
            System.out.println("Reconnected");

            // The typed codec encodes long as a decimal string. The optional label is omitted.
            var echoed =
                    connector
                            .request(new Echo(Long.MAX_VALUE, null))
                            .submit(Echoed.class)
                            .toCompletableFuture()
                            .join();
            if (echoed.score() != Long.MAX_VALUE
                    || echoed.nickname() != null
                    || !echoed.label().equals("default")) {
                throw new IllegalStateException("Unexpected Echoed: " + echoed);
            }
            System.out.println("Reply: " + echoed);

            var left =
                    connector.request(new Leave()).submit(Left.class).toCompletableFuture().join();
            if (!left.accepted()) {
                throw new IllegalStateException("Leave was rejected");
            }
            System.out.println("Leave acknowledged");
        } finally {
            connector.close().submit().toCompletableFuture().join();
        }
        System.out.println("Closed");
    }

    public record Publish() {}

    public record Reconnect() {}

    public record Leave() {}

    public record Notice(String text) {}

    public record Echo(long score, @Nullable String nickname) {}

    public record Echoed(long score, @Nullable String nickname, String label) {}

    public record Left(boolean accepted) {}
}
