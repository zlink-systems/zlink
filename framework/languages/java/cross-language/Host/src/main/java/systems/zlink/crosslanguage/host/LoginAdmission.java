package systems.zlink.crosslanguage.host;

import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkStreamError;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory;
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions;
import systems.zlink.stream.connector.ZLinkStreamDispatchMode;

import java.io.IOException;
import java.io.UncheckedIOException;
import java.net.URI;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.time.Duration;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;

public final class LoginAdmission {
    private LoginAdmission() {}

    static String setting(String name) {
        return Objects.requireNonNull(
                System.getenv(name), "Missing login admission setting " + name);
    }

    static int number(String name) {
        return Integer.parseInt(setting(name));
    }

    private static Path marker(String name) {
        return Path.of(setting("ZLINK_LOGIN_DIR"), name);
    }

    private static void write(String name, String value) {
        try {
            Files.writeString(
                    marker(name), value, StandardOpenOption.CREATE, StandardOpenOption.APPEND);
        } catch (IOException error) {
            throw new UncheckedIOException(error);
        }
    }

    private static void waitFor(String name) {
        var path = marker(name);
        long deadline =
                System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(number("ZLINK_LOGIN_TIMEOUT_MS"));
        try {
            while (!Files.exists(path)) {
                if (System.nanoTime() >= deadline) {
                    throw new IllegalStateException("Timed out waiting for " + name);
                }
                Thread.sleep(5);
            }
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("Interrupted waiting for " + name, error);
        }
    }

    static void connect(String endpoint) throws Exception {
        var defaults = ZLinkStreamConnectorOptions.createDefault(URI.create(endpoint));
        var options =
                new ZLinkStreamConnectorOptions(
                        defaults.endpoint(),
                        ZLinkStreamDispatchMode.IMMEDIATE,
                        Duration.ofMillis(number("ZLINK_LOGIN_TIMEOUT_MS")),
                        defaults.maxReconnectAttempts(),
                        defaults.connectTimeout(),
                        defaults.maxSendPayloadSize(),
                        false,
                        defaults.heartbeatInterval(),
                        defaults.heartbeatTimeout(),
                        false,
                        defaults.reconnectInitialDelay(),
                        defaults.reconnectMaxDelay(),
                        defaults.reconnectBackoffFactor());
        var held = ZLinkStreamConnectorFactory.create(options);
        var contender = ZLinkStreamConnectorFactory.create(options);
        try (var waiting =
                contender.onAdmissionWaiting(
                        event -> {
                            write(
                                    "waiting",
                                    event.requestPacketName()
                                            + "|"
                                            + event.retryAfter().toMillis()
                                            + "\n");
                            return CompletableFuture.completedFuture(null);
                        })) {
            held.connect().submit().toCompletableFuture().join();
            contender.connect().submit().toCompletableFuture().join();
            request(contender, "ZLINK_LOGIN_CONTROL").join();
            var pending = request(held, "ZLINK_LOGIN_HELD");
            waitFor("held");
            CompletableFuture.allOf(pending, request(contender, "ZLINK_LOGIN_CONTENDER")).join();
        } finally {
            CompletableFuture.allOf(
                            held.close().submit().toCompletableFuture(),
                            contender.close().submit().toCompletableFuture())
                    .join();
        }
    }

    private static CompletableFuture<Void> request(ZLinkStreamConnector client, String name) {
        var expected = setting(name);
        return client.request(setting("ZLINK_LOGIN_PACKET"), expected)
                .submit(String.class)
                .thenAccept(
                        reply -> {
                            if (!expected.equals(reply)) {
                                throw new IllegalStateException("Unexpected login reply: " + reply);
                            }
                        })
                .toCompletableFuture();
    }

    public record Session(ZLinkSessionContext context) implements ZLinkSession {
        @Override
        public CompletionStage<Void> onConnected() {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDisconnected() {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onError(ZLinkStreamError error) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDispatch(
                ZLinkSessionDispatchContext dispatch, ZLinkMessage payload) {
            var value = payload.decode(String.class);
            write("server.events", value + "|" + context.sessionId() + "\n");
            if (value.equals(setting("ZLINK_LOGIN_HELD"))) {
                write("held", "");
                return CompletableFuture.runAsync(() -> waitFor("release"))
                        .thenCompose(ignored -> context.client().reply(value).submit());
            }
            return context.client().reply(value).submit();
        }
    }
}
