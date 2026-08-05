package systems.zlink.e2e.kotlin.registrationcodec.client.scenarios

import systems.zlink.e2e.kotlin.registrationcodec.EchoAttrRes
import systems.zlink.e2e.kotlin.registrationcodec.client.support.ScenarioAssert
import systems.zlink.e2e.kotlin.registrationcodec.client.support.ScenarioHttpClient

class AttributeRegistrationScenario(
    private val server: ScenarioHttpClient,
    private val assert: ScenarioAssert,
) {
    fun run() {
        val attr = server.post<EchoAttrRes>("/registration/attribute")
        assert.that(attr.value == "echo:attr-request" && attr.handler == "attr", "RC-A2 request mismatch")
        assert.that(
            server.waitForEvidence("Send", "EchoAttrMsg", "attr-send")
                .any { it.marker == "Send" && it.packetName == "EchoAttrMsg" && it.value == "attr-send" },
            "RC-A2 send evidence missing",
        )
        println("scenario RC-A2 passed")
    }
}
