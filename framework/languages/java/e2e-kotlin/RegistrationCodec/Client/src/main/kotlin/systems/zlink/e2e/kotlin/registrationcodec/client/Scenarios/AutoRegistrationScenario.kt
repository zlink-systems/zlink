package systems.zlink.e2e.kotlin.registrationcodec.client.scenarios

import systems.zlink.e2e.kotlin.registrationcodec.EchoAutoRes
import systems.zlink.e2e.kotlin.registrationcodec.client.support.ScenarioAssert
import systems.zlink.e2e.kotlin.registrationcodec.client.support.ScenarioHttpClient

class AutoRegistrationScenario(
    private val server: ScenarioHttpClient,
    private val assert: ScenarioAssert,
) {
    fun run() {
        val auto = server.post<EchoAutoRes>("/registration/auto")
        assert.that(auto.value == "echo:auto-request" && auto.handler == "auto", "RC-A1 request mismatch")
        assert.that(
            server.waitForEvidence("Send", "EchoAutoMsg", "auto-send")
                .any { it.marker == "Send" && it.packetName == "EchoAutoMsg" && it.value == "auto-send" },
            "RC-A1 send evidence missing",
        )
        println("scenario RC-A1 passed")
    }
}
