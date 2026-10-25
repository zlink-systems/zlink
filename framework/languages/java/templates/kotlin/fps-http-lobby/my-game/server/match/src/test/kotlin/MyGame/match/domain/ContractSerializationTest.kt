package MyGame.match.domain

import MyGame.*
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class ContractSerializationTest {
    @Test
    fun publicFieldsUseDotnetJsonNamesAndEnumValues() {
        val move = Json.mapper.readTree(
            Json.mapper.writeValueAsBytes(MoveRequest(Vec3(1.0, 2.0, 3.0), 7)),
        )
        assertEquals(1.0, move["delta"]["x"].asDouble())
        assertEquals(2.0, move["delta"]["y"].asDouble())
        assertEquals(3.0, move["delta"]["z"].asDouble())
        assertEquals(7L, move["sequence"].asLong())
        assertFalse(move.has("Delta"))
        assertFalse(move["delta"].has("X"))

        val room = Json.mapper.readTree(
            Json.mapper.writeValueAsBytes(
                RoomView(
                    "room",
                    "matchA",
                    listOf(PlayerView("a", Vec3(48.0, 0.0, 18.0), 100, 4)),
                    MatchPhase.Playing,
                    null,
                ),
            ),
        )
        assertTrue(room["phase"].isTextual)
        assertEquals("Playing", room["phase"].asText())
        assertEquals("a", room["players"][0]["playerId"].asText())
        assertEquals(4L, room["players"][0]["lastSequence"].asLong())
        assertTrue(room.has("winner") && room["winner"].isNull)
    }
}
