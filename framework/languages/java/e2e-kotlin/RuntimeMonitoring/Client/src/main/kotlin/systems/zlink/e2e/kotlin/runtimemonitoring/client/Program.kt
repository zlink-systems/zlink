package systems.zlink.e2e.kotlin.runtimemonitoring.client

import com.fasterxml.jackson.databind.ObjectMapper

fun main(args: Array<String>) {
    systems.zlink.e2e.kotlin.runtimemonitoring.Env.configure(args)
    try {
        ClientScenario(ObjectMapper()).run()
    } catch (blocked: ScenarioBlocker) {
        println("scenario ${systems.zlink.e2e.kotlin.runtimemonitoring.Env.get("e2e.scenario", "all")} blocked: ${blocked.message}")
        return
    }
    println("runtime-monitoring kotlin e2e result=passed")
}
