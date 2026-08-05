package systems.zlink.e2e.kotlin.spotactortransfer.client

import com.fasterxml.jackson.databind.ObjectMapper
import java.net.URI
import java.net.http.HttpClient
import java.net.http.HttpRequest
import java.net.http.HttpResponse
import java.nio.file.Files
import java.nio.file.Path
import java.time.Duration
import java.util.Properties
import java.util.UUID
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.async
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import systems.zlink.e2e.spotactortransfer.shared.Contracts
import systems.zlink.framework.kotlin.awaitReply
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.stream.connector.ZLinkStreamCompression
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions
import systems.zlink.stream.connector.ZLinkStreamDispatchMode
import systems.zlink.stream.connector.ZLinkStreamPacketNameResolver

fun main(vararg args: String) = runBlocking {
    require(args.size == 4 && args[0] == "--config" && args[2] == "--scenario") {
        "Usage: spot-actor-transfer-kotlin-client --config <path> --scenario <selector>"
    }
    val scenario = args[3]
    when (scenario) {
        "ST-E1", "ST-E2" ->
            KotlinBoundSessionScenario(scenario, loadOptions(args[1])).run()
        else -> systems.zlink.e2e.spotactortransfer.client.Program.main(*args)
    }
}

private class KotlinBoundSessionScenario(
    private val scenario: String,
    private val options: Properties,
) {
    private val json = ObjectMapper()
    private val http = HttpClient.newBuilder().connectTimeout(Duration.ofSeconds(3)).build()
    private val nodeA = options.required("nodeAHttpEndpoint")
    private val nodeB = options.required("nodeBHttpEndpoint")

    suspend fun run() {
        if (scenario == "ST-E1") {
            successfulTransfer()
        } else {
            failedTransfer()
        }
        println("scenario $scenario passed")
        println("spot-actor-transfer e2e result=passed")
    }

    private suspend fun successfulTransfer() {
        val actorId = id("actor-bound-transfer")
        val spotRid = id("spot-bound-transfer")
        createSpot(spotRid)
        createActor(actorId, Contracts.STATEFUL, 81)
        val connector = connector().kotlin()
        try {
            connector.connect().await()
            val bound = connector.request(Contracts.BindSessionReq("ST-E1", actorId))
                .awaitReply<Contracts.BindSessionRes>()
            require(bound.nodeRid() == "actor-a") { "ST-E1 session did not bind source actor" }
            assertBoundPush(connector, actorId, "before-transfer", "actor-a")
            require(join(actorId, spotRid).accepted()) { "ST-E1 remote transfer failed" }
            assertBoundPush(connector, actorId, "after-transfer", "actor-b")
        } finally {
            connector.close().await()
        }
    }

    private suspend fun failedTransfer() {
        val actorId = id("actor-bound-failed-transfer")
        val spotRid = id("spot-bound-failed-transfer")
        createSpot(spotRid)
        createActor(actorId, Contracts.FAIL_OUT, 82)
        val connector = connector().kotlin()
        try {
            connector.connect().await()
            connector.request(Contracts.BindSessionReq("ST-E2", actorId))
                .awaitReply<Contracts.BindSessionRes>()
            require(!join(actorId, spotRid).accepted()) {
                "ST-E2 injected transfer failure returned success"
            }
            assertBoundPush(connector, actorId, "after-failed-transfer", "actor-a")
        } finally {
            connector.close().await()
        }
    }

    private suspend fun assertBoundPush(
        connector: systems.zlink.framework.kotlin.ZLinkKotlinStreamConnector,
        actorId: String,
        marker: String,
        expectedNode: String,
    ) {
        val notification = kotlinx.coroutines.coroutineScope {
            val waiting = async(start = CoroutineStart.UNDISPATCHED) {
                withTimeout(8_000) {
                    val encoded = connector.messages("BoundPushNotify").first().payload()
                    try {
                        json.readValue(
                            encoded.payload().toByteArray(),
                            Contracts.BoundPushNotify::class.java,
                        )
                    } finally {
                        encoded.payload().close()
                    }
                }
            }
            val reply = connector.request(Contracts.BoundPushReq(scenario, marker))
                .metadata("actor-id", actorId)
                .awaitReply<Contracts.BoundPushRes>()
            require(reply.nodeRid() == expectedNode) { "bound push reply used the wrong node" }
            waiting.await()
        }
        require(notification.nodeRid() == expectedNode) { "Flow push used the wrong node" }
        require(notification.marker() == marker) { "Flow push marker mismatch" }
    }

    private fun createSpot(spotRid: String) {
        post(
            "$nodeB/spots",
            Contracts.CreateSpotReq(spotRid, "accept"),
            Contracts.CreateSpotRes::class.java,
        )
    }

    private fun createActor(actorId: String, actorType: String, state: Int) {
        post(
            "$nodeA/actors",
            Contracts.ActorCreateReq(actorId, actorType, state),
            Contracts.ActorCreateRes::class.java,
        )
    }

    private fun join(actorId: String, spotRid: String): Contracts.JoinTargetRes = post(
        "$nodeA/actors/$actorId/join",
        Contracts.JoinTargetReq(scenario, spotRid, "accept"),
        Contracts.JoinTargetRes::class.java,
    )

    private fun <T> post(uri: String, body: Any, type: Class<T>): T {
        val request = HttpRequest.newBuilder(URI.create(uri))
            .timeout(Duration.ofSeconds(14))
            .header("Content-Type", "application/json")
            .POST(HttpRequest.BodyPublishers.ofByteArray(json.writeValueAsBytes(body)))
            .build()
        val response = http.send(request, HttpResponse.BodyHandlers.ofByteArray())
        check(response.statusCode() / 100 == 2) {
            "HTTP ${response.statusCode()}: ${String(response.body())}"
        }
        return json.readValue(response.body(), type)
    }

    private fun connector() = ZLinkStreamConnectorFactory.create(
        ZLinkStreamConnectorOptions(
            URI.create(options.required("streamAEndpoint")),
            ZLinkStreamDispatchMode.IMMEDIATE,
            Duration.ofSeconds(10),
            2,
            Duration.ofSeconds(5),
            64 * 1024,
            64 * 1024,
            Int.MAX_VALUE,
            true,
            Duration.ofSeconds(1),
            Duration.ofSeconds(5),
            false,
            Duration.ofMillis(250),
            Duration.ofSeconds(5),
            2.0,
            false,
            ZLinkStreamCompression.LZ4,
            ZLinkStreamPacketNameResolver.defaultResolver(),
            null,
        ),
    )

    private fun id(prefix: String): String =
        "$prefix-${UUID.randomUUID().toString().replace("-", "")}"
}

private fun loadOptions(path: String): Properties = Properties().also { properties ->
    Files.newInputStream(Path.of(path)).use(properties::load)
}

private fun Properties.required(name: String): String =
    getProperty(name)?.takeIf(String::isNotBlank)
        ?: error("$name is required")
