package systems.zlink.samples.kotlin.tictactoe.client

import java.net.URI
import java.time.Duration
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import systems.zlink.stream.connector.ZLinkStreamJson
import systems.zlink.framework.kotlin.await
import systems.zlink.framework.kotlin.awaitReply
import systems.zlink.framework.kotlin.ZLinkKotlinStreamConnector
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.httpclient.kotlin.fetch
import systems.zlink.httpclient.kotlin.zlinkHttpClient
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.AuthenticateReq
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.AuthenticateRes
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.CreateGameHttpReq
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.CreateGameHttpRes
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.GameStateNotify
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.JoinGameReq
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.JoinGameRes
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.LeaveGameReq
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.ObserveMilestoneReq
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.ObserveMilestoneRes
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.PlaceMarkReq
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.PlaceMarkRes
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.PlayerJoinedNotify
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.WinMilestoneNotify
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions
import systems.zlink.stream.connector.ZLinkStreamDispatchMode

class TicTacToeClientScenario {
    suspend fun run(options: TicTacToeClientOptions) = coroutineScope {
        val game = zlinkHttpClient(options.apiUrl).use { api ->
            api.post("/games")
                .body(CreateGameHttpReq(options.gameName))
                .fetch<CreateGameHttpRes>()
        }
        ensure(game.playEndpoints.size >= 2)
        val hostStream = playerConnector(game.playEndpoints[0])
        val guestStream = playerConnector(game.playEndpoints[1])
        val observerStream = playerConnector(game.playEndpoints[1])

        try {
            hostStream.connect().await()
            guestStream.connect().await()
            observerStream.connect().await()

            ensure(game.roomId.isNotBlank())
            ensure(game.gameName == options.gameName)
            ensure(game.requiredLevel == 3)
            ensure(game.playEndpoints.size >= 2)
            ensure(game.playEndpoints.distinct().size == game.playEndpoints.size)
            ensure(game.playNodes.size == game.playEndpoints.size)
            ensure(game.playNodes.map { it.streamEndpoint }.toSet() == game.playEndpoints.toSet())

            val xAuthentication = hostStream.request(AuthenticateReq(options.xActorId)).awaitReply<AuthenticateRes>()
            ensure(xAuthentication.player.actorId == options.xActorId)
            ensure(xAuthentication.player.displayName.isNotBlank())
            ensure(xAuthentication.player.level >= game.requiredLevel)
            ensure(xAuthentication.player.wins == 99)

            val oAuthentication = guestStream.request(AuthenticateReq(options.oActorId)).awaitReply<AuthenticateRes>()
            ensure(oAuthentication.player.actorId == options.oActorId)
            ensure(oAuthentication.player.actorId != xAuthentication.player.actorId)
            ensure(oAuthentication.player.displayName.isNotBlank())
            ensure(oAuthentication.player.level >= game.requiredLevel)

            val observerAuthentication = observerStream.request(AuthenticateReq(options.observerActorId)).awaitReply<AuthenticateRes>()
            ensure(observerAuthentication.player.actorId == options.observerActorId)
            val subscription = observerStream.request(ObserveMilestoneReq()).awaitReply<ObserveMilestoneRes>()
            ensure(subscription.subscribed)
            println("observer-connected endpoint=${game.playEndpoints[1]}")
            println("observer-subscription=verified subscribed=${subscription.subscribed}")

            val hostNoSelfJoin = async(start = CoroutineStart.UNDISPATCHED) {
                hostStream.expectNone<PlayerJoinedNotify>("PlayerJoinedNotify")
                    .within(Duration.ofMillis(400))
                    .await()
            }
            val xJoinWait = hostStream.waitFor<JoinGameRes>()
                .where { message -> message.payload().state.roomId == game.roomId }
                .let { wait -> async(start = CoroutineStart.UNDISPATCHED) { wait.await() } }
            hostStream.send(JoinGameReq(game.roomId)).await()
            val xJoin = xJoinWait.await().payload()
            ensure(xJoin.state.roomId == game.roomId)
            ensure(xJoin.state.status == "WaitingForPlayers")
            ensure(xJoin.state.xActorId == options.xActorId)
            hostNoSelfJoin.await()

            val hostSawGuestJoin = hostStream.waitFor<PlayerJoinedNotify>()
                .where { message -> message.payload().actorId == options.oActorId }
                .let { wait -> async { wait.await() } }
            val hostSawGameStart = hostStream.waitFor<GameStateNotify>()
                .where { message -> message.payload().state.status == "InProgress" }
                .let { wait -> async { wait.await() } }

            val guestNoSelfJoin = async(start = CoroutineStart.UNDISPATCHED) {
                guestStream.expectNone<PlayerJoinedNotify>("PlayerJoinedNotify")
                    .within(Duration.ofMillis(400))
                    .await()
            }
            val oJoinWait = guestStream.waitFor<JoinGameRes>()
                .where { message -> message.payload().state.roomId == game.roomId }
                .let { wait -> async(start = CoroutineStart.UNDISPATCHED) { wait.await() } }
            guestStream.send(JoinGameReq(game.roomId)).await()
            val oJoin = oJoinWait.await().payload()
            ensure(oJoin.state.roomId == game.roomId)
            ensure(oJoin.state.status == "InProgress")
            ensure(oJoin.state.oActorId == options.oActorId)

            val guestJoinNotify = hostSawGuestJoin.await().payload()
            ensure(guestJoinNotify.actorId == options.oActorId)
            ensure(guestJoinNotify.displayName == oAuthentication.player.displayName)
            ensure(guestJoinNotify.level == oAuthentication.player.level)
            ensure(guestJoinNotify.mark == "O")
            ensure(guestJoinNotify.roomId == game.roomId)
            ensure(guestJoinNotify.state.status == "InProgress")
            guestNoSelfJoin.await()

            val gameStarted = hostSawGameStart.await().payload()
            ensure(gameStarted.state.nextTurn == "X")

            val guestSawHostMove1 = guestStream.waitFor<GameStateNotify>()
                .where { message -> message.payload().state.lastMoveCell == 0 }
                .let { wait -> async { wait.await() } }
            val hostMove1 = hostStream.request(PlaceMarkReq(0)).awaitReply<PlaceMarkRes>()
            ensure(hostMove1.state.board == "X........")
            ensure(hostMove1.state.nextTurn == "O")
            ensure(hostMove1.state.lastMoveActorId == options.xActorId)
            ensure(hostMove1.state.lastMoveCell == 0)

            val hostMove1Notify = guestSawHostMove1.await().payload()
            ensure(hostMove1Notify.state.board == hostMove1.state.board)
            ensure(hostMove1Notify.state.nextTurn == "O")
            ensure(hostMove1Notify.state.lastMoveActorId == options.xActorId)
            ensure(hostMove1Notify.state.lastMoveCell == 0)

            // The common scenario checks that a leave request during an active
            // game is ignored before the guest takes the next turn.
            guestStream.send(LeaveGameReq(game.roomId)).await()

            val hostSawGuestMove1 = hostStream.waitFor<GameStateNotify>()
                .where { message -> message.payload().state.lastMoveCell == 3 }
                .let { wait -> async { wait.await() } }

            val guestMove1 = guestStream.request(PlaceMarkReq(3)).awaitReply<PlaceMarkRes>()
            ensure(guestMove1.state.board == "X..O.....")
            ensure(guestMove1.state.nextTurn == "X")
            ensure(guestMove1.state.lastMoveActorId == options.oActorId)
            ensure(guestMove1.state.lastMoveCell == 3)

            val guestMove1Notify = hostSawGuestMove1.await().payload()
            ensure(guestMove1Notify.state.board == guestMove1.state.board)
            ensure(guestMove1Notify.state.nextTurn == "X")
            ensure(guestMove1Notify.state.lastMoveActorId == options.oActorId)
            ensure(guestMove1Notify.state.lastMoveCell == 3)

            val guestSawHostMove2 = guestStream.waitFor<GameStateNotify>()
                .where { message -> message.payload().state.lastMoveCell == 1 }
                .let { wait -> async { wait.await() } }
            val hostMove2 = hostStream.request(PlaceMarkReq(1)).awaitReply<PlaceMarkRes>()
            ensure(hostMove2.state.board == "XX.O.....")
            ensure(hostMove2.state.nextTurn == "O")
            ensure(hostMove2.state.lastMoveActorId == options.xActorId)
            ensure(hostMove2.state.lastMoveCell == 1)

            val hostMove2Notify = guestSawHostMove2.await().payload()
            ensure(hostMove2Notify.state.board == hostMove2.state.board)
            ensure(hostMove2Notify.state.nextTurn == "O")
            ensure(hostMove2Notify.state.lastMoveActorId == options.xActorId)
            ensure(hostMove2Notify.state.lastMoveCell == 1)

            val hostSawGuestMove2 = hostStream.waitFor<GameStateNotify>()
                .where { message -> message.payload().state.lastMoveCell == 4 }
                .let { wait -> async { wait.await() } }
            val guestMove2 = guestStream.request(PlaceMarkReq(4)).awaitReply<PlaceMarkRes>()
            ensure(guestMove2.state.board == "XX.OO....")
            ensure(guestMove2.state.nextTurn == "X")
            ensure(guestMove2.state.lastMoveActorId == options.oActorId)
            ensure(guestMove2.state.lastMoveCell == 4)

            val guestMove2Notify = hostSawGuestMove2.await().payload()
            ensure(guestMove2Notify.state.board == guestMove2.state.board)
            ensure(guestMove2Notify.state.nextTurn == "X")
            ensure(guestMove2Notify.state.lastMoveActorId == options.oActorId)
            ensure(guestMove2Notify.state.lastMoveCell == 4)

            val guestSawHostWin = guestStream.waitFor<GameStateNotify>()
                .where { message -> message.payload().state.status == "Won" }
                .let { wait -> async { wait.await() } }
            val observerSawMilestone = observerStream.waitFor<WinMilestoneNotify>()
                .where { message -> message.payload().actorId == options.xActorId && message.payload().wins == 100 }
                .let { wait -> async { wait.await() } }
            val hostWin = hostStream.request(PlaceMarkReq(2)).awaitReply<PlaceMarkRes>()
            ensure(hostWin.state.board == "XXXOO....")
            ensure(hostWin.state.status == "Won")
            ensure(hostWin.state.winner == options.xActorId)

            val hostWinNotify = guestSawHostWin.await().payload()
            ensure(hostWinNotify.state.board == hostWin.state.board)
            ensure(hostWinNotify.state.status == "Won")
            ensure(hostWinNotify.state.winner == options.xActorId)

            val milestone = observerSawMilestone.await().payload()
            ensure(milestone.actorId == xAuthentication.player.actorId)
            ensure(milestone.displayName == xAuthentication.player.displayName)
            ensure(milestone.wins == 100)
            ensure(milestone.roomId == game.roomId)
            println(
                "observer-win-milestone=verified actor=${milestone.actorId} " +
                    "wins=${milestone.wins}",
            )

            hostStream.send(LeaveGameReq(game.roomId)).await()
            guestStream.send(LeaveGameReq(game.roomId)).await()
            println("tictactoe completed")
        } finally {
            hostStream.close().await()
            guestStream.close().await()
            observerStream.close().await()
        }
    }

    private fun playerConnector(endpoint: String): ZLinkKotlinStreamConnector =
        ZLinkStreamConnectorFactory.create(
            ZLinkStreamConnectorOptions(
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
                ZLinkStreamJson.codec(),
            ),
        ).kotlin()
}

private fun ensure(condition: Boolean) {
    if (!condition) {
        throw IllegalStateException("Ensure failed")
    }
}
