package systems.zlink.e2e.kotlin.registrationcodec.client.scenarios

import systems.zlink.e2e.kotlin.registrationcodec.client.support.CodecScenarioResult
import systems.zlink.e2e.kotlin.registrationcodec.client.support.ScenarioAssert
import systems.zlink.e2e.kotlin.registrationcodec.client.support.ScenarioHttpClient

class RcB2ProtobufCodecScenario(
    private val server: ScenarioHttpClient,
    private val assert: ScenarioAssert,
) {
    fun run(): CodecScenarioResult {
        val protobufReply = server.post("/codec/protobuf", null, Map::class.java)
        val value = protobufReply["value"] as String
        assert.that(value == "echo:protobuf-request", "RC-B2 request mismatch")
        assert.that(
            server.waitForEvidence("Send", "ProtobufEcho", "protobuf-send")
                .any { it.marker == "Send" && it.packetName == "ProtobufEcho" && it.value == "protobuf-send" },
            "RC-B2 send evidence missing",
        )
        println("scenario RC-B2 passed")
        return CodecScenarioResult("RC-B2", "ProtobufEcho", value)
    }
}
