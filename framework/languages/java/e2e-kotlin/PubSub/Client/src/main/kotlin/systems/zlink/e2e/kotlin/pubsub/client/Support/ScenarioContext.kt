package Support

import systems.zlink.e2e.kotlin.pubsub.client.Scenarios
import systems.zlink.e2e.kotlin.pubsub.client.Support
import com.fasterxml.jackson.databind.JsonNode
import com.fasterxml.jackson.databind.ObjectMapper
import com.fasterxml.jackson.module.kotlin.readValue
import java.net.URI
import java.net.URLEncoder
import java.net.ConnectException
import java.net.http.HttpClient
import java.net.http.HttpRequest
import java.net.http.HttpResponse
import java.nio.file.Files
import java.nio.file.Path
import java.time.Duration
import systems.zlink.e2e.kotlin.pubsub.client.Scenarios.FanoutBasicDeliveryScenario
import systems.zlink.e2e.kotlin.pubsub.client.Scenarios.LateSubscriberScenario
import systems.zlink.e2e.kotlin.pubsub.client.Scenarios.MissingMessageNameScenario
import systems.zlink.e2e.kotlin.pubsub.client.Scenarios.PublisherRestartScenario
import systems.zlink.e2e.kotlin.pubsub.client.Scenarios.SlowSubscriberScenario
import systems.zlink.e2e.kotlin.pubsub.client.Scenarios.SubscriberReconnectScenario
import systems.zlink.e2e.kotlin.pubsub.client.Scenarios.TopicFilterScenario
import systems.zlink.e2e.kotlin.pubsub.shared.EventMsg
import systems.zlink.e2e.kotlin.pubsub.shared.EvidenceSnapshot

