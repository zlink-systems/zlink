package systems.zlink.samples.bingo.client;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;
import systems.zlink.samples.bingo.client.configuration.SampleNames;
import systems.zlink.samples.bingo.shared.contracts.BingoMessages;
import systems.zlink.samples.bingo.shared.contracts.Messages;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamMessage;

public final class BingoClientScenario {
    public void run(
        ZLinkStreamConnector client1,
        ZLinkStreamConnector client2,
        ZLinkStreamConnector observer) throws Exception {
        client1.connect().submit().toCompletableFuture().join();
        client2.connect().submit().toCompletableFuture().join();
        observer.connect().submit().toCompletableFuture().join();

        Messages.AuthenticateRes client1Auth =
            client1.request(BingoMessages.authenticateReq("player-1")).submit(Messages.AuthenticateRes.class).toCompletableFuture().join();
        ensure(client1Auth.getActorId().equals("player-1"));
        AtomicInteger client1OwnJoinNotifications = new AtomicInteger();
        client1.on(SampleNames.PlayerJoinedPacket, Messages.PlayerJoinedNotify.class, message -> {
            if (client1Auth.getActorId().equals(message.payload().getActorId())) {
                client1OwnJoinNotifications.incrementAndGet();
            }
            return CompletableFuture.completedFuture(null);
        });

        var client1MatchCompletion = client1.waitFor(Messages.MatchBingoRes.class)
            .submit(Messages.MatchBingoRes.class);
        client1.send(BingoMessages.matchBingoReq("two-player")).submit()
            .toCompletableFuture().join();
        Messages.MatchBingoRes client1Match =
            client1MatchCompletion.toCompletableFuture().join().payload();
        ensure(client1Match.getState().getStatus().equals("WaitingForPlayers"));
        ensure(client1Match.getState().getHostActorId().equals(client1Auth.getActorId()));

        Messages.AuthenticateRes observerAuth =
            observer.request(BingoMessages.authenticateReq("observer")).submit(Messages.AuthenticateRes.class).toCompletableFuture().join();
        ensure(observerAuth.getActorId().equals("observer"));
        Messages.ObserveBingoEventsRes observed = observer
            .request(BingoMessages.observeBingoEventsReq(client1Match.getRoomId()))
            .submit(Messages.ObserveBingoEventsRes.class).toCompletableFuture().join();
        ensure(observed.getSubscribed());

        var client1SawClient2Join =
            client1.waitFor(SampleNames.PlayerJoinedPacket).submit(Messages.PlayerJoinedNotify.class);
        var client1Started =
            client1.waitFor(SampleNames.GameStartedPacket).submit(Messages.BingoGameStartedNotify.class);

        Messages.AuthenticateRes client2Auth =
            client2.request(BingoMessages.authenticateReq("player-2")).submit(Messages.AuthenticateRes.class).toCompletableFuture().join();
        ensure(client2Auth.getActorId().equals("player-2"));
        ensure(!client2Auth.getActorId().equals(client1Auth.getActorId()));
        AtomicInteger client2OwnJoinNotifications = new AtomicInteger();
        client2.on(SampleNames.PlayerJoinedPacket, Messages.PlayerJoinedNotify.class, message -> {
            if (client2Auth.getActorId().equals(message.payload().getActorId())) {
                client2OwnJoinNotifications.incrementAndGet();
            }
            return CompletableFuture.completedFuture(null);
        });

        var client2MatchCompletion = client2.waitFor(Messages.MatchBingoRes.class)
            .submit(Messages.MatchBingoRes.class);
        client2.send(BingoMessages.matchBingoReq("two-player")).submit()
            .toCompletableFuture().join();
        Messages.MatchBingoRes client2Match =
            client2MatchCompletion.toCompletableFuture().join().payload();
        ensure(client2Match.getRoomId().equals(client1Match.getRoomId()));
        ensure(client2Match.getState().getStatus().equals("Running"));

        Messages.PlayerJoinedNotify join = client1SawClient2Join.toCompletableFuture().join().payload();
        ensure(join.getActorId().equals(client2Auth.getActorId()));
        ensure(client1Started.toCompletableFuture().join().payload().getState().getStatus().equals("Running"));

        Messages.SubmitBingoCardRes client2Card = client2
            .request(BingoMessages.submitBingoCardReq(client2Match.getRoomId(), BingoClientCards.Player2))
            .submit(Messages.SubmitBingoCardRes.class).toCompletableFuture().join();
        ensure(client2Card.getState().getStatus().equals("Running"));
        ensure(client2Card.getState().getPlayersList().stream()
            .anyMatch(player -> player.getActorId().equals(client2Auth.getActorId())
                && player.getCardCount() == 9));
        var rewardAnnounced = observer.waitFor(SampleNames.RewardAnnouncedPacket)
            .where(
                Messages.BingoRewardAnnouncedNotify.class,
                message -> message.payload().getRoomId().equals(client1Match.getRoomId()))
            .submit(Messages.BingoRewardAnnouncedNotify.class);

        var client1Ended =
            client1.waitFor(SampleNames.GameEndedPacket).submit(Messages.BingoGameEndedNotify.class);
        var client2Ended =
            client2.waitFor(SampleNames.GameEndedPacket).submit(Messages.BingoGameEndedNotify.class);
        List<CompletionStage<ZLinkStreamMessage<Messages.BingoNumberDrawnNotify>>> client1Draws =
            new ArrayList<>();
        List<CompletionStage<ZLinkStreamMessage<Messages.BingoNumberDrawnNotify>>> client2Draws =
            new ArrayList<>();
        for (int drawSeq = 1; drawSeq <= 15; drawSeq++) {
            int expectedDrawSeq = drawSeq;
            client1Draws.add(client1.waitFor(SampleNames.NumberDrawnPacket)
                .where(
                    Messages.BingoNumberDrawnNotify.class,
                    message -> message.payload().getDrawSeq() == expectedDrawSeq)
                .submit(Messages.BingoNumberDrawnNotify.class));
            client2Draws.add(client2.waitFor(SampleNames.NumberDrawnPacket)
                .where(
                    Messages.BingoNumberDrawnNotify.class,
                    message -> message.payload().getDrawSeq() == expectedDrawSeq)
                .submit(Messages.BingoNumberDrawnNotify.class));
        }

        Messages.SubmitBingoCardRes client1Card = client1
            .request(BingoMessages.submitBingoCardReq(client1Match.getRoomId(), BingoClientCards.Player1))
            .submit(Messages.SubmitBingoCardRes.class).toCompletableFuture().join();
        ensure(client1Card.getState().getStatus().equals("Running"));
        ensure(client1Card.getState().getPlayersCount() == 2);
        ensure(client1Card.getState().getPlayersList().stream()
            .allMatch(player -> player.getCardCount() == 9));

        List<Messages.BingoNumberDrawnNotify> drawnNumbers = new ArrayList<>();
        for (int drawSeq = 1; drawSeq <= 15; drawSeq++) {
            Messages.BingoNumberDrawnNotify client1Drawn =
                client1Draws.get(drawSeq - 1).toCompletableFuture().join().payload();
            Messages.BingoNumberDrawnNotify client2Drawn =
                client2Draws.get(drawSeq - 1).toCompletableFuture().join().payload();
            drawnNumbers.add(client1Drawn);
            ensure(client1Drawn.getDrawSeq() == drawSeq);
            ensure(client2Drawn.getDrawSeq() == drawSeq);
            ensure(client2Drawn.getNumber() == client1Drawn.getNumber());
            ensure(client2Drawn.getState().equals(client1Drawn.getState()));

            if (client1Drawn.getState().getStatus().equals("Finished")) {
                break;
            }
        }
        ensure(!drawnNumbers.isEmpty());
        ensure(drawnNumbers.getLast().getState().getStatus().equals("Finished"));

        Messages.BingoRoomState client1Result = client1Ended.toCompletableFuture().join().payload().getState();
        Messages.BingoRoomState client2Result = client2Ended.toCompletableFuture().join().payload().getState();
        ensure(client1Result.getStatus().equals("Finished"));
        ensure(client2Result.getStatus().equals("Finished"));
        ensure(client2Result.getDrawnNumbersList().equals(client1Result.getDrawnNumbersList()));
        ensure(client2Result.getWinnersList().equals(client1Result.getWinnersList()));
        ensure(client2Result.getPlayersList().stream().map(Messages.BingoPlayerState::getActorId).toList()
                .equals(client1Result.getPlayersList().stream().map(Messages.BingoPlayerState::getActorId).toList()));
        ensure(client1Result.getDrawnNumbersList().equals(drawnNumbers.stream()
                .map(Messages.BingoNumberDrawnNotify::getNumber)
                .toList()));
        ensure(client1Result.getWinnersList().equals(List.of(client1Auth.getActorId())));
        ensure(client1Result.getPlayersList().stream().allMatch(player -> player.getCardCount() == 9));
        ensure(client1Result.getPlayersList().stream().allMatch(player -> player.getMarks(4)));

        Messages.BingoRewardAnnouncedNotify reward = rewardAnnounced.toCompletableFuture().join().payload();
        ensure(reward.getActorId().equals(client1Auth.getActorId()));
        ensure(reward.getItemId().equals("rare-golden-dauber"));
        ensure(reward.getItemName().equals("Golden Dauber"));
        ensure(reward.getRarity().equals("Legendary"));

        Messages.StopObservingBingoEventsRes stopped = observer
            .request(BingoMessages.stopObservingBingoEventsReq(client1Match.getRoomId()))
            .submit(Messages.StopObservingBingoEventsRes.class).toCompletableFuture().join();
        ensure(stopped.getStopped());
        ensure(client1OwnJoinNotifications.get() == 0);
        ensure(client2OwnJoinNotifications.get() == 0);
    }

    private static void ensure(boolean condition) {
        if (!condition) {
            throw new IllegalStateException("Ensure failed");
        }
    }

    private static final class BingoClientCards {
        private static final List<Integer> Player1 = List.of(1, 2, 3, 4, 0, 6, 7, 8, 9);
        private static final List<Integer> Player2 = List.of(10, 11, 12, 13, 0, 14, 4, 5, 6);
    }
}
