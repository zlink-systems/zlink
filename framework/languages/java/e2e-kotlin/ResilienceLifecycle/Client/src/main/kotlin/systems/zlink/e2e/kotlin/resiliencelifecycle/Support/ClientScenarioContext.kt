package systems.zlink.e2e.kotlin.resiliencelifecycle

import com.fasterxml.jackson.databind.JsonNode
import com.fasterxml.jackson.databind.ObjectMapper
import java.io.IOException
import java.net.URI
import java.net.http.HttpClient
import java.net.http.HttpRequest
import java.net.http.HttpResponse
import java.nio.file.Files
import java.nio.file.Path
import java.time.Duration
import java.util.concurrent.TimeUnit

class ClientScenarioContext(
    val json: ObjectMapper,
    val options: ClientOptions,
) {
    private val http: HttpClient = HttpClient.newHttpClient()

    fun collectProviders(prefix: String, attempts: Int, expectedCount: Int): MutableSet<String> {
        val providers = linkedSetOf<String>()
        var index = 0
        while (index < attempts && providers.size < expectedCount) {
            val reply = requestWork("$prefix-$index")
            ensure(reply.value() == "work:$prefix-$index", "reply payload mismatch for $prefix-$index")
            providers.add(reply.providerRid())
            index++
        }
        return providers
    }

    fun collectStableProvidersWithout(prefix: String, forbidden: String, required: String): Set<String> {
        repeat(30) { window ->
            val providers = collectProviders("$prefix-window-$window", 5, 1)
            if (!providers.contains(forbidden) && providers.contains(required)) {
                return providers
            }
            sleep(300)
        }
        throw IllegalStateException("$prefix did not converge away from $forbidden to $required")
    }

    fun waitForTopology(expectedRouters: Int) {
        postJson(
            "${options.consumerHttpEndpoint}/topology/wait",
            Contracts.TopologyWaitReq(expectedRouters, null, null),
            Contracts.TopologyWaitRes::class.java,
            Duration.ofSeconds(15),
        )
    }

    fun waitForTopologyEndpoint(routingId: String, endpoint: String) {
        postJson(
            "${options.consumerHttpEndpoint}/topology/wait",
            Contracts.TopologyWaitReq(1, routingId, endpoint),
            Contracts.TopologyWaitRes::class.java,
            Duration.ofSeconds(20),
        )
    }

    fun waitForTopologyMissing(routingId: String) {
        postJson(
            "${options.consumerHttpEndpoint}/topology/wait",
            Contracts.TopologyWaitReq(0, routingId, null),
            Contracts.TopologyWaitRes::class.java,
            Duration.ofSeconds(20),
        )
    }

    fun readTopology(): Contracts.TopologyReadRes =
        json.readValue(
            get("${options.consumerHttpEndpoint}/topology/read"),
            Contracts.TopologyReadRes::class.java,
        )

    fun waitForWeight(baseUrl: String, expected: Int) {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5)
        while (System.nanoTime() < deadline) {
            try {
                val node = json.readTree(get("$baseUrl/admin/weight"))
                if (node.path("weight").asInt(-1) == expected) {
                    return
                }
            } catch (_: Exception) {
            }
            sleep(100)
        }
        throw IllegalStateException("weight did not become $expected for $baseUrl")
    }

    fun waitForEvidence(baseUrl: String, marker: String) {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(10)
        while (System.nanoTime() < deadline) {
            try {
                val entries = json.readTree(get("$baseUrl/evidence")).path("entries")
                if (entries.isArray) {
                    for (entry in entries) {
                        if (entry.path("marker").asText() == marker) {
                            return
                        }
                    }
                }
            } catch (_: Exception) {
            }
            sleep(100)
        }
        throw IllegalStateException("marker $marker was not observed at $baseUrl")
    }

    fun waitForEvidenceAny(marker: String, vararg baseUrls: String) {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(10)
        while (System.nanoTime() < deadline) {
            for (baseUrl in baseUrls) {
                try {
                    val entries = json.readTree(get("$baseUrl/evidence")).path("entries")
                    if (entries.isArray) {
                        for (entry in entries) {
                            if (entry.path("marker").asText() == marker) {
                                return
                            }
                        }
                    }
                } catch (_: Exception) {
                }
            }
            sleep(100)
        }
        throw IllegalStateException("marker $marker was not observed at any provider")
    }

    fun waitForEvidenceValueAny(marker: String, value: String, vararg baseUrls: String) {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(10)
        while (System.nanoTime() < deadline) {
            for (baseUrl in baseUrls) {
                try {
                    val entries = json.readTree(get("$baseUrl/evidence")).path("entries")
                    if (entries.isArray) {
                        for (entry in entries) {
                            if (
                                entry.path("marker").asText() == marker &&
                                entry.path("value").asText() == value
                            ) {
                                return
                            }
                        }
                    }
                } catch (_: Exception) {
                }
            }
            sleep(100)
        }
        throw IllegalStateException("marker $marker value $value was not observed at any provider")
    }

    fun waitForEvidenceValue(baseUrl: String, marker: String, value: String) {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(10)
        while (System.nanoTime() < deadline) {
            try {
                val entries = json.readTree(get("$baseUrl/evidence")).path("entries")
                if (entries.isArray) {
                    for (entry in entries) {
                        if (
                            entry.path("marker").asText() == marker &&
                            entry.path("value").asText() == value
                        ) {
                            return
                        }
                    }
                }
            } catch (_: Exception) {
            }
            sleep(100)
        }
        throw IllegalStateException("marker $marker value $value was not observed at $baseUrl")
    }

    fun waitForDispatchErrorAny(packetName: String, vararg baseUrls: String) {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(30)
        while (System.nanoTime() < deadline) {
            for (baseUrl in baseUrls) {
                try {
                    val entries: JsonNode = json.readTree(get("$baseUrl/evidence")).path("entries")
                    if (entries.isArray) {
                        for (entry in entries) {
                            val marker = entry.path("marker").asText()
                            val value = entry.path("value").asText()
                            if (
                                marker == "DispatchError" &&
                                value.contains("HANDLER_MISSING") &&
                                value.contains("REPLY_ERROR") &&
                                value.contains(packetName)
                            ) {
                                return
                            }
                        }
                    }
                } catch (_: Exception) {
                }
            }
            sleep(100)
        }
        throw IllegalStateException("dispatch error marker for $packetName was not observed")
    }

    fun expectSingleProviderDownFailure(scenario: String, value: String) {
        try {
            requestWork(value, Duration.ofMillis(700))
            throw IllegalStateException("$scenario down-window request unexpectedly completed")
        } catch (_: RuntimeException) {
            // The scenario only requires a public failure while the sole admissible provider is down.
        }
    }

    fun requestWork(value: String, timeout: Duration = Duration.ofSeconds(3)): Contracts.WorkRes =
        postJson(
            "${options.consumerHttpEndpoint}/profile/request",
            Contracts.WorkReq(value),
            Contracts.WorkRes::class.java,
            timeout.plusSeconds(1),
            "timeoutMillis" to timeout.toMillis().toString(),
        )

    fun submitWork(value: String, timeout: Duration = Duration.ofSeconds(3)): java.util.concurrent.CompletableFuture<Contracts.WorkRes> =
        java.util.concurrent.CompletableFuture.supplyAsync { requestWork(value, timeout) }

    fun sendWork(value: String) {
        postJson(
            "${options.consumerHttpEndpoint}/profile/send",
            Contracts.WorkMsg(value),
            Map::class.java,
            Duration.ofSeconds(5),
        )
    }

    fun requestUnhandled(value: String) {
        postJson(
            "${options.consumerHttpEndpoint}/profile/unhandled",
            Contracts.UnhandledReq(value),
            Contracts.WorkRes::class.java,
            Duration.ofSeconds(5),
        )
    }

    fun requestUnhandledRaw(value: String): HttpJsonResult =
        postJsonRaw(
            "${options.consumerHttpEndpoint}/profile/unhandled",
            Contracts.UnhandledReq(value),
            Duration.ofSeconds(5),
        )

    fun adminA(): String =
        options.httpAEndpoint ?: throw IllegalStateException("e2e.http.a.endpoint is required")

    fun adminB(): String =
        options.httpBEndpoint ?: throw IllegalStateException("e2e.http.b.endpoint is required")

    fun signal(name: String) {
        val dir = controlDir()
        try {
            Files.createDirectories(dir)
            Files.writeString(dir.resolve(name), "ok\n")
        } catch (error: IOException) {
            throw IllegalStateException("failed to write control signal $name", error)
        }
    }

    fun waitForSignal(name: String) {
        val file = controlDir().resolve(name)
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(30)
        while (System.nanoTime() < deadline) {
            if (Files.exists(file)) {
                return
            }
            sleep(100)
        }
        throw IllegalStateException("control signal was not observed: $name")
    }

    fun hasSignal(name: String): Boolean = Files.exists(controlDir().resolve(name))

    private fun controlDir(): Path {
        val value = options.controlDir ?: throw IllegalStateException("e2e.control.dir is required")
        if (value.isBlank()) {
            throw IllegalStateException("e2e.control.dir is required")
        }
        return Path.of(value)
    }

    fun get(url: String): String {
        try {
            val request = HttpRequest.newBuilder(URI.create(url))
                .timeout(Duration.ofSeconds(5))
                .GET()
                .build()
            val response = http.send(request, HttpResponse.BodyHandlers.ofString())
            ensure(response.statusCode() in 200..299, "GET $url returned ${response.statusCode()}")
            return response.body()
        } catch (error: IOException) {
            throw IllegalStateException("GET failed: $url", error)
        } catch (error: InterruptedException) {
            Thread.currentThread().interrupt()
            throw IllegalStateException("GET interrupted: $url", error)
        }
    }

    fun post(url: String) {
        try {
            val request = HttpRequest.newBuilder(URI.create(url))
                .timeout(Duration.ofSeconds(5))
                .POST(HttpRequest.BodyPublishers.noBody())
                .build()
            val response = http.send(request, HttpResponse.BodyHandlers.ofString())
            ensure(response.statusCode() in 200..299, "POST $url returned ${response.statusCode()}")
        } catch (error: IOException) {
            throw IllegalStateException("POST failed: $url", error)
        } catch (error: InterruptedException) {
            Thread.currentThread().interrupt()
            throw IllegalStateException("POST interrupted: $url", error)
        }
    }

    private fun <T> postJson(
        url: String,
        body: Any,
        responseType: Class<T>,
        timeout: Duration,
        vararg query: Pair<String, String>,
    ): T {
        try {
            val uri = if (query.isEmpty()) {
                URI.create(url)
            } else {
                URI.create(url + "?" + query.joinToString("&") { "${it.first}=${it.second}" })
            }
            val request = HttpRequest.newBuilder(uri)
                .timeout(timeout)
                .header("Content-Type", "application/json")
                .POST(HttpRequest.BodyPublishers.ofByteArray(json.writeValueAsBytes(body)))
                .build()
            val response = http.send(request, HttpResponse.BodyHandlers.ofString())
            ensure(response.statusCode() in 200..299, "POST $uri returned ${response.statusCode()}: ${response.body()}")
            return json.readValue(response.body(), responseType)
        } catch (error: IOException) {
            throw IllegalStateException("POST failed: $url", error)
        } catch (error: InterruptedException) {
            Thread.currentThread().interrupt()
            throw IllegalStateException("POST interrupted: $url", error)
        }
    }

    private fun postJsonRaw(
        url: String,
        body: Any,
        timeout: Duration,
    ): HttpJsonResult {
        try {
            val uri = URI.create(url)
            val request = HttpRequest.newBuilder(uri)
                .timeout(timeout)
                .header("Content-Type", "application/json")
                .POST(HttpRequest.BodyPublishers.ofByteArray(json.writeValueAsBytes(body)))
                .build()
            val response = http.send(request, HttpResponse.BodyHandlers.ofString())
            return HttpJsonResult(response.statusCode(), response.body())
        } catch (error: IOException) {
            throw IllegalStateException("POST failed: $url", error)
        } catch (error: InterruptedException) {
            Thread.currentThread().interrupt()
            throw IllegalStateException("POST interrupted: $url", error)
        }
    }
}

data class HttpJsonResult(
    val status: Int,
    val body: String,
)

fun sleep(millis: Long) {
    try {
        Thread.sleep(millis)
    } catch (error: InterruptedException) {
        Thread.currentThread().interrupt()
        throw IllegalStateException("interrupted", error)
    }
}

fun ensure(condition: Boolean, message: String) {
    if (!condition) {
        throw IllegalStateException(message)
    }
}
