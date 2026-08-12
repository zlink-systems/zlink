package systems.zlink.e2e.kotlin.channelegress.client

import com.fasterxml.jackson.databind.ObjectMapper
import com.fasterxml.jackson.module.kotlin.jacksonObjectMapper
import java.net.URI
import java.net.http.HttpClient
import java.net.http.HttpRequest
import java.net.http.HttpResponse
import java.nio.file.Files
import java.nio.file.Path
import java.time.Duration
import java.util.Properties
import java.util.UUID
import java.util.concurrent.CompletableFuture
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.runBlocking
import systems.zlink.contracts.messaging.Message
import systems.zlink.e2e.kotlin.channelegress.shared.Contracts
import systems.zlink.e2e.kotlin.channelegress.shared.EvidenceState
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.stream.connector.ZLinkStreamCodec
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions
import systems.zlink.stream.connector.ZLinkStreamEncodedPayload

private val JSON: ObjectMapper = jacksonObjectMapper()
private val HTTP: HttpClient = HttpClient.newBuilder().connectTimeout(Duration.ofSeconds(3)).build()

fun main(args: Array<String>) {
    require(args.size == 4 && args[0] == "--config" && args[2] == "--scenario") {
        "Usage: channel-egress-kotlin-client --config <path> --scenario <id>"
    }
    ScenarioSuite.run(args[3], ClientOptions.load(Path.of(args[1])))
}

data class ClientOptions(
    val sessionEndpoint: String,
    val playEndpoint: String,
    val auditEndpoint: String,
    val workflowAEndpoint: String,
    val workflowBEndpoint: String,
    val apiAEndpoint: String,
    val apiBEndpoint: String,
    val callerEndpoint: String,
    val fanoutSubscriberEndpoint: String,
) {
    companion object {
        fun load(path: Path): ClientOptions {
            val values = Properties().apply { Files.newInputStream(path).use(::load) }
            fun required(name: String): String = values.getProperty(name)?.takeIf { it.isNotBlank() }
                ?: error("missing client property $name")
            return ClientOptions(
                required("sessionEndpoint"),
                required("playEndpoint"),
                required("auditEndpoint"),
                required("workflowAEndpoint"),
                required("workflowBEndpoint"),
                required("apiAEndpoint"),
                required("apiBEndpoint"),
                required("callerEndpoint"),
                required("fanoutSubscriberEndpoint"),
            )
        }
    }
}

private object ClientHttp {
    inline fun <reified T : Any> get(endpoint: String, path: String): T =
        execute(
            HttpRequest.newBuilder(URI.create(endpoint + path)).timeout(Duration.ofSeconds(30)).GET().build(),
        )

    inline fun <reified T : Any> post(endpoint: String, path: String, value: Any): T {
        val body = JSON.writeValueAsBytes(value)
        return execute(
            HttpRequest.newBuilder(URI.create(endpoint + path))
                .timeout(Duration.ofSeconds(40))
                .header("Content-Type", "application/json")
                .POST(HttpRequest.BodyPublishers.ofByteArray(body))
                .build(),
        )
    }

    inline fun <reified T : Any> execute(request: HttpRequest): T {
        val response = HTTP.send(request, HttpResponse.BodyHandlers.ofByteArray())
        check(response.statusCode() in 200..299) {
            "HTTP ${response.statusCode()} from ${request.uri()}: ${String(response.body())}"
        }
        return JSON.readValue(response.body(), T::class.java)
    }
}

private object ScenarioSuite {
    private val all = listOf(
        "CH-E2E-01", "CH-E2E-02", "CH-E2E-03",
        "CH-E2E-04A", "CH-E2E-04B", "CH-E2E-04C",
        "CH-E2E-05", "CH-E2E-06", "CH-E2E-07A", "CH-E2E-07B", "CH-E2E-07C",
        "CH-E2E-08", "CH-E2E-09", "CH-E2E-10", "CH-E2E-11", "CH-E2E-12",
    )

