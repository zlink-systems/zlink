package systems.zlink.samples.tictactoe.client;

import java.net.URI;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.atomic.AtomicInteger;
import systems.zlink.httpclient.ZLinkHttpClient;
import systems.zlink.samples.tictactoe.shared.contracts.AuthenticateReq;
import systems.zlink.samples.tictactoe.shared.contracts.AuthenticateRes;
import systems.zlink.samples.tictactoe.shared.contracts.CreateGameHttpReq;
import systems.zlink.samples.tictactoe.shared.contracts.CreateGameHttpRes;
import systems.zlink.samples.tictactoe.shared.contracts.GameStateNotify;
import systems.zlink.samples.tictactoe.shared.contracts.JoinGameReq;
import systems.zlink.samples.tictactoe.shared.contracts.JoinGameRes;
import systems.zlink.samples.tictactoe.shared.contracts.LeaveGameReq;
import systems.zlink.samples.tictactoe.shared.contracts.ObserveMilestoneReq;
import systems.zlink.samples.tictactoe.shared.contracts.ObserveMilestoneRes;
import systems.zlink.samples.tictactoe.shared.contracts.PlaceMarkReq;
import systems.zlink.samples.tictactoe.shared.contracts.PlaceMarkRes;
import systems.zlink.samples.tictactoe.shared.contracts.PlayerJoinedNotify;
import systems.zlink.samples.tictactoe.shared.contracts.WinMilestoneNotify;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory;
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions;
import systems.zlink.stream.connector.ZLinkStreamDispatchMode;
import systems.zlink.stream.connector.ZLinkStreamMessageKind;

