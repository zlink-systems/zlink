package systems.zlink.e2e.kotlin.runtimemonitoring.client

import com.fasterxml.jackson.databind.ObjectMapper
import java.io.IOException
import java.net.URI
import java.net.http.HttpClient
import java.net.http.HttpRequest
import java.net.http.HttpResponse
import java.time.Duration

class MonitoringEvidenceClient(
    private val json: ObjectMapper,
) {
    private val http = HttpClient.newHttpClient()

    fun waitForEvent(baseUrl: String, surface: String, expected: Set<String>) {
        waitForEvent(baseUrl, surface, "", expected)
    }

    fun waitForEvent(
        baseUrl: String,
        surface: String,
        sourceName: String,
        expected: Set<String>,
    ) {
        val deadline = System.nanoTime() + Duration.ofSeconds(20).toNanos()
        while (System.nanoTime() < deadline) {
            val observed = events(baseUrl, surface, sourceName)
            if (observed.containsAll(expected)) {
                return
            }
            ScenarioAssert.sleep(200)
        }
        val observed = events(baseUrl, surface, sourceName)
        throw IllegalStateException(
            "missing $surface events $expected at $baseUrl; observed=$observed; evidence=${get("$baseUrl/evidence")}",
        )
    }

    fun waitForAnyEvent(baseUrl: String, surface: String, expected: Set<String>) {
        val deadline = System.nanoTime() + Duration.ofSeconds(20).toNanos()
        while (System.nanoTime() < deadline) {
            val observed = events(baseUrl, surface)
            if (expected.any(observed::contains)) {
                return
            }
            ScenarioAssert.sleep(200)
        }
        val observed = events(baseUrl, surface)
        throw IllegalStateException(
            "missing any $surface event $expected at $baseUrl; observed=$observed; evidence=${get("$baseUrl/evidence")}",
        )
    }

    fun events(baseUrl: String, surface: String): Set<String> {
        return events(baseUrl, surface, "")
    }

    fun events(baseUrl: String, surface: String, sourceName: String): Set<String> {
        return try {
            val root = json.readTree(get("$baseUrl/evidence")).path("entries")
            root.filter { entry ->
                surface == entry.path("surface").asText() &&
                    (sourceName.isBlank() || sourceName == entry.path("sourceName").asText())
            }.mapTo(mutableSetOf()) { entry -> entry.path("event").asText() }
        } catch (_: Exception) {
            emptySet()
        }
    }

    fun post(url: String): String {
        try {
            val response = http.send(
                HttpRequest.newBuilder(URI.create(url))
                    .timeout(Duration.ofSeconds(10))
                    .POST(HttpRequest.BodyPublishers.noBody())
                    .build(),
                HttpResponse.BodyHandlers.ofString(),
            )
            ScenarioAssert.ensure(
                response.statusCode() in 200..299,
                "POST $url returned ${response.statusCode()}",
            )
            return response.body()
        } catch (error: IOException) {
            throw IllegalStateException("POST failed: $url", error)
        } catch (error: InterruptedException) {
            Thread.currentThread().interrupt()
            throw IllegalStateException("POST interrupted: $url", error)
        }
    }

    fun <T> postJson(url: String, responseType: Class<T>): T {
        try {
            val response = http.send(
                HttpRequest.newBuilder(URI.create(url))
                    .timeout(Duration.ofSeconds(10))
                    .header("Content-Type", "application/json")
                    .POST(HttpRequest.BodyPublishers.noBody())
                    .build(),
                HttpResponse.BodyHandlers.ofString(),
            )
            ScenarioAssert.ensure(
                response.statusCode() in 200..299,
                "POST $url returned ${response.statusCode()}: ${response.body()}",
            )
            return json.readValue(response.body(), responseType)
        } catch (error: IOException) {
            throw IllegalStateException("POST failed: $url", error)
        } catch (error: InterruptedException) {
            Thread.currentThread().interrupt()
            throw IllegalStateException("POST interrupted: $url", error)
        }
    }

    fun postRaw(url: String): Pair<Int, String> {
        return try {
            val response = http.send(
                HttpRequest.newBuilder(URI.create(url))
                    .timeout(Duration.ofSeconds(10))
                    .POST(HttpRequest.BodyPublishers.noBody())
                    .build(),
                HttpResponse.BodyHandlers.ofString(),
            )
            response.statusCode() to response.body()
        } catch (error: IOException) {
            throw IllegalStateException("POST failed: $url", error)
        } catch (error: InterruptedException) {
            Thread.currentThread().interrupt()
            throw IllegalStateException("POST interrupted: $url", error)
        }
    }

    fun json(url: String) = json.readTree(get(url))

    fun postBestEffort(url: String) {
        try {
            post(url)
        } catch (_: RuntimeException) {
        }
    }

    private fun get(url: String): String {
        try {
            val response = http.send(
                HttpRequest.newBuilder(URI.create(url))
                    .timeout(Duration.ofSeconds(3))
                    .GET()
                    .build(),
                HttpResponse.BodyHandlers.ofString(),
            )
            ScenarioAssert.ensure(
                response.statusCode() in 200..299,
                "GET $url returned ${response.statusCode()}",
            )
            return response.body()
        } catch (error: IOException) {
            throw IllegalStateException("GET failed: $url", error)
        } catch (error: InterruptedException) {
            Thread.currentThread().interrupt()
            throw IllegalStateException("GET interrupted: $url", error)
        }
    }
}
