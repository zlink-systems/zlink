package systems.zlink.tutorial.streamclient;

import systems.zlink.stream.connector.*;

import java.net.URI;
import java.time.Duration;
import java.util.List;

public final class ReceivingProgram {
    public record LeaderboardUpdate(int rank) {}

    public record Ready(String stage) {}

    public record MatchFound(String matchId) {}

    public record OrderChanged(String status) {}

    public record ReceivingStage(String stage) {}

    private int handled;
    private int frames;
    private boolean running = true;

    private void renderFrame() {
        frames++;
        running = false;
    }

    public static void run(String endpoint) throws Exception {
        new ReceivingProgram().receive(endpoint);
    }

    private void receive(String endpoint) throws Exception {
        ZLinkStreamConnector connector =
                ZLinkStreamConnectorFactory.create(
                        new ZLinkStreamConnectorOptions(
                                URI.create(endpoint),
                                ZLinkStreamDispatchMode.MANUAL,
                                Duration.ofSeconds(30),
                                3));
        AutoCloseable subscription =
                connector.on(
                        LeaderboardUpdate.class,
                        message -> {
                            handled++;
                            return java.util.concurrent.CompletableFuture.completedFuture(null);
                        });
        try {
            connector.connect().submit().toCompletableFuture().join();
            connector.send(new ReceivingStage("pump")).submit().toCompletableFuture().join();
            connector.waitFor(Ready.class).submit(Ready.class).toCompletableFuture().join();
            // --8<-- [start:receiving-pump]
            while (running) {
                connector.dispatch().submit().toCompletableFuture().join();
                renderFrame();
            }
            // --8<-- [end:receiving-pump]
            // --8<-- [start:receiving-unsubscribe]
            subscription.close();
            // --8<-- [end:receiving-unsubscribe]
            connector
                    .send(new ReceivingStage("unsubscribed"))
                    .submit()
                    .toCompletableFuture()
                    .join();
            connector.waitFor(Ready.class).submit(Ready.class).toCompletableFuture().join();
            connector.dispatch().submit().toCompletableFuture().join();
            connector.send(new ReceivingStage("match")).submit().toCompletableFuture().join();
            // --8<-- [start:receiving-wait]
            ZLinkStreamMessage<MatchFound> found =
                    connector
                            .waitFor(MatchFound.class)
                            .where(
                                    MatchFound.class,
                                    message -> "match-7f3a".equals(message.payload().matchId()))
                            .timeout(Duration.ofSeconds(30))
                            .submit(MatchFound.class)
                            .toCompletableFuture()
                            .join();
            // --8<-- [end:receiving-wait]
            // --8<-- [start:receiving-sequence]
            connector
                    .expectNone(OrderChanged.class)
                    .within(Duration.ofMillis(100))
                    .submit()
                    .toCompletableFuture()
                    .join();
            connector.send(new ReceivingStage("orders")).submit().toCompletableFuture().join();
            List<ZLinkStreamMessage<OrderChanged>> steps =
                    connector
                            .waitForSequence(OrderChanged.class)
                            .expect(OrderChanged.class, m -> m.payload().status().equals("paid"))
                            .expect(OrderChanged.class, m -> m.payload().status().equals("shipped"))
                            .timeout(Duration.ofSeconds(2))
                            .submit(OrderChanged.class)
                            .toCompletableFuture()
                            .join();
            // --8<-- [end:receiving-sequence]
            // --8<-- [start:receiving-count]
            int count = connector.receivedCount("LeaderboardUpdate");
            // --8<-- [end:receiving-count]
            if (handled != 1
                    || frames != 1
                    || count != 2
                    || !found.payload().matchId().equals("match-7f3a")
                    || steps.size() != 2)
                throw new IllegalStateException(
                        "Receiving tutorial result did not match expected messages.");
            System.out.printf(
                    "receiving: handler=%d, frames=%d, match=%s, sequence=%s, count=%d%n",
                    handled,
                    frames,
                    found.payload().matchId(),
                    String.join(",", steps.stream().map(m -> m.payload().status()).toList()),
                    count);
        } finally {
            subscription.close();
            connector.close().submit().toCompletableFuture().join();
        }
    }
}
