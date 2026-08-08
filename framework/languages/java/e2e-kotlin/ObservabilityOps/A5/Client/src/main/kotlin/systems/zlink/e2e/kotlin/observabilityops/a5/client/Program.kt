package systems.zlink.e2e.kotlin.observabilityops.a5.client

import com.fasterxml.jackson.databind.JsonNode
import com.fasterxml.jackson.databind.ObjectMapper
import java.net.URI
import java.net.http.HttpClient
import java.net.http.HttpRequest
import java.net.http.HttpResponse
import java.time.Duration

private val json = ObjectMapper()
private val http = HttpClient.newBuilder().connectTimeout(Duration.ofSeconds(5)).build()

fun main(args: Array<String>) {
    require(args.size == 2 && args[0] == "--endpoint") {
        "Usage: observability-ops-kotlin-a5-client --endpoint <url>"
    }
    val scenario = A5Client(args[1])
    scenario.run()
    println("scenario OBS-A5 passed")
}

private class A5Client(private val endpoint: String) {
    fun run() {
        setMode("NORMAL")
        val keyBefore = snapshot().path("count").asInt()
        ensure(request("key-transition", false).statusCode() == 200, "NORMAL request failed")
        waitForEvent(keyBefore) { it.path("outcome").asText() in SUCCESS }

        setMode("OFF")
        val offBefore = snapshot().path("count").asInt()
        ensure(request("off", false).statusCode() == 200, "OFF request failed")
        waitForCount(offBefore, "OFF produced flow evidence")

        setMode("ERRORS")
        val errorBefore = snapshot().path("count").asInt()
        ensure(request("errors-only-normal", false).statusCode() == 200, "ERRORS normal request failed")
        waitForCount(errorBefore, "ERRORS produced success evidence")
        ensure(request("errors-only-failure", true).statusCode() >= 500, "ERRORS failure did not fail")
        waitForEvent(errorBefore) {
            it.path("outcome").asText() == "ERROR" || it.path("errorType").isTextual
        }

        setMode("NORMAL")
        val resumedBefore = snapshot().path("count").asInt()
        ensure(request("key-transition-again", false).statusCode() == 200, "resumed request failed")
        waitForEvent(resumedBefore) { it.path("outcome").asText() in SUCCESS }
    }

    private fun setMode(value: String) {
        val response = post("/mode?value=$value")
        ensure(response.statusCode() == 200 && response.body().contains("\"mode\":\"$value\""), "mode change failed")
    }

    private fun request(value: String, fail: Boolean) = post("/request?value=$value&fail=$fail")
    private fun snapshot() = json.readTree(get("/flows?after=0").body())

    private fun waitForEvent(after: Int, predicate: (JsonNode) -> Boolean) {
        val deadline = System.nanoTime() + Duration.ofSeconds(10).toNanos()
        var last = snapshot()
        while (System.nanoTime() < deadline) {
            last = snapshot()
            if (last.path("count").asInt() > after && last.path("events").any(predicate)) return
            Thread.sleep(50)
        }
        error("message-flow evidence did not converge: $last")
    }

    private fun waitForCount(expected: Int, message: String) {
        val deadline = System.nanoTime() + Duration.ofSeconds(5).toNanos()
        while (System.nanoTime() < deadline) {
            if (snapshot().path("count").asInt() == expected) return
            Thread.sleep(50)
        }
        error("$message; expected=$expected")
    }

    private fun get(path: String) = request(path, "GET")
    private fun post(path: String) = request(path, "POST")
    private fun request(path: String, method: String): HttpResponse<String> =
        http.send(
            HttpRequest.newBuilder(URI.create(endpoint + path))
                .timeout(Duration.ofSeconds(15))
                .method(method, HttpRequest.BodyPublishers.noBody())
                .build(),
            HttpResponse.BodyHandlers.ofString(),
        )

    private fun ensure(condition: Boolean, message: String) { if (!condition) error(message) }
    private companion object { val SUCCESS = setOf("SENT", "REPLY_RECEIVED", "RECEIVED", "DISPATCHED", "REPLIED") }
}