    fun run(selector: String, options: ClientOptions) {
        val selected = if (selector.equals("all", true)) all else selector.split(',')
        selected.map(String::trim).map(String::uppercase).forEach { scenario ->
            when (scenario) {
                "CH-E2E-01" -> ch01(options)
                "CH-E2E-02" -> ch02(options)
                "CH-E2E-03" -> ch03(options)
                "CH-E2E-04A" -> ch04a(options)
                "CH-E2E-04B" -> ch04b(options)
                "CH-E2E-04C" -> ch04c(options)
                "CH-E2E-05" -> ch05(options)
                "CH-E2E-06" -> ch06(options)
                "CH-E2E-07A" -> ch07a(options)
                "CH-E2E-07B" -> ch07b(options)
                "CH-E2E-07C" -> ch07c(options)
                "CH-E2E-08" -> ch08(options)
                "CH-E2E-09" -> ch09(options)
                "CH-E2E-10" -> ch10(options)
                "CH-E2E-11" -> ch11(options)
                "CH-E2E-12" -> ch12(options)
                else -> error("ChannelEgressRouting scenario is not implemented: $scenario")
            }
            println("scenario $scenario passed")
        }
    }

    private fun ch01(options: ClientOptions) {
        val operation = id("ch-01")
        val forward = request(options.sessionEndpoint, Contracts.PLAY_CHANNEL, operation)
        val reverse = request(options.playEndpoint, Contracts.SESSION_CHANNEL, "$operation-reverse")
        succeeded(forward, "CH-E2E-01 forward")
        succeeded(reverse, "CH-E2E-01 reverse")
        check(forward.reply!!.role == "play") { "forward request did not reach Play" }
        check(reverse.reply!!.role == "session") { "reverse request did not reach Session" }
        waitFor(options.playEndpoint, operation)
        waitFor(options.sessionEndpoint, "$operation-reverse")
    }

    private fun ch02(options: ClientOptions) {
        val operation = id("ch-02")
        val result = request(options.sessionEndpoint, Contracts.PLAY_CHANNEL, operation, "cascade")
        succeeded(result, "CH-E2E-02 cascade")
        check(result.reply!!.downstream.size == 2) { "cascade did not return both downstream replies" }
        waitFor(options.auditEndpoint, "$operation-audit")
        waitForAny(options.workflowAEndpoint, options.workflowBEndpoint, "$operation-workflow")
    }

    private fun ch03(options: ClientOptions) {
        val operation = id("ch-03")
        val spotId = "$operation-spot"
        val created = ClientHttp.post<Contracts.SpotCreateRes>(
            options.playEndpoint,
            "/objects/spots",
            Contracts.SpotCreateReq(spotId),
        )
        check(created.spotId == spotId) { "CH-E2E-03 created a different Spot" }
        val reply = ClientHttp.post<Contracts.SpotWorkflowRes>(
            options.playEndpoint,
            "/objects/spots/$spotId/workflow",
            Contracts.SpotWorkflowReq(operation),
        )
        check(reply.sequence == listOf("handler-start", "workflow-reply", "handler-end", "timer-start")) {
            "CH-E2E-03 handler sequence was ${reply.sequence}"
        }
        waitFor(
            options.playEndpoint,
            "sequence=handler-start,workflow-reply,handler-end,timer-start,workflow-reply,timer-end",
        )
    }

    private fun ch04a(options: ClientOptions) {
        val result = weighted(options.callerEndpoint, options, "ch-04a", 800)
        val ratio = result.second / 800.0
        check(ratio in 0.65..0.85) { "CH-E2E-04A weight-300 ratio was $ratio counts=$result" }
    }

    private fun ch04b(options: ClientOptions) {
        postControl(options.workflowBEndpoint, "/control/weight/0")
        waitWorkflowTargets(options.callerEndpoint, 1)
        postControl(options.workflowAEndpoint, "/control/hold")
        val heldId = id("ch-04b-held")
        val held = CompletableFuture.supplyAsync {
            request(options.callerEndpoint, Contracts.WORKFLOW_CHANNEL, heldId, "hold")
        }
        waitFor(options.workflowAEndpoint, heldId)

        postControl(options.workflowBEndpoint, "/control/weight/100")
        postControl(options.workflowAEndpoint, "/control/drain")
        waitWorkflowTargets(options.callerEndpoint, 1)
        val converged = requestEventuallyExpectedRole(
            options.callerEndpoint,
            "workflow-b",
            "ch-04b-new-0",
        )
        succeeded(converged, "CH-E2E-04B first post-drain request")
        repeat(50) { index ->
            if (index == 0) return@repeat
            val next = request(options.callerEndpoint, Contracts.WORKFLOW_CHANNEL, id("ch-04b-new-$index"))
            succeeded(next, "CH-E2E-04B new request")
            val reply = requireNotNull(next.reply)
            check(reply.role == "workflow-b") { "draining server accepted a new request: ${reply.role}" }
        }
        postControl(options.workflowAEndpoint, "/control/release")
        val first = held.get(15, TimeUnit.SECONDS)
        succeeded(first, "CH-E2E-04B held request")
        check(first.reply!!.role == "workflow-a") { "held request changed target after drain" }
    }

