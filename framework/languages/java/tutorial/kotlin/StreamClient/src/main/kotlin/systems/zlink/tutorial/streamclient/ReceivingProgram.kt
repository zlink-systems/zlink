package systems.zlink.tutorial.streamclient

import java.net.URI
import java.time.Duration
import java.util.concurrent.CompletableFuture
import systems.zlink.framework.kotlin.await
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.stream.connector.*

private data class LeaderboardUpdate(val rank: Int = 0)

private data class Ready(val stage: String = "")

private data class MatchFound(val matchId: String = "")

private data class OrderChanged(val status: String = "")

private data class ReceivingStage(val stage: String)

suspend fun runReceiving(endpoint: String) {
    val connector =
        ZLinkStreamConnectorFactory.create(
                ZLinkStreamConnectorOptions(
                    URI.create(endpoint),
                    ZLinkStreamDispatchMode.MANUAL,
                    Duration.ofSeconds(30),
                    3,
                )
            )
            .kotlin()
    var handled = 0
    var frames = 0
    var running = true
    fun renderFrame() {
        frames++
        running = false
    }
    val subscription =
        connector.on<LeaderboardUpdate> {
            handled++
            CompletableFuture.completedFuture(null)
        }
    try {
        connector.connect().await()
        connector.send(ReceivingStage("pump")).await()
        connector.waitFor<Ready>().await()
        // --8<-- [start:receiving-pump]
        while (running) {
            connector.dispatch().await()
            renderFrame()
        }
        // --8<-- [end:receiving-pump]
        // --8<-- [start:receiving-unsubscribe]
        subscription.close()
        // --8<-- [end:receiving-unsubscribe]
        connector.send(ReceivingStage("unsubscribed")).await()
        connector.waitFor<Ready>().await()
        connector.dispatch().await()
        connector.send(ReceivingStage("match")).await()
        // --8<-- [start:receiving-wait]
        val found =
            connector
                .waitFor<MatchFound>()
                .where { it.payload.matchId == "match-7f3a" }
                .timeout(Duration.ofSeconds(30))
                .await()
        // --8<-- [end:receiving-wait]
        // --8<-- [start:receiving-sequence]
        connector.expectNone<OrderChanged>().within(Duration.ofMillis(100)).await()
        connector.send(ReceivingStage("orders")).await()
        val steps =
            connector
                .waitForSequence<OrderChanged>()
                .expect { it.payload.status == "paid" }
                .expect { it.payload.status == "shipped" }
                .timeout(Duration.ofSeconds(2))
                .await()
        // --8<-- [end:receiving-sequence]
        // --8<-- [start:receiving-count]
        val count = connector.receivedCount("LeaderboardUpdate")
        // --8<-- [end:receiving-count]
        check(
            handled == 1 &&
                frames == 1 &&
                count == 2 &&
                found.payload.matchId == "match-7f3a" &&
                steps.size == 2
        )
        println(
            "receiving: handler=$handled, frames=$frames, match=${found.payload.matchId}, sequence=${steps.joinToString(",") { it.payload.status }}, count=$count"
        )
    } finally {
        subscription.close()
        connector.close().await()
    }
}
