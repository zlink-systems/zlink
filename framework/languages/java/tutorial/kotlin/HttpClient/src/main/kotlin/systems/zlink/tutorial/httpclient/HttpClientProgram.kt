package systems.zlink.tutorial.httpclient

import java.nio.charset.StandardCharsets
import java.time.Duration
import java.util.function.Supplier
import kotlinx.coroutines.runBlocking
import systems.zlink.framework.errors.ZLinkFrameworkException
import systems.zlink.httpclient.kotlin.await
import systems.zlink.httpclient.kotlin.awaitDownload
import systems.zlink.httpclient.kotlin.awaitRaw
import systems.zlink.httpclient.kotlin.fetch
import systems.zlink.httpclient.kotlin.zlinkHttpClient

fun main() = runBlocking {
    // --8<-- [start:http-client-create]
    val client = zlinkHttpClient("http://127.0.0.1:5380") {
        timeout(Duration.ofSeconds(3))
    }
    // --8<-- [end:http-client-create]

    try {
        // --8<-- [start:http-first-request]
        val first = client.get("/players/p1/profile").fetch<PlayerProfile>()
        println("first request: ${first.playerId} ${first.nickname}")
        // --8<-- [end:http-first-request]

        // --8<-- [start:http-request-shaping]
        val status = client
            .get("/ops/nodes/game-server-1/status")
            .header("x-trace-id", "tutorial-1")
            .timeout(Duration.ofSeconds(5))
            .await<NodeStatus>()
        // The admin route has another base URL, so this example uses a separate client.
        val admin = zlinkHttpClient("http://127.0.0.1:5381") {
            basicAuth("ops", "tutorial-admin")
        }
        try {
            val weight = admin
                .post("/admin/channels/profile/weight")
                .query("value", "2")
                .await<WeightChanged>()
            println("request shaping: status ${status.status} weight ${weight.body.weight}")
        } finally {
            admin.close()
        }
        // --8<-- [end:http-request-shaping]

        // --8<-- [start:http-json-body]
        val player = client.post("/players/p2")
            .body(CreatePlayer("rookie"))
            .awaitRaw()
        val room = client.post("/rooms")
            .body(OpenRoom("lobby"))
            .awaitRaw()
        val roomId = stringBody(room)
        val chat = client.post("/rooms/$roomId/chat")
            .body(PostChat("p2", "hello"))
            .awaitRaw()
        println("json body: player ${player.status} room $roomId chat ${chat.status}")
        // roomId is reused by the response, compression, download, and upload steps below.
        // --8<-- [end:http-json-body]

        // --8<-- [start:http-response-kinds]
        val typed = client.get("/players/p2").await<PlayerInfo>()
        val raw = client.get("/players/p2").awaitRaw()
        val fetched = client.get("/players/p2").fetch<PlayerInfo>()
        println("response kinds: typed ${typed.status} raw ${raw.headers["content-type"]} fetch ${fetched.nickname}")
        // --8<-- [end:http-response-kinds]

        // The redirect target is an actor URL; create p1 because this standalone
        // program may start before the tutorial's actor example has run.
        client.post("/players/p1")
            .body(CreatePlayer("rookie"))
            .awaitRaw()

        // --8<-- [start:http-compressed-response]
        val compressed = zlinkHttpClient("http://127.0.0.1:5380") {
            timeout(Duration.ofSeconds(3))
            compression()
        }
        try {
            val response = compressed.get("/rooms/$roomId").await<RoomState>()
            val encodingRemoved = !response.headers.containsKey("content-encoding") && response.body != null
            println("compressed response: ${response.status} encoding-removed $encodingRemoved")
        } finally {
            compressed.close()
        }
        // --8<-- [end:http-compressed-response]

        // --8<-- [start:http-redirect]
        val redirects = zlinkHttpClient("http://127.0.0.1:5380") {
            followRedirects()
        }
        try {
            val response = redirects.get("/player/p1").await<PlayerInfo>()
            println("redirect: ${response.status} ${response.body.playerId}")
        } finally {
            redirects.close()
        }
        // --8<-- [end:http-redirect]

        // --8<-- [start:http-basic-auth]
        val unauthenticated = zlinkHttpClient("http://127.0.0.1:5381") {}
        val authenticated = zlinkHttpClient("http://127.0.0.1:5381") {
            basicAuth("ops", "tutorial-admin")
        }
        try {
            val without = unauthenticated.post("/admin/channels/profile/weight")
                .query("value", "2")
                .awaitRaw()
            val with = authenticated.post("/admin/channels/profile/weight")
                .query("value", "2")
                .awaitRaw()
            println("basic auth: without ${without.status} with ${with.status}")
        } finally {
            unauthenticated.close()
            authenticated.close()
        }
        // --8<-- [end:http-basic-auth]

        // --8<-- [start:http-download-stream]
        var chunks = 0
        var bytes = 0
        client.get("/rooms/$roomId/export").awaitDownload { chunk ->
            chunks += 1
            bytes += chunk.size
        }
        println("download stream: chunks $chunks bytes $bytes")
        // --8<-- [end:http-download-stream]

        // --8<-- [start:http-upload-stream]
        val lines = listOf(
            "{\"playerId\":\"p2\",\"text\":\"import-1\"}\n".toByteArray(StandardCharsets.UTF_8),
            "{\"playerId\":\"p2\",\"text\":\"import-2\"}\n".toByteArray(StandardCharsets.UTF_8),
            "{\"playerId\":\"p2\",\"text\":\"import-3\"}\n".toByteArray(StandardCharsets.UTF_8),
        )
        var nextLine = 0
        val imported = client.post("/rooms/$roomId/import")
            .bodyStream(Supplier {
                if (nextLine < lines.size) lines[nextLine++] else null
            }, "application/x-ndjson")
            .await<Imported>()
        println("upload stream: imported ${imported.body.imported}")
        // --8<-- [end:http-upload-stream]

        // --8<-- [start:http-error-kinds]
        val badRequestKind = try {
            client.post("/players/p3").await<String>()
            error("expected bad request")
        } catch (failure: ZLinkFrameworkException) {
            failure.kind().name
        }
        val connectionRefusedKind = try {
            val closedPort = zlinkHttpClient("http://127.0.0.1:6280") {}
            try {
                closedPort.get("/").awaitRaw()
                error("expected connection refusal")
            } finally {
                closedPort.close()
            }
        } catch (failure: ZLinkFrameworkException) {
            failure.kind().name
        }
        println("error kinds: bad request $badRequestKind connection refused $connectionRefusedKind")
        // --8<-- [end:http-error-kinds]
    } finally {
        client.close()
    }
}

private fun stringBody(response: systems.zlink.httpclient.RawHttpResponse): String {
    val body = response.body.trim()
    return if (body.startsWith('"') && body.endsWith('"')) {
        body.substring(1, body.length - 1)
    } else {
        body
    }
}

private data class PlayerProfile(val playerId: String, val nickname: String, val level: Int)
private data class NodeStatus(
    val meshName: String,
    val channelName: String,
    val calledBy: String,
    val uptime: String,
    val processId: Int,
)
private data class WeightChanged(val channel: String, val weight: Int)
private data class CreatePlayer(val nickname: String)
private data class OpenRoom(val title: String)
private data class PostChat(val playerId: String, val text: String)
private data class PlayerInfo(val playerId: String, val nickname: String)
private data class RoomState(val title: String, val chat: List<String>)
private data class Imported(val imported: Int)