    private fun ch04c(options: ClientOptions) {
        val result = request(options.callerEndpoint, Contracts.WORKFLOW_CHANNEL, id("ch-04c"))
        succeeded(result, "CH-E2E-04C replacement request")
        val reply = requireNotNull(result.reply)
        check(reply.lifecycle == "workflow-new") { "request used stale lifecycle ${reply.lifecycle}" }
    }

    private fun ch05(options: ClientOptions) {
        val rejectedId = id("ch-05-server-only")
        val rejected = request(options.workflowBEndpoint, Contracts.WORKFLOW_CHANNEL, rejectedId)
        error(rejected, "NOT_FOUND", "CH-E2E-05 server-only request")
        check(count(options.workflowBEndpoint, rejectedId) == 0) { "server-only request ran its local handler" }
        succeeded(
            request(options.callerEndpoint, Contracts.WORKFLOW_CHANNEL, id("ch-05-normal")),
            "CH-E2E-05 normal Client request",
        )
    }

    private fun ch06(options: ClientOptions) {
        succeeded(
            request(options.sessionEndpoint, Contracts.PLAY_CHANNEL, id("ch-06-route")),
            "CH-E2E-06 distinct RouteMesh channel",
        )
        succeeded(
            request(options.callerEndpoint, Contracts.WORKFLOW_CHANNEL, id("ch-06-workflow")),
            "CH-E2E-06 distinct ClientServer channel",
        )
    }

    private fun ch07a(options: ClientOptions) {
        val operation = id("ch-07a")
        val result = request(options.sessionEndpoint, "missing.channel", operation)
        error(result, "NOT_FOUND", "CH-E2E-07A missing channel")
        check(result.elapsedMilliseconds < 1_000) { "missing channel did not fail immediately" }
        check(count(options.sessionEndpoint, operation) == 0) { "missing channel reached an application handler" }
    }

    private fun ch07b(options: ClientOptions) {
        var remote = 0
        repeat(20) { index ->
            val result = request(options.apiAEndpoint, Contracts.API_CHANNEL, id("ch-07b-$index"))
            succeeded(result, "CH-E2E-07B server-role request")
            if (result.reply!!.role == "api-b") remote++
        }
        check(remote > 0) { "server-role requests never selected the remote member" }
        check(
            requestCount(options.apiAEndpoint, "ch-07b-") + requestCount(options.apiBEndpoint, "ch-07b-") == 20,
        ) { "CH-E2E-07B handler count did not equal 20" }
    }

    private fun ch07c(options: ClientOptions) {
        val operation = id("ch-07c")
        val result = request(options.sessionEndpoint, Contracts.API_CHANNEL, operation)
        error(result, "UNAVAILABLE", "CH-E2E-07C unavailable target")
        check(count(options.sessionEndpoint, operation) == 0) { "unavailable request reached another handler" }
    }

    private fun ch08(options: ClientOptions) {
        val operation = id("ch-08")
        val spotId = "$operation-spot"
        val actorId = "$operation-actor"
        ClientHttp.post<Contracts.SpotCreateRes>(
            options.playEndpoint,
            "/objects/spots",
            Contracts.SpotCreateReq(spotId),
        )
        ClientHttp.post<Contracts.ActorCreateRes>(
            options.playEndpoint,
            "/objects/actors",
            Contracts.ActorCreateReq(actorId),
        )
        val result = ClientHttp.post<Contracts.StateAddressRes>(
            options.callerEndpoint,
            "/objects/state-address",
            Contracts.StateAddressReq(operation, spotId, actorId),
        )
        check(result.downstream.size == 2) { "state-address handler did not return Spot and Actor replies" }
        check(result.downstream[0].startsWith("spot:$spotId:")) { "Spot result was not first" }
        check(result.downstream[1].startsWith("actor:$actorId:")) { "Actor result was not second" }
        waitFor(options.playEndpoint, "spot-request")
        waitFor(options.playEndpoint, "actor-request")
    }

