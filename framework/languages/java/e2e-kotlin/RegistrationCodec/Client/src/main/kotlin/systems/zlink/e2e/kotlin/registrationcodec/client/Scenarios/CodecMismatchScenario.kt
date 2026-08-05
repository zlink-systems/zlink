package systems.zlink.e2e.kotlin.registrationcodec.client.scenarios

import systems.zlink.e2e.kotlin.registrationcodec.JsonEchoRes
import systems.zlink.e2e.kotlin.registrationcodec.client.support.ScenarioAssert
import systems.zlink.e2e.kotlin.registrationcodec.client.support.ScenarioHttpClient

class CodecMismatchScenario(
    private val codecRequester: ScenarioHttpClient,
    private val assert: ScenarioAssert,
) {
    fun run() {
        val mismatch = codecRequester.post("/codec/protobuf/request", null, Map::class.java)
        assert.that(mismatch["error"] == true, "RC-B5 Protobuf mismatch was not rejected")
        val secondJson = codecRequester.post<JsonEchoRes>("/codec/json/request")
        assert.that(secondJson.value == "echo:json-from-requester", "RC-B5 JSON traffic did not recover after mismatch")
        println("scenario RC-B5 passed")
    }
}