public final class TicTacToeClientScenario {
    public void run(TicTacToeClientOptions options) throws Exception {
        CreateGameHttpRes game;
        try (ZLinkHttpClient api = ZLinkHttpClient.create(options.apiUrl()).build()) {
            game = api.post("/games")
                .body(new CreateGameHttpReq(options.gameName()))
                .submit(CreateGameHttpRes.class)
                .toCompletableFuture().join()
                .body();
        }
        ensure(game.playEndpoints().size() >= 2);
        ZLinkStreamConnector host = playerConnector(
            game.playEndpoints().get(0), "host");
        ZLinkStreamConnector guest = playerConnector(
            game.playEndpoints().get(1), "guest");
        ZLinkStreamConnector observer = playerConnector(
            game.playEndpoints().get(1), "observer");

        try {
            host.connect().submit().toCompletableFuture().join();
            guest.connect().submit().toCompletableFuture().join();
            observer.connect().submit().toCompletableFuture().join();

            ensure(game.roomId() != null && !game.roomId().isBlank());
            ensure(options.gameName().equals(game.gameName()));
            ensure(game.requiredLevel() == 3);

            AuthenticateRes hostAuth = host
                .request(new AuthenticateReq(options.xActorId()))
                .submit(AuthenticateRes.class).toCompletableFuture().join();
            ensure(options.xActorId().equals(hostAuth.player().actorId()));
            ensure(hostAuth.player().wins() == 99);

            AuthenticateRes guestAuth = guest
                .request(new AuthenticateReq(options.oActorId()))
                .submit(AuthenticateRes.class).toCompletableFuture().join();
            ensure(options.oActorId().equals(guestAuth.player().actorId()));
            ensure(!guestAuth.player().actorId().equals(hostAuth.player().actorId()));

            AuthenticateRes observerAuth = observer
                .request(new AuthenticateReq(options.observerActorId()))
                .submit(AuthenticateRes.class).toCompletableFuture().join();
            ensure(options.observerActorId().equals(observerAuth.player().actorId()));
            ObserveMilestoneRes subscribed = observer
                .request(new ObserveMilestoneReq())
                .submit(ObserveMilestoneRes.class).toCompletableFuture().join();
            ensure(subscribed.subscribed());
            System.out.println(
                "observer-connected endpoint=" + game.playEndpoints().get(1));
            System.out.println("observer-subscription=verified subscribed=" + subscribed.subscribed());

            AtomicInteger hostOwnJoinNotifications = new AtomicInteger();
            host.on(PlayerJoinedNotify.class, message -> {
                if (options.xActorId().equals(message.payload().actorId())) {
                    hostOwnJoinNotifications.incrementAndGet();
                }
                return CompletableFuture.completedFuture(null);
            });
            AtomicInteger guestOwnJoinNotifications = new AtomicInteger();
            guest.on(PlayerJoinedNotify.class, message -> {
                if (options.oActorId().equals(message.payload().actorId())) {
                    guestOwnJoinNotifications.incrementAndGet();
                }
                return CompletableFuture.completedFuture(null);
            });

            var hostJoinCompletion = host.waitFor(JoinGameRes.class)
                .submit(JoinGameRes.class);
            host.send(new JoinGameReq(game.roomId())).submit()
                .toCompletableFuture().join();
            JoinGameRes hostJoin = hostJoinCompletion
                .toCompletableFuture().join().payload();
            ensure(hostJoin.state().roomId().equals(game.roomId()));
            ensure("WaitingForPlayers".equals(hostJoin.state().status()));
            ensure(options.xActorId().equals(hostJoin.state().xActorId()));

            var hostSawGuestJoin = host
                .waitFor(PlayerJoinedNotify.class)
                .where(PlayerJoinedNotify.class,
                    message -> options.oActorId().equals(message.payload().actorId()))
                .submit(PlayerJoinedNotify.class);
            var hostSawGameStart = host
                .waitFor(GameStateNotify.class)
                .where(GameStateNotify.class,
                    message -> "InProgress".equals(message.payload().state().status())
                        && options.oActorId().equals(message.payload().state().oActorId()))
                .submit(GameStateNotify.class);

            var guestJoinCompletion = guest.waitFor(JoinGameRes.class)
                .submit(JoinGameRes.class);
            guest.send(new JoinGameReq(game.roomId())).submit()
                .toCompletableFuture().join();
            JoinGameRes guestJoin = guestJoinCompletion
                .toCompletableFuture().join().payload();
            ensure(guestJoin.state().roomId().equals(game.roomId()));
            ensure("InProgress".equals(guestJoin.state().status()));
            ensure(options.oActorId().equals(guestJoin.state().oActorId()));

            PlayerJoinedNotify guestJoinNotify = hostSawGuestJoin.toCompletableFuture().join().payload();
            ensure("O".equals(guestJoinNotify.mark()));
            ensure("InProgress".equals(guestJoinNotify.state().status()));

            GameStateNotify gameStart = hostSawGameStart.toCompletableFuture().join().payload();
            ensure("X".equals(gameStart.state().nextTurn()));

            var guestSawHostMove1 = guest
                .waitFor(GameStateNotify.class)
                .where(GameStateNotify.class,
                    message -> options.xActorId().equals(message.payload().state().lastMoveActorId())
                        && Integer.valueOf(0).equals(message.payload().state().lastMoveCell()))
                .submit(GameStateNotify.class);
            PlaceMarkRes hostMove1 = host
                .request(new PlaceMarkReq(0))
                .submit(PlaceMarkRes.class).toCompletableFuture().join();
            ensure("X........".equals(hostMove1.state().board()));
            ensure("O".equals(hostMove1.state().nextTurn()));
            ensure(options.xActorId().equals(hostMove1.state().lastMoveActorId()));
            ensure(Integer.valueOf(0).equals(hostMove1.state().lastMoveCell()));

            GameStateNotify hostMove1Notify = guestSawHostMove1.toCompletableFuture().join().payload();
            ensure(hostMove1Notify.state().board().equals(hostMove1.state().board()));
            ensure("O".equals(hostMove1Notify.state().nextTurn()));
            ensure(options.xActorId().equals(hostMove1Notify.state().lastMoveActorId()));
            ensure(Integer.valueOf(0).equals(hostMove1Notify.state().lastMoveCell()));

            guest.send(new LeaveGameReq(game.roomId()))
                .submit().toCompletableFuture().join();
            var hostSawGuestMove1 = host
                .waitFor(GameStateNotify.class)
                .where(GameStateNotify.class,
                    message -> options.oActorId().equals(message.payload().state().lastMoveActorId())
                        && Integer.valueOf(3).equals(message.payload().state().lastMoveCell()))
                .submit(GameStateNotify.class);
            PlaceMarkRes guestMove1 = guest
                .request(new PlaceMarkReq(3))
                .submit(PlaceMarkRes.class).toCompletableFuture().join();
            ensure("X..O.....".equals(guestMove1.state().board()));
            ensure("X".equals(guestMove1.state().nextTurn()));
            ensure(options.oActorId().equals(guestMove1.state().lastMoveActorId()));
            ensure(Integer.valueOf(3).equals(guestMove1.state().lastMoveCell()));

            GameStateNotify guestMove1Notify = hostSawGuestMove1.toCompletableFuture().join().payload();
            ensure(guestMove1Notify.state().board().equals(guestMove1.state().board()));
            ensure("X".equals(guestMove1Notify.state().nextTurn()));
            ensure(options.oActorId().equals(guestMove1Notify.state().lastMoveActorId()));
            ensure(Integer.valueOf(3).equals(guestMove1Notify.state().lastMoveCell()));

            var guestSawHostMove2 = guest
                .waitFor(GameStateNotify.class)
                .where(GameStateNotify.class,
                    message -> options.xActorId().equals(message.payload().state().lastMoveActorId())
                        && Integer.valueOf(1).equals(message.payload().state().lastMoveCell()))
                .submit(GameStateNotify.class);
            PlaceMarkRes hostMove2 = host
                .request(new PlaceMarkReq(1))
                .submit(PlaceMarkRes.class).toCompletableFuture().join();
            ensure("XX.O.....".equals(hostMove2.state().board()));
            ensure("O".equals(hostMove2.state().nextTurn()));
            ensure(options.xActorId().equals(hostMove2.state().lastMoveActorId()));
            ensure(Integer.valueOf(1).equals(hostMove2.state().lastMoveCell()));

            GameStateNotify hostMove2Notify = guestSawHostMove2.toCompletableFuture().join().payload();
            ensure(hostMove2Notify.state().board().equals(hostMove2.state().board()));
            ensure("O".equals(hostMove2Notify.state().nextTurn()));
            ensure(options.xActorId().equals(hostMove2Notify.state().lastMoveActorId()));
            ensure(Integer.valueOf(1).equals(hostMove2Notify.state().lastMoveCell()));

            var hostSawGuestMove2 = host
                .waitFor(GameStateNotify.class)
                .where(GameStateNotify.class,
                    message -> options.oActorId().equals(message.payload().state().lastMoveActorId())
                        && Integer.valueOf(4).equals(message.payload().state().lastMoveCell()))
                .submit(GameStateNotify.class);
            PlaceMarkRes guestMove2 = guest
                .request(new PlaceMarkReq(4))
                .submit(PlaceMarkRes.class).toCompletableFuture().join();
            ensure("XX.OO....".equals(guestMove2.state().board()));
            ensure("X".equals(guestMove2.state().nextTurn()));
            ensure(options.oActorId().equals(guestMove2.state().lastMoveActorId()));
            ensure(Integer.valueOf(4).equals(guestMove2.state().lastMoveCell()));

            GameStateNotify guestMove2Notify = hostSawGuestMove2.toCompletableFuture().join().payload();
            ensure(guestMove2Notify.state().board().equals(guestMove2.state().board()));
            ensure("X".equals(guestMove2Notify.state().nextTurn()));
            ensure(options.oActorId().equals(guestMove2Notify.state().lastMoveActorId()));
            ensure(Integer.valueOf(4).equals(guestMove2Notify.state().lastMoveCell()));

            var guestSawHostWin = guest
                .waitFor(GameStateNotify.class)
                .where(GameStateNotify.class,
                    message -> "Won".equals(message.payload().state().status())
                        && options.xActorId().equals(message.payload().state().winner()))
                .submit(GameStateNotify.class);
            var observerSawMilestone = observer
                .waitFor(WinMilestoneNotify.class)
                .where(WinMilestoneNotify.class,
                    message -> options.xActorId().equals(message.payload().actorId())
                        && message.payload().wins() == 100)
                .submit(WinMilestoneNotify.class);
            PlaceMarkRes hostWin = host
                .request(new PlaceMarkReq(2))
                .submit(PlaceMarkRes.class).toCompletableFuture().join();
            ensure("XXXOO....".equals(hostWin.state().board()));
            ensure("Won".equals(hostWin.state().status()));
            ensure(options.xActorId().equals(hostWin.state().winner()));

            GameStateNotify hostWinNotify = guestSawHostWin.toCompletableFuture().join().payload();
            ensure(hostWinNotify.state().board().equals(hostWin.state().board()));
            ensure("Won".equals(hostWinNotify.state().status()));
            ensure(options.xActorId().equals(hostWinNotify.state().winner()));

            WinMilestoneNotify milestone = observerSawMilestone.toCompletableFuture().join().payload();
            ensure(milestone.wins() == 100);
            ensure(hostOwnJoinNotifications.get() == 0);
            ensure(guestOwnJoinNotifications.get() == 0);
            System.out.println("observer-win-milestone=verified actor="
                + milestone.actorId()
                + " wins=" + milestone.wins());

            java.util.concurrent.CompletableFuture.allOf(
                host.send(new LeaveGameReq(game.roomId()))
                    .submit().toCompletableFuture(),
                guest.send(new LeaveGameReq(game.roomId()))
                    .submit().toCompletableFuture())
                .join();
            System.out.println("tictactoe completed");
        } finally {
            host.close().submit().toCompletableFuture().join();
            guest.close().submit().toCompletableFuture().join();
            observer.close().submit().toCompletableFuture().join();
        }
    }

    private static ZLinkStreamConnector playerConnector(String endpoint, String role) {
        ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(new ZLinkStreamConnectorOptions(
            URI.create(endpoint),
            ZLinkStreamDispatchMode.IMMEDIATE,
            TicTacToeSampleDefaults.RequestTimeout,
            2,
            Duration.ofSeconds(5),
            64 * 1024,
            false,
            Duration.ofSeconds(1),
            TicTacToeSampleDefaults.RequestTimeout.plusSeconds(5),
            true,
            Duration.ofMillis(250),
            Duration.ofSeconds(5),
            2.0,
            null));
        connector.observeInbound(observation -> {
            if (observation.kind() != ZLinkStreamMessageKind.CONTROL) {
                System.out.println(
                    "stream-inbound sample=TicTacToe role=" + role
                        + " kind=" + observation.kind()
                        + " name=" + observation.packetName()
                        + " seq=" + (observation.requestSeq() == null ? "-" : observation.requestSeq())
                        + " bytes=" + observation.payloadLength());
            }
            return CompletableFuture.completedFuture(null);
        });
        return connector;
    }

    private static void ensure(boolean condition) {
        if (!condition) {
            throw new IllegalStateException("Ensure failed");
        }
    }
}
