package systems.zlink.e2e.kotlin.registrationcodec.client.scenarios

import systems.zlink.e2e.kotlin.registrationcodec.JsonGoldenRes
import systems.zlink.e2e.kotlin.registrationcodec.client.support.ScenarioAssert
import systems.zlink.e2e.kotlin.registrationcodec.client.support.ScenarioHttpClient

class RcB6JsonGoldenScenario(
    private val server: ScenarioHttpClient,
    private val assert: ScenarioAssert,
) {
    fun run() {
        val result = server.post<JsonGoldenRes>("/codec/json-golden")
        assert.that(result.displayName == "Ada Lovelace", "RC-B6 display name changed")
        assert.that(result.status == "ready", "RC-B6 status changed")
        assert.that(result.balance == -9_223_372_036_854_775_000L, "RC-B6 int64 value changed")
        assert.that(
            result.payload.contentEquals(byteArrayOf(0x00, 0x7f, 0x80.toByte(), 0xff.toByte())),
            "RC-B6 bytes value changed",
        )
        assert.that(result.score == 2_147_000_001, "RC-B6 int32 value changed")
        assert.that(result.ratio == 0.125, "RC-B6 floating value changed")
        assert.that(result.optionalNote == null, "RC-B6 nullable value changed")
        assert.that(result.contentType == "application/json", "RC-B6 did not use default JSON")
        assert.that(
            server.waitForEvidence(
                "Request",
                "JsonGoldenReq",
                "Ada Lovelace|ready|-9223372036854775000|2147000001|4",
            ).any { it.marker == "Request" && it.packetName == "JsonGoldenReq" },
            "RC-B6 server evidence missing",
        )
        println("scenario RC-B6 passed")
    }
}
