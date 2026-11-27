package systems.zlink.crosslanguage.host;

import systems.zlink.framework.configuration.ZLinkFrameworkOptions;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationStore;
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
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;

final class LoginAdmissionQueue {
    private static final String PACKET = LoginAdmission.setting("ZLINK_LOGIN_QUEUE_PACKET");
    private static final Duration TIMEOUT = Duration.ofSeconds(30);

    private LoginAdmissionQueue() {}

    static void configure(
            ZLinkFrameworkOptions options,
            HostArgs args,
            ZLinkLocationStore locationStore,
            ZLinkRedisRelocationStore relocationStore) {
        options.addLocationStore(Objects.requireNonNull(locationStore));
        options.addRelocationStore(Objects.requireNonNull(relocationStore));
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        var role = LoginAdmission.setting("ZLINK_LOGIN_QUEUE_ROLE");
        var objects =
                options.addRouteMesh(args.require("mesh-name"))
                        .listen(args.require("bind-endpoint"))
                        .setRoutingIdPrefix(role)
                        .objects();
        if (role.equals("object-server")) {
            objects.server();
        } else {
            objects.client();
        }
        options.addStreamNode("login")
                .bind(args.require("stream-endpoint"))
                .registerSession(Session.class)
                .authenticationPacketName(PACKET)
                .maxConcurrentAuthentications(
                        LoginAdmission.number("ZLINK_LOGIN_QUEUE_LOCAL_LIMIT"))
                .admissionQueueEnabled(true)
                .admissionQueueKey(LoginAdmission.setting("ZLINK_LOGIN_QUEUE_KEY"))
                .admissionGlobalLimit(LoginAdmission.number("ZLINK_LOGIN_QUEUE_GLOBAL_LIMIT"));
    }

    static void connect(String firstEndpoint, String secondEndpoint) throws InterruptedException {
        var held = connector(firstEndpoint);
        var second = connector(secondEndpoint);
        var third = connector(secondEndpoint);
        try {
            for (var client : List.of(held, second, third)) {
                client.connect().submit().toCompletableFuture().join();
            }
            observePosition(second, "second");
            observePosition(third, "third");
            var first = request(held, "held");
            awaitMarker("held", first);
            CompletableFuture.allOf(first, request(second, "second"), request(third, "third"))
                    .join();
        } finally {
            CompletableFuture.allOf(
                            held.close().submit().toCompletableFuture(),
                            second.close().submit().toCompletableFuture(),
                            third.close().submit().toCompletableFuture())
                    .join();
        }
    }

    private static ZLinkStreamConnector connector(String endpoint) {
        var defaults = ZLinkStreamConnectorOptions.createDefault(URI.create(endpoint));
        return ZLinkStreamConnectorFactory.create(
                new ZLinkStreamConnectorOptions(
                        defaults.endpoint(),
                        ZLinkStreamDispatchMode.IMMEDIATE,
                        TIMEOUT,
                        defaults.maxReconnectAttempts(),
                        defaults.connectTimeout(),
                        defaults.maxSendPayloadSize(),
                        false,
                        defaults.heartbeatInterval(),
                        defaults.heartbeatTimeout(),
                        false,
                        defaults.reconnectInitialDelay(),
                        defaults.reconnectMaxDelay(),
                        defaults.reconnectBackoffFactor()));
    }

    private static void observePosition(ZLinkStreamConnector client, String value) {
        client.onAdmissionPosition(
                (position, estimatedWaitMs) -> {
                    if (position < 1 || estimatedWaitMs < 0) {
                        throw new IllegalStateException("Invalid admission position for " + value);
                    }
                    append(value + ".position", position + "|" + estimatedWaitMs + "\n");
                    return CompletableFuture.completedFuture(null);
                });
    }

    private static CompletableFuture<Void> request(ZLinkStreamConnector client, String value) {
        return client.request(PACKET, value)
                .submit(String.class)
                .thenAccept(
                        reply -> {
                            if (!value.equals(reply)) {
                                throw new IllegalStateException("Unexpected login reply: " + reply);
                            }
                            System.out.println("admitted " + value);
                        })
                .toCompletableFuture();
    }

    private static void awaitMarker(String name, CompletableFuture<?> pending)
            throws InterruptedException {
        var file = marker(name);
        long deadline = System.nanoTime() + TIMEOUT.toNanos();
        while (!Files.exists(file)) {
            if (pending != null && pending.isDone()) {
                pending.join();
                throw new IllegalStateException("Request completed before " + name);
            }
            if (System.nanoTime() >= deadline) {
                throw new IllegalStateException("Timed out waiting for " + name);
            }
            TimeUnit.MILLISECONDS.sleep(10);
        }
    }

    private static Path marker(String name) {
        return Path.of(LoginAdmission.setting("ZLINK_LOGIN_QUEUE_DIR"), name);
    }

    private static void append(String name, String value) {
        try {
            Files.writeString(
                    marker(name), value, StandardOpenOption.CREATE, StandardOpenOption.APPEND);
        } catch (IOException error) {
            throw new UncheckedIOException(error);
        }
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
            var node = context.sessionId();
            if (!PACKET.equals(dispatch.packetName())) {
                throw new IllegalStateException(
                        "Unexpected packet for " + node + ": " + dispatch.packetName());
            }
            var role = LoginAdmission.setting("ZLINK_LOGIN_QUEUE_ROLE");
            append("handlers.events", "enter|" + role + "|" + value + "\n");
            CompletionStage<Void> release = CompletableFuture.completedFuture(null);
            if (value.equals("held")) {
                append("held", "");
                release =
                        CompletableFuture.runAsync(
                                () -> {
                                    try {
                                        awaitMarker("release", null);
                                    } catch (InterruptedException error) {
                                        Thread.currentThread().interrupt();
                                        throw new IllegalStateException(error);
                                    }
                                });
            }
            return release.thenCompose(ignored -> context.client().reply(value).submit())
                    .whenComplete(
                            (ignored, error) ->
                                    append(
                                            "handlers.events",
                                            "leave|" + role + "|" + value + "\n"));
        }
    }
}
