package systems.zlink.samples.kotlin.bingo.client

import java.time.Duration
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import systems.zlink.framework.kotlin.await
import systems.zlink.framework.kotlin.awaitReply
import systems.zlink.framework.kotlin.ZLinkKotlinStreamConnector
import systems.zlink.samples.kotlin.bingo.client.configuration.SampleNames
import systems.zlink.samples.kotlin.bingo.shared.contracts.AuthenticateReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.AuthenticateRes
import systems.zlink.samples.kotlin.bingo.shared.contracts.BingoGameEndedNotify
import systems.zlink.samples.kotlin.bingo.shared.contracts.BingoGameStartedNotify
import systems.zlink.samples.kotlin.bingo.shared.contracts.BingoNumberDrawnNotify
import systems.zlink.samples.kotlin.bingo.shared.contracts.BingoPlayerState
import systems.zlink.samples.kotlin.bingo.shared.contracts.BingoRewardAnnouncedNotify
import systems.zlink.samples.kotlin.bingo.shared.contracts.MatchBingoReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.MatchBingoRes
import systems.zlink.samples.kotlin.bingo.shared.contracts.ObserveBingoEventsReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.ObserveBingoEventsRes
import systems.zlink.samples.kotlin.bingo.shared.contracts.PlayerJoinedNotify
import systems.zlink.samples.kotlin.bingo.shared.contracts.StopObservingBingoEventsReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.StopObservingBingoEventsRes
import systems.zlink.samples.kotlin.bingo.shared.contracts.SubmitBingoCardReq
import systems.zlink.samples.kotlin.bingo.shared.contracts.SubmitBingoCardRes
import systems.zlink.samples.kotlin.bingo.shared.contracts.card
import systems.zlink.samples.kotlin.bingo.shared.contracts.drawnNumbers
import systems.zlink.samples.kotlin.bingo.shared.contracts.marks
import systems.zlink.samples.kotlin.bingo.shared.contracts.players
import systems.zlink.samples.kotlin.bingo.shared.contracts.winners

