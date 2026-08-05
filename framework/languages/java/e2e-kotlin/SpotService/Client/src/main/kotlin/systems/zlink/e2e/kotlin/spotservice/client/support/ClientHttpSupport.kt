package systems.zlink.e2e.kotlin.spotservice.client.support

import com.fasterxml.jackson.databind.ObjectMapper
import java.net.HttpURLConnection
import java.net.URI
import java.nio.charset.StandardCharsets

internal fun postAdmin(endpoint: String, pathAndQuery: String): String {
    val connection = URI.create(endpoint.trimEnd('/') + pathAndQuery)
        .toURL()
        .openConnection() as HttpURLConnection
    connection.requestMethod = "POST"
    connection.connectTimeout = 5_000
    connection.readTimeout = 5_000
    return readResponse(connection)
}

internal fun <T> postJson(
    endpoint: String,
    path: String,
    request: Any,
    replyType: Class<T>
): T {
    val json = ObjectMapper()
    val connection = URI.create(endpoint.trimEnd('/') + path)
        .toURL()
        .openConnection() as HttpURLConnection
    connection.requestMethod = "POST"
    connection.connectTimeout = 5_000
    connection.readTimeout = 10_000
    connection.doOutput = true
    connection.setRequestProperty("Content-Type", "application/json")
    connection.outputStream.use { output ->
        output.write(json.writeValueAsBytes(request))
    }
    return json.readValue(readResponse(connection), replyType)
}

internal fun postJsonArray(
    endpoint: String,
    path: String,
    request: Any
): List<String> {
    val json = ObjectMapper()
    val connection = URI.create(endpoint.trimEnd('/') + path)
        .toURL()
        .openConnection() as HttpURLConnection
    connection.requestMethod = "POST"
    connection.connectTimeout = 5_000
    connection.readTimeout = 10_000
    connection.doOutput = true
    connection.setRequestProperty("Content-Type", "application/json")
    connection.outputStream.use { output ->
        output.write(json.writeValueAsBytes(request))
    }
    val values = json.readValue(readResponse(connection), Array<String>::class.java)
    return values.toList()
}

internal suspend fun waitForEvidence(endpoint: String, expected: String) {
    val deadline = System.nanoTime() + java.time.Duration.ofSeconds(30).toNanos()
    var lastBody = ""
    while (System.nanoTime() < deadline) {
        lastBody = get(endpoint.trimEnd('/') + "/evidence")
        if (lastBody.contains(expected)) {
            return
        }
        sleep(200)
    }
    throw IllegalStateException("evidence did not include $expected: $lastBody")
}

private fun get(url: String): String {
    val connection = URI.create(url)
        .toURL()
        .openConnection() as HttpURLConnection
    connection.requestMethod = "GET"
    connection.connectTimeout = 5_000
    connection.readTimeout = 5_000
    return readResponse(connection)
}

private fun readResponse(connection: HttpURLConnection): String {
    val status = connection.responseCode
    val stream = if (status in 200..299) connection.inputStream else connection.errorStream
    val body = stream.use { input ->
        input?.readBytes()?.toString(StandardCharsets.UTF_8) ?: ""
    }
    if (status !in 200..299) {
        throw IllegalStateException("HTTP $status response: $body")
    }
    return body
}
