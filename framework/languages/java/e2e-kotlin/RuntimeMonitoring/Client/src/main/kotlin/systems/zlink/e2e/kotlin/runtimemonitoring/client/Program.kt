package systems.zlink.e2e.kotlin.runtimemonitoring.client

import com.fasterxml.jackson.databind.ObjectMapper

fun main(args: Array<String>) {
    systems.zlink.e2e.kotlin.runtimemonitoring.Env.configure(args)
    ClientScenario(ObjectMapper()).run()
    println("runtime-monitoring kotlin e2e result=passed")
}