class BingoClientScenario {
    suspend fun run(
        client1: ZLinkKotlinStreamConnector,
        client2: ZLinkKotlinStreamConnector,
        observer: ZLinkKotlinStreamConnector,
    ) = coroutineScope {
        client1.connect().await()
        client2.connect().await()
        observer.connect().await()
        val client1Auth = client1.request(AuthenticateReq("player-1")).awaitReply<AuthenticateRes>()
        ensure(client1Auth.actorId == "player-1")

        val client1NoSelfJoin = async(start = CoroutineStart.UNDISPATCHED) {
            client1.expectNone<PlayerJoinedNotify>(SampleNames.PlayerJoinedPacket)
                .within(Duration.ofMillis(400))
                .await()
        }
        val client1MatchWait = client1.waitFor<MatchBingoRes>()
            .let { wait -> async(start = CoroutineStart.UNDISPATCHED) { wait.await() } }
        client1.send(MatchBingoReq("two-player")).await()
        val client1Match = client1MatchWait.await().payload()
        ensure(client1Match.state.status == "WaitingForPlayers")
        ensure(client1Match.state.hostActorId == client1Auth.actorId)
        client1NoSelfJoin.await()

        val observerAuth = observer.request(AuthenticateReq("observer")).awaitReply<AuthenticateRes>()
        ensure(observerAuth.actorId == "observer")
        val observed = observer
            .request(ObserveBingoEventsReq(client1Match.roomId))
            .awaitReply<ObserveBingoEventsRes>()
        ensure(observed.subscribed)

        val client1SawClient2Join = client1.waitFor<PlayerJoinedNotify>()
            .where { message -> message.payload().actorId == "player-2" }
            .let { wait -> async { wait.await() } }
        val client1Started = async { client1.waitFor<BingoGameStartedNotify>().await() }

        val client2Auth = client2.request(AuthenticateReq("player-2")).awaitReply<AuthenticateRes>()
        ensure(client2Auth.actorId == "player-2")
        ensure(client2Auth.actorId != client1Auth.actorId)

        val client2NoSelfJoin = async(start = CoroutineStart.UNDISPATCHED) {
            client2.expectNone<PlayerJoinedNotify>(SampleNames.PlayerJoinedPacket)
                .within(Duration.ofMillis(400))
                .await()
        }
        val client2MatchWait = client2.waitFor<MatchBingoRes>()
            .let { wait -> async(start = CoroutineStart.UNDISPATCHED) { wait.await() } }
        client2.send(MatchBingoReq("two-player")).await()
        val client2Match = client2MatchWait.await().payload()
        ensure(client2Match.roomId == client1Match.roomId)
        ensure(client2Match.state.status == "Running")

        val join = client1SawClient2Join.await().payload()
        ensure(join.actorId == client2Auth.actorId)
        ensure(join.roomId == client1Match.roomId)
        ensure(join.state.players.map { player -> player.actorId }.toSet() ==
            setOf(client1Auth.actorId, client2Auth.actorId))
        client2NoSelfJoin.await()
        ensure(client1Started.await().payload().state.status == "Running")

        val client2Card = client2.request(SubmitBingoCardReq(client2Match.roomId, BingoClientCards.Player2)).awaitReply<SubmitBingoCardRes>()
        ensure(client2Card.state.status == "Running")
        ensure(client2Card.state.players
            .single { player -> player.actorId == client2Auth.actorId }
            .card.size == 9)

        val rewardAnnounced = observer.waitFor<BingoRewardAnnouncedNotify>()
            .where { message -> message.payload().roomId == client1Match.roomId }
            .let { wait -> async { wait.await() } }
        val client1Ended = async { client1.waitFor<BingoGameEndedNotify>().await() }
        val client2Ended = async { client2.waitFor<BingoGameEndedNotify>().await() }

        val client1Draws = (1..15).map { expectedDrawSeq ->
            async {
                client1.waitFor<BingoNumberDrawnNotify>()
                    .where { message -> message.payload().drawSeq == expectedDrawSeq }
                    .await()
            }
        }
        val client2Draws = (1..15).map { expectedDrawSeq ->
            async {
                client2.waitFor<BingoNumberDrawnNotify>()
                    .where { message -> message.payload().drawSeq == expectedDrawSeq }
                    .await()
            }
        }

        val client1Card = client1.request(SubmitBingoCardReq(client1Match.roomId, BingoClientCards.Player1)).awaitReply<SubmitBingoCardRes>()
        ensure(client1Card.state.status == "Running")
        ensure(client1Card.state.players.size == 2)
        ensure(client1Card.state.players.all { player -> player.card.size == 9 })

        val drawnNumbers = mutableListOf<BingoNumberDrawnNotify>()
        for (drawSeq in 1..15) {
            val client1Drawn = client1Draws[drawSeq - 1].await().payload()
            val client2Drawn = client2Draws[drawSeq - 1].await().payload()
            drawnNumbers += client1Drawn
            ensure(client1Drawn.drawSeq == drawSeq)
            ensure(client2Drawn.drawSeq == drawSeq)
            ensure(client2Drawn.number == client1Drawn.number)
            ensure(client2Drawn.state == client1Drawn.state)

            if (client1Drawn.state.status == "Finished") {
                break
            }
        }
        ensure(drawnNumbers.isNotEmpty())
        ensure(drawnNumbers.last().state.status == "Finished")
        client1Draws.drop(drawnNumbers.size).forEach { wait -> wait.cancel() }
        client2Draws.drop(drawnNumbers.size).forEach { wait -> wait.cancel() }

        val client1Result = client1Ended.await().payload().state
        val client2Result = client2Ended.await().payload().state
        ensure(client1Result.status == "Finished")
        ensure(client2Result.status == "Finished")
        ensure(client2Result.drawnNumbers == client1Result.drawnNumbers)
        ensure(client2Result.winners == client1Result.winners)
        ensure(client2Result.players.map { player -> player.actorId } ==
            client1Result.players.map { player -> player.actorId })
        ensure(client1Result.drawnNumbers == drawnNumbers.map { notify -> notify.number })
        ensure(client1Result.winners == listOf(client1Auth.actorId))
        ensure(client1Result.players.all { player -> player.card.size == 9 })
        ensure(client1Result.players.all { player -> player.marks[4] })

        val reward = rewardAnnounced.await().payload()
        ensure(reward.roomId == client1Match.roomId)
        ensure(reward.actorId == client1Auth.actorId)
        ensure(reward.drawSeq == drawnNumbers.last().drawSeq)
        ensure(reward.itemId == "rare-golden-dauber")
        ensure(reward.itemName == "Golden Dauber")
        ensure(reward.rarity == "Legendary")

        val stopped = observer
            .request(StopObservingBingoEventsReq(client1Match.roomId))
            .awaitReply<StopObservingBingoEventsRes>()
        ensure(stopped.stopped)
        println("bingo=completed")
    }
}

private fun ensure(condition: Boolean) {
    if (!condition) {
        throw IllegalStateException("Ensure failed")
    }
}

private object BingoClientCards {
    val Player1: List<Int> = listOf(1, 2, 3, 4, 0, 6, 7, 8, 9)
    val Player2: List<Int> = listOf(10, 11, 12, 13, 0, 14, 4, 5, 6)
}
