package systems.zlink.e2e.kotlin.registrationcodec.client.scenarios

import systems.zlink.e2e.kotlin.registrationcodec.EchoManualRes
import systems.zlink.e2e.kotlin.registrationcodec.client.support.ScenarioAssert
import systems.zlink.e2e.kotlin.registrationcodec.client.support.ScenarioHttpClient

class ManualRegistrationScenario(
    private val server: ScenarioHttpClient,
    private val assert: ScenarioAssert,
) {
    fun run() {
        val manual = server.post<EchoManualRes>("/registration/manual")
        assert.that(manual.value == "echo:manual-request" && manual.handler == "manual", "RC-A3 request mismatch")
        assert.that(
            server.waitForEvidence("Send", "EchoManualMsg", "manual-send")
                .any { it.marker == "Send" && it.packetName == "EchoManualMsg" && it.value == "manual-send" },
            "RC-A3 send evidence missing",
        )
        println("scenario RC-A3 passed")
    }
}
