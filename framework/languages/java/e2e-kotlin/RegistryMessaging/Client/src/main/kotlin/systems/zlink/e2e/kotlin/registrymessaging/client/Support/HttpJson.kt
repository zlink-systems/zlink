package systems.zlink.e2e.kotlin.registrymessaging.client.Support

import com.fasterxml.jackson.module.kotlin.jacksonObjectMapper
import com.fasterxml.jackson.module.kotlin.readValue
import java.net.URI
import java.net.http.HttpClient
import java.net.http.HttpRequest
import java.net.http.HttpResponse
import java.time.Duration

class HttpJson(
    @PublishedApi
    internal val baseUrl: String,
) : AutoCloseable {
    @PublishedApi
    internal val mapper = jacksonObjectMapper()

    @PublishedApi
    internal val client = HttpClient.newBuilder()
        .connectTimeout(Duration.ofSeconds(5))
        .build()

    inline fun <reified T> get(path: String): T {
        val request = HttpRequest.newBuilder(URI.create(baseUrl + path))
            .timeout(Duration.ofMinutes(5))
            .GET()
            .build()
        return send<T>(request)
    }

    inline fun <reified T> post(path: String, body: Any? = emptyMap<String, String>()): T {
        val bytes = mapper.writeValueAsBytes(body)
        val request = HttpRequest.newBuilder(URI.create(baseUrl + path))
            .timeout(Duration.ofMinutes(5))
            .header("content-type", "application/json")
            .POST(HttpRequest.BodyPublishers.ofByteArray(bytes))
            .build()
        return send<T>(request)
    }

    @PublishedApi
    internal inline fun <reified T> send(request: HttpRequest): T {
        val response = client.send(request, HttpResponse.BodyHandlers.ofString())
        if (response.statusCode() !in 200..299) {
            throw IllegalStateException("HTTP ${response.statusCode()} from ${request.uri()}: ${response.body()}")
        }
        return try {
            mapper.readValue(response.body())
        } catch (error: Exception) {
            throw IllegalStateException(
                "Invalid JSON response from ${request.uri()}: ${response.body()}",
                error,
            )
        }
    }

    override fun close() {
    }
}