    private fun ch09(options: ClientOptions) {
        val listenerRows = ClientHttp.get<Array<Contracts.ListenerStatus>>(
            options.apiAEndpoint,
            "/status/listeners",
        ).toList() + ClientHttp.get<Array<Contracts.ListenerStatus>>(
            options.workflowAEndpoint,
            "/status/listeners",
        ).toList()
        val expected = setOf("RouteMesh", "ClientServer", "Fanout", "STREAM")
        check(listenerRows.map { it.kind }.toSet().containsAll(expected)) {
            "CH-E2E-09 listener status omitted ${expected - listenerRows.map { it.kind }.toSet()}"
        }
        val failures = mutableListOf<String>()
        val missing = listenerRows.filter { it.kind in expected && it.advertisedEndpoint.isNullOrBlank() }
        if (missing.isNotEmpty()) {
            failures += "public listener state has no confirmed endpoint for " +
                missing.joinToString { "${it.kind}(${it.detail})" }
        }
        listenerRows.filter { it.kind in expected && !it.advertisedEndpoint.isNullOrBlank() }.forEach { row ->
            val endpoint = row.advertisedEndpoint.orEmpty()
            if (!row.isReady) failures += "${row.kind} listener was not ready"
            if (!endpoint.startsWith("tcp://127.0.0.1:")) {
                failures += "${row.kind} advertised endpoint did not use AdvertiseHost: $endpoint"
            }
            if (endpoint.endsWith(":0") || endpoint.contains("0.0.0.0")) {
                failures += "${row.kind} advertised endpoint retained wildcard or port 0: $endpoint"
            }
        }

        fun verify(label: String, action: () -> Unit) {
            runCatching(action).exceptionOrNull()?.let { failures += "$label: ${it.message}" }
        }
        verify("RouteMesh remote message") {
            succeeded(
                request(options.sessionEndpoint, Contracts.API_CHANNEL, id("ch-09-route")),
                "CH-E2E-09 RouteMesh port 0 request",
            )
        }
        verify("ClientServer remote message") {
            succeeded(
                request(options.callerEndpoint, Contracts.WORKFLOW_CHANNEL, id("ch-09-workflow")),
                "CH-E2E-09 ClientServer port 0 request",
            )
        }
        verify("fanout remote message") {
            val fanoutId = id("ch-09-fanout")
            ClientHttp.post<Map<String, Any>>(
                options.apiAEndpoint,
                "/fanout/publish",
                Contracts.FanoutProbeEvent(fanoutId),
            )
            waitFor(options.fanoutSubscriberEndpoint, fanoutId)
        }

        val streamEndpoint = listenerRows.single { it.kind == "STREAM" }.advertisedEndpoint
        if (!streamEndpoint.isNullOrBlank()) {
            verify("STREAM remote message") {
                val streamId = id("ch-09-stream")
                runBlocking {
                    val connector = ZLinkStreamConnectorFactory.create(
                        ZLinkStreamConnectorOptions.createDefault(URI.create(streamEndpoint)),
                    ).kotlin()
                    try {
                        connector.connect().await()
                        connector.send(
                            ZLinkStreamEncodedPayload(
                                "StreamProbeMsg",
                                Message.from(streamId.toByteArray()),
                                emptyMap(),
                                ZLinkStreamCodec.RAW,
                            ),
                        ).await()
                    } finally {
                        connector.close().await()
                    }
                }
                waitFor(options.apiAEndpoint, "packet=StreamProbeMsg")
            }
        }
        check(failures.isEmpty()) { "CH-E2E-09 failures: ${failures.joinToString("; ")}" }
    }

    private fun ch10(options: ClientOptions) {
        val operation = id("ch-10")
        val result = ClientHttp.post<Contracts.SendRes>(
            options.callerEndpoint,
            "/send",
            Contracts.InvokeReq(Contracts.WORKFLOW_CHANNEL, operation),
        )
        check(result.succeeded) { "CH-E2E-10 send failed: ${result.error}" }
        waitForAny(options.workflowAEndpoint, options.workflowBEndpoint, operation)
        check(count(options.workflowAEndpoint, operation) + count(options.workflowBEndpoint, operation) == 1) {
            "one-way send was not handled exactly once"
        }
    }

    private fun ch11(options: ClientOptions) {
        val operation = id("ch-11")
        succeeded(
            request(options.sessionEndpoint, Contracts.API_CHANNEL, operation),
            "CH-E2E-11 request",
        )
        val send = ClientHttp.post<Contracts.SendRes>(
            options.sessionEndpoint,
            "/send",
            Contracts.InvokeReq(Contracts.API_CHANNEL, "$operation-send"),
        )
        check(send.succeeded) { "CH-E2E-11 send failed: ${send.error}" }
        waitForAny(options.apiAEndpoint, options.apiBEndpoint, operation)
        waitForAny(options.apiAEndpoint, options.apiBEndpoint, "$operation-send")
    }

