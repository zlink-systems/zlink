package systems.zlink.e2e.kotlin.registrationcodec.client.scenarios

import systems.zlink.e2e.kotlin.registrationcodec.EchoManualRes
import systems.zlink.e2e.kotlin.registrationcodec.client.support.ScenarioAssert
import systems.zlink.e2e.kotlin.registrationcodec.client.support.ScenarioHttpClient

class RcA5FilterOrderingScenario(
    private val server: ScenarioHttpClient,
    private val assert: ScenarioAssert,
) {
    fun run() {
        val filtered = server.post<EchoManualRes>("/registration/filter-order")
        assert.that(filtered.value == "echo:filter-order-request", "RC-A5 request mismatch")
        val expected = listOf(
            "first-before",
            "second-before",
            "second-after",
            "first-after",
        )
        val actual = server.waitForEvidence("Filter", "EchoManualReq", "first-after")
            .filter { it.marker == "Filter" && it.packetName == "EchoManualReq" }
            .map { it.value }
        assert.that(
            actual.size >= expected.size && actual.takeLast(expected.size) == expected,
            "RC-A5 filter order mismatch: $actual",
        )
        println("scenario RC-A5 passed")
    }
}
