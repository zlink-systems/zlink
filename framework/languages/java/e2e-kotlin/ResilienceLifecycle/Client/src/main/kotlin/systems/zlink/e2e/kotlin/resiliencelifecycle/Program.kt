package systems.zlink.e2e.kotlin.resiliencelifecycle

import com.fasterxml.jackson.databind.ObjectMapper

fun main(args: Array<String>) {
    Env.configure(args)
    val options = ClientOptions.fromEnv()
    ClientScenario(ObjectMapper(), options).run(options.mode)
    println("resilience-lifecycle kotlin e2e result=passed")
}
