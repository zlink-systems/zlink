package systems.zlink.e2e.kotlin.registrationcodec.client.scenarios

import systems.zlink.e2e.kotlin.registrationcodec.PackedEchoRes
import systems.zlink.e2e.kotlin.registrationcodec.client.support.CodecScenarioResult
import systems.zlink.e2e.kotlin.registrationcodec.client.support.ScenarioAssert
import systems.zlink.e2e.kotlin.registrationcodec.client.support.ScenarioHttpClient

class RcB3MessagePackCodecScenario(
    private val server: ScenarioHttpClient,
    private val assert: ScenarioAssert,
) {
    fun run(): CodecScenarioResult {
        val packedReply = server.post<PackedEchoRes>("/codec/messagepack")
        assert.that(packedReply.value == "echo:msgpack-request", "RC-B3 request mismatch")
        assert.that(
            server.waitForEvidence("Send", "PackedEchoMsg", "msgpack-send")
                .any { it.marker == "Send" && it.packetName == "PackedEchoMsg" && it.value == "msgpack-send" },
            "RC-B3 send evidence missing",
        )
        println("scenario RC-B3 passed")
        return CodecScenarioResult("RC-B3", "PackedEchoReq", packedReply.value)
    }
}
