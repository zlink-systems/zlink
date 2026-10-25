package MyGame.lobby.application

import MyGame.*
import java.net.URI
import java.net.Socket
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionException
import java.util.concurrent.CompletionStage
import org.junit.jupiter.api.Assertions.*
import org.junit.jupiter.api.Test

class MatchmakerTest {
    private val httpTimeoutMillis = 5000
    private val endpoint = "tcp://localhost:1"

    private fun matchmaker(
        provision: (String, List<String>) -> CompletionStage<MatchResponse>,
    ) = Matchmaker(RoomProvisioner(provision))

    @Test
    fun cancellationDoesNotChangePairedRequests() {
        val prepared = CompletableFuture<MatchResponse>()
        var roomId = ""
        matchmaker { id, members ->
            roomId = id
            assertEquals(setOf("a", "b"), members.toSet())
            prepared
        }.use { matches ->
            val request = MatchRequest("a")
            val first = matches.match(request).toCompletableFuture()
            val second = matches.match(MatchRequest("b")).toCompletableFuture()
            assertFalse(matches.cancelWaiting(request, first))
            val assignment = MatchResponse(roomId, endpoint)
            prepared.complete(assignment)
            assertEquals(assignment, first.join())
            assertEquals(assignment, second.join())
        }
    }

    @Test
    fun staleCancellationDoesNotRemoveReplacement() {
        matchmaker { id, _ -> CompletableFuture.completedFuture(MatchResponse(id, endpoint)) }
            .use { matches ->
                val request = MatchRequest("a")
                val first = matches.match(request).toCompletableFuture()
                assertTrue(matches.cancelWaiting(request, first))
                assertTrue(first.isCancelled)
                val replacement = matches.match(request).toCompletableFuture()
                assertFalse(matches.cancelWaiting(request, first))
                assertFalse(replacement.isDone)
                val second = matches.match(MatchRequest("b")).toCompletableFuture()
                assertEquals(replacement.join(), second.join())
            }
    }

    @Test
    fun disconnectedWaiterCanRequestAgainAndMatch() {
        val config = NodeConfig(httpEndpoint = "http://127.0.0.1:0", sessionEndpoint = endpoint)
        matchmaker { id, _ -> CompletableFuture.completedFuture(MatchResponse(id, endpoint)) }
            .use { matches ->
                MyGame.lobby.infrastructure.HttpLobby(config, matches).use { lobby ->
                    val uri = URI.create(lobby.endpoint())
                    request(uri, "a").use { abandoned ->
                        abandoned.shutdownOutput()
                        assertEquals(
                            -1,
                            abandoned.getInputStream().read(),
                            "Server must close the disconnected waiter's connection",
                        )
                    }
                    request(uri, "a").use { first ->
                        request(uri, "b").use { second ->
                            val a = first.getInputStream().readAllBytes().toString(Charsets.UTF_8)
                            val b = second.getInputStream().readAllBytes().toString(Charsets.UTF_8)
                            assertTrue(a.startsWith("HTTP/1.1 200"), a)
                            assertTrue(b.startsWith("HTTP/1.1 200"), b)
                            assertEquals(
                                Json.mapper.readTree(a.substringAfter("\r\n\r\n")),
                                Json.mapper.readTree(b.substringAfter("\r\n\r\n")),
                            )
                            val roomId = Json.mapper.readTree(a.substringAfter("\r\n\r\n"))["roomId"].asText()
                            assertTrue(roomId.startsWith(Names.ROOM_PREFIX), roomId)
                            assertTrue(roomId.removePrefix(Names.ROOM_PREFIX).matches(Regex("[0-9a-f]{32}")), roomId)
                        }
                    }
                }
            }
    }

    private fun request(uri: URI, player: String): Socket {
        val socket = Socket(uri.host, uri.port)
        socket.soTimeout = httpTimeoutMillis
        val body = Json.mapper.writeValueAsBytes(MatchRequest(player))
        val headers =
            "POST ${LobbyNames.MATCH_PATH} HTTP/1.1\r\nHost: localhost\r\nContent-Length: ${body.size}\r\nConnection: close\r\n\r\n"
        socket.getOutputStream().write(headers.toByteArray(Charsets.US_ASCII))
        socket.getOutputStream().write(body)
        socket.getOutputStream().flush()
        return socket
    }

    @Test
    fun bothResponsesWaitForPreparedRoom() {
        val prepared = CompletableFuture<MatchResponse>()
        var roomId = ""
        matchmaker { id, members ->
            roomId = id
            assertEquals(listOf("a", "b"), members)
            prepared
        }.use { matches ->
            val first = matches.match(MatchRequest("a")).toCompletableFuture()
            assertFalse(first.isDone)
            assertThrows(IllegalArgumentException::class.java) { matches.match(MatchRequest("a")) }
            val second = matches.match(MatchRequest("b")).toCompletableFuture()
            assertFalse(first.isDone)
            assertFalse(second.isDone)
            val assignment = MatchResponse(roomId, endpoint)
            prepared.complete(assignment)
            assertEquals(assignment, first.join())
            assertEquals(assignment, second.join())
        }
    }

    @Test
    fun modesStaySeparateAndShutdownFailsWaiters() {
        val matches = matchmaker { id, _ -> CompletableFuture.completedFuture(MatchResponse(id, endpoint)) }
        val a = matches.match(MatchRequest("a", "first")).toCompletableFuture()
        val b = matches.match(MatchRequest("b", "second")).toCompletableFuture()
        assertFalse(a.isDone)
        assertFalse(b.isDone)
        matches.close()
        assertTrue(a.isCompletedExceptionally)
        assertTrue(b.isCompletedExceptionally)
        assertThrows(IllegalStateException::class.java) { matches.match(MatchRequest("c", "first")) }
    }

    @Test
    fun preparationFailureReachesBothPlayers() {
        matchmaker { _, _ -> CompletableFuture.failedFuture(IllegalStateException("Room rejected")) }
            .use { matches ->
                val first = matches.match(MatchRequest("a")).toCompletableFuture()
                val second = matches.match(MatchRequest("b")).toCompletableFuture()
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
        matchmaker { _, _ -> throw AssertionError("No pair expected") }.use { matches ->
            assertThrows(IllegalArgumentException::class.java) {
                matches.match(MatchRequest("", Names.DEFAULT_MODE))
            }
            assertThrows(IllegalArgumentException::class.java) { matches.match(MatchRequest("a", "")) }
        }
    }
}
