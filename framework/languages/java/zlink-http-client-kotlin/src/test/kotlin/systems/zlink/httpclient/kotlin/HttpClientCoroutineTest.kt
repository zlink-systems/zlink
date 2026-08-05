/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient.kotlin

import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpHandler
import com.sun.net.httpserver.HttpServer
import java.net.InetSocketAddress
import java.nio.charset.StandardCharsets
import java.lang.reflect.Modifier
import java.util.ArrayDeque
import java.util.concurrent.Executors
import java.util.concurrent.CompletionStage
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.yield
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertFalse
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.api.Test
import systems.zlink.httpclient.ZLinkHttpExecutionTurn
import systems.zlink.httpclient.ZLinkHttpClient
import java.util.concurrent.CompletableFuture

private data class Player(val id: Int, val name: String)

private data class CreateGameReq(val name: String)

private data class CreateGameRes(val id: String, val ranked: Boolean)

private class TestServer(handler: HttpHandler) : AutoCloseable {
    private val server: HttpServer = HttpServer.create(InetSocketAddress("127.0.0.1", 0), 0).apply {
        executor = Executors.newCachedThreadPool()
        createContext("/", handler)
        start()
    }
    val baseUrl: String = "http://127.0.0.1:${server.address.port}"
    override fun close() = server.stop(0)
}

private fun respond(exchange: HttpExchange, status: Int, body: String) {
    val bytes = body.toByteArray(StandardCharsets.UTF_8)
    exchange.sendResponseHeaders(status, if (bytes.isEmpty()) -1 else bytes.size.toLong())
    exchange.responseBody.use { it.write(bytes) }
}

private fun readBody(exchange: HttpExchange): String =
    String(exchange.requestBody.readAllBytes(), StandardCharsets.UTF_8)

class HttpClientCoroutineTest {

    @Test
    fun `server coroutine facade exposes yield and not the removed yieldAwait`() {
        val methods = Class.forName(
            "systems.zlink.httpclient.kotlin.HttpClientCoroutinesKt",
        ).declaredMethods.filter { Modifier.isPublic(it.modifiers) }

        assertTrue(methods.any { method ->
            method.name == "yield" &&
                method.parameterTypes.firstOrNull()?.name ==
                    "systems.zlink.httpclient.ZLinkHttpServerRequestBuilder"
        })
        assertTrue(methods.none { it.name == "yieldAwait" })
    }

    @Test
    fun `cancelling coroutine wait does not cancel submitted stage`() = runBlocking {
        val submitted = CompletableFuture<String>()
        val waiter = async {
            submitted.awaitWithoutCancellingOperation()
        }
        yield()

        waiter.cancelAndJoin()

        assertTrue(!submitted.isCancelled)
        assertTrue(!submitted.isDone)
        submitted.complete("completed-after-caller-cancel")
        assertEquals("completed-after-caller-cancel", submitted.join())
    }

    @Test
    fun `cancelling after request admission leaves the HTTP operation and client lease intact`() = runBlocking {
        val requestStarted = CountDownLatch(1)
        val releaseResponse = CountDownLatch(1)
        TestServer { exchange ->
            requestStarted.countDown()
            assertTrue(releaseResponse.await(2, TimeUnit.SECONDS))
            respond(exchange, 200, "{}")
        }.use { server ->
            zlinkHttpClient(server.baseUrl).use { client ->
                val submitted = client.get("/slow").submitRaw().toCompletableFuture()
                val waiter = async { submitted.awaitWithoutCancellingOperation() }

                assertTrue(requestStarted.await(2, TimeUnit.SECONDS))
                waiter.cancelAndJoin()

                assertFalse(submitted.isCancelled)
                assertFalse(submitted.isDone)
                releaseResponse.countDown()
                assertEquals(200, submitted.get(2, TimeUnit.SECONDS).status())
            }
        }
    }

    @Test
    fun `cancelling the caller does not discard a pending retry`() = runBlocking {
        val attempts = AtomicInteger()
        val firstAttempt = CountDownLatch(1)
        val secondAttempt = CountDownLatch(1)
        TestServer { exchange ->
            if (attempts.incrementAndGet() == 1) {
                firstAttempt.countDown()
                exchange.close()
            } else {
                secondAttempt.countDown()
                respond(exchange, 200, "{}")
            }
        }.use { server ->
            zlinkHttpClient(server.baseUrl) {
                retry(2)
            }.use { client ->
                val submitted = client.get("/retry").submitRaw().toCompletableFuture()
                val waiter = async { submitted.awaitWithoutCancellingOperation() }

                assertTrue(firstAttempt.await(2, TimeUnit.SECONDS))
                waiter.cancelAndJoin()

                assertFalse(submitted.isCancelled)
                assertTrue(secondAttempt.await(2, TimeUnit.SECONDS))
                assertEquals(200, submitted.get(2, TimeUnit.SECONDS).status())
            }
        }
    }

