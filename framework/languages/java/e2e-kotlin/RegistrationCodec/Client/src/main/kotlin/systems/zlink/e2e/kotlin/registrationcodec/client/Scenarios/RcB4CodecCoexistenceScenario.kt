package systems.zlink.e2e.kotlin.registrationcodec.client.scenarios

import systems.zlink.e2e.kotlin.registrationcodec.CodecRoundtripRes
import systems.zlink.e2e.kotlin.registrationcodec.client.support.ScenarioAssert
import systems.zlink.e2e.kotlin.registrationcodec.client.support.ScenarioHttpClient

class RcB4CodecCoexistenceScenario(
    private val server: ScenarioHttpClient,
    private val assert: ScenarioAssert,
) {
    fun run() {
        val reply = server.post<CodecRoundtripRes>("/codec/roundtrip")
        assert.that(reply.jsonValue == "echo:json-request", "RC-B4 JSON reply mismatch")
        assert.that(reply.protobufValue == "echo:protobuf-request", "RC-B4 Protobuf reply mismatch")
        assert.that(reply.messagePackValue == "echo:msgpack-request", "RC-B4 MessagePack reply mismatch")
        println("scenario RC-B4 passed")
    }
}
