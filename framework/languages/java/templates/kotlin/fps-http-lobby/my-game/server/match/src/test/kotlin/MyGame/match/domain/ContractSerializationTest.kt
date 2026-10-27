package MyGame.match.domain

import MyGame.*
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class ContractSerializationTest {
    private val serializer = ZLinkJsonMessageSerializer()

    @Test
    fun publicFieldsUseDotnetJsonNamesAndEnumValues() {
        val move = Json.mapper.readTree(
            serializer.serialize(MoveRequest(Vec3(1.0, 2.0, 3.0), 7)).bytes(),
        )
        assertEquals(1.0, move["delta"]["x"].asDouble())
        assertEquals(2.0, move["delta"]["y"].asDouble())
        assertEquals(3.0, move["delta"]["z"].asDouble())
        assertTrue(move["sequence"].isTextual)
        assertEquals("7", move["sequence"].asText())
        assertFalse(move.has("Delta"))
        assertFalse(move["delta"].has("X"))

        val room = Json.mapper.readTree(
            serializer.serialize(
                RoomView(
                    "room",
                    "matchA",
                    listOf(PlayerView("a", Vec3(48.0, 0.0, 18.0), 100, 4)),
                    MatchPhase.Playing,
                    null,
                ),
            ).bytes(),
        )
        assertTrue(room["phase"].isTextual)
        assertEquals("Playing", room["phase"].asText())
        assertEquals("a", room["players"][0]["playerId"].asText())
        assertTrue(room["players"][0]["lastSequence"].isTextual)
        assertEquals("4", room["players"][0]["lastSequence"].asText())
        assertTrue(room.has("winner") && room["winner"].isNull)
    }
    @Test
    fun defaultTypedSerializerMatchesGoldenJson() {
        val golden = listOf(
            """{"delta":{"x":1,"y":0,"z":-2},"sequence":"7"}""",
            """{"outcome":"hit","targetId":null,"sequence":"9007199254740991"}""",
            """{"roomId":"room-x","nodeId":"matchA","players":[{"playerId":"a","position":{"x":48,"y":0,"z":18},"hp":100,"lastSequence":"4"}],"phase":"Playing","winner":null}""",
            """{"reason":"move-budget","retryAfterMs":120}""",
        )
        val samples = listOf(
            MoveRequest(Vec3(1.0, 0.0, -2.0), 7),
            ShotResult("hit", null, Game.MAX_SEQUENCE),
            RoomView("room-x", "matchA", listOf(
                PlayerView("a", Vec3(48.0, 0.0, 18.0), 100, 4)), MatchPhase.Playing, null),
            ActionRejected("move-budget", 120),
        )
        val values = Comparator<com.fasterxml.jackson.databind.JsonNode> { left, right ->
            if (left.isNumber && right.isNumber) left.decimalValue().compareTo(right.decimalValue())
            else if (left == right) 0 else 1
        }
        for ((expected, sample) in golden.zip(samples)) {
            val actual = Json.mapper.readTree(serializer.serialize(sample).bytes())
            assertTrue(Json.mapper.readTree(expected).equals(values, actual), "Wire golden: ${sample.javaClass.simpleName}")
        }
        assertEquals(listOf("Waiting", "Playing", "Finished"), MatchPhase.entries.map { it.name })
        for (phase in MatchPhase.entries) {
            assertEquals("\"${phase.name}\"", String(serializer.serialize(phase).bytes(), Charsets.UTF_8))
        }
    }
}
