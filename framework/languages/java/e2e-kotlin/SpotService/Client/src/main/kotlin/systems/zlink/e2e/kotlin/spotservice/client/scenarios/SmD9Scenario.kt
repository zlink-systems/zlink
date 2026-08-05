package systems.zlink.e2e.kotlin.spotservice.client.scenarios

import systems.zlink.e2e.kotlin.spotservice.client.support.ActorSessionScenarioContext
import systems.zlink.e2e.kotlin.spotservice.client.support.ensure
import systems.zlink.e2e.kotlin.spotservice.client.support.eventually

internal object SmD9Scenario {
    suspend fun run(context: ActorSessionScenarioContext) {
        val observed = eventually {
            val names = context.observedInboundNames()
            ensure(names.size >= 4, "SM-D9 inbound observer did not observe stream replies")
            names
        }
        ensure(
            observed.all { it.isNotBlank() },
            "SM-D9 inbound observer returned a blank packet name",
        )

        println("scenario SM-D9 passed")
    }
}