class ScenarioContext(
    private val options: ClientOptions,
    private val json: ObjectMapper,
) {
    private val http = HttpClient.newHttpClient()
    private val publisherUrl = options.publisherHttp
    private val processes = ServerProcessLauncher(options)

    fun run() {
        when (options.mode) {
            "PS-A1" -> FanoutBasicDeliveryScenario.run(this)
            "PS-A2" -> TopicFilterScenario.run(this)
            "PS-A3" -> runLateSubscriberOnly()
            "PS-A4" -> SubscriberReconnectScenario.run(this)
            "PS-B1" -> SlowSubscriberScenario.run(this)
            "PS-B2" -> PublisherRestartScenario.run(this)
            "PS-C1" -> MissingMessageNameScenario.run(this)
            "PS-D1" -> runAutomaticDiscovery()
            "PS-D2" -> runChannelIsolation()
            "PS-D3" -> runPublisherSetConvergence()
            "PS-D4" -> runPublisherReplacement()
            "PS-D5" -> runStoreOutage(false)
            "PS-D5-RECOVERY" -> runStoreOutage(true)
            "PS-D6" -> runPortZeroRestart()
            "PS-D7A" -> runObserverIsolation()
            "PS-D7B" -> runManualConnectionIsolation()
            "PS-E1" -> runManualSubscriberWithoutStore()
            "PS-F1" -> runAutomaticAndManualPublisher()
            "PS-F2" -> runPublisherConnectionIsolation()
            "PS-F3" -> runReservedTopicValidation()
            "PS-F4" -> runOrderlyPublisherDisconnect()
            "PS-F5" -> runLivenessWithUnsubscribedTraffic()
            "subscriber-restarted" -> SubscriberReconnectScenario.run(this)
            "slow-subscriber" -> SlowSubscriberScenario.run(this)
            "publisher-restarted" -> PublisherRestartScenario.run(this)
            else -> runDefault()
        }
    }

    private fun runAutomaticDiscovery() {
        waitForFanoutReady(options.sub1Http, 1)
        publish("all", EventMsg("ps-d1", 1, "automatic-discovery"))
        waitForEvent("sub-1", "ps-d1", 1)
        ensure(publisherIds(options.sub1Http).isNotEmpty(), "PS-D1 status did not expose a publisher")
        println("scenario PS-D1 passed")
    }

    private fun runChannelIsolation() {
        val audit = required(options.auditPublisherHttp, "audit-publisher-http")
        waitForFanoutReady(options.sub1Http, 1)
        publishAt(audit, "all", EventMsg("ps-d2-audit", 1, "audit-channel"))
        sleep(500)
        ensure(!hasEvent(snapshot("sub-1"), "ps-d2-audit", 1), "PS-D2 received an event from another ChannelName")
        publish("all", EventMsg("ps-d2", 1, "events-channel"))
        waitForEvent("sub-1", "ps-d2", 1)
        ensure(publisherIds(options.sub1Http).none { it == "audit-publisher" }, "PS-D2 status included another ChannelName")
        println("scenario PS-D2 passed")
    }

    private fun runPublisherSetConvergence() {
        val publisher2 = startPublisher2()
        publisher2.use {
            waitHealthy(required(options.publisher2Http, "publisher2-http"))
            waitForFanoutReady(options.sub1Http, 2)
            publish("all", EventMsg("ps-d3-a", 1, "publisher-a"))
            publishAt(required(options.publisher2Http, "publisher2-http"), "all", EventMsg("ps-d3-b", 1, "publisher-b"))
            waitForEvent("sub-1", "ps-d3-a", 1)
            waitForEvent("sub-1", "ps-d3-b", 1)
            postPublisherControl("/shutdown")
            waitForFanoutReady(options.sub1Http, 1)
            publishAt(required(options.publisher2Http, "publisher2-http"), "all", EventMsg("ps-d3-after", 1, "publisher-b-after"))
            waitForEvent("sub-1", "ps-d3-after", 1)
        }
        println("scenario PS-D3 passed")
    }

    private fun runPublisherReplacement() {
        val pid = options.publisherPid ?: throw IllegalArgumentException("--publisher-pid is required for PS-D4")
        ensure(ProcessHandle.of(pid).isPresent, "PS-D4 publisher process $pid is not present")
        ProcessHandle.of(pid).get().destroyForcibly()
        waitForFanoutReady(options.sub1Http, 0)
        val replacement = startPublisher2()
        replacement.use {
            waitHealthy(required(options.publisher2Http, "publisher2-http"))
            waitForFanoutReady(options.sub1Http, 1)
            publishAt(required(options.publisher2Http, "publisher2-http"), "all", EventMsg("ps-d4", 1, "replacement"))
            waitForEvent("sub-1", "ps-d4", 1)
        }
        println("scenario PS-D4 passed")
    }

    private fun runStoreOutage(recovery: Boolean) {
        if (recovery) {
            waitForFanoutReady(options.sub1Http, 1)
        }
        val sequence = if (recovery) 2 else 1
        publish("all", EventMsg("ps-d5", sequence, if (recovery) "store-recovered" else "store-paused"))
        waitForEvent("sub-1", "ps-d5", sequence)
        if (recovery) {
            ensure(statusAt(options.sub1Http).path("isReady").asBoolean(false), "PS-D5 status did not recover after Store unpause")
        }
        println("scenario ${if (recovery) "PS-D5-RECOVERY" else "PS-D5"} passed")
    }

    private fun runPortZeroRestart() {
        waitForFanoutReady(options.sub1Http, 1)
        val firstEndpoint = listenerEndpoint(options.publisherHttp)
        publish("all", EventMsg("ps-d6", 1, "port-zero-first"))
        waitForEvent("sub-1", "ps-d6", 1)
        postPublisherControl("/shutdown")
        waitDown(publisherUrl)

        val replacement = startPublisher2()
        replacement.use {
            waitHealthy(required(options.publisher2Http, "publisher2-http"))
            val secondEndpoint = listenerEndpoint(required(options.publisher2Http, "publisher2-http"))
            ensure(firstEndpoint != secondEndpoint, "PS-D6 port 0 restart reused the listener endpoint")
            ensure(!firstEndpoint.endsWith(":0") && !secondEndpoint.endsWith(":0"), "PS-D6 public listener status returned port 0")
            waitForFanoutReady(options.sub1Http, 1)
            publishAt(required(options.publisher2Http, "publisher2-http"), "all", EventMsg("ps-d6", 2, "port-zero-replacement"))
            waitForEvent("sub-1", "ps-d6", 2)
        }
        println("scenario PS-D6 passed")
    }

    private fun runObserverIsolation() {
        val subscriber = options.sub1Http
        waitForFanoutReady(subscriber, 1)
        post("$subscriber/observer/start?name=slow&capacity=1&slow=true")
        waitForObserver(subscriber, "slow")
        val publisher2 = startPublisher2()
        publisher2.use {
            waitHealthy(required(options.publisher2Http, "publisher2-http"))
            waitForFanoutReady(subscriber, 2)
            post("$subscriber/observer/start?name=normal&capacity=1&slow=false")
            waitForObserver(subscriber, "normal")
            ensure(
                observerEvidence(subscriber)
                    .last { it.path("observer").asText() == "normal" }
                    .path("readyPublisherCount").asInt(-1) == 2,
                "PS-D7A normal observer did not receive the current publisher set",
            )
            publishAt(required(options.publisher2Http, "publisher2-http"), "all", EventMsg("ps-d7a", 1, "observer-isolation"))
            waitForEvent("sub-1", "ps-d7a", 1)
            val slowCount = observerEvidence(subscriber).count { it.path("observer").asText() == "slow" }
            ensure(slowCount == 1, "PS-D7A slow observer was not bounded")
            post("$subscriber/observer/release?name=slow")
            post("$subscriber/observer/cancel?name=slow")
            ensure(observerEvidence(subscriber).any { it.path("observer").asText() == "normal" }, "PS-D7A normal observer was cancelled")
        }
        println("scenario PS-D7A passed")
    }

    private fun runManualConnectionIsolation() {
        val manual = required(options.sub4Http, "sub4-http")
        val publisher2 = startPublisher2()
        publisher2.use {
            waitHealthy(required(options.publisher2Http, "publisher2-http"))
            waitForFanoutReady(options.sub1Http, 2)
            val before = publisherIds(options.sub1Http)
            post("$manual/connections?operation=connect&endpoint=${encode(required(options.publisher2Endpoint, "publisher2-endpoint"))}")
            post("$manual/connections?operation=disconnect&endpoint=${encode(required(options.publisher2Endpoint, "publisher2-endpoint"))}")
            publish("all", EventMsg("ps-d7b", 1, "automatic-before"))
            waitForEvent("sub-1", "ps-d7b", 1)
            publishAt(required(options.publisher2Http, "publisher2-http"), "all", EventMsg("ps-d7b", 2, "automatic-after"))
            waitForEvent("sub-1", "ps-d7b", 2)
            val after = publisherIds(options.sub1Http)
            ensure(before == after, "PS-D7B manual endpoint mutation changed automatic status: before=$before after=$after")
        }
        println("scenario PS-D7B passed")
    }

    private fun runManualSubscriberWithoutStore() {
        val manual = required(options.sub4Http, "sub4-http")
        publish("all", EventMsg("ps-e1", 1, "manual-without-store"))
        waitForEvidenceAt(manual, "EventMsg", "ps-e1", 1)
        ensure(hasEvent(snapshotAt(manual), "ps-e1", 1), "PS-E1 manual subscriber did not receive the event")
        println("scenario PS-E1 passed")
    }

    private fun runAutomaticAndManualPublisher() {
        waitForFanoutReady(options.sub1Http, 1)
        val manual = required(options.sub4Http, "sub4-http")
        val publisher2 = startPublisher2()
        publisher2.use {
            waitHealthy(required(options.publisher2Http, "publisher2-http"))
            waitForFanoutReady(manual, 1)
            publishAt(required(options.publisher2Http, "publisher2-http"), "all", EventMsg("ps-f1", 1, "manual-publisher"))
            waitForEvidenceAt(manual, "EventMsg", "ps-f1", 1)
            publish("all", EventMsg("ps-f1", 2, "automatic-publisher"))
            waitForEvent("sub-1", "ps-f1", 2)
        }
        println("scenario PS-F1 passed")
    }

    private fun runPublisherConnectionIsolation() {
        val endpoint = required(options.publisher2Endpoint, "publisher2-endpoint")
        val uri = URI.create(endpoint)
        NetworkFaultProxy.start(uri.host, uri.port).use { fault ->
          val publisher2 = startPublisher2()
          publisher2.use {
            waitHealthy(required(options.publisher2Http, "publisher2-http"))
            waitForFanoutReady(options.sub1Http, 2)
            fault.block()
            waitUntil { publisherIds(options.sub1Http) == setOf("publisher-a") }
            publish("all", EventMsg("ps-f2", 1, "publisher-a-during-b-failure"))
            waitForEvent("sub-1", "ps-f2", 1)
            fault.unblock()
            waitForFanoutReady(options.sub1Http, 2)
            publishAt(required(options.publisher2Http, "publisher2-http"), "all", EventMsg("ps-f2", 2, "publisher-b-after-recovery"))
            waitForEvent("sub-1", "ps-f2", 2)
          }
        }
        println("scenario PS-F2 passed")
    }

    private fun runReservedTopicValidation() {
        val response = postPublisherReserved()
        ensure(response.statusCode() in 400..499, "PS-F3 reserved topic was accepted: ${response.statusCode()}")
        postPublisherControl("/publish-reserved-prefix")
        waitForEvent("sub-1", "ps-f3", 2)
        println("scenario PS-F3 passed")
    }

    private fun runOrderlyPublisherDisconnect() {
        val publisher2 = startPublisher2()
        publisher2.use {
            waitHealthy(required(options.publisher2Http, "publisher2-http"))
            waitForFanoutReady(options.sub1Http, 2)
            postPublisherControl("/shutdown")
            waitForFanoutReady(options.sub1Http, 1)
            publishAt(required(options.publisher2Http, "publisher2-http"), "all", EventMsg("ps-f4", 1, "remaining-publisher"))
            waitForEvent("sub-1", "ps-f4", 1)
        }
        println("scenario PS-F4 passed")
    }

    private fun runLivenessWithUnsubscribedTraffic() {
        waitForFanoutReady(options.sub1Http, 1)
        val deadline = System.nanoTime() + Duration.ofSeconds(17).toNanos()
        var sequence = 1
        while (System.nanoTime() < deadline) {
            publish("events.a", EventMsg("ps-f5-a", sequence++, "unsubscribed"))
            sleep(1_000)
        }
        ensure(!hasEvent(snapshot("sub-1"), "ps-f5-a", 1), "PS-F5 subscriber handled an unsubscribed topic")
        ensure(statusAt(options.sub1Http).path("isReady").asBoolean(false), "PS-F5 liveness status became not-ready")
        publish("events.b", EventMsg("ps-f5-b", 1, "subscribed"))
        waitForEvent("sub-1", "ps-f5-b", 1)
        println("scenario PS-F5 passed")
    }

    private fun startPublisher2(): LaunchedServer =
        processes.startPublisher(
            name = "publisher-b",
            publisherEndpoint = required(options.publisher2Endpoint, "publisher2-endpoint"),
            httpEndpoint = required(options.publisher2Http, "publisher2-http"),
            routingId = options.publisher2Rid,
            noStore = options.publisher2NoStore,
            listenPort = options.publisher2Port,
            advertiseHost = options.publisher2AdvertiseHost,
        )

    private fun runDefault() {
        touch(options.publisherReadyFile)
        waitForFile(options.prelateContinueFile)

        publish("all", EventMsg("prelate", 0, "before-late"))
        waitForEvent("sub-1", "prelate", 0)
        waitForEvent("sub-2", "prelate", 0)
        touch(options.lateReadyFile)
        waitForFile(options.lateContinueFile)

        FanoutBasicDeliveryScenario.run(this)
        TopicFilterScenario.run(this)
        LateSubscriberScenario.run(this)
        MissingMessageNameScenario.run(this)
    }

    private fun runLateSubscriberOnly() {
        touch(options.publisherReadyFile)
        waitForFile(options.prelateContinueFile)

        publish("all", EventMsg("prelate", 0, "before-late"))
        waitForEvent("sub-1", "prelate", 0)
        waitForEvent("sub-2", "prelate", 0)
        touch(options.lateReadyFile)
        waitForFile(options.lateContinueFile)

        LateSubscriberScenario.run(this)
    }

    fun runSubscriberRestartAfterReconnect() {
        val reconnectHttp = required(options.reconnectHttp, "reconnect-http")
        processes.startSubscriber("sub-reconnect", "alpha", reconnectHttp).use { subscriber ->
            waitHealthy(reconnectHttp)
            publish("all", EventMsg("ps-a4", 1, "before-disconnect"))
            waitForEventAt(reconnectHttp, "ps-a4", 1)
            subscriber.stop()
        }

        waitDown(reconnectHttp)
        for (sequence in 2..4) {
            publish("all", EventMsg("ps-a4", sequence, "gap-$sequence"))
        }
        waitForEvent("sub-1", "ps-a4", 4)
        waitForEvent("sub-2", "ps-a4", 4)

        processes.startSubscriber("sub-reconnect", "alpha", reconnectHttp).use {
            waitHealthy(reconnectHttp)
            for (sequence in 5..8) {
                publish("all", EventMsg("ps-a4", sequence, "after-reconnect-$sequence"))
            }
            waitForEventAt(reconnectHttp, "ps-a4", 8)
            val restarted = snapshotAt(reconnectHttp)
            ensure(
                (2..4).none { hasEvent(restarted, "ps-a4", it) },
                "PS-A4 restarted subscriber received event from disconnected interval",
            )
        }
        println("scenario PS-A4 passed")
    }

    fun runSlowSubscriberIsolation() {
        for (sequence in 0 until 8) {
            publish("all", EventMsg("ps-b1", sequence, "slow-isolation-$sequence"))
        }
        waitForEvent("sub-2", "ps-b1", 7)
        waitForEvent("sub-3", "ps-b1", 7)
        println("scenario PS-B1 passed")
    }

    fun runPublisherRestartRecovery() {
        publish("all", EventMsg("ps-b2", 1, "before-publisher-restart"))
        for (rid in listOf("sub-1", "sub-2", "sub-3")) {
            waitForEvent(rid, "ps-b2", 1)
        }

        postPublisherControl("/shutdown")
        waitDown(publisherUrl)
        ensurePublishFailsWhilePublisherDown()

        processes.startPublisher().use {
            waitHealthy(publisherUrl)
            sleep(500)
            for (sequence in 3..42) {
                publish("all", EventMsg("ps-b2", sequence, "after-publisher-restart-$sequence"))
                sleep(100)
            }
            for (rid in listOf("sub-1", "sub-2", "sub-3")) {
                waitForAnyEventInRange(rid, "ps-b2", 20..42)
            }
        }
        println("scenario PS-B2 passed")
    }

    fun runFanoutBasicDelivery() {
        for (index in 0 until 20) {
            publish("all", EventMsg("warmup", index, "warmup-$index"))
        }
        for (rid in listOf("sub-1", "sub-2", "sub-3")) {
            waitForAnyEvent(rid, "warmup")
        }

        for (sequence in 0 until 12) {
            publish("all", EventMsg("ps-a1", sequence, "fanout-$sequence"))
        }
        for (rid in listOf("sub-1", "sub-2", "sub-3")) {
            for (sequence in 0 until 4) {
                waitForEvent(rid, "ps-a1", sequence)
            }
        }
        println("scenario PS-A1 passed")
    }

    fun runTopicFilter() {
        publish("alpha", EventMsg("ps-a2", 1, "alpha-only"))
        publish("beta", EventMsg("ps-a2", 2, "beta-only"))
        publish("gamma", EventMsg("ps-a2", 3, "gamma-only"))

        waitForEvent("sub-1", "ps-a2", 1)
        waitForEvent("sub-2", "ps-a2", 2)
        waitForEvent("sub-3", "ps-a2", 3)
        sleep(500)

        val sub1 = snapshot("sub-1")
        val sub2 = snapshot("sub-2")
        val sub3 = snapshot("sub-3")
        ensure(hasEvent(sub1, "ps-a2", 1), "PS-A2 sub-1 missed alpha")
        ensure(
            !hasEvent(sub1, "ps-a2", 2) && !hasEvent(sub1, "ps-a2", 3),
            "PS-A2 sub-1 recorded an uninterested topic",
        )
        ensure(hasEvent(sub2, "ps-a2", 2), "PS-A2 sub-2 missed beta")
        ensure(
            !hasEvent(sub2, "ps-a2", 1) && !hasEvent(sub2, "ps-a2", 3),
            "PS-A2 sub-2 recorded an uninterested topic",
        )
        ensure(hasEvent(sub3, "ps-a2", 3), "PS-A2 sub-3 missed gamma")
        ensure(
            !hasEvent(sub3, "ps-a2", 1) && !hasEvent(sub3, "ps-a2", 2),
            "PS-A2 sub-3 recorded an uninterested topic",
        )
        println("scenario PS-A2 passed")
    }

    fun runLateSubscriber() {
        val late = snapshot("sub-3")
        ensure(!hasEvent(late, "prelate", 0), "PS-A3 late subscriber received replayed pre-late event")
        publish("all", EventMsg("ps-a3", 1, "after-late"))
        waitForEvent("sub-3", "ps-a3", 1)
        println("scenario PS-A3 passed")
    }

    fun runMissingPacket() {
        publishMissing("all", EventMsg("ps-c1", 1, "missing-packet"))
        waitForDispatchError("sub-1", "MissingEventMsg")
        publish("all", EventMsg("ps-c1", 2, "normal-after-missing"))
        waitForEvent("sub-1", "ps-c1", 2)
        waitForEvent("sub-2", "ps-c1", 2)
        waitForEvent("sub-3", "ps-c1", 2)
        println("scenario PS-C1 passed")
    }

    private fun publish(topic: String, message: EventMsg) {
        publishAt(publisherUrl, topic, message)
    }

    private fun publishMissing(topic: String, message: EventMsg) {
        postPublisherAt(publisherUrl, "/publish-missing", PublishReq(topic, message))
    }

    private fun postPublisher(path: String, body: PublishReq) {
        postPublisherAt(publisherUrl, path, body)
    }

    private fun publishAt(endpoint: String, topic: String, message: EventMsg) {
        postPublisherAt(endpoint, "/publish", PublishReq(topic, message))
    }

    private fun postPublisherAt(endpoint: String, path: String, body: PublishReq) {
        val request = HttpRequest.newBuilder(URI.create("$endpoint$path"))
            .timeout(Duration.ofSeconds(5))
            .header("Content-Type", "application/json")
            .POST(HttpRequest.BodyPublishers.ofByteArray(json.writeValueAsBytes(body)))
            .build()
        val response = http.send(request, HttpResponse.BodyHandlers.ofString())
        ensure(response.statusCode() in 200..299, "publisher $endpoint returned HTTP ${response.statusCode()}: ${response.body()}")
    }

    private fun postPublisherReserved(): HttpResponse<String> {
        val request = HttpRequest.newBuilder(URI.create("$publisherUrl/publish-reserved"))
            .timeout(Duration.ofSeconds(5))
            .POST(HttpRequest.BodyPublishers.noBody())
            .build()
        return http.send(request, HttpResponse.BodyHandlers.ofString())
    }

    private fun post(url: String) {
        val request = HttpRequest.newBuilder(URI.create(url))
            .timeout(Duration.ofSeconds(5))
            .POST(HttpRequest.BodyPublishers.noBody())
            .build()
        val response = http.send(request, HttpResponse.BodyHandlers.ofString())
        ensure(response.statusCode() in 200..299, "POST $url returned HTTP ${response.statusCode()}: ${response.body()}")
    }

    private fun postPublisherControl(path: String) {
        postPublisherControlAt(publisherUrl, path)
    }

    private fun postPublisherControlAt(endpoint: String, path: String) {
        val request = HttpRequest.newBuilder(URI.create("$endpoint$path"))
            .timeout(Duration.ofSeconds(5))
            .POST(HttpRequest.BodyPublishers.noBody())
            .build()
        val response = http.send(request, HttpResponse.BodyHandlers.ofString())
        ensure(response.statusCode() in 200..299, "publisher returned HTTP ${response.statusCode()}: ${response.body()}")
    }

    private fun ensurePublishFailsWhilePublisherDown() {
        try {
            publish("all", EventMsg("ps-b2", 2, "during-publisher-down"))
            throw IllegalStateException("PS-B2 expected publish attempt to fail while publisher is down.")
        } catch (error: Exception) {
            if (!isConnectionFailure(error)) {
                throw error
            }
        }
    }

    private fun waitForAnyEvent(subscriberRid: String, scenario: String) {
        waitForEvidence(subscriberRid, marker = "EventMsg", scenario = scenario)
    }

    private fun waitForAnyEventInRange(
        subscriberRid: String,
        scenario: String,
        range: IntRange,
    ) {
        waitUntil {
            val current = snapshot(subscriberRid)
            range.any { hasEvent(current, scenario, it) }
        }
    }

    private fun waitForEvent(
        subscriberRid: String,
        scenario: String,
        sequence: Int,
    ) {
        waitForEvidence(subscriberRid, marker = "EventMsg", scenario = scenario, sequence = sequence)
    }

    private fun waitForDispatchError(subscriberRid: String, packetName: String) {
        waitForEvidence(
            subscriberRid,
            marker = "DispatchError",
            valueContains = "HANDLER_MISSING/DROP/$packetName",
        )
    }

    private fun waitForEvidence(
        subscriberRid: String,
        marker: String,
        scenario: String? = null,
        sequence: Int? = null,
        valueContains: String? = null,
    ): EvidenceSnapshot {
        val endpoint = subscriberEndpoint(subscriberRid)
        val query = buildList {
            add("marker=${encode(marker)}")
            scenario?.let { add("scenario=${encode(it)}") }
            sequence?.let { add("sequence=$it") }
            valueContains?.let { add("contains=${encode(it)}") }
            add("timeoutMs=${EVIDENCE_TIMEOUT.toMillis()}")
        }.joinToString("&")
        val request = HttpRequest.newBuilder(URI.create("$endpoint/evidence/wait?$query"))
            .timeout(EVIDENCE_TIMEOUT.plusSeconds(5))
            .GET()
            .build()
        val response = http.send(request, HttpResponse.BodyHandlers.ofString())
        ensure(
            response.statusCode() in 200..299,
            "subscriber $subscriberRid evidence wait failed with HTTP ${response.statusCode()}: ${response.body()}",
        )
        return json.readValue(response.body())
    }

    private fun waitForEventAt(
        endpoint: String,
        scenario: String,
        sequence: Int,
    ): EvidenceSnapshot =
        waitForEvidenceAt(endpoint, marker = "EventMsg", scenario = scenario, sequence = sequence)

    private fun waitForEvidenceAt(
        endpoint: String,
        marker: String,
        scenario: String? = null,
        sequence: Int? = null,
    ): EvidenceSnapshot {
        val query = buildList {
            add("marker=${encode(marker)}")
            scenario?.let { add("scenario=${encode(it)}") }
            sequence?.let { add("sequence=$it") }
            add("timeoutMs=${EVIDENCE_TIMEOUT.toMillis()}")
        }.joinToString("&")
        val request = HttpRequest.newBuilder(URI.create("$endpoint/evidence/wait?$query"))
            .timeout(EVIDENCE_TIMEOUT.plusSeconds(5))
            .GET()
            .build()
        val response = http.send(request, HttpResponse.BodyHandlers.ofString())
        ensure(
            response.statusCode() in 200..299,
            "subscriber evidence wait failed with HTTP ${response.statusCode()}: ${response.body()}",
        )
        return json.readValue(response.body())
    }

    private fun snapshot(subscriberRid: String): EvidenceSnapshot {
        val endpoint = subscriberEndpoint(subscriberRid)
        return snapshotAt(endpoint)
    }

    private fun snapshotAt(endpoint: String): EvidenceSnapshot {
        val request = HttpRequest.newBuilder(URI.create("$endpoint/evidence"))
            .timeout(Duration.ofSeconds(3))
            .GET()
            .build()
        val response = http.send(request, HttpResponse.BodyHandlers.ofString())
        return json.readValue(response.body())
    }

    private fun subscriberEndpoint(subscriberRid: String): String =
        when (subscriberRid) {
            "sub-1" -> options.sub1Http
            "sub-2" -> options.sub2Http
            "sub-3" -> options.sub3Http
            "sub-4" -> options.sub4Http
            else -> throw IllegalArgumentException("unknown subscriber $subscriberRid")
        }

    private fun statusAt(endpoint: String): JsonNode {
        val request = HttpRequest.newBuilder(URI.create("$endpoint/status"))
            .timeout(Duration.ofSeconds(3))
            .GET()
            .build()
        val response = http.send(request, HttpResponse.BodyHandlers.ofString())
        ensure(response.statusCode() in 200..299, "subscriber status returned HTTP ${response.statusCode()}: ${response.body()}")
        return json.readTree(response.body())
    }

    private fun listenerEndpoint(endpoint: String): String {
        val request = HttpRequest.newBuilder(URI.create("$endpoint/status"))
            .timeout(Duration.ofSeconds(3))
            .GET()
            .build()
        val response = http.send(request, HttpResponse.BodyHandlers.ofString())
        ensure(response.statusCode() in 200..299, "publisher status returned HTTP ${response.statusCode()}: ${response.body()}")
        return json.readTree(response.body()).path("listenerEndpoint").asText()
            .also { ensure(it.isNotBlank(), "publisher status omitted listenerEndpoint") }
    }

    private fun publisherIds(endpoint: String): Set<String> =
        statusAt(endpoint).path("publishers")
            .filter { it.path("state").asText() == "READY" }
            .map { it.path("nodeRid").asText() }
            .toSet()

    private fun waitForFanoutReady(endpoint: String, count: Int) {
        waitUntil {
            val status = statusAt(endpoint)
            val readyCount = status.path("readyPublisherCount").asInt(-1)
            if (count == 0) {
                readyCount == 0
            } else {
                status.path("isReady").asBoolean(false) && readyCount >= count
            }
        }
    }

    private fun observerEvidence(endpoint: String): JsonNode {
        val request = HttpRequest.newBuilder(URI.create("$endpoint/observer/evidence"))
            .timeout(Duration.ofSeconds(3))
            .GET()
            .build()
        val response = http.send(request, HttpResponse.BodyHandlers.ofString())
        ensure(response.statusCode() in 200..299, "observer evidence returned HTTP ${response.statusCode()}: ${response.body()}")
        return json.readTree(response.body())
    }

    private fun waitForObserver(endpoint: String, name: String) {
        val request = HttpRequest.newBuilder(URI.create("$endpoint/observer/wait?name=${encode(name)}&timeoutMs=30000"))
            .timeout(Duration.ofSeconds(35))
            .GET()
            .build()
        val response = http.send(request, HttpResponse.BodyHandlers.ofString())
        ensure(response.statusCode() in 200..299, "observer $name did not receive status: ${response.body()}")
    }

    private fun waitForObserverCount(endpoint: String, name: String, expected: Int) {
        waitUntil {
            observerEvidence(endpoint).count { it.path("observer").asText() == name } >= expected
        }
    }

    private fun hasEvent(
        snapshot: EvidenceSnapshot,
        scenario: String,
        sequence: Int,
    ): Boolean =
        snapshot.entries.any {
            it.marker == "EventMsg" &&
                it.scenario == scenario &&
                it.sequence == sequence
        }

    private fun waitUntil(check: () -> Boolean) {
        val deadline = System.nanoTime() + EVIDENCE_TIMEOUT.toNanos()
        var last: Throwable? = null
        while (System.nanoTime() < deadline) {
            try {
                if (check()) {
                    return
                }
            } catch (error: Throwable) {
                last = error
            }
            sleep(100)
        }
        throw IllegalStateException("timed out waiting for evidence", last)
    }

    private fun waitHealthy(endpoint: String) {
        waitUntil {
            try {
                val request = HttpRequest.newBuilder(URI.create("$endpoint/health"))
                    .timeout(Duration.ofSeconds(1))
                    .GET()
                    .build()
                http.send(request, HttpResponse.BodyHandlers.discarding()).statusCode() == 200
            } catch (error: Exception) {
                if (isConnectionFailure(error)) {
                    false
                } else {
                    throw error
                }
            }
        }
    }

    private fun waitDown(endpoint: String) {
        waitUntil {
            try {
                val request = HttpRequest.newBuilder(URI.create("$endpoint/health"))
                    .timeout(Duration.ofSeconds(1))
                    .GET()
                    .build()
                http.send(request, HttpResponse.BodyHandlers.discarding()).statusCode() != 200
            } catch (error: Exception) {
                if (isConnectionFailure(error)) {
                    true
                } else {
                    throw error
                }
            }
        }
    }

    private fun touch(file: String) {
        if (file.isBlank()) {
            return
        }
        Files.writeString(Path.of(file), "ready")
    }

    private fun waitForFile(file: String) {
        if (file.isBlank()) {
            return
        }
        waitUntil { Files.exists(Path.of(file)) }
    }

    private fun ensure(condition: Boolean, message: String) {
        if (!condition) {
            throw IllegalStateException(message)
        }
    }

    private fun required(value: String, option: String): String =
        value.takeIf { it.isNotBlank() } ?: throw IllegalArgumentException("--$option is required for ${options.mode}.")

    private fun isConnectionFailure(error: Throwable): Boolean {
        var current: Throwable? = error
        while (current != null) {
            if (current is ConnectException) {
                return true
            }
            current = current.cause
        }
        return false
    }

    private fun sleep(milliseconds: Long) {
        try {
            Thread.sleep(milliseconds)
        } catch (error: InterruptedException) {
            Thread.currentThread().interrupt()
            throw IllegalStateException("sleep interrupted", error)
        }
    }

    private fun encode(value: String): String =
        URLEncoder.encode(value, Charsets.UTF_8)

    private class PublishReq() {
        var topic: String = ""
        var message: EventMsg = EventMsg()

        constructor(topic: String, message: EventMsg) : this() {
            this.topic = topic
            this.message = message
        }
    }

    companion object {
        private val EVIDENCE_TIMEOUT: Duration = Duration.ofSeconds(30)
    }
}
