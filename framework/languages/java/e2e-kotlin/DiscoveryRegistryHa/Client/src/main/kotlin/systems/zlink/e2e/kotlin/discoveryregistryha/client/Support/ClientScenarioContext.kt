package systems.zlink.e2e.kotlin.discoveryregistryha.client.Support

import com.fasterxml.jackson.databind.JsonNode
import com.fasterxml.jackson.databind.ObjectMapper
import java.net.URI
import java.net.http.HttpClient
import java.net.http.HttpRequest
import java.net.http.HttpResponse
import java.time.Duration
import java.time.Instant
import java.util.concurrent.CompletableFuture
import systems.zlink.e2e.kotlin.discoveryregistryha.ClientOptions
import systems.zlink.e2e.kotlin.discoveryregistryha.Contracts

class ClientScenarioContext(
    private val json: ObjectMapper,
    val options: ClientOptions,
) {
    private val http = HttpClient.newHttpClient()

    fun runStoreFailureBaseline() {
        waitForLivePeerRows()
        waitForHealthyStatus()
        val providers = requestUntilAnyProvider()
        println("scenario ${options.scenario()} passed providers=$providers")
    }

    fun runStoreFailurePollingFallback() {
        waitForPollingOnlyStatus()
        waitForLivePeerRows()
        for (absentRid in options.expectedAbsentRids()) {
            waitForPeerRowsExcluding(
                absentRid,
                Duration.ofMillis(options.locationPollingMillis() * 8 + options.locationLeaseTtlMillis()),
            )
        }
        val providers = requestUntilAnyProvider()
        println("scenario ${options.scenario()} passed providers=$providers")
    }

    fun runStoreFailureCrashLeaseExpiry() {
        waitForPeerRowsExcluding(
            options.deadRid(),
            Duration.ofMillis(options.locationLeaseTtlMillis() * 2 + options.locationPollingMillis() * 4),
        )
        sleep(options.locationPollingMillis() * 4)
        val providers = requestSurvivorOnly()
        println("scenario ${options.scenario()} passed providers=$providers")
    }

    fun runStoreFailureGracefulRemoval() {
        val started = Instant.now()
        waitForPeerRowsExcluding(options.deadRid(), Duration.ofMillis(options.locationLeaseTtlMillis()))
        val elapsed = Duration.between(started, Instant.now())
        ScenarioAssert.that(
            elapsed < Duration.ofMillis(options.locationLeaseTtlMillis()),
            "SF-C2 row removal did not beat owner lease TTL: $elapsed",
        )
        sleep(options.locationPollingMillis() * 4)
        val providers = requestSurvivorOnly("SF-C2", "sf-c2", 12)
        println("scenario ${options.scenario()} passed providers=$providers")
    }

    fun runStoreFailureFailStaticOutage() {
        val providers = driveRequests(
            "sf-b1-outage",
            Duration.ofMillis((options.locationLeaseTtlMillis() * 7) / 10),
            "SF-B1",
        )
        waitForUnhealthyStatus("SF-B1")
        waitForOwnerLeaseFailure("SF-B1")
        println("scenario ${options.scenario()} passed providers=$providers")
    }

    fun runStoreFailureGraceExceeded() {
        val providers = driveRequests(
            "sf-b2-grace",
            Duration.ofMillis(options.locationStoreFailureGraceMillis() + options.locationHeartbeatMillis() * 2),
            "SF-B2",
        )
        waitForUnhealthyStatus("SF-B2")
        println("scenario ${options.scenario()} passed providers=$providers")
    }

    fun runStoreFailureRecovered(scenarioName: String) {
        waitForRecoveredStatus(scenarioName)
        val providers = requestUntilAnyProvider()
        println("scenario ${options.scenario()} passed providers=$providers")
    }

    fun runStoreFailureRecoveredWithPeers(scenarioName: String) {
        waitForRecoveredStatus(scenarioName)
        waitForLivePeerRows()
        val providers = requestUntilAnyProvider()
        println("scenario ${options.scenario()} passed providers=$providers")
    }

    fun runStoreFailureShortOutageTraffic() {
        val providers = driveRequests(
            "sf-d1",
            Duration.ofMillis(options.locationLeaseTtlMillis() * 2),
            "SF-D1",
        )
        println("scenario ${options.scenario()} passed providers=$providers")
    }

    fun runStoreFailureLongOutageRecovery() {
        val traffic = driveTolerantRequests(
            "sf-d2",
            Duration.ofMillis(options.locationLeaseTtlMillis() * 2 + options.locationHeartbeatMillis() * 4),
            "SF-D2",
        )
        ScenarioAssert.that(
            traffic.contains("api-a"),
            "SF-D2 no request was served by the surviving provider",
        )
        waitForLivePeerRows()
        waitForPeerRowsExcluding(
            options.deadRid(),
            Duration.ofMillis(options.locationLeaseTtlMillis() * 2 + options.locationHeartbeatMillis() * 4),
        )
        val providers = requestSurvivorOnly("SF-D2", "sf-d2-after", 8)
        println("scenario ${options.scenario()} passed providers=$providers")
    }

    fun runStoreFailureStatusHealthy() {
        val status = waitForHealthyStatus()
        ScenarioAssert.that(status.path("lastRefreshAt").asText("").isNotBlank(), "SF-D3 healthy status did not expose last refresh time")
        ScenarioAssert.that(status.path("ownerLeaseRenewedAt").asText("").isNotBlank(), "SF-D3 healthy status did not expose owner lease renewal time")
        val providers = requestUntilAnyProvider()
        println("scenario ${options.scenario()} passed providers=$providers")
    }

    fun runStoreFailureStatusOutage() {
        val status = waitForStatus(
            Duration.ofMillis(options.locationHeartbeatMillis() * 8),
            { current ->
                !current.path("storeHealthy").asBoolean(true) &&
                    !current.path("ownerLeaseHealthy").asBoolean(true) &&
                    current.path("lastError").asText("").isNotBlank()
            },
            "SF-D3 outage did not surface as unhealthy status with owner lease failure",
        )
        ScenarioAssert.that(!status.path("watchEnabled").asBoolean(true), "SF-D3 outage status did not expose polling-mode watch state")
        ScenarioAssert.that(status.path("pollingIntervalMillis").asLong(0) > 0, "SF-D3 outage status did not expose polling interval")
        println("scenario ${options.scenario()} passed providers=[]")
    }

    fun runStoreFailureStatusRecovered() {
        val status = waitForRecoveredStatus("SF-D3")
        ScenarioAssert.that(status.path("lastRefreshAt").asText("").isNotBlank(), "SF-D3 recovered status did not expose last refresh time")
        ScenarioAssert.that(status.path("ownerLeaseRenewedAt").asText("").isNotBlank(), "SF-D3 recovered status did not expose owner lease renewal time")
        val providers = requestUntilAnyProvider()
        println("scenario ${options.scenario()} passed providers=$providers")
    }

    fun runStoreFailureStoreDelayNonBlocking() {
        waitForLivePeerRows()
        val baselineP99 = percentileMillis(measureRequests("sf-e1-baseline", 10), 0.99)
        val storeDelayMilliseconds = 1_000
        setStoreDelay(storeDelayMilliseconds)
        try {
            val delayedStoreRead = CompletableFuture.supplyAsync(::measureStoreRead)
            sleep(150)
            val concurrentP99 = percentileMillis(measureRequests("sf-e1-concurrent", 12), 0.99)
            val delayedStoreReadMilliseconds = delayedStoreRead.join()
            val budget = maxOf(baselineP99 * 8, 750)

            ScenarioAssert.that(
                delayedStoreReadMilliseconds >= (storeDelayMilliseconds * 0.75).toLong(),
                "SF-E1 delayed store read finished too quickly: $delayedStoreReadMilliseconds ms",
            )
            ScenarioAssert.that(
                concurrentP99 <= budget,
                "SF-E1 unrelated request p99 grew too much during store delay. " +
                    "baseline=$baselineP99 ms concurrent=$concurrentP99 ms budget=$budget ms",
            )
            val providers = requestUntilAnyProvider()
            println("scenario SF-E1 passed providers=$providers")
        } finally {
            setStoreDelay(0)
        }
    }

    private fun requestUntilAnyProvider(): Set<String> {
        requireConsumerEndpoint()
        for (index in 0 until 60) {
            val value = "sf-a1-msg-$index"
            try {
                val reply = postWorkRequest(value, waitForRoute = true)
                ScenarioAssert.that(reply.value == "work:$value", "reply payload mismatch")
                ScenarioAssert.that(options.expectedRids().contains(reply.providerRid), "unexpected provider ${reply.providerRid}")
                return setOf(reply.providerRid)
            } catch (error: Exception) {
                if (index == 59) {
                    throw IllegalStateException("consumer request failed", error)
                }
                sleep(100)
            }
        }
        throw IllegalStateException("consumer request did not complete")
    }

    private fun requestSurvivorOnly(
        scenarioName: String = "SF-C1",
        markerPrefix: String = "sf-c1",
        count: Int = 12,
    ): Set<String> {
        requireConsumerEndpoint()
        val survivors = options.expectedRids()
        ScenarioAssert.that(survivors.isNotEmpty(), "$scenarioName requires at least one survivor rid")
        val providers = linkedSetOf<String>()
        val started = Instant.now()
        for (index in 0 until count) {
            val value = "$markerPrefix-msg-$index"
            val reply = postWorkRequest(value)
            ScenarioAssert.that(reply.value == "work:$value", "$scenarioName reply payload mismatch")
            ScenarioAssert.that(survivors.contains(reply.providerRid), "$scenarioName request reached non-survivor provider ${reply.providerRid}")
            ScenarioAssert.that(reply.providerRid != options.deadRid(), "$scenarioName request reached crashed provider ${options.deadRid()}")
            providers.add(reply.providerRid)
        }
        val elapsed = Duration.between(started, Instant.now())
        ScenarioAssert.that(elapsed < Duration.ofSeconds(12), "$scenarioName requests were slow, suggesting repeated routing to the dead provider: $elapsed")
        return providers
    }

    private fun driveRequests(markerPrefix: String, window: Duration, scenarioName: String): Set<String> {
        requireConsumerEndpoint()
        val providers = linkedSetOf<String>()
        val deadline = Instant.now().plus(window)
        var index = 0
        while (Instant.now().isBefore(deadline)) {
            val value = "$markerPrefix-msg-$index"
            val reply = postWorkRequest(value)
            ScenarioAssert.that(reply.value == "work:$value", "$scenarioName reply payload mismatch")
            ScenarioAssert.that(options.expectedRids().contains(reply.providerRid), "$scenarioName unexpected provider ${reply.providerRid}")
            providers.add(reply.providerRid)
            index++
            sleep(150)
        }
        ScenarioAssert.that(index > 0, "$scenarioName produced no request traffic")
        return providers
    }

    private fun driveTolerantRequests(markerPrefix: String, window: Duration, scenarioName: String): Set<String> {
        requireConsumerEndpoint()
        val providers = linkedSetOf<String>()
        val deadline = Instant.now().plus(window)
        var lastSuccess = Instant.now()
        var maxGap = Duration.ZERO
        var index = 0
        while (Instant.now().isBefore(deadline)) {
            val value = "$markerPrefix-msg-$index"
            try {
                val reply = postWorkRequest(value)
                ScenarioAssert.that(reply.value == "work:$value", "$scenarioName reply payload mismatch")
                providers.add(reply.providerRid)
                val gap = Duration.between(lastSuccess, Instant.now())
                if (gap > maxGap) {
                    maxGap = gap
                }
                lastSuccess = Instant.now()
            } catch (_: Exception) {
                // A request may land on the provider killed during the store outage.
            }
            index++
            sleep(150)
        }
        val finalGap = Duration.between(lastSuccess, Instant.now())
        if (finalGap > maxGap) {
            maxGap = finalGap
        }
        ScenarioAssert.that(providers.isNotEmpty(), "$scenarioName produced no successful request traffic")
        ScenarioAssert.that(maxGap < Duration.ofMillis(options.locationLeaseTtlMillis() * 2), "$scenarioName successful traffic stalled for $maxGap")
        return providers
    }

    private fun postWorkRequest(value: String, waitForRoute: Boolean = false): Contracts.WorkRes {
        val endpoint = requireConsumerEndpoint()
        val body = json.writeValueAsString(Contracts.WorkReq(value))
        val path = if (waitForRoute) "/profile/request/wait" else "/profile/request"
        val responseBody = post("$endpoint$path", body)
        return json.readValue(responseBody, Contracts.WorkRes::class.java)
    }

    private fun measureStoreRead(): Long {
        val started = Instant.now()
        get("${requireConsumerEndpoint()}/locations/peers")
        return Duration.between(started, Instant.now()).toMillis()
    }

    private fun measureRequests(markerPrefix: String, count: Int): List<Long> =
        (0 until count).map { index ->
            val started = Instant.now()
            val value = "$markerPrefix-msg-$index"
            val reply = postWorkRequest(value)
            ScenarioAssert.that(reply.value == "work:$value", "SF-E1 reply payload mismatch")
            ScenarioAssert.that(options.expectedRids().contains(reply.providerRid), "SF-E1 unexpected provider ${reply.providerRid}")
            Duration.between(started, Instant.now()).toMillis()
        }

    private fun setStoreDelay(delayMilliseconds: Int) {
        post(
            "${requireConsumerEndpoint()}/admin/store-delay",
            json.writeValueAsString(Contracts.StoreDelayReq(delayMilliseconds)),
        )
    }

    private fun percentileMillis(values: List<Long>, percentile: Double): Long {
        val sorted = values.sorted()
        val index = kotlin.math.ceil(percentile * sorted.size).toInt() - 1
        return sorted[index.coerceIn(0, sorted.lastIndex)]
    }

    private fun waitForLivePeerRows() {
        val expected = options.expectedRids().toSet()
        val observed = waitUntil(Duration.ofSeconds(20), "consumer location query missing expected peer rows ${options.expectedRids()}") {
            val rids = peerRids()
            if (containsAllRids(rids, expected)) rids else null
        }
        ScenarioAssert.that(containsAllRids(observed, expected), "location rows missing expected providers $expected: $observed")
    }

    private fun waitForPeerRowsExcluding(deadRid: String, timeout: Duration) {
        val survivors = options.expectedRids().toSet()
        val observed = waitUntil(timeout, "consumer location query still includes removed provider $deadRid") {
            val rids = peerRids()
            val hasSurvivors = containsAllRids(rids, survivors)
            val hasDead = rids.any { it == deadRid || it.contains(deadRid) }
            if (hasSurvivors && !hasDead) rids else null
        }
        ScenarioAssert.that(containsAllRids(observed, survivors), "location rows missing survivor providers $survivors: $observed")
        ScenarioAssert.that(observed.none { it == deadRid || it.contains(deadRid) }, "location rows still include crashed provider $deadRid: $observed")
    }

    private fun peerRids(): Set<String> =
        try {
            json.readTree(get("${requireConsumerEndpoint()}/locations/peers"))
                .mapNotNull { entry -> entry.path("nodeRid").asText("").takeIf(String::isNotBlank) }
                .toSet()
        } catch (_: Exception) {
            emptySet()
        }

    private fun waitForHealthyStatus(): JsonNode {
        var status: JsonNode? = null
        var lastError: Exception? = null
        val deadline = System.nanoTime() + Duration.ofSeconds(20).toNanos()
        while (System.nanoTime() < deadline) {
            try {
                status = json.readTree(get("${requireConsumerEndpoint()}/locations/status"))
                if (status.path("storeHealthy").asBoolean(false)) {
                    break
                }
            } catch (error: Exception) {
                lastError = error
            }
            sleep(100)
        }
        ScenarioAssert.that(status?.path("storeHealthy")?.asBoolean(false) == true, "consumer location runtime status did not become healthy; lastStatus=$status, lastError=$lastError")
        ScenarioAssert.that(status?.path("ownerLeaseHealthy")?.asBoolean(false) == true, "owner lease status is not healthy")
        return status!!
    }

    private fun waitForPollingOnlyStatus() {
        val status = waitUntil(Duration.ofSeconds(10), "consumer location runtime status did not report polling-only mode") {
            val current = json.readTree(get("${requireConsumerEndpoint()}/locations/status"))
            val healthy = current.path("storeHealthy").asBoolean(false)
            val watchEnabled = current.path("watchEnabled").asBoolean(true)
            val pollingMillis = current.path("pollingIntervalMillis").asLong(0)
            if (healthy && !watchEnabled && pollingMillis > 0) current else null
        }
        ScenarioAssert.that(status.path("storeHealthy").asBoolean(false), "SF-A2 store status is not healthy")
        ScenarioAssert.that(!status.path("watchEnabled").asBoolean(true), "SF-A2 polling-only consumer unexpectedly reports watch enabled")
    }

    private fun waitForUnhealthyStatus(scenarioName: String) {
        val status = waitForStatus(
            Duration.ofMillis(options.locationHeartbeatMillis() * 8),
            { current -> !current.path("storeHealthy").asBoolean(true) && current.path("lastError").asText("").isNotBlank() },
            "$scenarioName did not record store outage",
        )
        ScenarioAssert.that(!status.path("storeHealthy").asBoolean(true), "$scenarioName store status is not unhealthy")
    }

    private fun waitForOwnerLeaseFailure(scenarioName: String) {
        waitForStatus(
            Duration.ofMillis(options.locationHeartbeatMillis() * 8),
            { current -> !current.path("ownerLeaseHealthy").asBoolean(true) },
            "$scenarioName owner lease heartbeat failure did not surface",
        )
    }

    private fun waitForRecoveredStatus(scenarioName: String): JsonNode =
        waitForStatus(
            Duration.ofMillis(options.locationHeartbeatMillis() * 10),
            { current -> current.path("storeHealthy").asBoolean(false) && current.path("ownerLeaseHealthy").asBoolean(false) },
            "$scenarioName status did not recover after store outage",
        )

    private fun waitForStatus(timeout: Duration, accept: (JsonNode) -> Boolean, failureMessage: String): JsonNode =
        waitUntil(timeout, failureMessage) {
            try {
                val current = json.readTree(get("${requireConsumerEndpoint()}/locations/status"))
                if (accept(current)) current else null
            } catch (_: Exception) {
                null
            }
        }

    private fun <T> waitUntil(timeout: Duration, failureMessage: String, action: () -> T?): T {
        val deadline = System.nanoTime() + timeout.toNanos()
        while (System.nanoTime() < deadline) {
            val result = action()
            if (result != null) {
                return result
            }
            sleep(100)
        }
        throw IllegalStateException(failureMessage)
    }

    private fun containsAllRids(observed: Set<String>, expected: Set<String>): Boolean =
        expected.all { rid -> observed.any { value -> value == rid || value.contains(rid) } }

    private fun requireConsumerEndpoint(): String {
        val endpoint = options.consumerHttpEndpoint()
        ScenarioAssert.that(!endpoint.isNullOrBlank(), "consumer HTTP endpoint is required")
        return endpoint
    }

    private fun get(url: String): String =
        try {
            val response = http.send(
                HttpRequest.newBuilder(URI.create(url))
                    .timeout(Duration.ofSeconds(3))
                    .GET()
                    .build(),
                HttpResponse.BodyHandlers.ofString(),
            )
            ScenarioAssert.that(response.statusCode() in 200..299, "GET $url returned ${response.statusCode()}")
            response.body()
        } catch (error: java.io.IOException) {
            throw IllegalStateException("GET failed: $url", error)
        } catch (error: InterruptedException) {
            Thread.currentThread().interrupt()
            throw IllegalStateException("GET interrupted: $url", error)
        }

    private fun post(url: String, body: String): String =
        try {
            val response = http.send(
                HttpRequest.newBuilder(URI.create(url))
                    .timeout(Duration.ofSeconds(12))
                    .header("Content-Type", "application/json")
                    .POST(HttpRequest.BodyPublishers.ofString(body))
                    .build(),
                HttpResponse.BodyHandlers.ofString(),
            )
            ScenarioAssert.that(response.statusCode() in 200..299, "POST $url returned ${response.statusCode()}: ${response.body()}")
            response.body()
        } catch (error: java.io.IOException) {
            throw IllegalStateException("POST failed: $url", error)
        } catch (error: InterruptedException) {
            Thread.currentThread().interrupt()
            throw IllegalStateException("POST interrupted: $url", error)
        }

    private fun sleep(millis: Long) {
        try {
            Thread.sleep(millis)
        } catch (error: InterruptedException) {
            Thread.currentThread().interrupt()
            throw IllegalStateException("interrupted", error)
        }
    }
}