    @Test
    fun `server coroutine selects retained or yielded turn`() = runBlocking {
        val asyncCalls = AtomicInteger()
        val yieldCalls = AtomicInteger()
        val turn = object : ZLinkHttpExecutionTurn {
            override fun <T : Any?> async(operation: CompletionStage<T>): CompletionStage<T> {
                asyncCalls.incrementAndGet()
                return operation
            }

            override fun <T : Any?> yield(operation: CompletionStage<T>): CompletionStage<T> {
                yieldCalls.incrementAndGet()
                return operation
            }
        }
        TestServer { exchange -> respond(exchange, 200, """{"id":7,"name":"Aria"}""") }.use { server ->
            ZLinkHttpClient.create(server.baseUrl).buildServer(turn).use { client ->
                assertEquals(7, client.get("/p").await<Player>().body().id)
            assertEquals(7, client.get("/p").yield<Player>().body().id)
                client.post("/p").await()
                assertEquals(2, asyncCalls.get())
                assertEquals(1, yieldCalls.get())
            }
        }
    }

    @Test
    fun `suspend awaitRaw returns the response`() = runBlocking {
        TestServer { exchange -> respond(exchange, 200, "{}") }.use { server ->
            zlinkHttpClient(server.baseUrl).use { client ->
                val response = client.get("/r").awaitRaw()
                assertEquals(200, response.status())
            }
        }
    }

    @Test
    fun `suspend typed await round trips via DSL`() = runBlocking {
        var received: String? = null
        TestServer { exchange ->
            received = readBody(exchange)
            respond(exchange, 200, """{"id":"game-7","ranked":true}""")
        }.use { server ->
            zlinkHttpClient(server.baseUrl).use { client ->
                val response = client.post("/games").body(CreateGameReq("ranked-0611")).await<CreateGameRes>()
                assertTrue(received!!.contains("ranked-0611"))
                assertEquals("game-7", response.body().id)
                assertTrue(response.body().ranked)
            }
        }
    }

    @Test
    fun `suspend reified await decodes response`() = runBlocking {
        TestServer { exchange -> respond(exchange, 200, """{"id":8,"name":"Nia"}""") }.use { server ->
            zlinkHttpClient(server.baseUrl).use { client ->
                val response = client.get("/players/8").await<Player>()
                assertEquals(8, response.body().id)
                assertEquals("Nia", response.body().name)
            }
        }
    }

    @Test
    fun `suspend fetch returns decoded body`() = runBlocking {
        TestServer { exchange -> respond(exchange, 200, """{"id":"game-8","ranked":false}""") }.use { server ->
            zlinkHttpClient(server.baseUrl).use { client ->
                val body = client.get("/games/game-8").fetch<CreateGameRes>()
                assertEquals("game-8", body.id)
                assertEquals(false, body.ranked)
            }
        }
    }

    @Test
    fun `suspend await with explicit type`() = runBlocking {
        TestServer { exchange -> respond(exchange, 200, """{"id":7,"name":"Aria"}""") }.use { server ->
            zlinkHttpClient(server.baseUrl).use { client ->
                val response = client.get("/players/7").await(Player::class.java)
                assertEquals(7, response.body().id)
                assertEquals("Aria", response.body().name)
            }
        }
    }

    @Test
    fun `suspend awaitDownload streams chunks to sink`() = runBlocking {
        TestServer { exchange -> respond(exchange, 200, "streamed-response-payload") }.use { server ->
            zlinkHttpClient(server.baseUrl).use { client ->
                val sink = StringBuilder()
                val response = client.get("/download").awaitDownload { chunk ->
                    sink.append(String(chunk, StandardCharsets.UTF_8))
                }
                assertEquals(200, response.status())
                assertEquals("streamed-response-payload", sink.toString())
            }
        }
    }

    @Test
    fun `suspend streaming upload sends chunks`() = runBlocking {
        var received: String? = null
        TestServer { exchange ->
            received = readBody(exchange)
            respond(exchange, 200, "{}")
        }.use { server ->
            zlinkHttpClient(server.baseUrl).use { client ->
                val chunks = ArrayDeque(listOf("part-1;".toByteArray(), "part-2;".toByteArray(), "part-3".toByteArray()))
                client.post("/s")
                    .bodyStream({ if (chunks.isEmpty()) null else chunks.poll() }, "application/octet-stream")
                    .awaitRaw()
                assertEquals("part-1;part-2;part-3", received)
            }
        }
    }

    @Test
    fun `non-blocking coroutines do not serialize requests`() = runBlocking {
        TestServer { exchange ->
            Thread.sleep(200)
            respond(exchange, 200, "{}")
        }.use { server ->
            zlinkHttpClient(server.baseUrl).use { client ->
                val start = System.nanoTime()
                val responses = (1..20).map { async { client.get("/r").awaitRaw() } }.awaitAll()
                val elapsedMs = (System.nanoTime() - start) / 1_000_000
                responses.forEach { assertEquals(200, it.status()) }
                assertTrue(elapsedMs < 2000, "elapsed=$elapsedMs")
            }
        }
    }
}
