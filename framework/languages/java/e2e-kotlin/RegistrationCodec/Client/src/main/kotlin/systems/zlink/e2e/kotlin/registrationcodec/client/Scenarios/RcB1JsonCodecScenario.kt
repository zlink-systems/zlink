package systems.zlink.e2e.kotlin.registrationcodec.client.scenarios

import systems.zlink.e2e.kotlin.registrationcodec.JsonEchoRes
import systems.zlink.e2e.kotlin.registrationcodec.client.support.CodecScenarioResult
import systems.zlink.e2e.kotlin.registrationcodec.client.support.ScenarioAssert
import systems.zlink.e2e.kotlin.registrationcodec.client.support.ScenarioHttpClient

class RcB1JsonCodecScenario(
    private val server: ScenarioHttpClient,
    private val assert: ScenarioAssert,
) {
    fun run(): CodecScenarioResult {
        val jsonReply = server.post<JsonEchoRes>("/codec/json")
        assert.that(jsonReply.value == "echo:json-request" && jsonReply.handler == "json", "RC-B1 request mismatch")
        assert.that(
            server.waitForEvidence("Send", "JsonEchoMsg", "json-send")
                .any { it.marker == "Send" && it.packetName == "JsonEchoMsg" && it.value == "json-send" },
            "RC-B1 send evidence missing",
        )
        println("scenario RC-B1 passed")
        return CodecScenarioResult("RC-B1", "JsonEchoReq", jsonReply.value)
    }
}
