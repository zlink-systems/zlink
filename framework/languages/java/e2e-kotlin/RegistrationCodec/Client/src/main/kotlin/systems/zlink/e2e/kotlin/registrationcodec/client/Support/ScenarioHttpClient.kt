package systems.zlink.e2e.kotlin.registrationcodec.client.support

import com.fasterxml.jackson.databind.ObjectMapper
import com.fasterxml.jackson.module.kotlin.readValue
import java.net.URI
import java.net.http.HttpClient
import java.net.http.HttpRequest
import java.net.http.HttpResponse
import java.time.Duration
import systems.zlink.e2e.kotlin.registrationcodec.EvidenceEntry
import systems.zlink.e2e.kotlin.registrationcodec.EvidenceWaitReq
import systems.zlink.e2e.kotlin.registrationcodec.EvidenceWaitRes

class ScenarioHttpClient(
    private val baseUrl: String,
    private val json: ObjectMapper,
) {
    private val http = HttpClient.newHttpClient()

    inline fun <reified T> post(path: String): T =
        post(path, null, T::class.java)

    fun <T> post(path: String, body: Any?, responseType: Class<T>): T {
        val builder = HttpRequest.newBuilder(URI.create("$baseUrl$path"))
            .timeout(Duration.ofSeconds(3))
            .POST(
                if (body == null) {
                    HttpRequest.BodyPublishers.noBody()
                } else {
                    HttpRequest.BodyPublishers.ofByteArray(json.writeValueAsBytes(body))
                },
            )
        if (body != null) {
            builder.header("Content-Type", "application/json")
        }
        val response = http.send(builder.build(), HttpResponse.BodyHandlers.ofString())
        if (response.statusCode() !in 200..299) {
            throw IllegalStateException("POST $path failed with ${response.statusCode()}: ${response.body()}")
        }
        return json.readValue(response.body(), responseType)
    }

    fun waitForEvidence(marker: String, packetName: String, value: String): List<EvidenceEntry> {
        val response = post(
            "/evidence/wait",
            EvidenceWaitReq(marker, packetName, value),
            EvidenceWaitRes::class.java,
        )
        return response.entries
    }
}
