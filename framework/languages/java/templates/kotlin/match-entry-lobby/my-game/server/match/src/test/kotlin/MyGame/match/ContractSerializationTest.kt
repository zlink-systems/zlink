package MyGame.match

import MyGame.*
import com.fasterxml.jackson.databind.JsonNode
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer
import java.util.Comparator
import kotlin.test.Test
import kotlin.test.assertTrue

class ContractSerializationTest {
    @Test
    fun defaultTypedSerializerMatchesSharedGoldenJson() {
        val serializer = ZLinkJsonMessageSerializer()
        val golden = listOf(
            """{"playerId":"p1","mode":"duel"}""",
            """{"dx":1,"dy":0,"sequence":"7"}""",
            """{"targetId":"p2","sequence":"9007199254740993"}""",
            """{"x":41,"y":10,"sequence":"7"}""",
            """{"outcome":"hit","targetId":"p2","sequence":"9007199254740993"}""",
            """{"roomId":"room-x","nodeId":"matchA","phase":"Playing","winner":null,"players":[{"playerId":"p1","x":41,"y":10,"hp":75,"lastSequence":"9007199254740993"}]}""",
            """{"reason":"move-budget","retryAfterMs":120}""",
            """{"winner":"p1"}""",
        )
        val samples = listOf(
            QueueRequest("p1", "duel"),
            MoveRequest(1.0, 0.0, 7),
            ShootRequest("p2", 9_007_199_254_740_993L),
            MoveResult(41.0, 10.0, 7),
            ShotResult("hit", "p2", 9_007_199_254_740_993L),
            RoomView(
                "room-x",
                "matchA",
                Game.PLAYING_PHASE,
                null,
                listOf(PlayerView("p1", 41.0, 10.0, 75, 9_007_199_254_740_993L)),
            ),
            ActionRejected(Rejection.MOVE_BUDGET, 120),
            MatchEnded("p1"),
        )
        val values = Comparator<JsonNode> { left, right ->
            if (left.isNumber && right.isNumber) left.decimalValue().compareTo(right.decimalValue())
            else if (left == right) 0 else 1
        }
        for ((expected, sample) in golden.zip(samples)) {
            val actual = Json.mapper.readTree(serializer.serialize(sample).bytes())
            assertTrue(
                Json.mapper.readTree(expected).equals(values, actual),
                "Wire golden: ${sample.javaClass.simpleName}",
            )
        }
    }
}
