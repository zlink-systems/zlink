package systems.zlink.e2e.kotlin.registrationcodec.client.scenarios

import systems.zlink.e2e.kotlin.registrationcodec.DiLifecycleRes
import systems.zlink.e2e.kotlin.registrationcodec.client.support.ScenarioAssert
import systems.zlink.e2e.kotlin.registrationcodec.client.support.ScenarioHttpClient

class RcA4DiLifecycleScenario(
    private val server: ScenarioHttpClient,
    private val assert: ScenarioAssert,
) {
    fun run() {
        val diReplies = server.post("/registration/di-lifecycle", null, Array<DiLifecycleRes>::class.java).toList()
        assert.that(diReplies.map { it.scopedId }.distinct().size == 3, "RC-A4 scoped dependency was not recreated")
        assert.that(diReplies.map { it.singletonId }.distinct().size == 1, "RC-A4 singleton dependency changed")
        assert.that(diReplies[2].disposedCount == 3, "RC-A4 dispose count mismatch")
        assert.that(
            server.waitForEvidence("DI", "DiLifecycleReq", "${diReplies[2].scopedId}:${diReplies[2].singletonId}:di-2")
                .any { it.value.endsWith(":di-2") },
            "RC-A4 DI evidence missing",
        )
        println("scenario RC-A4 passed")
    }
}
