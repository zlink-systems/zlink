package MyGame.lobby.application

import MyGame.*
import java.util.concurrent.*
import org.junit.jupiter.api.Assertions.*
import org.junit.jupiter.api.Test

class MatchmakerTest {
    private val httpTimeoutMillis = 5000
    private val endpoint = "tcp://localhost:1"

    @Test
    fun cancellationDoesNotChangePairedRequests() {
        val prepared = CompletableFuture<MatchResponse>()
        Matchmaker { prepared }
            .use { matches ->
                val request = MatchRequest("a", Names.DEFAULT_MODE)
                val first = matches.match(request).toCompletableFuture()
                val second =
                    matches.match(MatchRequest("b", Names.DEFAULT_MODE)).toCompletableFuture()
                assertFalse(matches.cancelWaiting(request, first))
                val assignment = MatchResponse(Names.ROOM_PREFIX + "1", endpoint)
                prepared.complete(assignment)
                assertEquals(assignment, first.join())
                assertEquals(assignment, second.join())
            }
    }

    @Test
    fun staleCancellationDoesNotRemoveReplacement() {
        Matchmaker { id -> CompletableFuture.completedFuture(MatchResponse(id, endpoint)) }
            .use { matches ->
                val request = MatchRequest("a", Names.DEFAULT_MODE)
                val first = matches.match(request).toCompletableFuture()
                assertTrue(matches.cancelWaiting(request, first))
                assertTrue(first.isCancelled)
                val replacement = matches.match(request).toCompletableFuture()
                assertFalse(matches.cancelWaiting(request, first))
                assertFalse(replacement.isDone)
                val second =
                    matches.match(MatchRequest("b", Names.DEFAULT_MODE)).toCompletableFuture()
                assertEquals(replacement.join(), second.join())
            }
    }

    @Test
    fun disconnectedWaiterCanRequestAgainAndMatch() {
        val config = NodeConfig(httpEndpoint = "http://127.0.0.1:0", sessionEndpoint = endpoint)
        Matchmaker { id -> CompletableFuture.completedFuture(MatchResponse(id, endpoint)) }
            .use { matches ->
                MyGame.lobby.infrastructure.HttpLobby(config, matches).use { lobby ->
                    val uri = java.net.URI.create(lobby.endpoint())
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
                            assertTrue(a.contains(Names.ROOM_PREFIX + "1"), a)
                        }
                    }
                }
            }
    }

    private fun request(uri: java.net.URI, player: String): java.net.Socket {
        val socket = java.net.Socket(uri.host, uri.port)
        socket.soTimeout = httpTimeoutMillis
        val body = Json.mapper.writeValueAsBytes(MatchRequest(player, Names.DEFAULT_MODE))
        val headers =
            "POST ${Names.MATCH_PATH} HTTP/1.1\r\nHost: localhost\r\nContent-Length: ${body.size}\r\nConnection: close\r\n\r\n"
        socket.getOutputStream().write(headers.toByteArray(Charsets.US_ASCII))
        socket.getOutputStream().write(body)
        socket.getOutputStream().flush()
        return socket
    }

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