    private fun ch12(options: ClientOptions) {
        val result = weighted(options.workflowAEndpoint, options, "ch-12", 400)
        val localRatio = result.first / 400.0
        check(localRatio in 0.35..0.65) { "CH-E2E-12 local ratio was $localRatio counts=$result" }
    }

    private fun weighted(source: String, options: ClientOptions, prefix: String, total: Int): WeightedResult {
        var first = 0
        var second = 0
        val replies = mutableSetOf<String>()
        repeat(total) { index ->
            val result = request(source, Contracts.WORKFLOW_CHANNEL, "$prefix-$index")
            succeeded(result, "$prefix request $index")
            val reply = result.reply!!
            check(reply.role == "workflow-a" || reply.role == "workflow-b") { "unexpected workflow role ${reply.role}" }
            check(replies.add(reply.id)) { "duplicate workflow reply id ${reply.id}" }
            if (reply.role == "workflow-a") first++ else second++
        }
        check(first + second == total) { "workflow reply count mismatch" }
        val evidence = requestCount(options.workflowAEndpoint, prefix) + requestCount(options.workflowBEndpoint, prefix)
        check(evidence == total) { "workflow handler count mismatch: $evidence != $total" }
        return WeightedResult(first, second)
    }

    private fun request(endpoint: String, channel: String, operation: String, mode: String = "echo") =
        ClientHttp.post<Contracts.InvokeRes>(endpoint, "/request", Contracts.InvokeReq(channel, operation, mode))

    private fun postControl(endpoint: String, path: String) {
        ClientHttp.post<Map<String, Any>>(endpoint, path, emptyMap<String, Any>())
    }

    private fun waitWorkflowTargets(endpoint: String, expected: Int) {
        val deadline = System.nanoTime() + Duration.ofSeconds(20).toNanos()
        var last: Contracts.WorkflowStatus? = null
        while (System.nanoTime() < deadline) {
            last = ClientHttp.get(endpoint, "/status/workflow")
            if (last.readyTargetCount == expected) return
            pause()
        }
        error("workflow target count did not become $expected: $last")
    }

    private fun requestEventuallyExpectedRole(endpoint: String, expectedRole: String, prefix: String): Contracts.InvokeRes {
        val deadline = System.nanoTime() + Duration.ofSeconds(20).toNanos()
        var attempt = 0
        var last: Contracts.InvokeRes? = null
        while (System.nanoTime() < deadline) {
            val result = request(
                endpoint,
                Contracts.WORKFLOW_CHANNEL,
                id("$prefix-probe-${attempt++}"),
            )
            last = result
            if (result.succeeded) {
                check(result.reply?.role == expectedRole) {
                    "drained server accepted a new request: ${result.reply?.role}"
                }
                return result
            }
            pause()
        }
        error("workflow did not converge to $expectedRole: $last")
    }

    private fun succeeded(result: Contracts.InvokeRes, label: String) {
        check(result.succeeded) { "$label failed: ${result.error}" }
        check(result.reply != null) { "$label returned no reply" }
    }

    private fun error(result: Contracts.InvokeRes, expected: String, label: String) {
        check(!result.succeeded) { "$label unexpectedly succeeded" }
        check(result.error.equals(expected, true)) { "$label returned ${result.error}, expected $expected" }
    }

    private fun waitFor(endpoint: String, fragment: String) {
        val deadline = System.nanoTime() + Duration.ofSeconds(20).toNanos()
        while (System.nanoTime() < deadline) {
            if (count(endpoint, fragment) > 0) return
            pause()
        }
        error("timed out waiting for $fragment at $endpoint")
    }

    private fun waitForAny(first: String, second: String, fragment: String) {
        val deadline = System.nanoTime() + Duration.ofSeconds(20).toNanos()
        while (System.nanoTime() < deadline) {
            if (count(first, fragment) + count(second, fragment) > 0) return
            pause()
        }
        error("timed out waiting for $fragment")
    }

    private fun requestCount(endpoint: String, fragment: String): Int =
        evidence(endpoint).entries.count {
            it.marker == "request-start" && EvidenceState.evidenceLine(it).contains(fragment)
        }

    private fun count(endpoint: String, fragment: String): Int =
        evidence(endpoint).entries.count { EvidenceState.evidenceLine(it).contains(fragment) }

    private fun evidence(endpoint: String): Contracts.EvidenceSnapshot = ClientHttp.get(endpoint, "/evidence")

    private fun pause() = Thread.sleep(50)
    private fun id(prefix: String): String = "$prefix-${UUID.randomUUID().toString().replace("-", "")}"
    private data class WeightedResult(val first: Int, val second: Int)
}
