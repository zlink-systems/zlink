package MyGame.lobby.application

import MyGame.*
import java.util.concurrent.*
import org.junit.jupiter.api.Assertions.*
import org.junit.jupiter.api.Test

class MatchmakerTest {
    private val endpoint = "tcp://localhost:1"

    @Test
    fun bothResponsesWaitForPreparedRoom() {
        val prepared = CompletableFuture<MatchResponse>()
        Matchmaker { id ->
                assertEquals(Names.ROOM_PREFIX + "1", id)
                prepared
            }
            .use { matches ->
                val first =
                    matches.match(MatchRequest("a", Names.DEFAULT_MODE)).toCompletableFuture()
                assertFalse(first.isDone)
                assertThrows(IllegalArgumentException::class.java) {
                    matches.match(MatchRequest("a", Names.DEFAULT_MODE))
                }
                val second =
                    matches.match(MatchRequest("b", Names.DEFAULT_MODE)).toCompletableFuture()
                assertFalse(first.isDone)
                assertFalse(second.isDone)
                val assignment = MatchResponse(Names.ROOM_PREFIX + "1", endpoint)
                prepared.complete(assignment)
                assertEquals(assignment, first.join())
                assertEquals(assignment, second.join())
            }
    }

    @Test
    fun modesStaySeparateAndShutdownFailsWaiters() {
        val matches = Matchmaker { id ->
            CompletableFuture.completedFuture(MatchResponse(id, endpoint))
        }
        val a = matches.match(MatchRequest("a", "first")).toCompletableFuture()
        val b = matches.match(MatchRequest("b", "second")).toCompletableFuture()
        assertFalse(a.isDone)
        assertFalse(b.isDone)
        matches.close()
        assertTrue(a.isCompletedExceptionally)
        assertTrue(b.isCompletedExceptionally)
        assertThrows(IllegalStateException::class.java) {
            matches.match(MatchRequest("c", "first"))
        }
    }

    @Test
    fun preparationFailureReachesBothPlayers() {
        Matchmaker { CompletableFuture.failedFuture(IllegalStateException("Room rejected")) }
            .use { matches ->
                val first =
                    matches.match(MatchRequest("a", Names.DEFAULT_MODE)).toCompletableFuture()
                val second =
                    matches.match(MatchRequest("b", Names.DEFAULT_MODE)).toCompletableFuture()
                assertTrue(first.isCompletedExceptionally)
                assertTrue(second.isCompletedExceptionally)
                assertEquals(
                    "Room rejected",
                    assertThrows(CompletionException::class.java) { first.join() }.cause?.message,
                )
            }
    }

    @Test
    fun invalidRequestNeverEntersQueue() {
        Matchmaker { throw AssertionError("No pair expected") }
            .use { matches ->
                assertThrows(IllegalArgumentException::class.java) {
                    matches.match(MatchRequest("", Names.DEFAULT_MODE))
                }
                assertThrows(IllegalArgumentException::class.java) {
                    matches.match(MatchRequest("a", ""))
                }
            }
    }
}
